# CH552 USB-SPI 桥（SPI NOR / SPI NAND 烧录器）

用 CH552（增强型 8051 + USB 全速设备）做的 USB 转 SPI 桥，主要用途是
SPI NOR / SPI NAND Flash 的量产烧录、读取和校验。

板端固件已经完整实现；上位机通过一个「厂商自定义 bulk 接口」访问，
Linux 下用 libusb/usbfs 即可，无需内核驱动。接口协议见
[`docs/protocol.md`](docs/protocol.md)。

```
┌──────────┐   USB 2.0 FS    ┌──────────┐    SPI (最高 12 MHz)   ┌────────────┐
│   PC     │◄───────────────►│  CH552   │◄──────────────────────►│ NOR / NAND │
│ libusb   │  bulk 0x03/0x82 │ 24 MHz   │  SCK/MOSI/MISO/CS      │  Flash     │
└──────────┘                 └──────────┘                        └────────────┘
```

## 特性

* **USB 全速 12 Mbit/s，厂商自定义接口**，两个 bulk 端点，端点缓冲双缓冲。
* **主频可配**：24 MHz（5 V，默认）/ 16 / 12 / 6 MHz；3.3 V 供电请用 12 MHz
  （见 [`docs/hardware.md`](docs/hardware.md#11-主频与供电电压的对应关系重要)）。
* **SPI 主机硬件实现**，时钟 = Fsys / 分频，最高 12 MHz（Fsys = 24 MHz）。
* **零拷贝流式传输**：一整页（256 B / 2 KB）只用一个命令 + 若干裸数据帧，
  没有逐字节的命令开销。
* **软件控制 CS**，支持跨多个命令保持片选，适合 NOR/NAND 的多阶段时序
  （写使能 → 页编程 → 轮询状态）。
* Linux 免驱（libusb / usbfs），Windows 侧可用 WinUSB（见下文）。
* 可通过命令跳回 WCH 自带 USB bootloader 升级固件，不需要额外编程器。

## 目录结构

```
├── Makefile                    构建固件（SDCC）
├── firmware/
│   ├── include/
│   │   ├── ch552.h             CH552 寄存器定义（依据数据手册整理）
│   │   ├── ch554_usb.h         USB 协议常量 / 描述符结构
│   │   ├── spi.h               SPI 主机驱动接口
│   │   ├── usb.h               USB 设备核心接口
│   │   ├── protocol.h          USB-SPI 协议定义
│   │   └── debug.h             时钟 / 延时 / UART 调试
│   └── src/
│       ├── main.c              入口、主循环
│       ├── usb.c               USB 设备核心（描述符、EP0、中断、双缓冲）
│       ├── protocol.c          命令分发、流式传输
│       ├── spi.c               SPI0 驱动（性能关键路径）
│       └── debug.c             时钟配置、延时
├── docs/
│   ├── protocol.md             上位机接口文档（协议规范）
│   ├── hardware.md             引脚、接线、供电说明
│   └── handover.md             交接说明：现状、坑、下一步
└── host/
    ├── nor_id.py               最简 JEDEC ID 探测工具
    ├── README.md               工具用法与可复用的 Bridge 类
    └── 99-ch552-spi.rules      udev 规则（免 root 访问）
```

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/protocol.md`](docs/protocol.md) | 上位机接口规范：帧格式、命令表、时序与示例 |
| [`docs/hardware.md`](docs/hardware.md) | 引脚、接线、供电与信号完整性 |
| [`docs/handover.md`](docs/handover.md) | **交接说明**：现状、设计取舍、踩过的坑、下一步 |
| [`host/README.md`](host/README.md) | 上位机工具用法与可复用的 `Bridge` 类 |

## 编译

需要 SDCC 4.x（推荐 4.2.0）。

```bash
# Debian/Ubuntu
sudo apt install sdcc

# 或下载官方二进制包，解压后把 bin 加入 PATH
# https://sourceforge.net/projects/sdcc/files/sdcc-linux-amd64/

make                      # 24 MHz，USB 5V 供电（默认）
make FREQ_SYS=12000000    # 3.3V 供电时使用
```

如果 SDCC 不在 PATH 里，可以显式指定：

```bash
make SDCC=/path/to/sdcc/bin/sdcc PACKIHX=/path/to/sdcc/bin/packihx
```

输出：

```
build/ch552_spi_bridge.hex   Intel HEX，用 WCHISPTool 烧录
build/ch552_spi_bridge.bin   原始二进制，用 ch552tool / librech551 烧录
```

当前固件约 4.7 KB（可用 14 KB）。RAM：xRAM 端点缓冲固定占
0x0000–0x01FF（EP0 64B + EP3 双 64B + EP2 双 64B + 192B 回包暂存），
编译器 xdata 从 0x0200 起 512 B，iRAM 留约 158 B 栈空间。

### 烧录

* Windows：WCHISPTool（WCH 官网），选择 `build/ch552_spi_bridge.hex`。
* Linux / macOS（开源工具 `ch55xtool`）：
  ```bash
  pip install ch55xtool
  make flash            # 等价于 ch55xtool -f build/...bin -r
  ```
  （或者使用 `librech551`、`wchisp`。）

首次烧录需要按住板上的 BOOT/下载键再上电，或者短接 USB 重新枚举到
WCH bootloader。

## 硬件连接

默认引脚（详见 [`docs/hardware.md`](docs/hardware.md)）。SPI 的三根数据/时钟
脚由芯片固定，**片选和 `/WP`、`/HOLD` 等辅助 GPIO 全部集中在
`firmware/include/board.h`**，改几个宏即可换引脚。默认布局对应常见的
CH552 编程器板：

| CH552 | 方向 | SPI Flash | 说明 |
|-------|------|-----------|------|
| P1.7  | 输出 | SCK       | SPI0 硬件时钟 |
| P1.6  | 输入 | SO / IO1  | SPI0 MISO |
| P1.5  | 输出 | SI / IO0  | SPI0 MOSI |
| P1.4  | 输出 | CS#       | 软件片选，可在 `board.h` 改 |
| P3.2  | 输出 | /WP       | 推挽，上电为高（可写），`board.h` 可改 |
| P3.4  | 输出 | /HOLD     | 推挽，上电为高（正常响应），`board.h` 可改 |
| 3V3   | 电源 | VCC       | CH552 内部 LDO 输出能力有限，建议外部 3.3 V |
| GND   | —    | GND       | |

> 注意：CH552 的 P1.6 同时是 UART1 的 RXD、P1.7 是 UART1 的 TXD。本固件
> 不使用 UART1，因此不冲突。UART0（P3.0/P3.1）保留给调试口。

## 上位机

协议文档：[`docs/protocol.md`](docs/protocol.md)。

设备标识：

| 项目 | 值 |
|------|-----|
| VID / PID | `0x1A86` / `0x5507` |
| 接口 | 0，class `0xFF`（厂商自定义），无驱动绑定 |
| Bulk OUT | `0x03`，64 字节 |
| Bulk IN  | `0x82`，64 字节 |

Linux 下装一次 udev 规则即可免 root（仓库里已带该文件）：

```bash
sudo cp host/99-ch552-spi.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
# 然后拔插一次设备
```

规则内容是给 `plugdev` 组读写权限（`MODE="0660", GROUP="plugdev"`）。

Windows 下需要两步：

1. `pip install pyusb libusb-package` —— 后者提供 `libusb-1.0.dll`，
   pyusb 自己不带（脚本会自动检测并使用）。
2. 用 [Zadig](https://zadig.akeo.ie/) 给接口 0 绑定 WinUSB：
   `Options → List All Devices` → 选 `ch552-spi (Interface 0)` →
   `WinUSB` → `Replace Driver`，完成后拔插一次。

接口是厂商自定义类，不会与任何系统驱动冲突。烧录用的 WCH bootloader 是
另一个 VID:PID（`4348:55E0`），不受这个绑定影响。

### 现成的探测工具

[`host/nor_id.py`](host/nor_id.py) 是一个单文件示例：读 JEDEC ID、解析
厂商与容量，并对"没接芯片 / 接线错误"给出诊断。

```bash
pip install pyusb
python3 host/nor_id.py            # 默认 6 MHz
python3 host/nor_id.py --div 8    # 3 MHz，线长时更稳
```

```
桥       : 固件 v1.0, 协议 v1
SPI      : mode 0, SCK = 6.0 MHz (div=4)

JEDEC ID : EF 40 18 00
厂商     : Winbond (0xEF)
容量     : 0x18 -> 16 MB (128 Mbit)
```

### 自己实现的起点

命令帧是 `[cmd][len][payload]`，回包是 `[status][data]`。最小交互：

```python
import usb.core, usb.util

dev = usb.core.find(idVendor=0x1A86, idProduct=0x5507)
usb.util.claim_interface(dev, 0)
EP_OUT, EP_IN = 0x03, 0x82

def send(cmd, payload=b""):
    dev.write(EP_OUT, bytes([cmd, len(payload)]) + payload, 1000)

def recv(n):
    return bytes(dev.read(EP_IN, n, 1000))

send(0x08, b"\x00")                       # CMD_CS: 拉低片选
send(0x11, b"\x9F\xFF\xFF\xFF\xFF")      # CMD_SPI_XFER: 0x9F + 4 字节时钟
r = recv(6)                                # status + 5 字节 MISO
send(0x08, b"\x01")                       # CMD_CS: 释放
print("id", r[2:5].hex())                  # r[1] 是发送 0x9F 时的无效字节
```

## 性能

* SPI 时钟最高 **12 MHz**（`SPI0_CK_SE = 2`，即 Fsys/2）。上电默认
  **6 MHz**（`div = 4`）以兼顾杜邦线/长走线的信号完整性，识别到 Flash 后
  可用 `CMD_SET_SPI_CFG` 提到 12 MHz。
* SPI 字节循环约 16 个 Fsys 周期/字节，按指令周期估算 SPI 侧约
  1.2 MB/s。
* USB 全速 bulk 是真正的瓶颈（理论上限约 1.06 MB/s，实际 0.5–0.9 MB/s）。
* 页编程/页读取用流式命令，单个 256 B 页 = 1 个命令帧 + 4 个数据帧。
* **加速命令**（`CMD_NOR_PROGRAM` / `CMD_NOR_ERASE` /
  `CMD_NAND_READ_PAGES` / `CMD_NAND_PROGRAM_PAGES`）把每页的
  WREN/编程/等就绪循环搬进设备，主机一次发完整块。实测 W25Q32：

  | 路径 | 写入 | 擦除 |
  |---|---|---|
  | 主机逐页（`CMD_STREAM_WRITE`） | 10 KiB/s | 72 KiB/s |
  | 固件内循环 6 MHz | 149 KiB/s | 126 KiB/s |
  | 固件内循环 12 MHz | **188 KiB/s** | 126 KiB/s |

* SPI NAND（DS35M1GA，1.8V，2048+64，无 ECC）实测（Windows/WinUSB，
  1 MiB 校验通过）：

  | 路径 | 写入 | 读取 |
  |---|---|---|
  | 主机逐页（`CMD_STREAM_*`） | 42 KiB/s | 55 KiB/s |
  | 固件内循环 6 MHz | 195 KiB/s | 263 KiB/s |
  | 固件内循环 12 MHz | **434 KiB/s** | **438 KiB/s** |

* NOR/NAND 的擦除、编程时间由芯片本身决定，固件只负责不引入额外等待。

## 实测记录

在常见 CH552 SPI 编程器板（`/WP`=P3.2、`/HOLD`=P3.4）上验证：

| 项目 | 结果 |
|---|---|
| 枚举 | `1a86:5507`，厂商自定义接口，Linux 免驱 |
| JEDEC ID @ 6 MHz | `EF 40 16` → Winbond W25Q32（4 MB / 32 Mbit） |
| JEDEC ID @ 12 MHz | 同上（SPI 最高时钟可用） |
| RDSR | `0x00`（WIP=0、WEL=0） |
| 读地址 0 | 全 `0xFF`（空片），重复读取一致 |
| 固件加速写入 | 1 MiB @ 12 MHz，188 KiB/s，校验通过 |

尚未在硬件上验证：页编程/擦除/校验、SPI NAND、`CMD_STREAM_*` 大数据流、
Windows 下的 WinUSB 绑定。上述待办与调试方法见
[`docs/handover.md`](docs/handover.md)。

## 版本

* 固件 1.0 / 协议 1.0
* 目标芯片：CH552（CH554 兼容，但本固件未使用 USB Host 功能）

## 参考

* <https://github.com/ieiao/ch554_sdcc> — CH554/CH552 SDCC 支持包（寄存器
  定义与 USB 设备例程的写法参考）
* <https://github.com/SoCXin/CH552> — CH552 资料汇总与官方 SDK 工程
* CH552/CH551 数据手册（WCH）
