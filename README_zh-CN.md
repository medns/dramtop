

# ⚡ dramtop

**一款超轻量（~50 KB）、零依赖的 Linux 命令行工具，实时监控 DRAM 内存带宽（**`GB/s`**）、L1/L2 硬件预取、Cgroup 容器 CPU 限流及内存/算力瓶颈状态。**

[English](README.md) · **简体中文**

![Latest Release](https://img.shields.io/github/v/release/medns/dramtop?color=2563eb)
![Build](https://github.com/medns/dramtop/actions/workflows/build.yml/badge.svg)
![License: MIT](https://img.shields.io/badge/license-MIT-2563eb.svg)
![Platform](https://img.shields.io/badge/platform-Linux_x86__64_·_ARM64-2563eb.svg)

  


![dramtop 实时终端监控面板](docs/demo.svg)



---

## 🚀 快速开始

**从 GitHub Releases 下载安装**（单文件静态二进制，零外部依赖）：

```sh
sudo curl -sSL "https://github.com/medns/dramtop/releases/latest/download/dramtop-linux-$(uname -m)" -o /usr/local/bin/dramtop
sudo chmod +x /usr/local/bin/dramtop
dramtop
```

## ✨ 核心特性

| 能力维度 | 功能说明 |
|---|---|
| **全局与进程级内存带宽** | 实时采集系统总 DRAM 吞吐量（`GB/s`）、通道饱和度 `%`，并按内存带宽从高到低排列进程排行榜 |
| **按需读取与硬件预取拆分** | 精确区分 CPU 按需读取（Demand Read）与 L1/L2 硬件预取（HW Prefetch），支持 AMD Zen、Intel 与 ARM64 |
| **双层自适应 PMU 引擎** | 宿主机自动启用全核系统级计数器（`Per-CPU System PMU`），受限云容器内自动无缝降级为线程级计数器（`Thread-PMU Container Mode`） |
| **零配置带宽上限校准** | 启动时自动执行 ~0.25s 多线程 SIMD + PMU 内存探针，实测当前机器可达的 DRAM 峰值带宽上限 |
| **实时瓶颈状态诊断** | 自动判定 `MEMORY-BOUND`（访存带宽受限）、`COMPUTE-BOUND`（算力受限）及 `LOCK-CONTENTION / CFS-THROTTLED` |
| **原地 TUI 与 JSON 流式输出** | 默认采用类 `top` 原地刷新界面（退出不留刷屏历史），支持 `--batch` 日志模式与 `--json`（`NDJSON`）流式输出 |

## 💻 使用说明

```sh
dramtop                      # 启动实时 TUI 仪表盘（默认监控全部进程）
dramtop -P python3           # 按进程名过滤监控
dramtop -p 12345             # 按进程 PID 过滤监控
dramtop -i 0.25 -c 120       # 0.25s 高频刷新，并手动指定带宽上限为 120 GB/s
dramtop -b -n 20 > dram.log  # 批处理逐行文本输出（用于日志记录）
dramtop -J -n 5 | jq .       # JSON Lines (NDJSON) 结构化流式输出
```

| 命令行参数 | 说明 | TUI 快捷键 | 功能 |
|---|---|---|---|
| `-p, --pid <PID>` | 仅监控指定 PID 的进程 | `q` | 退出并恢复终端屏幕 |
| `-P, --proc <NAME>` | 仅监控名称匹配的进程 | `t` / `Tab` | 切换视图（`双视图` / `进程榜` / `时间线`） |
| `-i, --interval <S>` | 刷新间隔秒数（默认 `1.0s`） | `Space` / `p` | 暂停 / 继续画面刷新 |
| `-n, --count <N>` | 采样 `N` 次后自动退出 | `r` | 重置峰值/均值统计并清空时间线 |
| `-c, --ceiling <GB>` | 手动指定带宽上限（`GB/s`） | `+` / `-` | 调节刷新速度（`0.10s` – `5.00s`） |
| `-b, --batch` | 逐行滚动输出（关闭 TUI） | | |
| `-J, --json` | 输出按行分隔的 JSON | | |
| `-v, --version` | 显示版本号并退出 | | |
| `-h, --help` | 显示帮助信息 | | |

## 🛠️ 从源码编译

仅需 `gcc`（或 `clang`）及标准 Linux 头文件：

```sh
git clone https://github.com/medns/dramtop.git
cd dramtop
make && sudo make install
```

编译跨发行版静态二进制文件：

```sh
make static
```
