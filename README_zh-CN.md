# ⚡ dramtop

dramtop 是一个超轻量（~130 KB）、无依赖、类似 `top` 的命令行工具，实时显示整机和各进程的 DRAM 内存带宽（`GB/s`），并判断负载受限于内存带宽还是计算。对于大模型推理、数据库等对内存带宽敏感的负载，可以用它定位占用带宽的进程，并查看带宽距离机器上限还有多少余量。

支持 Linux 和 Apple Silicon Mac。在 Mac 上，进程在 GPU 和神经网络引擎上产生的内存流量也计入该进程。

[English](README.md) · **简体中文**

[![Latest Release](https://img.shields.io/github/v/release/medns/dramtop?color=2563eb)](https://github.com/medns/dramtop/releases)
[![Build](https://github.com/medns/dramtop/actions/workflows/build.yml/badge.svg)](https://github.com/medns/dramtop/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-2563eb.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux_x86__64_·_ARM64_·_macOS-2563eb.svg)](#-快速开始)

![dramtop 实时终端监控面板](docs/demo.svg)

---

## 🚀 快速开始

从 [GitHub Releases](https://github.com/medns/dramtop/releases) 下载预编译的二进制文件。Linux（静态编译，x86_64 / aarch64）：

```sh
sudo curl -fsSL "https://github.com/medns/dramtop/releases/latest/download/dramtop-linux-$(uname -m)" -o /usr/local/bin/dramtop
sudo chmod +x /usr/local/bin/dramtop
sudo dramtop
```

macOS（Apple Silicon）：

```sh
sudo mkdir -p /usr/local/bin
sudo curl -fsSL "https://github.com/medns/dramtop/releases/latest/download/dramtop-macos-arm64" -o /usr/local/bin/dramtop
sudo chmod +x /usr/local/bin/dramtop
sudo dramtop
```

## ✨ 功能

- 按 DRAM 带宽对进程实时排序，便于定位占用内存带宽最多的进程
- 自动判断瓶颈：`MEMORY-BOUND`（带宽饱和或受内存延迟限制）、`COMPUTE-BOUND`（数据位于缓存中，受计算限制）或 `LOCK-CONTENTION / CFS-THROTTLED`（锁竞争或 CFS 限流）
- 内核线程和已退出进程产生的流量单独显示为 `[kernel/ephem]`，不计入其他进程
- 全屏界面退出后不在终端中留下输出；也支持纯文本（`--batch`）和 JSON（`--json`）输出，便于记录日志和脚本处理

**Linux**

- 统计 CPU 从 DRAM 读入的缓存行，包括硬件预取，不包括写回和设备 DMA；与软件计时的读带宽测试结果基本一致
- 利用率以本机实测可达到的读带宽为基准，该带宽在启动时测量，耗时不到 1 秒；利用率达到 100% 表示带宽已经饱和
- 在 AMD Zen 和 Intel 上分别显示按需读取和 L1/L2 硬件预取的带宽；Arm64 上只显示总量
- 支持容器环境：无法使用整机计数器时自动改用线程级计数器，并显示 cgroup 的 CPU 配额和限流情况

**Apple Silicon**

- 显示性能核、能效核、GPU、神经网络引擎、媒体引擎和显示引擎各自的读写带宽，数据由内存控制器直接计数，是精确值，无需 `sudo`
- 进程在 GPU 和神经网络引擎上产生的流量计入该进程，在本地运行大模型时可以看到模型占用的全部带宽；同时显示各进程的 GPU 占用率
- 整机利用率以芯片的标称带宽为基准；启动时另外测量 CPU 和 GPU 各自可达到的带宽，并分别显示两者的利用率。GPU 负载高而带宽未饱和时，诊断为 `COMPUTE-BOUND (GPU)`
- 进程级数据是估算值，计数在每次线程切换时结算。进程独占一个 CPU 簇时，估算值接近实际；多个繁忙进程共用 CPU 簇或 GPU 时可能存在偏差，对应的行会标记 `~` 或 `?`
- 部分较新的芯片上，macOS 只把这些计数器开放给苹果自己的服务。这时 dramtop 会自动切换到近似模式：整机和各部分的带宽改由内存控制器的带宽直方图估算，流量很大时读数偏低。如果 macOS 只提供整机总量，就只显示总量。进程级带宽这时只是粗略估算，而且只在 macOS 允许配置 CPU 计数器时才有，否则只显示各进程的 GPU 流量

## 💻 使用说明

```sh
dramtop                      # 全屏显示所有进程
dramtop -P python3           # 只显示名称包含 python3 的进程
dramtop -p 12345             # 只显示 PID 为 12345 的进程
dramtop -i 0.25 -c 120       # 每 0.25 秒刷新一次，峰值按 120 GB/s 计算
dramtop -b -n 20 > dram.log  # 输出 20 次纯文本采样
dramtop -J -n 5 | jq .       # 每次采样输出一行 JSON
```

| 参数 | 说明 |
|---|---|
| `-p, --pid <PID>` | 只监控指定进程（Linux 上包括它之后 fork 的子进程） |
| `-P, --proc <NAME>` | 只监控名称包含 NAME 的进程 |
| `-i, --interval <S>` | 刷新间隔，单位为秒，默认 `1` |
| `-n, --count <N>` | 采样 N 次后退出 |
| `-c, --ceiling <GB/s>` | 直接指定带宽峰值，跳过启动时的测量 |
| `-b, --batch` | 每次采样输出一行文本，不进入全屏界面。输出不是终端，或 `TERM` 未设置、为 `dumb` 时自动启用 |
| `-J, --json` | 每次采样输出一行 JSON |
| `--no-color` | 关闭颜色（也可以设置 `NO_COLOR` 环境变量） |
| `-v, --version` | 显示版本号 |
| `-h, --help` | 显示帮助 |

全屏界面快捷键：`q` 退出，`t` / `Tab` 切换视图（双栏 / 进程 / 时间线），`Space` / `p` 暂停或继续，`r` 清空最大值、均值和时间线，`+` / `-` 加快或减慢刷新（0.1 到 5 秒）。

## 🔐 运行要求

**Linux**

- 需要硬件性能计数器（PMU）。物理机可以直接使用；虚拟机需要开启虚拟 PMU，大多数云主机和 WSL2 不提供。
- 以 root 运行，或授予 `CAP_PERFMON`（Linux 5.8+）。普通用户只能看到自己的进程，且要求 `kernel.perf_event_paranoid` 不大于 2；整机模式（`Per-CPU System PMU`）要求不大于 0。
- Docker 默认拦截 `perf_event_open`，启动容器时需要加上 `--cap-add PERFMON`（或 `--privileged`）；在 Kubernetes 中需要为容器添加 `PERFMON` capability。

**macOS**

- 需要 Apple Silicon Mac。虚拟机中无法访问内存控制器计数器。
- 进程级带宽需要以 `sudo` 运行；不加 `sudo` 时，进程列表只显示各进程的 GPU 流量，按 GPU 时间分摊。`-p` 和 `-P` 只有在能获取进程级带宽时才能使用。近似模式下，进程级带宽最多只是粗略估算。
- 进程级采样依赖 CPU 计数器和内核追踪。Instruments 的 CPU Counters 等工具占用它们时，进程级采样会自动关闭。

## 🛠️ 从源码编译

Linux 上需要 `gcc` 或 `clang`、`make` 和内核头文件；macOS 上需要 Xcode 命令行工具。

```sh
git clone https://github.com/medns/dramtop.git
cd dramtop
make && sudo make install
```

编译 Linux 静态二进制文件（Release 中的 Linux 版本即在 Alpine 上用 musl 以这种方式编译）：

```sh
make static
```
