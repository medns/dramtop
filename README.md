# ⚡ dramtop

**An ultra-lightweight (~120 KB), zero-dependency Linux tool that shows real-time DRAM bandwidth for the whole system and for each process, plus L1/L2 hardware prefetch, cgroup CPU throttling, and whether a workload is memory-bound or compute-bound.**

**English** · [简体中文](README_zh-CN.md)

[![Latest Release](https://img.shields.io/github/v/release/medns/dramtop?color=2563eb)](https://github.com/medns/dramtop/releases)
[![Build](https://github.com/medns/dramtop/actions/workflows/build.yml/badge.svg)](https://github.com/medns/dramtop/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-2563eb.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux_x86__64_·_ARM64-2563eb.svg)](#-quick-start)

![dramtop live terminal dashboard](docs/demo.svg)

---

## 🚀 Quick Start

Download the prebuilt static binary (x86_64 or aarch64) from GitHub Releases:

```sh
sudo curl -fsSL "https://github.com/medns/dramtop/releases/latest/download/dramtop-linux-$(uname -m)" -o /usr/local/bin/dramtop
sudo chmod +x /usr/local/bin/dramtop
sudo dramtop
```

## ✨ Features

| Feature | Details |
|---|---|
| Per-process bandwidth | DRAM traffic in `GB/s` for the whole system and for each process, sorted by usage, with utilization relative to the measured peak |
| Demand vs. prefetch | Splits demand reads from L1/L2 hardware prefetch on AMD Zen and Intel; on Arm64 only the total is shown |
| Container support | Uses per-CPU counters when the kernel allows them (`Per-CPU System PMU`) and falls back to per-thread counters in restricted containers (`Thread-PMU Container Mode`) |
| Peak calibration | Measures how much read bandwidth the machine can actually reach, in under a second at startup; use `-c` to set it yourself |
| Bottleneck hints | Labels the workload `MEMORY-BOUND`, `COMPUTE-BOUND` or `LOCK-CONTENTION / CFS-THROTTLED` from bandwidth, IPC, DRAM misses per 1000 instructions and CFS throttling |
| Output modes | A full-screen `top`-style view that leaves your scrollback clean, plus `--batch` text and `--json` lines for logs and scripts |

> dramtop counts the cache lines that CPU cores read from DRAM (demand reads and hardware prefetch). Write-backs and device DMA are not counted. On AMD Zen 4 the numbers agree with a software-timed read benchmark to within about 1%.

## 💻 Usage

```sh
dramtop                      # full-screen view of all processes
dramtop -P python3           # only processes whose name contains "python3"
dramtop -p 12345             # only PID 12345
dramtop -i 0.25 -c 120       # refresh every 0.25s and assume a 120 GB/s peak
dramtop -b -n 20 > dram.log  # 20 samples as plain text
dramtop -J -n 5 | jq .       # one JSON object per sample
```

| Flag | Description | Key | Action |
|---|---|---|---|
| `-p, --pid <PID>` | Watch one process, including children it forks | `q` | Quit |
| `-P, --proc <NAME>` | Watch processes whose name contains NAME | `t` / `Tab` | Switch view (split / processes / timeline) |
| `-i, --interval <S>` | Refresh interval in seconds (default `1`) | `Space` / `p` | Pause or resume |
| `-n, --count <N>` | Exit after N samples | `r` | Clear max/avg and the timeline |
| `-c, --ceiling <GB/s>` | Set the peak bandwidth instead of measuring it | `+` / `-` | Faster / slower refresh (0.1s to 5s) |
| `-b, --batch` | One line of text per sample instead of the full-screen view | | |
| `-J, --json` | One JSON object per sample | | |
| `-v, --version` | Print the version | | |
| `-h, --help` | Show help | | |

## 🔐 Requirements

- Hardware performance counters. Bare metal works; a VM needs a virtual PMU, which most cloud VMs and WSL2 don't provide.
- Root, or `CAP_PERFMON` on Linux 5.8+. Normal users only see their own processes and need `kernel.perf_event_paranoid` set to 2 or lower; the system-wide `Per-CPU System PMU` mode needs 0 or lower.
- Docker blocks `perf_event_open` by default, so start the container with `--cap-add PERFMON` (or `--privileged`). On Kubernetes, add the `PERFMON` capability.

## 🛠️ Build from Source

You need `gcc` or `clang` and the Linux kernel headers:

```sh
git clone https://github.com/medns/dramtop.git
cd dramtop
make && sudo make install
```

To build a static binary (this is how the release builds are made, with musl on Alpine):

```sh
make static
```
