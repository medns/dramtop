# ⚡ dramtop

dramtop is an ultra-lightweight (~130 KB), dependency-free, `top`-like command-line tool that shows real-time DRAM bandwidth (`GB/s`) for the whole system and for each process, and identifies whether a workload is bound by memory bandwidth or by compute. For bandwidth-sensitive workloads such as LLM inference and databases, it shows which processes consume the bandwidth and how much headroom the machine has left.

It supports Linux and Apple Silicon Macs. On macOS, the memory traffic that a process generates on the GPU and the Neural Engine is attributed to that process.

**English** · [简体中文](README_zh-CN.md)

[![Latest Release](https://img.shields.io/github/v/release/medns/dramtop?color=2563eb)](https://github.com/medns/dramtop/releases)
[![Build](https://github.com/medns/dramtop/actions/workflows/build.yml/badge.svg)](https://github.com/medns/dramtop/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-2563eb.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux_x86__64_·_ARM64_·_macOS-2563eb.svg)](#-quick-start)

![dramtop live terminal dashboard](docs/demo.svg)

---

## 🚀 Quick Start

Download a prebuilt binary from [GitHub Releases](https://github.com/medns/dramtop/releases). Linux (static, x86_64 or aarch64):

```sh
sudo curl -fsSL "https://github.com/medns/dramtop/releases/latest/download/dramtop-linux-$(uname -m)" -o /usr/local/bin/dramtop
sudo chmod +x /usr/local/bin/dramtop
sudo dramtop
```

macOS (Apple Silicon):

```sh
sudo mkdir -p /usr/local/bin
sudo curl -fsSL "https://github.com/medns/dramtop/releases/latest/download/dramtop-macos-arm64" -o /usr/local/bin/dramtop
sudo chmod +x /usr/local/bin/dramtop
sudo dramtop
```

## ✨ Features

- Processes sorted by real-time DRAM bandwidth, so the heaviest users of memory bandwidth are easy to identify
- Bottleneck diagnosis: `MEMORY-BOUND` (bandwidth saturated or limited by memory latency), `COMPUTE-BOUND` (data in cache, limited by compute) or `LOCK-CONTENTION / CFS-THROTTLED` (lock contention or CFS throttling)
- Traffic from kernel threads and exited processes is reported separately as `[kernel/ephem]` and is not attributed to other processes
- A full-screen interface that leaves no output in the terminal after exit, plus plain-text (`--batch`) and JSON (`--json`) output for logging and scripting

**Linux**

- Counts the cache lines that CPU cores read from DRAM, including hardware prefetch but not write-backs or device DMA; results closely match a software-timed read benchmark
- Utilization is relative to the read bandwidth the machine can actually reach, measured at startup in under a second; 100% means the memory bandwidth is saturated
- Separate demand-read and L1/L2 hardware-prefetch bandwidth on AMD Zen and Intel; Arm64 reports only the total
- Container support: falls back to per-thread counters when system-wide counters are unavailable, and reports the cgroup's CPU quota and throttling

**Apple Silicon**

- Read and write bandwidth of the performance cores, efficiency cores, GPU, Neural Engine, media engine and display engine, counted directly by the memory controller. These values are exact and do not require `sudo`
- Traffic that a process generates on the GPU and the Neural Engine is included in its bandwidth, so a locally running LLM shows its full bandwidth usage; per-process GPU utilization is also shown
- Chip-wide utilization is relative to the chip's rated bandwidth. The bandwidth that the CPU and the GPU can each reach is also measured at startup and used to show the utilization of each. A busy GPU whose bandwidth is not saturated is diagnosed as `COMPUTE-BOUND (GPU)`
- Per-process values are estimates, with counts settled at every context switch. The estimate is close to the actual traffic when a process has a CPU cluster to itself; when several busy processes share a cluster or the GPU, the split may be less accurate and the affected rows are marked `~` or `?`
- On some newer chips, macOS opens these counters only to Apple's own services. dramtop then switches to an approximate mode by itself: the whole chip and its parts are read from the memory controller's bandwidth histograms, which read low under very heavy traffic. Where macOS publishes only the chip total, only the total is shown. Per-process bandwidth is then only a rough estimate, and only where macOS lets dramtop configure the CPU counters; otherwise only each process's GPU traffic is shown

## 💻 Usage

```sh
dramtop                      # full-screen view of all processes
dramtop -P python3           # only processes whose name contains "python3"
dramtop -p 12345             # only PID 12345
dramtop -i 0.25 -c 120       # refresh every 0.25s and assume a 120 GB/s peak
dramtop -b -n 20 > dram.log  # 20 samples as plain text
dramtop -J -n 5 | jq .       # one JSON object per sample
```

| Flag | Description |
|---|---|
| `-p, --pid <PID>` | Monitor a single process (on Linux, including children it forks later) |
| `-P, --proc <NAME>` | Monitor processes whose name contains NAME |
| `-i, --interval <S>` | Refresh interval in seconds (default `1`) |
| `-n, --count <N>` | Exit after N samples |
| `-c, --ceiling <GB/s>` | Set the peak bandwidth instead of measuring it at startup |
| `-b, --batch` | One line of text per sample instead of the full-screen interface. Enabled automatically when output is not a terminal, or when `TERM` is unset or `dumb` |
| `-J, --json` | One JSON object per sample |
| `--no-color` | Disable colors (setting `NO_COLOR` has the same effect) |
| `-v, --version` | Print the version |
| `-h, --help` | Show help |

Keyboard shortcuts in the full-screen interface: `q` quit, `t` / `Tab` switch view (split / processes / timeline), `Space` / `p` pause or resume, `r` reset max/avg and the timeline, `+` / `-` faster or slower refresh (0.1s to 5s).

## 🔐 Requirements

**Linux**

- Hardware performance counters. Bare metal works; a VM needs a virtual PMU, which most cloud VMs and WSL2 do not provide.
- Root, or `CAP_PERFMON` on Linux 5.8+. Normal users only see their own processes and need `kernel.perf_event_paranoid` set to 2 or lower; the system-wide `Per-CPU System PMU` mode needs 0 or lower.
- Docker blocks `perf_event_open` by default, so start the container with `--cap-add PERFMON` (or `--privileged`). On Kubernetes, add the `PERFMON` capability.

**macOS**

- An Apple Silicon Mac. Virtual machines do not expose the memory controller counters.
- Per-process bandwidth requires `sudo`; without it, the process list shows only each process's GPU traffic, split by GPU time. `-p` and `-P` can only be used when per-process bandwidth is available. In approximate mode, per-process bandwidth is a rough estimate at best.
- Per-process sampling uses the CPU counters and kernel tracing, so it is disabled automatically while Instruments' CPU Counters or another tool is using them.

## 🛠️ Build from Source

On Linux you need `gcc` or `clang`, `make` and the kernel headers; on macOS, the Xcode Command Line Tools.

```sh
git clone https://github.com/medns/dramtop.git
cd dramtop
make && sudo make install
```

To build a static Linux binary (this is how the Linux release builds are made, with musl on Alpine):

```sh
make static
```
