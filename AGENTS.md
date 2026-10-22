# AGENTS.md

给在这个仓库里工作的 AI/人类协作者的速查与硬性约束。先读这一页，再看
`README.md`、`docs/protocol.md`。

## 项目是什么

CH552（8051 内核）做的 USB-SPI 桥：USB 全速 bulk 进，硬件 SPI 出，用来
烧录 SPI NOR / SPI NAND。性能优先，主机侧 Linux + Windows 通用。

- 固件：`firmware/`，SDCC 4.x，当前版本 **1.2**
- 上位机：`host/`，纯 pyusb + 标准库
- 许可：AGPL-3.0（`LICENSE`）

## 硬性约束（违反会直接坏）

1. **不要给 SDCC 加 `-Wall`**。它会被传给汇编器 `sdas` 导致构建失败。
2. **USB ISR 向量只在定义 `main()` 的模块生成**。`usb_isr` 定义在
   `usb.c`，必须在 `main.c` 里再声明一次：
   `void usb_isr(void) __interrupt(INT_NO_USB) __using(1);`
   不能放公共头文件（会重复定义向量表）。
3. **SPI 主机模式等 `S0_FREE`，不要等 `S0_R_FIFO`**。后者只在从机模式维护，
   等它会死循环、锁死整个主循环。
4. **命令处理里禁止无界等待主机**。所有自旋都要有上限并返回错误
   （SPI 首字节 2000 次、IN 回收 8000 次、流式帧 65536 次）。
5. **端点缓冲布局固定**（绝对地址，不能重叠）：
   ```
   0x0000 ep0_buf[64]   0x0040 ep3_buf[128]
   0x00C0 ep2_buf[128]  0x0140 usb_reply_buf[192]
   0x0200 ...           编译器 xdata（512 B）
   ```
6. **iRAM 只剩栈（约 113 B）**。新的循环状态放 `__xdata`；SDCC 默认把
   非重入函数的局部变量静态分配在 iRAM，容易撑爆 DSEG。
7. **主频与供电绑定**：24 MHz 要求 VCC > 4.4 V；3.3 V 必须
   `make VCC=3V3`（12 MHz）。**32 MHz 实测 USB 不枚举，不要用。**
8. **协议字段一律小端**（addr/len/count/page）；只有 SPI 前缀字节按器件
   要求排列（NOR 地址 MSB first）。混用会导致"地址 0 正常、非零就错位"。
9. **NOR 等待忙位要区分「没开始」和「已完成」**。命令被忽略（WP# 低、
   块保护）时 WIP 不会置起，不能当成成功。
10. **NAND（DS35M1GA）三个专有细节**：
    - 上电后 `A0h` 块保护 = `0x38`（全片锁定），擦写会 E_FAIL；
      主机必须先 `SET FEATURE A0h=0x00` 解锁
    - `9Fh` READ ID 后跟 **1 个 dummy** 才是 ID
    - `03h` READ FROM CACHE 是「命令 + 2B 列地址 + **1 dummy** + 数据」；
      `02h` PROGRAM LOAD 没有 dummy
11. **写路径先释放 OUT 缓冲、再移 SPI**，让 SPI 与 USB 重叠（写入 +45%）。
    `stream_write` / `stream_xfer` / `nor_program` / `nand_program_pages`
    都已按此实现，改动时别退回串行顺序。
12. **不要提交 `host/ufprog/`**。它是 GPL-2.0-only 的本地克隆，与
    AGPL-3.0 不兼容，已在 `.gitignore` 中。

## 构建 / 烧录

```bash
make                 # 5V / 24 MHz（默认），SPI 最高 12 MHz
make VCC=3V3         # 3.3V / 12 MHz，SPI 最高 6 MHz
make flash           # ch55xtool，需要板子先按住 BOOT 上电
```

- 编译参数变了会自动重建（`build/.config` 时间戳），不需要 `make clean`
- 烧录：Windows 用 WCHISPTool 烧 `build/ch552_spi_bridge.hex`；
  Linux 用 `make flash`（`pip install ch55xtool`）
- 软件跳 bootloader（`CMD_BOOTLOADER`）目前不可用，必须按 BOOT

## 测试

```bash
python3 host/nor_id.py --status                  # EP0 状态，bulk 挂了也能用
python3 host/nor_id.py                           # 读 JEDEC ID
python3 host/speedtest.py --size 64K --div 2 --engine fw -y
python3 host/nand_speedtest.py --id-only
python3 host/nand_speedtest.py --blocks 4 --div 2 -y
python3 host/nand_speedtest.py --blocks 4 --div 2 --ecc ondie -y
python3 host/nand_speedtest.py --blocks 4 --div 2 --ecc host -y
```

注意：host 里 `FSYS_MHZ` 写死 24，3.3V 固件下打印的 SCK 是实际值的 2 倍；
`--div 2` 在 12 MHz 主频下实际是 6 MHz。

### 基线（W25Q32 / DS35M1GA，12 MHz SPI，校验通过）

| | 写入 | 读取 |
|---|---|---|
| NOR 固件加速 | 205 KiB/s | 353–366 KiB/s |
| NAND 固件加速（Windows） | 434 KiB/s | 438 KiB/s |
| NAND 固件加速（Linux VM） | 344 KiB/s | 352 KiB/s |
| NAND + ECC（片上/主机 BCH） | ~430 KiB/s | ~430 KiB/s |

USB 全速 bulk 是瓶颈（约 0.43 MiB/s），SPI 和 ECC 都不是。

## 代码结构

```
firmware/include/board.h     唯一的板级配置（CS、/WP、/HOLD）
firmware/src/main.c          初始化 + 主循环 + ISR 向量声明
firmware/src/usb.c           描述符、EP0、ISR、双缓冲、EP0 状态查询
firmware/src/protocol.c      命令分发、流式传输、NOR/NAND 加速命令
firmware/src/spi.c           SPI0 主机驱动（性能关键路径）
host/nor_id.py               ID 探测 + 状态查询 + Bridge 类
host/speedtest.py            NOR 测速（--engine host|fw）
host/nand_speedtest.py       NAND 测速（--ecc none|ondie|host）
host/bch.py                  BCH-4（512B/7B）参考实现
```

默认引脚：CS=P1.4、MOSI=P1.5、MISO=P1.6、SCK=P1.7、/WP=P3.2、
/HOLD=P3.4。1.8V NAND 必须电平转换。

## 协议速查

帧格式 `[cmd][len][payload]`，回包 `[status][data]`。关键命令：

| cmd | 说明 |
|---|---|
| `0x20/21/22` | `CMD_STREAM_WRITE/READ/XFER`，主机逐页驱动 |
| `0x23` | `CMD_NOR_PROGRAM` `[addr24][len24]` + 数据 → `[status][done24]` |
| `0x24` | `CMD_NOR_ERASE` `[addr24][len24]` → `[status][done24]` |
| `0x25` | `CMD_NAND_SET_CFG` `[page16][oob16][flags]` |
| `0x26` | `CMD_NAND_READ_PAGES` `[page24][npages16]` → 数据 + status |
| `0x27` | `CMD_NAND_PROGRAM_PAGES` `[page24][npages16]` + 数据 → done 页数 |
| `0x42`（EP0） | 厂商状态查询，32 字节，bulk 挂了也能诊断 |

完整定义见 `docs/protocol.md`。

## 调试

bulk 没反应时先跑 `nor_id.py --status`：

| 现象 | 含义 |
|---|---|
| 请求 STALL | 芯片里不是这版固件 |
| `tick` 涨、`phase=0` | 主循环空闲，命令没送到 |
| `tick` 不涨、`phase=1` | 卡在某条命令，看 `cmd` |
| `tick` 不涨、`isr` 暴涨 | 中断风暴 |

可选 UART0：`make EXTRA_FLAGS=-DDE_PRINTF=1`（P3.1 TX / P3.0 RX，115200）。

## 已知未完成

- 坏块替换策略（目前只检测并跳过）
- 4 字节地址 NOR（>128 Mb）
- `CMD_BOOTLOADER` 软件跳转（0x3800 后不枚举）
- NAND x2/x4（CH552 SPI 只有单线）
- 长时间擦写稳定性、断电恢复

## 提交习惯

- 注释/提交信息用中文，代码标识符用英文
- 改固件后至少跑一次对应的 `speedtest` / `nand_speedtest` 校验
- 不要在 `host/ufprog/` 上做 `git add`
