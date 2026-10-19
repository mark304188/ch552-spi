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
│   └── hardware.md             引脚、接线、供电说明
└── toolchain/                  （可选）本地 SDCC，见下
```

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

当前固件约 3.8 KB（可用 14 KB），RAM 使用：xRAM 端点缓冲固定占
0x0000–0x01FF，编译器 xdata 512 B，iRAM 剩余约 190 B 栈空间。

### 烧录

* Windows：WCHISPTool（WCH 官网），选择 `build/ch552_spi_bridge.hex`。
* Linux / macOS：
  ```bash
  pip install ch552tool
  ch552tool flash -f build/ch552_spi_bridge.bin
  ```
  （或者使用 `librech551`、`wchisp`。）

首次烧录需要按住板上的 BOOT/下载键再上电，或者短接 USB 重新枚举到
WCH bootloader。

## 硬件连接

默认引脚（详见 [`docs/hardware.md`](docs/hardware.md)）。SPI 的三根数据/时钟
脚由芯片固定，**片选和辅助 GPIO 全部集中在 `firmware/include/board.h`**，
改一个宏即可换引脚：

| CH552 | 方向 | SPI Flash | 说明 |
|-------|------|-----------|------|
| P1.7  | 输出 | SCK       | SPI0 硬件时钟 |
| P1.6  | 输入 | SO / IO1  | SPI0 MISO |
| P1.5  | 输出 | SI / IO0  | SPI0 MOSI |
| P1.4  | 输出 | CS#       | 软件片选，可在 `board.h` 改 |
| P1.3  | 开漏 | RST#      | 可选，`CMD_GPIO_WRITE` 控制，可在 `board.h` 改 |
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

Linux 下建议加一条 udev 规则，免 root：

```
# /etc/udev/rules.d/99-ch552-spi.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="5507", MODE="0666"
```

Windows 下用 Zadig 给该接口绑定 WinUSB（或 libusbK），之后 libusb 的
Windows 后端即可直接使用；因为接口是厂商自定义类，不会与任何系统驱动
冲突。

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
* NOR/NAND 的擦除、编程时间由芯片本身决定，固件只负责不引入额外等待。

## 版本

* 固件 1.0 / 协议 1.0
* 目标芯片：CH552（CH554 兼容，但本固件未使用 USB Host 功能）

## 参考

* <https://github.com/ieiao/ch554_sdcc> — CH554/CH552 SDCC 支持包（寄存器
  定义与 USB 设备例程的写法参考）
* <https://github.com/SoCXin/CH552> — CH552 资料汇总与官方 SDK 工程
* CH552/CH551 数据手册（WCH）
