# 上位机

## `nor_id.py` — SPI NOR JEDEC ID 探测

一个单文件、无第三方依赖（除 pyusb）的最小示例：读 `0x9F`，解析厂商与
容量，并对"没接芯片 / 接线错误"给出诊断。

```bash
pip install pyusb

python3 host/nor_id.py            # 默认 6 MHz SPI
python3 host/nor_id.py --div 8    # 3 MHz，杜邦线较长时更稳
python3 host/nor_id.py -v         # 打印收发的原始 USB 帧
```

输出示例：

```
桥       : 固件 v1.0, 协议 v1
SPI      : mode 0, SCK = 6.0 MHz (div=4)

JEDEC ID : EF 40 18 00
厂商     : Winbond (0xEF)
类型     : 0x40
容量     : 0x18 -> 16 MB (128 Mbit)
扩展字节 : 0x00（厂商自定义，部分型号为设备 ID）
```

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

完整的帧格式和命令表见 [`../docs/protocol.md`](../docs/protocol.md)。

## 后续扩展建议

如果之后要在本目录里继续做完整工具，建议结构：

```
host/
├── ch552spi/            Python 包（pyusb）
│   ├── device.py        USB 枚举 / bulk 收发
│   ├── protocol.py      组帧 / 解帧 / 命令常量
│   └── flash.py         NOR / NAND 高层操作（擦除、页编程、校验）
├── ch552prog.py         CLI：id / read / write / erase / verify
└── c/                   libusb 版本（可选）
```

大块数据请走 `CMD_STREAM_READ` / `CMD_STREAM_WRITE`（一条命令 + 裸数据帧），
不要用 `CMD_SPI_WRITE` 逐 62 字节发，否则会因为每帧一次的往返把吞吐拉低。
