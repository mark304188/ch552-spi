# 硬件说明

## 1. 最小系统

CH552 内部有 5V→3.3V LDO 和 24 MHz 振荡器，USB 全速收发器也是内置的，
所以最小系统只需要：

```
USB VBUS ──┐
           ├── VCC (5V)
USB D-  ───┤ P3.7 (UDM)
USB D+  ───┤ P3.6 (UDP)
           │
GND ───────┴── GND
```

* `V33` 脚：内部 LDO 的输出，需要外接 0.1 µF + 1 µF 到地；如果外部已有
  3.3 V，可以把 3.3 V 接到 V33 并在 `GLOBAL_CFG` 里关闭 LDO（本固件没做，
  默认使用内部 LDO）。
* USB 时钟来自内部 24 MHz 振荡器经 PLL 得到的 48 MHz。WCH 出厂已做校准，
  bootloader 本身也是用同一时钟枚举的，所以不需要外部晶振；如果个别批次
  出现枚举不稳，可以改用外部 24 MHz 晶振（`CLOCK_CFG` 的 `bOSC_EN_XT`，
  见 `docs/hardware.md` 末尾）。
* `RST` 脚建议接 10 kΩ 上拉到 VCC，并留一个对地按键用于强制进 ISP。
* USB 线上建议串 22 Ω 电阻，D+ 上拉由芯片内部完成。

> CH552 内部 LDO 的输出电流有限（数据手册建议 ≤ 30 mA 量级）。烧录
> SPI NOR/NAND 时目标芯片的峰值电流一般不大，但如果目标是多颗并联或者
> 需要 1.8 V，请用外部电源方案。

### 1.1 主频与供电电压的对应关系（重要）

CH552 数据手册表 8.2.2 对主频有供电电压限制：

| Fsys | VCC（VCC 脚）要求 | 说明 |
|---|---|---|
| 24 MHz | **> 4.4 V** | USB 5 V 供电时可用，本固件默认 |
| 16 MHz | > 3.3 V | 3.3 V 供电时属于临界，不建议 |
| 12 MHz | 无限制 | **3.3 V 供电时必须选这个** |
| 6 MHz | 无限制 | 最低可用主频，USB 全速的下限 |

也就是说：**如果整板由 3.3 V 供电，默认的 24 MHz 是超规格的**。这时请重新
编译：

```bash
make FREQ_SYS=12000000
```

主频变化后 `SCK = Fsys / div`，同样的 `div` 得到一半的 SPI 时钟；上位机
可以照常用 `CMD_SET_SPI_CFG`，只是绝对频率跟着变。

> 32 MHz（`MASK_SYS_CK_SEL = 111`）在 CH552 上被标记为
> **"Reserved, disabled"**：手册 V1.1（2017-09）把最高主频从 32 MHz 降到了
> 24 MHz。它对端到端吞吐也没有帮助（瓶颈在 USB 全速 bulk），因此固件默认
> 禁止；仅作为实验提供 `make FREQ_SYS=32000000 ALLOW_OVERCLOCK=1`。

## 2. 引脚配置

### 2.1 哪些引脚能改，哪些不能

**不能改**：SPI0 的时钟和数据脚由芯片内部固定，没有引脚复用选项。

| 引脚 | 功能 |
|---|---|
| P1.7 | SCK |
| P1.6 | MISO |
| P1.5 | MOSI |

**能改**：片选和辅助 GPIO 都是普通 GPIO，全部集中在
`firmware/include/board.h` 一个文件里：

```c
/* 片选：P1.4（SPI0 的 SCS 脚，主模式下是普通 GPIO）。
 * 可以改成任意空闲的 P1.0 ~ P1.4。 */
#define BOARD_CS_BIT 4

/* 辅助 GPIO：索引 0 默认 P1.3，可改成任意空闲 P1 脚；
 * 0xFF 表示不使用该槽位。默认不接，需要时接 /WP 或 /RESET。 */
#define BOARD_GPIO0_BIT 3      /* P1.3 */
#define BOARD_GPIO1_BIT 0xFF
#define BOARD_GPIO2_BIT 0xFF
#define BOARD_GPIO3_BIT 0xFF
```

改完之后重新 `make` 即可，其它代码（SPI 初始化、方向配置、CS 时序、
`CMD_GPIO_*` 命令）会自动跟着变。板上还有 `board.c` 负责上电时把这些
引脚置成空闲状态。

`P1.4` 在主模式下不会被 SPI 外设占用，所以既能当片选，也能当普通 GPIO；
WCH 官方例程也是这么做的。

### 2.2 片选（CS）

* 低有效，`BOARD_CS_ACTIVE` 的极性直接体现在 `CS_ASSERT()` /
  `CS_DEASSERT()` 两个宏里，需要高有效时改这两行。
* 由软件驱动，因此一条命令可以在多个 SPI 阶段之间保持低电平。
* 上电、USB 复位、`CMD_SET_SPI_CFG` 都会把它释放。

### 2.3 辅助 GPIO

* **开漏**驱动：写 0 拉低，写 1 释放（高阻）。这是驱动目标复位线最安全
  的方式，不会和板上其它驱动器冲突。
* 如果目标板没有上拉电阻，需要外部加 10 kΩ 到 3.3 V。
* 索引与物理引脚的映射在 `board.h`，上位机用索引访问，因此换板子不需要
  改上位机代码。

方向配置（`spi_init()` 里完成）：

```c
P1_MOD_OC &= ~(bSCK | bMISO | bMOSI | bSCS);  // 全部推挽
P1_DIR_PU |=  (bSCK | bMOSI | bSCS);          // 输出
P1_DIR_PU &= ~bMISO;                           // 输入，无上拉
```

MISO 不要加上拉：Flash 在未被选中时会把 SO 置为高阻，上拉会掩盖总线
冲突，也不利于判断芯片是否在位。

## 3. 可选控制脚

| 引脚 | 用途 | 控制方式 |
|---|---|---|
| P1.3 | 辅助 GPIO（开漏），默认不接；可接 `/WP` 或 `/RESET` | `CMD_GPIO_WRITE`，pin = 0 |
| P3.0/P3.1 | UART0 调试口（仅 `DE_PRINTF=1` 时编译） | 115200 8N1 |
| P3.2/P3.3 | 未使用 | 可自行在 `board.h` 里改成辅助 GPIO |

`P1.3` 上电后是高阻（开漏释放），不会影响目标板；需要动作时由上位机
显式拉低。它**不是必须接**的：标准 SOIC-8 的 NOR/NAND 没有复位脚，
`/WP` 和 `/HOLD` 用两个上拉电阻处理即可（见下一节）。

> 如果要接 NAND 的 R/B#：最简单的做法是用 `0F C0` 读 feature 寄存器判断
> OIP，不需要额外接线。确实需要硬件握手时，把 `BOARD_GPIO1_BIT` 改成
> `P3.2`/`P3.3` 即可——但 `board.c` 目前按开漏输出配置辅助引脚，做输入
> 需要把对应槽位改成高阻输入（`gpio_drive` 里一行改动）。

## 4. 接线示例（SPI NOR，SOIC-8）

标准 25 系列 SPI NOR 的 8 脚封装：

| 脚 | 名称 | 说明 |
|---|---|---|
| 1 | /CS | 片选 |
| 2 | DO (IO1) | 数据输出 |
| 3 | **/WP (IO2)** | 写保护，低有效 |
| 4 | GND | 地 |
| 5 | DI (IO0) | 数据输入 |
| 6 | CLK | 时钟 |
| 7 | **/HOLD (IO3)** | 保持，低有效 |
| 8 | VCC | 电源 |

```
CH552            W25Qxx / GD25Qxx (SOIC-8)
P1.7  SCK    ─── 6  CLK
P1.6  MISO   ─── 2  DO (IO1)
P1.5  MOSI   ─── 5  DI (IO0)
P1.4  CS#    ─── 1  CS#
3V3          ─── 8  VCC
GND          ─── 4  GND

                 3  /WP   (IO2) ── 10k ── 3V3
                 7  /HOLD (IO3) ── 10k ── 3V3
```

`/WP` 和 `/HOLD` **不接 MCU，但必须各用一个 10 kΩ 上拉到 3.3 V**：

* `/WP` 为低时芯片忽略所有 program/erase 命令（读和读 ID 不受影响），
  典型症状是"能认 ID，写不进去"。
* `/HOLD` 为低时芯片连时钟都不响应，典型症状是**读 ID 返回全 `FF`**。
* 不要靠芯片内部上拉：通常只有几十 µA 的弱上拉，长线或干扰下不可靠，
  各家 datasheet 的典型应用电路都要求外部 10 kΩ。

> 少数型号（例如 W25Q32FW 的 IM 订购选项）把 pin 3 复用为 `/RESET`
> 而不是 `/WP`，此时 pin 3 同样只需要 10 kΩ 上拉。

### 4.1 需要软件控制写保护时

把 `/WP` 改接到 P1.3（辅助 GPIO 0），保留那个 10 kΩ 上拉：

```
P1.3  ─── 3  /WP (IO2) ── 10k ── 3V3
```

辅助 GPIO 是开漏驱动，"释放"为高阻，由上拉拉高：

| 操作 | 效果 |
|---|---|
| `CMD_GPIO_WRITE pin=0 value=0` | 拉低 `/WP` → 写保护 |
| `CMD_GPIO_WRITE pin=0 value=1` | 释放 → 上拉拉高 → 可写 |

固件默认上电即释放，所以不操作时芯片处于可写状态。如果希望开机就是
保护状态，把 `board.c` 的 `board_init()` 里该引脚改成 `board_gpio_write(0, 0)`。

## 5. 接线示例（SPI NAND，WSON8）

```
CH552            SPI NAND (WSON8)
P1.7  SCK    ─── CLK
P1.6  MISO   ─── DO (IO1)
P1.5  MOSI   ─── DI (IO0)
P1.4  CS#    ─── CS#
3V3          ─── VCC
GND          ─── GND

                 3  /WP   (IO2) ── 10k ── 3V3
                 7  /HOLD (IO3) ── 10k ── 3V3
```

规则和 NOR 完全一样。NAND 的 `R/B#` 只有在 TSOP48 之类的大封装上才有，
WSON8 没有这个脚；等待内部操作完成用 `GET FEATURE (0F C0)` 轮询 OIP
即可，不需要额外接线。

> 老型号如果没有 OIP 位，可以读 `0F C0` 状态字节的 bit0，或者换用带
> R/B# 的封装并把该脚接到 P3.2/P3.3（见第 3 节）。

## 6. 供电与信号完整性

* SCK 最高 12 MHz（Fsys/2）。杜邦线连接时建议降到 3–6 MHz
  （`CMD_SET_SPI_CFG(8, 0)` 或 `(16, 0)`），否则容易受反射影响。
* PCB 上建议 SCK/MOSI 串 22–33 Ω，MISO 直连。
* Flash 的 VCC 就近放 0.1 µF + 1 µF 去耦。
* 如果目标板自己给 Flash 供电，务必**断开 CH552 的 3V3 到目标 VCC**，
  只共地，否则两边 LDO 会打架。

## 7. 固件升级

两种方式：

1. **USB bootloader 热跳转**：上位机发 `CMD_BOOTLOADER`，设备重新枚举为
   WCH bootloader（VID:PID = `4348:55E0`），然后用 WCHISPTool /
   ch552tool 烧录。
2. **冷启动**：按住 BOOT 键（或短接 P3.6 到 3.3 V 的特定组合）后上电，
   芯片直接进 bootloader。

注意：CH552 的 bootloader 位于 `0x3800–0x3FFF`（2 KB），用户程序区为
`0x0000–0x37FF`（14 KB）。本固件约 3.8 KB。

## 8. 电流/功耗参考

| 状态 | 电流（典型） |
|---|---|
| 运行，USB 已枚举，SPI 空闲 | ~12 mA |
| 运行，SPI 12 MHz 连续传输 | ~15 mA |
| USB 挂起（本固件不主动休眠） | ~10 mA |

本固件为了「随时可恢复、不打断正在编程的 Flash」，没有实现 USB 挂起
休眠。如果需要低功耗，可以在 `usb.c` 的 `UIF_SUSPEND` 分支里加入
`PCON |= PD` 和唤醒配置。

## 9. 使用外部晶振

如果内部振荡器的 USB 时钟裕量不足（极少见），可以改用 24 MHz 晶振：

```c
SAFE_MOD = 0x55;
SAFE_MOD = 0xAA;
CLOCK_CFG |= bOSC_EN_XT;    /* 使能外部振荡器 */
CLOCK_CFG &= ~bOSC_EN_INT;  /* 关闭内部振荡器 */
SAFE_MOD = 0x00;
```

改完后 Fsys 仍然是 24 MHz（`SYS_CK_24M`），SPI 分频关系不变，固件其余
部分不需要修改。晶振负载电容按晶振规格选取（通常 2 × 12–22 pF）。
