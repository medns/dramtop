# ⚡ dramtop

**一个超轻量（~120KB）、零依赖的 Linux 命令行工具，实时查看整机和各进程的 DRAM 内存带宽（`GB/s`），以及 L1/L2 硬件预取、cgroup CPU 限流情况，并判断负载是卡在内存还是计算上。**

[English](README.md) · **简体中文**

[![Latest Release](https://img.shields.io/github/v/release/medns/dramtop?color=2563eb)](https://github.com/medns/dramtop/releases)
[![Build](https://github.com/medns/dramtop/actions/workflows/build.yml/badge.svg)](https://github.com/medns/dramtop/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-2563eb.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux_x86__64_·_ARM64-2563eb.svg)](#-快速开始)

![dramtop 实时终端监控面板](docs/demo.svg)

---

## 🚀 快速开始

从 GitHub Releases 下载预编译的静态二进制（x86_64 / aarch64）：

```sh
sudo curl -fsSL "https://github.com/medns/dramtop/releases/latest/download/dramtop-linux-$(uname -m)" -o /usr/local/bin/dramtop
sudo chmod +x /usr/local/bin/dramtop
sudo dramtop
```

## ✨ 核心特性

| 功能 | 说明 |
|---|---|
| 进程级带宽 | 显示整机和每个进程的 DRAM 流量（`GB/s`），按占用排序，并给出相对实测峰值的利用率 |
| 区分预取 | 在 AMD Zen 和 Intel 上分开显示按需读取和 L1/L2 硬件预取；Arm64 上只显示总量 |
| 支持容器 | 内核允许时使用 per-CPU 计数器（`Per-CPU System PMU`），否则改用线程级计数器，受限的容器里也能运行（`Thread-PMU Container Mode`） |
| 自动测峰值 | 启动时用不到 1 秒测出本机实际能达到的读带宽上限，也可以用 `-c` 直接指定 |
| 瓶颈判断 | 根据带宽、IPC、每千条指令的 DRAM miss 数和 CFS 限流，标出 `MEMORY-BOUND`、`COMPUTE-BOUND` 或 `LOCK-CONTENTION / CFS-THROTTLED` |
| 多种输出 | 默认是类似 `top` 的全屏界面，退出后不会在终端里留下输出；也支持 `--batch` 纯文本和 `--json`，方便记录日志或接入脚本 |

> dramtop 统计的是 CPU 核心从 DRAM 读入的缓存行（按需读取和硬件预取），不包括写回 DRAM 和设备 DMA 的流量。在 AMD Zen 4 上，结果与软件计时的顺序读测试相比误差约 1%。

## 💻 使用说明

```sh
dramtop                      # 全屏查看所有进程
dramtop -P python3           # 只看名字包含 python3 的进程
dramtop -p 12345             # 只看 PID 为 12345 的进程
dramtop -i 0.25 -c 120       # 每 0.25 秒刷新一次，峰值按 120 GB/s 计算
dramtop -b -n 20 > dram.log  # 输出 20 次纯文本采样
dramtop -J -n 5 | jq .       # 每次采样输出一行 JSON
```

| 参数 | 说明 | 按键 | 作用 |
|---|---|---|---|
| `-p, --pid <PID>` | 只看指定进程（包括它之后 fork 的子进程） | `q` | 退出 |
| `-P, --proc <NAME>` | 只看名字包含 NAME 的进程 | `t` / `Tab` | 切换视图（双栏 / 进程 / 时间线） |
| `-i, --interval <S>` | 刷新间隔，单位为秒（默认 `1`） | `Space` / `p` | 暂停 / 继续 |
| `-n, --count <N>` | 采样 N 次后退出 | `r` | 清空最大值、均值和时间线 |
| `-c, --ceiling <GB/s>` | 直接指定带宽峰值，不在启动时测量 | `+` / `-` | 加快 / 放慢刷新（0.1 到 5 秒） |
| `-b, --batch` | 每次采样输出一行文本，不进入全屏界面（输出不是终端，或 `TERM` 未设置、为 `dumb` 时自动使用） | | |
| `-J, --json` | 每次采样输出一行 JSON | | |
| `--no-color` | 关闭颜色（设置 `NO_COLOR` 环境变量也可以） | | |
| `-v, --version` | 显示版本号 | | |
| `-h, --help` | 显示帮助 | | |

## 🔐 运行要求

- 需要硬件性能计数器（PMU）。物理机可以直接用；虚拟机需要开启虚拟 PMU，大多数云主机和 WSL2 都不提供。
- 以 root 运行，或者授予 `CAP_PERFMON`（Linux 5.8+）。普通用户只能看到自己的进程，并且要求 `kernel.perf_event_paranoid` 不大于 2；整机模式（`Per-CPU System PMU`）要求不大于 0。
- Docker 默认会拦截 `perf_event_open`，启动容器时需要加上 `--cap-add PERFMON`（或 `--privileged`）；Kubernetes 中需要给容器加上 `PERFMON` capability。

## 🛠️ 从源码编译

需要 `gcc` 或 `clang`、`make`，以及 Linux 内核头文件：

```sh
git clone https://github.com/medns/dramtop.git
cd dramtop
make && sudo make install
```

编译静态二进制（Release 里的包就是在 Alpine 上用 musl 这样编译出来的）：

```sh
make static
```
