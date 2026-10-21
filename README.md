

# ⚡ dramtop

**An ultra-lightweight (~50 KB), zero-dependency CLI tool for Linux that monitors real-time DRAM memory bandwidth (**`GB/s`**), L1/L2 hardware prefetch, Cgroup CPU throttling, and memory-vs-compute bottlenecks.**

**English** · [简体中文](README_zh-CN.md)

![Latest Release](https://img.shields.io/github/v/release/medns/dramtop?color=2563eb)
![Build](https://github.com/medns/dramtop/actions/workflows/build.yml/badge.svg)
![License: MIT](https://img.shields.io/badge/license-MIT-2563eb.svg)
![Platform](https://img.shields.io/badge/platform-Linux_x86__64_·_ARM64-2563eb.svg)

  


![dramtop live terminal dashboard](docs/demo.svg)



---

## 🚀 Quick Start

**Install from GitHub Releases** (single static binary, zero dependencies):

```sh
sudo curl -sSL "https://github.com/medns/dramtop/releases/latest/download/dramtop-linux-$(uname -m)" -o /usr/local/bin/dramtop
sudo chmod +x /usr/local/bin/dramtop
dramtop
```

## ✨ Features

| Capability | What it does |
|---|---|
| **System & Process Bandwidth** | Measures real-time DRAM throughput (`GB/s`), channel saturation `%`, and ranks top bandwidth-consuming processes |
| **Demand vs. HW Prefetch** | Separates CPU demand reads from L1/L2 hardware prefetcher fills across AMD Zen, Intel Xeon/Core, and ARM64 |
| **Two-Tier Adaptive PMU** | Uses per-CPU counters (`Per-CPU System PMU`) on hosts and auto-fallbacks to per-thread counters inside restricted containers(`Thread-PMU Container Mode`) |
| **Auto Bandwidth Calibration** | Runs a ~0.25s multi-threaded SIMD + PMU probe at startup to detect achievable peak DRAM bandwidth |
| **Bottleneck Diagnosis** | Classifies `MEMORY-BOUND`, `COMPUTE-BOUND`, and `LOCK-CONTENTION / CFS-THROTTLED` states in real time |
| **TUI, Batch & JSON Modes** | In-place `top`-style dashboard with zero scrollback pollution, plus `--batch` and `--json` (`NDJSON`) streaming |

## 💻 Usage

```sh
dramtop                      # Live TUI dashboard (all processes)
dramtop -P python3           # Filter by process name
dramtop -p 12345             # Filter by PID
dramtop -i 0.25 -c 120       # 0.25s refresh with manual 120 GB/s ceiling
dramtop -b -n 20 > dram.log  # Batch text mode for logging
dramtop -J -n 5 | jq .       # JSON Lines (NDJSON) streaming output
```

| Flag | Description | Hotkeys (TUI) | Action |
|---|---|---|---|
| `-p, --pid <PID>` | Monitor a specific PID | `q` | Quit and restore screen |
| `-P, --proc <NAME>` | Filter processes by name | `t` / `Tab` | Cycle view (`Split` / `Procs` / `Timeline`) |
| `-i, --interval <S>` | Refresh interval (default `1.0s`) | `Space` / `p` | Pause / resume live updates |
| `-n, --count <N>` | Exit after `N` samples | `r` | Reset peak/avg stats & timeline |
| `-c, --ceiling <GB>` | Override peak DRAM cap (`GB/s`) | `+` / `-` | Step refresh speed (`0.10s` – `5.00s`) |
| `-b, --batch` | Scroll line-by-line (no TUI) | | |
| `-J, --json` | Stream NDJSON objects | | |
| `-v, --version` | Print version and exit | | |
| `-h, --help` | Show help message | | |

## 🛠️ Build from Source

Requires only `gcc` (or `clang`) and standard Linux headers:

```sh
git clone https://github.com/medns/dramtop.git
cd dramtop
make && sudo make install
```

Build a portable static binary across any Linux distribution:

```sh
make static
```
