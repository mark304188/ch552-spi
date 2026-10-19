# 上位机

## `nor_id.py` — SPI NOR JEDEC ID 探测

一个单文件、无第三方依赖（除 pyusb）的最小示例：读 `0x9F`，解析厂商与
容量，并对"没接芯片 / 接线错误"给出诊断。

```bash
pip install pyusb

python3 host/nor_id.py            # 默认 6 MHz SPI
python3 host/nor_id.py --div 8    # 3 MHz，杜邦线较长时更稳
python3 host/nor_id.py --mode 3   # SPI mode 3（部分器件需要）
python3 host/nor_id.py --status   # 只读 EP0 固件状态，不碰 bulk
python3 host/nor_id.py -v         # 打印收发的原始 USB 帧
```

输出示例：

```
桥       : 固件 v1.0, 协议 v1
SPI      : mode 0, SCK = 6.0 MHz (div=4)

JEDEC ID : EF 40 16 00
厂商     : Winbond (0xEF)
类型     : 0x40
容量     : 0x16 -> 4 MB (32 Mbit)
扩展字节 : 0x00（厂商自定义，部分型号为设备 ID）
```

`--status` 输出示例（bulk 不工作时用它判断固件是否活着）：

```
桥       : 固件 v1.0, 协议 v1
寄存器   : UEP2_3_MOD=95 UEP2_CTRL=02 UEP3_CTRL=02 UEP3_DMA_L=40
主循环   : tick=2856 phase=0 cmd=0x00
中断计数 : isr=157 setup=50 out=0 in=0
端点状态 : frame_ready=0 had_frame=0 frame_len=0 rx_half=0 in=0x00
回包状态 : reply_len=0 reply_off=0 in_pre_len=0
控制     : USB_CTRL=29 UDEV_CTRL=A1
```

各字段含义与判定方法见 [`../docs/protocol.md`](../docs/protocol.md) §2.3
和 [`../docs/handover.md`](../docs/handover.md) §7。

接线异常时会直接提示原因，例如：

```
JEDEC ID : FF FF FF FF
厂商     : 未知 (FF FF FF)

诊断: 读回全 0xFF，MOSI/MISO 上没有器件回应。可能原因：
  - Flash 未供电，或 CS/SCK/MOSI/MISO 接线错误
  - CS 没有真正拉低（检查 P1.4 与 Flash 的 CS#）
  - SPI 时钟太快（试 --div 8 或 16）
```

### 代码结构

`nor_id.py` 里的 `Bridge` 类就是协议的最小实现，可以直接抄走：

| 方法 | 作用 |
|---|---|
| `ping()` | 校验设备与协议同步，返回协议版本 |
| `info()` | `CMD_GET_INFO`，读固件版本与当前 SPI 参数 |
| `set_spi(div, mode3)` | `CMD_SET_SPI_CFG`，`SCK = Fsys/div` |
| `cs(level)` | `CMD_CS`，软件片选 |
| `xfer(data)` | `CMD_SPI_XFER`，全双工交换 |
| `jedec_id()` | 用 `0x9F` 读 4 字节 ID |
| `vendor_status()` | `0xC0/0x42` 读 32 字节固件状态，**不经过 bulk** |

完整的帧格式和命令表见 [`../docs/protocol.md`](../docs/protocol.md)。

## `speedtest.py` — 擦除 / 写入 / 读回速度测试

一条命令跑完「擦除 → 逐页编程 → 读回校验」，每个阶段单独计时。默认走
流式命令（`CMD_STREAM_*`），也就是实际烧录的路径。

```bash
python3 host/speedtest.py                 # 64 KiB @ 0x000000，6 MHz
python3 host/speedtest.py --size 1M -y    # 1 MiB
python3 host/speedtest.py --div 2 -y      # 12 MHz
python3 host/speedtest.py --no-write      # 只测读取速度
python3 host/speedtest.py --erase-only -y # 只测擦除
python3 host/speedtest.py --no-erase -y   # 目标区域已擦除时直接写
python3 host/speedtest.py --pattern random --seed 7 -y
```

擦除和写入会覆盖目标区域，默认需要交互确认（非交互环境要加 `-y`）。
`--addr` 与 `--size` 都要求 4 KiB 对齐。

输出示例（W25Q32，12 MHz，VirtualBox 直通）：

```
状态轮询 : 11.09 ms/次（WIP 等待的时间分辨率）

[1] 擦除 16 个 4 KiB 扇区 ...
    完成，用时 0.910 s
    平均 56.9 ms/扇区, 70.3 KiB/s

[2] 编程 256 页 x 256 B (counter 模式) ...
    发送/SPI : 2.929 s  (21.9 KiB/s)  <- 桥 + USB 的写入开销
    等待 tPP : 3.027 s  (平均 11.82 ms/页)
    合计     : 7.952 s  (8.0 KiB/s, 平均 31.06 ms/页)

[3] 读回 64.0 KiB ...
    用时 0.176 s  (363.9 KiB/s)

[4] 校验 64.0 KiB ...
    一致，65536 字节全部匹配
```

**怎么读这些数字**

| 指标 | 含义 |
|---|---|
| `发送/SPI` | 主机把一页数据送进桥并移出到 SPI 的时间，**桥本身的写入开销** |
| `等待 tPP` | Flash 自己的页编程时间，外加 WIP 轮询的时间分辨率 |
| `读取` | 桥 → 主机的流式读吞吐，实际烧录校验时的上限 |
| `状态轮询` | 每次读状态寄存器（含 USB 往返）的耗时，决定了写速的下限 |

写速的瓶颈通常不是 SPI，而是**每页两次 USB 往返**（WREN、PP）加上 WIP
轮询。VirtualBox/USB 直通的往返约 2~4 ms/次，所以一页 30 ms 里有 25 ms
是 USB 延迟。裸机上会明显更快；要进一步提速，需要固件提供「按地址连续
编程」的复合命令（设备内部完成 WREN/PP/轮询），把往返摊薄到整块数据。

> 注意：擦除和编程前都必须先发 WREN（0x06）。脚本已经处理；自己写上位机
> 时最容易漏掉这一步，表现为“命令都成功但数据没变”。

## 后续扩展建议

如果之后要在本目录里继续做完整工具，建议结构：

```
host/
├── ch552spi/            Python 包（pyusb）
│   ├── device.py        USB 枚举 / bulk 收发
│   ├── protocol.py      组帧 / 解帧 / 命令常量
│   └── flash.py         NOR / NAND 高层操作（擦除、页编程、校验）
├── speedtest.py         速度/一致性测试（已有）
├── ch552prog.py         CLI：id / read / write / erase / verify
└── c/                   libusb 版本（可选）
```

大块数据请走 `CMD_STREAM_READ` / `CMD_STREAM_WRITE`（一条命令 + 裸数据帧），
不要用 `CMD_SPI_WRITE` 逐 62 字节发，否则会因为每帧一次的往返把吞吐拉低。
