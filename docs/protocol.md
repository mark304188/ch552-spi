# CH552 USB-SPI 桥 — 上位机接口文档

版本：协议 v1.0 / 固件 v1.0

本文是板端固件与上位机之间的完整接口说明。按本文实现即可在 Linux
（libusb）和 Windows（WinUSB/libusbK）上驱动该设备。

---

## 1. 设备枚举

| 项目 | 值 |
|---|---|
| `idVendor` | `0x1A86` |
| `idProduct` | `0x5507` |
| `bcdUSB` | `0x0200` |
| `bcdDevice` | `0x0100` |
| 配置数 / 接口数 | 1 / 1 |
| 接口 0 | `bInterfaceClass = 0xFF`（厂商自定义） |
| 端点 `0x82` | Bulk IN，`wMaxPacketSize = 64` |
| 端点 `0x03` | Bulk OUT，`wMaxPacketSize = 64` |

设备只有标准控制请求，没有 class/vendor 控制请求，也没有 HID/CDC
之类的复合接口，所以不会被系统驱动占用。

### 1.1 Linux

无需驱动。通过 libusb 打开即可：

```c
libusb_device_handle *h;
libusb_open(dev, &h);
libusb_claim_interface(h, 0);
```

建议 udev 规则（免 root）：

```
SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="5507", MODE="0666"
```

### 1.2 Windows

接口是厂商自定义类，Windows 不会自动绑定驱动。两种做法：

1. 用 **Zadig** 把接口 0 绑定到 **WinUSB**（或 libusbK）。之后用
   libusb 的 Windows 后端即可，代码与 Linux 完全一致。
2. 自行提供 INF（`USB\VID_1A86&PID_5507&MI_00`）指向 `winusb.sys`。

后续如果需要 Windows 完全免驱，可以在固件里加 MS OS 2.0 描述符
（BOS + WINUSB CompatibleID），协议本身不需要改动。

---

## 2. 传输模型

上位机通过两个 bulk 端点和一个设备状态机通信：

| 方向 | 端点 | 用途 |
|---|---|---|
| 主机 → 设备 | `0x03` (OUT) | 命令帧、写数据帧 |
| 设备 → 主机 | `0x82` (IN) | 状态回包、读数据帧 |

* 每个 USB 帧最多 **64 字节**。
* 两个端点内部都是**双缓冲**（各 128 字节），所以命令可以连续下发，
  不必等待前一条的回复。
* 所有多字节字段均为**小端**（低字节在前）。

### 2.1 命令帧格式

```
偏移  长度   字段
 0     1    cmd
 1     1    len          len + 2 <= 64
 2    len   payload
```

### 2.2 回包格式

```
偏移  长度   字段
 0     1    status       0x00 = 成功
 1     n    data         长度由具体命令决定
```

`status` 取值：

| 值 | 名称 | 含义 |
|---|---|---|
| `0x00` | `ST_OK` | 成功 |
| `0x01` | `ST_ERR` | 一般错误 |
| `0x02` | `ST_BUSY` | 外设忙 |
| `0x03` | `ST_PARAM` | 参数错误 |
| `0x04` | `ST_LEN` | 帧长度与协议不符 |
| `0x05` | `ST_TIMEOUT` | 主机停止收发数据，流式传输中止 |
| `0xFF` | `ST_UNKNOWN` | 未知命令 |

> **重要**：命令分两类——
> * **有回包**的命令：主机必须读回包，否则后续读操作会错位。
> * **无回包**的命令（写数据、CS 控制、配置）：主机不读回包。这类命令
>   以「主机的下一次 OUT 传输被接受」作为完成标志，能显著降低
>   页编程循环的往返次数。

### 2.3 固件状态查询（EP0 厂商请求，可选）

除了 bulk 通道，设备还实现了一个**走 EP0 的厂商请求**，用来在不依赖
bulk 端点的情况下确认固件版本和端点寄存器状态。上位机排查问题时很有用。

| 字段 | 值 |
|---|---|
| `bmRequestType` | `0xC0`（IN，vendor，device） |
| `bRequest` | `0x42` |
| `wValue` / `wIndex` | `0` / `0` |
| `wLength` | 16 |

返回 16 字节：

| 偏移 | 内容 |
|---|---|
| 0..2 | `'C' 'S' 'P'` |
| 3 | 协议版本 |
| 4 / 5 | 固件 major / minor |
| 6 | `UEP2_3_MOD` |
| 7 | `UEP2_CTRL` |
| 8 | `UEP3_CTRL` |
| 9 | `UEP3_DMA_L` |
| 10 | `ep3_frame_ready` |
| 11 | `rx_half` |
| 12 | `in_half` / `in_armed` / `in_pre_len` 位域 |
| 13 | `USB_CTRL` |
| 14 | `UDEV_CTRL` |
| 15 | 保留 |

固件不支持该请求时会 STALL。旧版本固件（没有这个请求）也会 STALL，
因此可以拿它区分"固件太旧"和"接线/硬件问题"。

---

## 3. 命令一览

| cmd | 名称 | payload | 回包 |
|---|---|---|---|
| `0x01` | `CMD_PING` | — | 5 字节 |
| `0x02` | `CMD_GET_INFO` | — | 10 字节 |
| `0x03` | `CMD_GET_CAPS` | — | 16 字节 |
| `0x04` | `CMD_SET_SPI_CFG` | `[div][mode3]` | 1 字节 |
| `0x05` | `CMD_GET_SPI_CFG` | — | 3 字节 |
| `0x08` | `CMD_CS` | `[level]` | 无 |
| `0x10` | `CMD_SPI_WRITE` | `[data…]` | 无 |
| `0x11` | `CMD_SPI_XFER` | `[data…]` | `len+1` 字节 |
| `0x12` | `CMD_SPI_READ` | `[n]` | `n+1` 字节 |
| `0x20` | `CMD_STREAM_WRITE` | 流描述符 | 1 字节 |
| `0x21` | `CMD_STREAM_READ` | 流描述符 | 数据 + 1 字节 |
| `0x22` | `CMD_STREAM_XFER` | 流描述符 | 数据 + 1 字节 |
| `0x30` | `CMD_GPIO_WRITE` | `[pin][value]`（0=WP, 1=HOLD） | 1 字节 |
| `0x31` | `CMD_GPIO_READ` | `[pin]` | 2 字节 |
| `0x40` | `CMD_BOOTLOADER` | — | 无（设备消失） |

---

## 4. 命令详解

### 4.1 `CMD_PING` (0x01)

链路自检。

* 请求：`01 00`
* 回包：`status, 'C', 'S', 'P', version`

```
00 43 53 50 01        status=0, "CSP", 协议版本 1
```

### 4.2 `CMD_GET_INFO` (0x02)

* 回包 10 字节：

```
[0] status
[1] idVendor 低字节    0x86
[2] idVendor 高字节    0x1A
[3] idProduct 低字节   0x07
[4] idProduct 高字节   0x55
[5] 固件版本 major
[6] 固件版本 minor
[7] SPI 模式：0 = mode0，1 = mode3
[8] SPI 分频：SCK = Fsys / 该值
[9] 保留
```

### 4.3 `CMD_GET_CAPS` (0x03)

* 回包 16 字节：

```
[0] status
[1] capability 低字节
[2] capability 高字节
[3] 最大帧长（64）
[4] 最大前缀字节数（8）
[5] 协议版本
[6..15] 保留（0）
```

capability 位：

| 位 | 含义 |
|---|---|
| `0x0001` | 支持 `CMD_STREAM_*` |
| `0x0002` | 支持软件片选 |
| `0x0004` | bulk 端点双缓冲 |
| `0x0008` | 支持 `CMD_SPI_XFER` / `CMD_SPI_READ` |
| `0x0010` | 支持运行时修改 SPI 分频/模式 |
| `0x0020` | 支持 GPIO 命令 |

### 4.4 `CMD_SET_SPI_CFG` (0x04)

设置 SPI 时钟与模式。

* 回包：1 字节 `status`（`ST_OK` / `ST_PARAM`）。

* payload：`[div][mode3]`
  * `div`：SPI0 分频系数，**`SCK = Fsys / div`**。最小有效值 2（即 Fsys/2）。
    固件默认 `Fsys = 24 MHz`，此时常用值：

    | div | SCK |
    |---|---|
    | 2 | 12 MHz |
    | 4 | 6 MHz（默认） |
    | 8 | 3 MHz |
    | 16 | 1.5 MHz |
    | 32 | 750 kHz |

    如果固件改为 12 MHz 主频（3.3 V 供电时需要），上表整体除以 2。
  * `mode3`：0 = SPI mode 0（SCK 空闲低），1 = SPI mode 3（SCK 空闲高）。
    硬件只支持 mode 0 和 mode 3（CH552 SPI 主机固定上升沿采样）。
    mode 1/2 不支持。
* 位序固定为 MSB first。
* 注意：该命令会重新初始化 SPI 引脚，若当前 CS 被拉低会被释放。
* 回包只表示参数合法，不表示时钟已稳定（硬件分频是立即生效的）。

推荐上电后先执行一次 `SET_SPI_CFG(4, 0)`，或在识别到 Flash 后再提速。

### 4.5 `CMD_GET_SPI_CFG` (0x05)

* 回包 3 字节：`status, div, mode3`

### 4.6 `CMD_CS` (0x08)

控制片选。**无回包**。

* payload：`[level]`
  * `0`：拉低 CS（选中）
  * `1`：释放 CS（高）
* CS 是软件 GPIO，可在多条命令之间保持低电平。
* `CMD_STREAM_*` 的行为：
  * 如果主机已经用 `CMD_CS 0` 拉低了 CS，流式命令**不会**改变 CS；
  * 否则流式命令自己拉低，并在结束时释放。

### 4.7 `CMD_SPI_WRITE` (0x10)

把 payload 原样从 MOSI 移出，忽略 MISO。**无回包**。

* payload：`[data…]`，最多 62 字节。
* 适合短命令，例如写使能（配 `CMD_CS` 使用）。

### 4.8 `CMD_SPI_XFER` (0x11)

全双工交换。

* payload：`[data…]`，最多 62 字节。
* 回包：`len+1` 字节，`[status][MISO 数据…]`，与请求字节一一对应。

### 4.9 `CMD_SPI_READ` (0x12)

只读。设备在 MOSI 上发送 `0xFF`（产生时钟），把 MISO 收回来。

* payload：`[n]`，`1 ≤ n ≤ 62`
* 回包：`n+1` 字节，`[status][数据…]`

### 4.10 流式命令（性能路径）

`CMD_STREAM_WRITE` / `CMD_STREAM_READ` / `CMD_STREAM_XFER` 用于大块数据。
一条命令即可完成「片选 → 发命令/地址 → 传一整页 → 释放片选」。

#### 请求

```
偏移  长度  字段
 0     1   cmd           0x20 / 0x21 / 0x22
 1     1   len           4 + pre_len
 2     3   count         数据字节数（小端 24 位，最大 16 MB）
 5     1   pre_len       前缀字节数（0..8）
 6   pre_len   pre       先移出的字节（命令码 + 地址等）
```

例：NOR 页编程（256 字节，地址 0x000123，命令 `02`）

```
20 08 00 01 00 04 02 00 01 23
```

含义：`CMD_STREAM_WRITE`，后面跟 0x100 = 256 字节数据，先发送
`02 00 01 23`（页编程命令 + 24 位地址）。

随后主机连续发送 4 个 64 字节的 OF 帧（共 256 字节）。设备在收到
最后一个字节后释放 CS，并回一个状态字节。

例：NOR 读（256 字节，地址 0x000123，命令 `03`）

```
21 08 00 01 00 04 03 00 01 23
```

随后主机从 IN 端点读 256 字节，再读 1 字节状态。

#### 数据阶段

| 命令 | 数据方向 | 主机动作 |
|---|---|---|
| `CMD_STREAM_WRITE` | 主机 → 设备 | 依次写 `ceil(count/64)` 个 OUT 帧 |
| `CMD_STREAM_READ` | 设备 → 主机 | 依次读 `ceil(count/64)` 个 IN 帧 |
| `CMD_STREAM_XFER` | 双向 | 写 `count` 字节的同时读回 `count` 字节 |

#### 回包

* 数据阶段结束后设备返回 **1 字节状态**。
* `CMD_STREAM_WRITE` 没有数据回包，只有这个状态字节。
* `CMD_STREAM_READ` / `CMD_STREAM_XFER`：先读 `count` 字节数据，再读
  1 字节状态。
* `count = 0` 时只有状态字节，可用于「只发前缀」的场景（见 6.1）。

#### 片选行为

* 若 CS 已经由 `CMD_CS 0` 拉低，流式命令不会改动它（便于多阶段时序）。
* 否则命令开始时拉低，结束时释放。

### 4.11 `CMD_GPIO_WRITE` (0x30)

控制辅助 GPIO。

* payload：`[pin][value]`
  * `pin`：引脚索引，映射由固件的 `board.h` 决定。默认布局：

    | 索引 | 引脚 | 接 Flash | 默认 |
    |---|---|---|---|
    | 0 | `P3.2` | `/WP` | 高（可写） |
    | 1 | `P3.4` | `/HOLD` | 高（正常） |
    | 2/3 | — | 未使用 | — |

  * `value`：`0` = 拉低（有效），`1` = 非有效电平
* 默认驱动方式为推挽，`value=1` 会**主动输出高电平**；如果 `board.h` 里
  把该槽位改成 `BOARD_GPIO_OD`，则 `value=1` 为高阻（依赖外部上拉）。
* 上电时固件已把所有槽位驱到高电平，所以不操作时 Flash 处于可写、可
  响应状态。
* 回包：1 字节 `status`（`ST_OK` / `ST_PARAM`，索引不存在时返回后者）。

### 4.12 `CMD_GPIO_READ` (0x31)

* payload：`[pin]`
* 回包：`[status][level]`，`level` 为该引脚当前电平（0/1）

### 4.13 `CMD_BOOTLOADER` (0x40)

设备重新枚举为 WCH USB bootloader（`0x4348:0x55E0`），用于升级固件。
无回包；执行后设备会从 USB 上消失。

---

## 5. SPI 时序与限制

* 硬件只支持 **mode 0** 和 **mode 3**（CPOL 可选，CPHA 固定为第一个边沿
  采样）。CH552 主机固定上升沿采样。
* 位序固定 **MSB first**。
* 时钟：`SCK = Fsys / div`，`div ≥ 2`。Fsys = 24 MHz 时最高 12 MHz。
* 每个字节之间有约 1~2 个 Fsys 周期的间隔（CPU 循环开销），不影响
  Flash 的时序要求（tCLH/tCLK 远小于此）。
* 片选由软件控制，`CMD_CS` 与流式命令都可以操作，注意成对使用。
* 分频关系 `SCK = Fsys / SPI0_CK_SE` 来自 WCH 例程的注释
  （`SPI0_CK_SE = 2` 在 16 MHz 例程中标注为 8 MHz），首次上板建议用示波器
  确认实际 SCK 频率，以它为准设置 `div`。

> NAND 的使能/就绪（R/B#）脚本固件没有占用。NAND 建议用
> `0x0F`/`0xC0` 读状态位的方式等待 OIP 清零（见 6.2），这样不需要
> 额外 GPIO。

---

## 6. 典型流程

> **约定**：下文用 `-> n` 标注该命令的**回包长度**，主机必须把 n 字节读走。
> 没有标注的命令无回包。读回包之前不要发下一条命令，否则回包流会错位。

### 6.1 读 JEDEC ID

```
CMD_SET_SPI_CFG [04 00]          -> 1     # 6 MHz，mode 0
CMD_CS [00]                               # 拉低 CS（无回包）
CMD_SPI_XFER [9F 00 00 00]       -> 5     # status + 4 字节 MISO
CMD_CS [01]
```

回包的 `r[1..3]` 是厂商 / 型号 / 容量。

### 6.2 SPI NAND 页读（2 KB 页）

```
# 1) PAGE READ (13h) + 24 位页地址，设备自动 CS
CMD_STREAM_WRITE count=0 pre=[13, A2, A1, A0]      -> 1

# 2) 轮询 GET FEATURE (0Fh C0h) 直到 OIP(bit0) == 0
loop:
  CMD_CS [00]
  CMD_SPI_XFER [0F C0 00]                          -> 4    # status + 3 字节
  CMD_CS [01]
  until (r[1] & 0x01) == 0

# 3) READ FROM CACHE (03h) + 2 字节列地址 + 1 dummy，读出 2048+64 字节
CMD_STREAM_READ count=2112 pre=[03, 00, 00, 00]    -> 2112 + 1
```

* `CMD_STREAM_READ` 的数据先到，最后一字节是状态。
* 前缀字节数由目标 NAND 决定：`03` 需要 2 字节列地址，部分型号还要
  1 个 dummy 字节（用 `0B` 时则一定是 1 个 dummy）。

### 6.3 SPI NOR 页编程（256 B）

最省往返的写法（设备自动管理 CS）：

```
CMD_STREAM_WRITE count=0   pre=[06]                 -> 1   # WREN
CMD_STREAM_WRITE count=256 pre=[02, A2, A1, A0]     -> 1   # 随后发 4 个数据帧
CMD_STREAM_WRITE count=0   pre=[05, 00]             -> 1   # 占位读状态
```

> `CMD_STREAM_WRITE` 的 `count` 是**数据阶段**字节数，前缀不算在内。
> 页编程命令本身（`02` + 地址）作为前缀发送，因此 256 字节数据正好是
> 4 个 USB 帧。

需要读取状态寄存器时：

```
CMD_CS [00]
CMD_SPI_XFER [05 00]                                -> 3   # status + SR + 1
CMD_CS [01]
# r[1] & 0x01 为 1 表示 WIP（内部忙）
```

也可以把 WREN 和页编程拆成独立片选周期（更接近数据手册时序）：

```
CMD_CS [00]; CMD_SPI_XFER [06 00] -> 2; CMD_CS [01]
CMD_CS [00]
CMD_STREAM_WRITE count=256 pre=[02, A2, A1, A0]     -> 1   # CS 保持低
CMD_CS [01]
```

### 6.4 扇区擦除

```
CMD_STREAM_WRITE count=0 pre=[06]                   -> 1   # WREN
CMD_STREAM_WRITE count=0 pre=[20, A2, A1, A0]       -> 1   # 4 KB 扇区擦除
# 之后轮询状态寄存器 bit0（见 6.3）
```

### 6.5 提速建议

1. 先用低速（`div = 8` 或 `16`）识别 Flash，再 `CMD_SET_SPI_CFG(2, 0)`
   提到 12 MHz。
2. 大块数据始终用 `CMD_STREAM_*`，不要用 `CMD_SPI_WRITE` 逐 62 字节发。
3. 页编程/擦除的 WIP 轮询尽量合并：一次 `CMD_SPI_XFER [05 00]` 只花一个
   往返，不要每条都单独发 `CMD_CS`。
4. 读操作用 `CMD_STREAM_READ`：设备内部用 `0xFF` 产生时钟，主机不需要
   准备 dummy 数据，也不占用 OUT 带宽。

## 7. 时序/性能参考

| 项目 | 值 |
|---|---|
| USB 帧 | 1 ms，64 字节 bulk 帧 |
| 命令往返（OUT+IN） | ≤ 2 帧 |
| SPI 字节 | ~16 个 Fsys 周期（≈0.67 µs @12 MHz SCK，Fsys=24 MHz） |
| 页编程 256 B | 1 命令帧 + 4 数据帧 + 1 状态帧 |
| 理论 SPI 吞吐 | ~1.2 MB/s |
| 实测瓶颈 | USB 全速 bulk（0.5–0.9 MB/s） |

---

## 8. 错误处理

* 每条有回包的命令都必须读回包，否则命令流会错位。
* 出现 `ST_LEN` / `ST_PARAM` / `ST_UNKNOWN` 时，说明主机侧组帧有误；
  设备已经丢弃该帧，可以直接发下一条命令。
* `ST_TIMEOUT` 只可能出现在流式命令中（主机在传输中途停止）。设备会
  释放 CS 并回到空闲状态，主机重新发起即可。
* 如果设备运行异常，可以发 `CMD_BOOTLOADER` 重新进入 bootloader，或者
  硬件复位。

---

## 9. 与参考实现的差异

本协议不是某个现成标准的实现，而是针对「烧录」场景做的精简设计：

* 相比 FTDI MPSSE：不需要主机侧解算 bit 级时序，SPI 完全由硬件产生，
  吞吐更高、代码更简单。
* 相比 flashrom 的 `ft2232_spi` / `ch341a_spi`：命令更贴近 Flash 操作
  （前缀 + 流式数据），一个页操作只需 1 个命令而不是 1 条命令 + N 次
  小传输。
* 相比 CDC-ACM 虚拟串口：没有串口参数协商和字符转义，纯 bulk 传输，
  Linux/Windows 行为一致。

---

## 10. 版本历史

| 协议版本 | 变更 |
|---|---|
| 1.0 | 初版：PING/INFO/CAPS、SPI 配置、CS 控制、缓冲与流式传输、GPIO、bootloader |
