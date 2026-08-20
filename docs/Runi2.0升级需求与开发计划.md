# Runi 2.0 升级需求与开发计划

> 状态：实施基线。本文描述本轮必须交付的功能、架构、数据结构、边界、开发顺序与测试矩阵。文中的“完成”必须以自动化测试通过为准，不能把设计目标写成已实现事实。

## 1. 升级目标

Runi 从单进程、单会话顺序执行的 Coding Agent，升级为面向本地代码仓库的轻量级 C++20 多会话 Agent Runtime Service。保留现有 `AgentLoop`、上下文治理、分层记忆、Checkpoint、Workspace 安全和审计工件，在外层增加四个模块：

1. 有界并发执行；
2. 轻量多 Agent 协作；
3. SQLite 一致性状态与线程安全缓存；
4. 本地 HTTP 服务和 C++ 客户端。

必须满足的工程原则：

- 所有线程、队列、Agent 容量、请求体、输出和重试均有上限；
- 不在持锁期间执行模型请求、网络 I/O、进程等待或文件扫描；
- 跨 Session 并行，同一 Session 的状态提交串行化；
- 多 Agent 并行读取，Workspace 修改由单一 Committer 串行提交；
- SQLite 是服务状态真相源，JSONL/Report 继续作为审计工件；
- 资源通过 RAII 管理，任务通过 `std::stop_token` 协作取消；
- 系统评测与模型能力分离，新增测试全部使用固定函数或 FakeModel/FakeTool。

## 2. 非目标与功能边界

本轮不实现：

- 跨机器调度、主从复制、服务发现和分布式共识；
- Redis、Kafka、独立向量数据库或通用 RAG 平台；
- 通用 DAG/工作流 DSL、远程 Agent Federation、完整 MCP/A2A 网关；
- HTTP/2、TLS 终止、反向代理和公网暴露；
- 多 Agent 同时直接写同一 Workspace；
- 对阻塞式第三方模型 SDK 的强制抢占。取消首先保证排队任务和步骤边界可停止，正在执行的阻塞调用只能在底层传输支持时主动中断。

首版网络服务只监听 loopback，支持一个请求一个连接的 HTTP/1.1。事件接口使用可通过 `Last-Event-ID` 恢复的有限 SSE 回放；持续长连接推送、TLS 和 Keep-Alive 复用属于后续扩展。

## 3. 总体架构

```text
Runi CLI / RuniClient
          │ HTTP/1.1 + SSE replay
          ▼
┌─────────────────────────────────────────────┐
│ RuniServer                                  │
│ request limit / routing / idempotency       │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ RuntimeService                              │
│ RunContext / admission / cancellation       │
└───────────────┬────────────────┬────────────┘
                │                │
                ▼                ▼
┌───────────────────────┐  ┌──────────────────┐
│ BoundedExecutor       │  │ SessionCoordinator│
│ jthread / queue / stop│  │ version / CAS     │
└────────────┬──────────┘  └────────┬─────────┘
             ▼                      │
┌───────────────────────────────────▼─────────┐
│ MultiAgentRuntime                           │
│ AgentRegistry / AgentLease / fan-out/fan-in │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ Existing AgentLoop / Model / Tools          │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ SQLiteStateStore + ContextCache + Artifacts │
└─────────────────────────────────────────────┘
```

依赖预算：C++20 标准库、SQLite3、Windows Winsock/BSD Socket 兼容层和项目现有 JSON 实现。不增加大型 Web 框架。

### 3.1 源码分包约束

`include/runi` 与 `src` 使用同名镜像模块：`core`、`model`、`tool`、`context`、`state`、`agent`、`orchestration`、`service` 和 `evaluation`。公共引用必须写完整模块路径；跨模块共享的模型动作与工具调用 DTO 放在 `model`，避免 `tool` 反向依赖 `agent`。`include/runi/runi.hpp` 仅聚合运行时 SDK，开发期 `evaluation` API 需要显式引入；内部模块仍优先包含最小依赖头文件。

## 4. 模块一：并发执行

### 4.1 数据结构

```cpp
struct RunContext {
    std::string run_id;
    std::string session_id;
    std::string workspace_id;
    std::chrono::steady_clock::time_point deadline;
    std::stop_source stop_source;
};

struct ExecutorSnapshot {
    std::size_t workers;
    std::size_t queued;
    std::size_t active;
    std::size_t accepted;
    std::size_t rejected;
};
```

`BoundedExecutor` 使用固定数量 `std::jthread`、有界 `deque`、`mutex` 和 `condition_variable_any`。`submit()` 返回 `future`；队列满返回 `executor_queue_full`，停止接单后返回 `executor_stopped`。析构函数执行优雅关闭并 Join 所有 Worker。

### 4.2 并发语义

- 队列容量只统计等待任务，活跃任务单独统计；
- Worker 使用带谓词等待，正确处理虚假唤醒；
- Shutdown 停止接单并请求取消，已接收任务获得 stop token 后收敛；
- 原子变量只记录独立计数，队列和生命周期不变量由互斥锁保护；
- Coordinator 和叶子任务使用不同执行角色，禁止父任务占满线程后同步等待同一池中的子任务。

## 5. 模块二：多 Agent

### 5.1 数据结构

```cpp
struct AgentDescriptor {
    std::string id;
    AgentRole role;
    std::set<std::string> capabilities;
    std::size_t max_concurrency;
    bool read_only;
};

struct AgentTask {
    std::string id;
    AgentRole role;
    std::set<std::string> required_capabilities;
    std::string input;
    JsonValue context_snapshot;
    std::chrono::steady_clock::time_point deadline;
};
```

`AgentRegistry` 按角色、能力和当前负载选择 Agent。`AgentLease` 是 move-only RAII 对象，析构时减少 `active_tasks` 并唤醒等待者。`MultiAgentRuntime` 提供有界 fan-out/fan-in、稳定结果顺序、`collect_all` 和 `fail_fast`。

### 5.2 上下文与 Workspace 边界

- 子 Agent 获得不可变 Session 快照，不共享可变 Prompt；
- Worker 返回证据、摘要和 `PatchProposal`；
- 多个只读 Worker 可以并行；
- `WorkspaceCommitter` 在锁内只完成校验和短提交，不执行模型调用；
- 提交前验证基础 SHA-256，发现漂移返回 `workspace_conflict`；
- 同一批多文件补丁先全部校验、再暂存、最后提交，失败时清理临时文件并恢复已替换文件。

## 6. 模块三：一致性、状态与缓存

### 6.1 SQLite 数据模型

```text
sessions(id, workspace_root, state_json, state_version, created_at, updated_at)
runs(id, session_id, parent_run_id, request_id, request, status,
     result, error, deadline_ms, created_at, updated_at)
run_events(run_id, sequence, event_type, payload_json, created_at)
memory_records(scope, record_key, kind, text, tags_json,
               source_path, source_sha256, expires_at_ms, version)
```

约束：

- `request_id` 唯一，实现创建 Run 的幂等性；
- `(run_id, sequence)` 唯一，支持 SSE 断点回放；
- Session 更新使用 `state_version` 乐观 CAS；
- Run 状态转换校验期望状态，防止完成任务被重复取消；
- 启动恢复将遗留 `running` 任务转换为 `interrupted`。

SQLite 配置：

```sql
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;
PRAGMA synchronous = NORMAL;
```

首版 `SqliteStateStore` 使用单连接和进程内互斥锁，将数据库操作串行化。事务必须短小，锁外执行模型和工具，因此不会把 Agent 主耗时放在数据库临界区。该边界适合单机轻量服务；多进程连接池不是本轮目标。

### 6.2 RAII 数据库对象

- `SqliteHandle` 析构调用 `sqlite3_close`；
- `SqliteStatement` 析构调用 `sqlite3_finalize`；
- `SqliteTransaction` 未提交时自动回滚；
- 所有 SQL 使用 Prepared Statement 和参数绑定。

### 6.3 ContextCache

缓存只保存确定性结果，例如文件摘要、Prompt Prefix 和工具 Schema，不缓存默认模型回答。实现线程安全 LRU、TTL、容量上限和 Singleflight。同一 Key 并发 Miss 只运行一次计算，其余调用等待共享 Future。

## 7. 模块四：服务层

### 7.1 API

```text
POST /v1/sessions
POST /v1/sessions/{id}/runs
GET  /v1/runs/{id}
GET  /v1/runs/{id}/events
POST /v1/runs/{id}:cancel
POST /v1/runs/{id}:resume
```

`POST runs` 使用 `Idempotency-Key`。任务入队返回 `202`；版本冲突返回 `409`；队列满返回 `503`；请求过大返回 `413`；未知资源返回 `404`。

### 7.2 网络实现

`SocketHttpServer` 只绑定 `127.0.0.1`，Accept 线程只接受连接并把连接任务提交到有界 Executor。连接 Worker 负责：

1. 有上限地读取 Header 和 Body；
2. 解析 Method、Target、Content-Length 和必要 Header；
3. 调用 `RuntimeService::handle()`；
4. 返回 HTTP/1.1 响应并关闭连接。

`RuniClient` 实现同步 loopback 客户端、连接/读取超时、Content-Length 校验、结构化状态码和 SSE 回放解析。首版不自动重试非幂等请求。

## 8. 开发顺序

实际顺序有意调整为 `并发基础 → 状态一致性 → 多 Agent → 服务层`，因为不能在没有状态边界时开放多 Agent 写入。

1. 新建独立测试目标和四层契约测试；
2. 实现 `BoundedExecutor` 与取消/关闭；
3. 实现 SQLite Schema、事务、CAS、幂等和恢复；
4. 实现 `ContextCache` 的 LRU/TTL/Singleflight；
5. 实现 `AgentRegistry`、`AgentLease` 和 fan-out/fan-in；
6. 实现 `WorkspaceCommitter` 的 SHA 冲突检测和批量提交；
7. 实现 `RuntimeService` 路由、Run 状态机和取消/恢复；
8. 实现 loopback `SocketHttpServer` 与 `RuniClient`；
9. 将服务处理器接入现有 `AgentLoop`；
10. 运行新增测试、原单元测试、CLI 测试、固定 benchmark 和上下文消融测试；
11. 编写新版整体架构文档。

## 9. 测试矩阵

### 9.1 并发执行

- 最大活跃任务数不超过 Worker 数；
- 队列满时拒绝第三个任务；
- Shutdown 后拒绝新任务；
- Stop Token 能让运行任务退出；
- 析构后无遗留 Worker；
- Future 传播返回值和异常。

### 9.2 多 Agent

- Registry 拒绝重复 Agent 和非法容量；
- 能力/角色匹配正确；
- Lease 达到容量后等待，析构后自动释放；
- fan-out 结果按输入顺序稳定返回；
- `collect_all` 保留局部失败；
- `fail_fast` 请求取消未完成任务；
- Workspace 多文件提交成功；
- SHA 不匹配时零文件被修改；
- 重复目标路径被拒绝。

### 9.3 状态与缓存

- Schema 初始化和重复打开；
- Session 创建、读取和版本 CAS；
- 错误版本返回冲突；
- Idempotency-Key 返回同一个 Run；
- 非法 Run 状态转换被拒绝；
- Event Sequence 单调且支持 after-sequence；
- running 任务重启恢复为 interrupted；
- Memory Scope、TTL 和来源 Hash 失效；
- Cache 命中、LRU 淘汰和 TTL 过期；
- 并发 Singleflight 只计算一次。

### 9.4 服务端/客户端

- 创建 Session 和 Run 返回正确状态码；
- 异步 Handler 完成后可查询结果；
- 重复 Idempotency-Key 不重复执行；
- 排队/运行任务可取消；
- interrupted 任务可恢复；
- SSE 根据 Last-Event-ID 回放；
- 请求体过大返回 413；
- 执行队列满返回 503；
- 真实 loopback Socket 完成一次 Client/Server 往返；
- Server Stop 后监听线程和连接 Worker 全部退出。

## 10. 完成定义

- 新增功能全部有自动化测试；
- Debug 构建零编译错误；
- 新增 Runtime 测试全部通过；
- 原有 CTest 全部通过；
- 没有覆盖当前工作区已有的上下文消融改动；
- 新版架构文档与实际代码一致，并明确未实现边界。
