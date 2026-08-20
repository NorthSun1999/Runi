<div align="center">

# 🐾 Runi

**A lightweight coding-agent runtime built with C++20.**

Runi operates on a local workspace through structured file, search, patch, and process tools. It can run as an interactive CLI or as a loopback HTTP service.

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.24%2B-064F8C?logo=cmake&logoColor=white)
![Platform](https://img.shields.io/badge/network%20runtime-Windows-0078D4?logo=windows&logoColor=white)
![Version](https://img.shields.io/badge/version-0.2.0-7B68EE)

[Build](#build) · [Run](#run) · [Configuration](#configuration) · [Targets](#targets)

</div>

---

## Overview

- Workspace-scoped tools with explicit approval policies.
- Ollama, OpenAI Responses-compatible, Anthropic Messages-compatible, and DeepSeek providers.
- Session, memory, checkpoint, trace, and run-report persistence.
- Bounded parallel read-only sub-agent fan-out/fan-in, SQLite service state, and a local C++ client/server API.

Runi is intentionally a single-machine runtime. The service binds to loopback, uses HTTP/1.1 and SQLite, and is not intended to be exposed directly to an untrusted network.

## Build

### Verified Windows toolchain

- Windows 10 or 11
- MSYS2 UCRT64 GCC with C++20 support
- CMake 3.24+
- Ninja
- SQLite3 development files for the same UCRT64 toolchain

Run the commands from an MSYS2 UCRT64 shell, or make sure the MSYS2 `ucrt64\bin` directory is on `PATH` before configuring. The preset deliberately does not hard-code a local compiler path.

Runi's provider client uses WinHTTP and the service uses Winsock, so the current network runtime is Windows-only.

### Debug build and tests

```powershell
git clone https://github.com/NorthSun1999/Runi.git
Set-Location Runi
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default --output-on-failure
```

The debug preset writes to `build/default` and enables all tests. The test suite uses deterministic model outputs and does not require provider credentials.

### Release build

```powershell
cmake --preset release
cmake --build --preset release --parallel
```

The release preset writes to `build/release`.

### VS Code

With the CMake Tools extension:

1. Start VS Code from an environment where MSYS2 `ucrt64\bin` is on `PATH`.
2. Run `CMake: Select Configure Preset` and choose `default` or `release`.
3. Run `CMake: Configure`.
4. Run `CMake: Build`, or press `F7`.

If the repository or compiler location changes, remove the affected preset build directory before configuring again; CMake caches absolute compiler and source paths.

## Run

### CLI

```powershell
# Interactive session
.\build\default\runi.exe --cwd .

# One-shot task
.\build\default\runi.exe --cwd . "Summarize this repository"

# All CLI options
.\build\default\runi.exe --help
```

### Local service

```powershell
.\build\default\runi_server.exe --cwd . --port 8765 --workers 4
.\build\default\runi_server.exe --help
```

The server listens on `127.0.0.1`. Its default tool approval mode is `never`; enable unattended workspace mutation only in a trusted local environment.

## Configuration

Provider configuration is needed only when running the CLI or service against a real model:

```powershell
Copy-Item .env.example .env
```

Select one provider with `RUNI_PROVIDER`, then fill only that provider's settings. Ollama does not require an API key for its default local server.

| Provider | Required setting |
|---|---|
| DeepSeek | `RUNI_DEEPSEEK_API_KEY` |
| OpenAI-compatible | `RUNI_OPENAI_API_KEY` |
| Anthropic-compatible | `RUNI_ANTHROPIC_API_KEY` |
| Ollama | `RUNI_OLLAMA_HOST` |

Command-line options override `.env` and system environment variables. Never commit `.env` or real credentials; the repository tracks `.env.example` as a safe template.

## Targets

| Target | Purpose |
|---|---|
| `runi_core` | Static runtime library |
| `runi` | Interactive and one-shot CLI |
| `runi_server` | Loopback HTTP service |
| `runi_eval` | Deterministic benchmark and context-ablation runner |
| `runi_unit_tests` | Core unit and contract tests |
| `runi_runtime_tests` | Concurrency, state, multi-agent, and service integration tests |

---

<div align="center">

**Small runtime. Clear boundaries. Reproducible steps.**

</div>
