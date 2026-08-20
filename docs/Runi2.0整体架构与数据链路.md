# Runi 2.0 整体架构与数据链路

> 本文描述当前代码已经实现并通过测试的架构。设计目标与开发计划见《Runi2.0升级需求与开发计划.md》。未实现能力在“功能边界”中单独列出。

## 1. 系统定位

Runi 2.0 是面向本地代码仓库的轻量级 C++20 Agent Runtime。它支持两种运行形态：

- **嵌入式 CLI**：`runi` 直接在当前进程中创建 `Runi`，沿用单 Agent 交互体验；
- **本地服务**：`runi_server` 在 loopback 上提供 HTTP API，以有界线程池并发处理多个 Session，并通过 SQLite 保存服务状态和 Agent Session。

新增能力由四个独立模块组成：

1. `BoundedExecutor`：有界并发、背压、取消和优雅关闭；
2. `AgentRegistry/MultiAgentRuntime`：Agent 容量租约和有界 fan-out/fan-in；
3. `SqliteStateStore/ContextCache`：事务状态、版本 CAS、事件回放、分层记忆和线程安全缓存；
4. `RuntimeService/SocketHttpServer/RuniClient`：异步任务 API、loopback HTTP 服务与 C++ 客户端。

## 2. 自顶向下模块分层

```text
┌──────────────────────────────────────────────────────────────┐
│ L1 接入层                                                    │
│ runi CLI │ runi_server │ RuniClient │ HTTP/1.1 + SSE replay │
├──────────────────────────────────────────────────────────────┤
│ L2 服务编排层                                                │
│ RuntimeService │ 路由 │ Idempotency-Key │ Cancel/Resume     │
├──────────────────────────────────────────────────────────────┤
│ L3 并发与多 Agent 层                                         │
│ BoundedExecutor │ AgentRegistry │ AgentLease │ Fan-out/in    │
├──────────────────────────────────────────────────────────────┤
│ L4 Agent 执行层                                              │
│ RuniAgentServiceHandler │ Runi │ AgentLoop │ ContextManager │
│ ModelClient │ ToolExecutor │ Checkpoint                       │
├──────────────────────────────────────────────────────────────┤
│ L5 状态与缓存层                                              │
│ SqliteStateStore │ SqliteSessionStore │ ContextCache         │
│ RunStore/Trace/Report │ LayeredMemory                        │
├──────────────────────────────────────────────────────────────┤
│ L6 系统资源层                                                │
│ SQLite/WAL │ Winsock/BSD Socket │ jthread │ Process │ Files │
└──────────────────────────────────────────────────────────────┘
```

依赖方向始终从上向下。`AgentLoop` 不依赖 HTTP，`SqliteStateStore` 不依赖 Agent，`MultiAgentRuntime` 不依赖具体模型 Provider，因此各模块可以独立测试和嵌入。

## 3. 源码分包与核心库

```text
include/runi/                 src/
├── core/                     ├── core/          基础错误、Result、JSON、Hash、时间
├── model/                    ├── model/         Provider、HTTP、模型协议 DTO
├── tool/                     ├── tool/          工具、进程、Workspace 与安全策略
├── context/                  ├── context/       Prompt 裁剪、记忆与 Checkpoint
├── state/                    ├── state/         Session、Run、SQLite 与缓存
├── agent/                    ├── agent/         AgentLoop、Action 解析与 Runi 门面
├── orchestration/            ├── orchestration/ 有界执行器、多 Agent 与统一提交
├── service/                  ├── service/       RuntimeService、HTTP Server/Client
├── evaluation/               └── evaluation/    固定输出评测与消融
└── runi.hpp                                  SDK 聚合头
```

`include/runi` 与 `src` 采用镜像目录；头文件引用使用完整模块路径，例如 `runi/service/service.hpp`。包级依赖保持 `core → model → tool/state → context/orchestration → agent → service/evaluation` 的总体方向，其中 `model/model_action.hpp` 是 Agent 与 Tool 共用的协议 DTO，避免 Tool 反向依赖 Agent。所有实现仍链接为轻量的 `runi_core` 静态库，并产出 `runi`、`runi_server`、`runi_eval` 和两组测试程序。`runi/runi.hpp` 只聚合运行时 SDK；开发期 `evaluation` API 继续显式引入，避免污染服务端/客户端接口面。

`runi_server` 为每个 Run 创建独立 ModelClient，避免 Provider 对象中的 Completion Metadata 在多个线程间发生数据竞争。

## 4. 服务请求的数据链路

### 4.1 创建 Session

```text
Client
  → POST /v1/sessions
  → SocketHttpServer 有上限读取 HTTP Request
  → RuntimeService 校验 Body 与 Workspace Root
  → SqliteStateStore::create_session
  → sessions 表写入 Version 0
  → 201 Created
```

服务默认只允许 `allowed_workspace_root` 及其子目录，路径在写库前执行规范化和边界检查。

### 4.2 创建并执行 Run

```text
POST /v1/sessions/{session_id}/runs
  │ Idempotency-Key
  ▼
SqliteStateStore::create_run
  ├── Key 已存在 → 返回原 Run，不重复执行
  └── 新 Run → status=queued + run.queued Event
                         │
                         ▼
                 BoundedExecutor::submit
                  ├── Queue Full → 503 + failed
                  └── Accepted
                         │
                         ▼
                per-session Mutex
                         │
                         ▼
             queued → running + run.started
                         │
                         ▼
             RuniAgentServiceHandler
              ├── WorkspaceContext
              ├── per-run ModelClient
              ├── SqliteSessionStore
              └── Runi::ask(stop_token)
                         │
                         ▼
             AgentLoop / Context / Tool
                         │
          ┌──────────────┼───────────────┐
          ▼              ▼               ▼
      succeeded        failed         cancelled
          │              │               │
          └────── Run Event + SQLite ─────┘
```

HTTP 线程只负责协议解析和提交任务，模型与工具执行在 Run Executor 中完成，所以长模型请求不会阻塞监听线程。

## 5. AgentLoop 内部数据链路

```text
User Request
  → Session Task Summary
  → History append(user)
  → TaskState / Run Artifact
  → ContextManager::build
      ├── Prompt Prefix
      ├── Layered Memory
      ├── Relevant Memory Top-K
      ├── History
      └── Current Request
  → ModelClient::complete
  → ModelActionParser
      ├── ToolCall → ToolExecutor → History/Memory/Checkpoint → 下一轮
      ├── Retry → History → 下一轮
      └── Final → Session/Memory/Checkpoint/Report → 返回
```

服务模式只替换 Session 持久化入口，不重写 AgentLoop。`SqliteSessionStore` 实现原有 `ISessionStore` 接口，因此 CLI 文件 SessionStore 和服务 SQLite SessionStore 共用同一 Agent 语义。

## 6. 并发模型

### 6.1 线程与队列

```text
1 Accept jthread
    └── Connection BoundedExecutor
            └── RuntimeService::handle
                    └── Run BoundedExecutor
                            └── Runi AgentLoop
```

每个 Executor 包含：

- 固定数量 `std::jthread`；
- 有界等待队列；
- `mutex + condition_variable_any`；
- `std::stop_token`；
- accepted/rejected/active/queued 指标；
- 析构自动 Stop 和 Join。

不存在 detached thread。队列满时立即返回结构化错误，不进行无界内存扩张。

### 6.2 Session 隔离

- 不同 Session 可以并行执行；
- 同一 Session 的完整 Agent Turn 通过 per-session Mutex 串行化；
- 这是为了保护现有 `SessionState/LayeredMemory/Checkpoint` 的复合不变量；
- SQLite 操作有独立短临界区，不持有数据库锁执行模型、工具或网络 I/O。

当前 per-session Mutex 会覆盖一次完整 Agent Turn。它不会阻塞其他 Session，但同一 Session 的第二个 Run 会占用一个 Run Worker 等待。Actor/Strand 化排队可作为后续优化。

## 7. 多 Agent 模型

### 7.1 Agent Registry

```text
AgentDescriptor
├── id
├── role: Coordinator / Worker / Reviewer
├── capabilities
├── max_concurrency
├── read_only
└── execute(task, stop_token)
```

`AgentRegistry::acquire()` 按角色、能力和当前负载选择 Agent。达到容量时等待到 Deadline；取消会唤醒等待者。

### 7.2 RAII AgentLease

```text
acquire
  → active_tasks++
  → move-only AgentLease
  → execute
  → ~AgentLease
  → active_tasks--
  → notify waiting tasks
```

异常、提前返回或取消都不会泄漏 Agent 容量。

### 7.3 Fan-out/Fan-in

`MultiAgentRuntime::run_parallel()` 将子任务提交给有界 Executor：

- `collect_all` 保留所有局部成功和失败；
- `fail_fast` 在首个失败后请求取消；
- 输出始终按照输入任务序号排列，而不是按照完成顺序；
- Agent Capacity 可以小于 Executor Worker 数，两层限制分别保护系统资源和具体 Agent。

当前 `MultiAgentRuntime` 是显式 C++ Host API。现有模型可见的 `delegate` 工具仍保持单子任务、深度受限和只读语义；本轮没有让模型自动生成通用多 Agent DAG。

## 8. Workspace 一致性

多 Agent Worker 应并行读取并返回 `PatchProposal`：

```text
path + expected_sha256 + new_content
```

`WorkspaceCommitter`：

1. 使用 `WorkspaceGuard` 解析所有目标；
2. 拒绝重复路径；
3. 校验全部目标 SHA-256；
4. 将新内容写入同目录临时文件；
5. 原文件改名为 Backup；
6. 临时文件改名为目标；
7. 全部成功后删除 Backup；
8. 中途失败则按逆序恢复。

因此普通运行时错误不会留下部分批次。操作系统崩溃或断电下的多文件原子性仍不等同数据库事务；审计与启动清理是后续增强方向。

## 9. SQLite 状态模型

```text
sessions
  PK id
  workspace_root
  state_json
  state_version

runs
  PK id
  FK session_id
  UNIQUE request_id
  status / result / error

run_events
  PK(run_id, sequence)
  event_type / payload_json

memory_records
  PK(scope, record_key)
  kind / text / tags
  source_path / source_sha256
  expires_at_ms / version
```

运行状态机：

```text
queued → running → succeeded
   │        ├────→ failed
   │        ├────→ cancelled
   │        └────→ interrupted → queued
   ├─────────────→ cancelled
   └─────────────→ failed
```

SQLite 使用 WAL、Foreign Key、Busy Timeout 和 `synchronous=NORMAL`。状态转换带期望旧状态，Session 更新带期望版本，分别防止非法状态覆盖和 Lost Update。

## 10. Memory 与 Cache

两者保持独立：

- `LayeredMemory/MemoryRecord` 保存 Agent 需要长期记住的事实；
- `ContextCache` 保存文件摘要、Prompt Prefix 等确定性计算结果。

`ContextCache` 实现：

- Mutex 保护 LRU 和 Entry Map；
- TTL 过期；
- 固定 Entry 容量；
- Singleflight：相同 Key 的并发 Miss 共享 `shared_future`；
- hit/miss/eviction/coalesced 指标。

默认不缓存完整模型回答，避免随机输出和潜在工具副作用被错误复用。

## 11. HTTP 与客户端协议

已实现端点：

```text
POST /v1/sessions
POST /v1/sessions/{id}/runs
GET  /v1/runs/{id}
GET  /v1/runs/{id}/events
POST /v1/runs/{id}:cancel
POST /v1/runs/{id}:resume
```

`SocketHttpServer` 使用原生 Winsock/BSD Socket 兼容层，只绑定 `127.0.0.1`。请求 Header 与 Body 共享总字节上限。连接队列满返回 `503`，Body 过大返回 `413`。

`runi_server` 的危险工具审批默认是 `never`；只有显式传入 `--approval auto` 才允许无人值守的写文件、补丁和命令执行。

`RuniClient` 提供同步 C++ 请求接口，支持连接/收发超时、Content-Length 校验、响应大小上限和结构化错误。事件端点返回 `text/event-stream`，客户端通过 `Last-Event-ID` 读取持久化事件之后的增量回放。

## 12. 资源所有权与 RAII

| 资源 | 所有者 | 释放方式 |
|---|---|---|
| Worker Thread | `BoundedExecutor` | `jthread` Stop + Join |
| Agent Capacity | `AgentLease` | 析构归还计数并 Notify |
| SQLite Connection | `SqliteStateStore` | 析构 `sqlite3_close` |
| SQLite Statement | 内部 `Statement` | 析构 `sqlite3_finalize` |
| SQLite Transaction | 内部 `Transaction` | 未 Commit 自动 Rollback |
| Socket | `SocketHandle` | 析构 `closesocket/close` |
| Listener | `SocketHttpServer` | Stop、Shutdown、Close、Join |
| Workspace 临时文件 | `WorkspaceCommitter` | 成功清理或失败回滚 |

所有拥有型对象禁止复制；需要跨异步任务共享的 StopSource、Session Mutex 和 Handler 使用 `shared_ptr`，其余对象优先唯一所有权或引用。

## 13. 取消与关闭链路

```text
POST :cancel
  → active_runs[run_id].stop_source.request_stop()
  → RuntimeService Handler
  → Runi::ask(stop_token)
  → AgentLoop 步骤边界检查
  → Checkpoint/Report
  → runs.status=cancelled
  → run.cancelled Event
```

Server Stop：

```text
停止 Accept
  → Close Listener
  → Join Accept Thread
  → Stop/Join Connection Executor
  → RuntimeService 请求取消 Active Runs
  → 等待 Active Map 清空
  → Stop/Join Run Executor
  → Close SQLite
```

## 14. 测试与验证

`runi_runtime_tests` 覆盖：

- Executor 并发上限、Queue Full、Future、Stop 和 Shutdown；
- Agent Registry 容量、Lease RAII、稳定 Fan-in 和失败策略；
- Workspace 多文件提交、冲突和重复路径；
- SQLite WAL、CAS、幂等 Run、状态机、Event Replay 和恢复；
- Memory Scope、TTL、SHA 失效；
- Cache LRU、TTL 和 Singleflight；
- RuntimeService 创建、查询、取消、恢复、过载和 SSE；
- SQLite SessionStore 到真实 AgentLoop 的集成；
- RuniClient 与 SocketHttpServer 的真实 loopback TCP 往返。

本轮验证结果：

```text
Debug full build: passed
CTest: 7/7 passed
  runi.unit
  runi.runtime
  runi.cli_help
  runi.cli_help_equals
  runi.server_help
  runi.benchmark
  runi.context_ablation
```

## 15. 当前功能边界

- 网络层为轻量 HTTP/1.1，一个请求一个连接；没有 TLS、HTTP/2 和 Keep-Alive Pool；
- SSE 是持久化事件的有限回放，不是无限长实时推送；
- Provider HTTP 当前仍为阻塞调用，取消在调用返回后的 AgentLoop 步骤边界生效；
- SQLite 使用单连接和进程内 Mutex，适合单机服务，不宣称分布式或高写入吞吐；
- 同一 Session 的完整 Agent Turn 串行化，跨 Session 才并行；
- MultiAgentRuntime 已作为显式 Host API 提供，但模型可见 `delegate` 尚未自动升级为并行任务规划器；
- WorkspaceCommitter 可恢复普通失败，但不提供断电级多文件原子事务；
- 服务默认 loopback，无公网鉴权和 TLS，禁止直接暴露到不可信网络。

这些边界保证 Runi 仍然保持轻量，同时已经具备 C++ 服务端项目所需的并发执行、资源生命周期、数据库事务、网络协议、客户端接入和 Agent 状态恢复骨架。
