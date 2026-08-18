<div align="center">

# 🐾 Runi

**A small, controlled, and auditable coding agent built with C++20.**

Runi lets an LLM inspect code, run commands, edit files, and preserve recoverable execution records inside a workspace.

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.24%2B-064F8C?logo=cmake&logoColor=white)
![Platform](https://img.shields.io/badge/network%20runtime-Windows-0078D4?logo=windows&logoColor=white)
![Version](https://img.shields.io/badge/version-0.1.0-7B68EE)

[Features](#-features) · [Quick Start](#-quick-start) · [Configuration](#-configuration) · [Build & Test](#-build--test)

</div>

---

## ✨ Features

- Read, search, write, and patch files, or run shell commands inside a workspace.
- Connect to Ollama, OpenAI Responses-compatible, Anthropic Messages-compatible, and DeepSeek APIs.
- Control risky tools with `ask`, `auto`, or `never` approval policies.
- Preserve sessions, memory, checkpoints, traces, and run reports.
- Detect workspace changes before restoring a checkpoint.
- Run deterministic tests and scripted coding-agent evaluations.

## 🚀 Quick Start

### Requirements

- Windows 10 or 11
- A C++20 compiler: MSVC, MinGW-w64/GCC, or Clang
- CMake 3.24+
- Ninja

> Runi's built-in HTTP client uses WinHTTP, so network providers currently require Windows.

### Clone and configure

```powershell
git clone https://github.com/NorthSun1999/Runi.git
Set-Location Runi
Copy-Item .env.example .env
```

Open `.env`, select one provider with `RUNI_PROVIDER`, and fill only that provider's settings. Ollama does not require an API key for its default local server.

### Build

```powershell
cmake --preset default
cmake --build --preset default
```

### Run

```powershell
# Interactive mode
.\build\default\runi.exe --cwd .

# One-shot task
.\build\default\runi.exe --cwd . "Summarize this repository"
```

## 🔌 Configuration

| Provider | Required setting |
|---|---|
| DeepSeek | `RUNI_DEEPSEEK_API_KEY` |
| OpenAI-compatible | `RUNI_OPENAI_API_KEY` |
| Anthropic-compatible | `RUNI_ANTHROPIC_API_KEY` |
| Ollama | `RUNI_OLLAMA_HOST` |

Command-line options override `.env` and system environment variables. Run the following command to see all options:

```powershell
.\build\default\runi.exe --help
```

Never commit `.env` or real credentials. The repository tracks `.env.example` as a safe template.

## 🛠️ Build & Test

```powershell
# Debug build and tests
cmake --preset default
cmake --build --preset default
ctest --preset default --output-on-failure

# Release build
cmake --preset release
cmake --build --preset release
```

---

<div align="center">

**Small runtime. Clear boundaries. Reproducible steps.**

</div>
