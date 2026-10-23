# CH552 USB-SPI Bridge (English)

> 中文版: [README.md](README.md)

A USB-to-SPI bridge built on the CH552 (enhanced 8051 with a full-speed USB
device controller), intended for production programming, reading and verifying
SPI NOR / SPI NAND flash.

The board firmware is complete. The host talks to it over a vendor-specific
bulk interface, so Linux needs no kernel driver (libusb/usbfs); Windows uses
WinUSB. The wire protocol is documented in [`docs/protocol.md`](docs/protocol.md).

```
┌──────────┐   USB 2.0 FS    ┌──────────┐    SPI (up to 12 MHz)  ┌────────────┐
│   PC     │◄───────────────►│  CH552   │◄──────────────────────►│ NOR / NAND │
│ libusb   │  bulk 0x03/0x82 │ 24 MHz   │  SCK/MOSI/MISO/CS      │  Flash     │
└──────────┘                 └──────────┘                        └────────────┘
```

## Features

* **USB full speed (12 Mbit/s), vendor-specific interface**, two bulk endpoints,
  both double-buffered.
* **Selectable system clock**: 24 MHz (5 V, default) / 16 / 12 / 6 MHz.
  A 3.3 V board must use 12 MHz (see
  [`docs/hardware.md`](docs/hardware.md#11-主频与供电电压的对应关系重要)).
* **Hardware SPI master**, clock = Fsys / divider, up to 12 MHz at Fsys = 24 MHz.
* **Zero-copy streaming**: a whole page (256 B / 2 KB) is one command plus raw
  data frames, with no per-byte command overhead.
* **Software chip select** that can stay asserted across several commands,
  matching NOR/NAND multi-phase sequences (write enable -> page program ->
  poll status).
* Driver-free on Linux (libusb/usbfs); WinUSB on Windows (see below).
* Firmware upgrades require the BOOT pin; there is no software jump into the
  WCH bootloader in 1.3 and later (the old command was unreliable).

## Layout

```
├── Makefile                    firmware build (SDCC)
├── firmware/
│   ├── include/                ch552.h, ch554_usb.h, spi.h, usb.h,
│   │                           protocol.h, debug.h
│   └── src/                    main.c, usb.c, protocol.c, spi.c, debug.c
├── docs/
│   ├── protocol.md             host interface specification
│   └── hardware.md             pins, wiring, power
└── host/
    ├── nor_id.py               minimal JEDEC ID probe
    ├── README.md               tool usage and reusable Bridge class
    └── 99-ch552-spi.rules      udev rule (no root needed)
```

## Documentation

| Document | Content |
|---|---|
| [`docs/protocol.md`](docs/protocol.md) | Frame format, command table, timing, examples |
| [`docs/hardware.md`](docs/hardware.md) | Pins, wiring, power, signal integrity |
| [`host/README.md`](host/README.md) | Host tool usage and the `Bridge` class |

## Building

Requires SDCC 4.x (4.2.0 recommended).

```bash
sudo apt install sdcc          # Debian/Ubuntu
make                           # 24 MHz, 5 V supply (default)
make VCC=3V3                   # 3.3 V -> 12 MHz, SPI up to 6 MHz
```

If SDCC is not on `PATH`:

```bash
make SDCC=/path/to/sdcc/bin/sdcc PACKIHX=/path/to/sdcc/bin/packihx
```

Output:

```
build/ch552_spi_bridge.hex   Intel HEX, flash with WCHISPTool
build/ch552_spi_bridge.bin   raw binary, flash with ch55xtool / librech551
```

Current firmware is about 7.8 KB of the 14 KB available. xRAM:
0x0000-0x01FF is reserved for the USB endpoint buffers (EP0 64 B, EP3 2x64 B,
EP2 2x64 B, 192 B reply staging); the compiler's xdata starts at 0x0200
(512 B); about 113 B of iRAM is left for the stack.

### Flashing

* Windows: WCHISPTool (from WCH), select `build/ch552_spi_bridge.hex`.
* Linux/macOS: `pip install ch55xtool` then `make flash` (or `librech551`,
  `wchisp`).

Hold the BOOT button while plugging in USB to enter the WCH ISP bootloader
(`4348:55E0`). There is no software jump in firmware 1.3 and later.

## Wiring

The SPI clock/data pins are fixed by the chip. Chip select and the auxiliary
`/WP` and `/HOLD` GPIOs are configured in `firmware/include/board.h`; the
defaults match the common CH552 programmer boards:

| CH552 | Direction | SPI flash | Note |
|---|---|---|---|
| P1.7 | out | SCK | SPI0 hardware clock |
| P1.6 | in | SO / IO1 | SPI0 MISO |
| P1.5 | out | SI / IO0 | SPI0 MOSI |
| P1.4 | out | CS# | software CS, change in `board.h` |
| P3.2 | out | /WP | push-pull, high at boot |
| P3.4 | out | /HOLD | push-pull, high at boot |
| 3V3 | power | VCC | external 3.3 V recommended |
| GND | - | GND | |

Note: P1.6/P1.7 are also UART1 RXD/TXD; this firmware does not use UART1.
UART0 (P3.0/P3.1) is kept for debugging.

## Host side

Device identity:

| Item | Value |
|---|---|
| VID / PID | `0x1A86` / `0x5507` |
| Interface | 0, class `0xFF` (vendor specific), no driver bound |
| Bulk OUT | `0x03`, 64 bytes |
| Bulk IN | `0x82`, 64 bytes |

Linux: install the udev rule once (the file is in the repository):

```bash
sudo cp host/99-ch552-spi.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
# unplug and replug
```

Windows: `pip install pyusb libusb-package`, then bind interface 0 to WinUSB
with [Zadig](https://zadig.akeo.ie/) (Options -> List All Devices ->
`ch552-spi (Interface 0)` -> WinUSB -> Replace Driver). The WCH ISP bootloader
is a different VID:PID (`4348:55E0`) and is not affected.

### Ready-made probe

[`host/nor_id.py`](host/nor_id.py) reads the JEDEC ID, decodes vendor and
capacity, and diagnoses "no chip / wrong wiring" cases.

```bash
pip install pyusb
python3 host/nor_id.py            # 6 MHz
python3 host/nor_id.py --div 8    # 3 MHz, safer on long wires
```

### Writing your own host

Frames are `[cmd][len][payload]` out, `[status][data]` back. Minimal exchange:

```python
import usb.core, usb.util

dev = usb.core.find(idVendor=0x1A86, idProduct=0x5507)
usb.util.claim_interface(dev, 0)
EP_OUT, EP_IN = 0x03, 0x82

def send(cmd, payload=b""):
    dev.write(EP_OUT, bytes([cmd, len(payload)]) + payload, 1000)

def recv(n):
    return bytes(dev.read(EP_IN, n, 1000))

send(0x08, b"\x00")                        # CMD_CS: assert
send(0x11, b"\x9F\xFF\xFF\xFF\xFF")    # CMD_SPI_XFER: 0x9F + 4 clocks
r = recv(6)                                # status + 5 MISO bytes
send(0x08, b"\x01")                        # CMD_CS: release
print("id", r[2:5].hex())                  # r[1] is the byte clocked during 0x9F
```

## Performance

* Maximum SPI clock is **12 MHz** (`SPI0_CK_SE = 2`, Fsys/2). The power-on
  default is **6 MHz** (`div = 4`) for signal integrity on long wires;
  `CMD_SET_SPI_CFG` can raise it to 12 MHz once the flash is identified.
* The SPI byte loop costs about 16 Fsys cycles per byte; the SPI side is
  roughly 1.2 MB/s at 12 MHz.
* USB full-speed bulk is the real bottleneck (theoretical ~1.06 MB/s,
  0.5-0.9 MB/s in practice).
* Page program/read use streaming commands: one 256 B page is one command
  frame plus four data frames.
* **Accelerator commands** (`CMD_NOR_PROGRAM`, `CMD_NOR_ERASE`,
  `CMD_NAND_READ_PAGES`, `CMD_NAND_PROGRAM_PAGES`) run the per-page
  WREN/program/poll loop inside the device, so the host sends one large block.
  Measured on a W25Q32:

  | Path | Write | Erase |
  |---|---|---|
  | Host-driven (`CMD_STREAM_WRITE`) | 10 KiB/s | 72 KiB/s |
  | Firmware loop, 6 MHz | 183 KiB/s | 126 KiB/s |
  | Firmware loop, 12 MHz | **250 KiB/s** | **122 KiB/s** |

  NOR read is about **459 KiB/s** at 12 MHz (Windows/WinUSB, verified).

* SPI NAND (DS35M1GA, 1.8 V, 2048+64, no ECC), Windows/WinUSB, 1 MiB verified:

  | Path | Write | Read |
  |---|---|---|
  | Host-driven (`CMD_STREAM_*`) | 42 KiB/s | 55 KiB/s |
  | Firmware loop, 6 MHz | 195 KiB/s | 263 KiB/s |
  | Firmware loop, 12 MHz | **434 KiB/s** | **438 KiB/s** |
  | + on-die ECC | ~430 KiB/s | ~430 KiB/s |
  | + host BCH-4 | ~430 KiB/s | ~430 KiB/s |

  > The table is pure sequential throughput, **without bad-block scanning or
  > erase**. A real burn must first read the bad-block marker from page 1/2 of
  > every block and skip bad blocks (1 Gbit = 1024 blocks, at least 2048 page
  > reads). A full-chip scan before programming is a fixed cost that hurts
  > small images the most, so the effective rate can be lower.

* NOR/NAND erase and program times are set by the chips; the firmware only
  avoids adding extra waits.

## Measurements

Verified on a common CH552 SPI programmer board (`/WP`=P3.2, `/HOLD`=P3.4):

| Item | Result |
|---|---|
| Enumeration | `1a86:5507`, vendor interface, driver-free on Linux |
| JEDEC ID @ 6 MHz | `EF 40 16` -> Winbond W25Q32 (4 MB / 32 Mbit) |
| JEDEC ID @ 12 MHz | same (maximum SPI clock works) |
| RDSR | `0x00` (WIP=0, WEL=0) |
| Read at address 0 | all `0xFF` (blank), repeatable |
| Accelerated R/W | 64 KiB @ 12 MHz, write 250 / read 459 KiB/s, verified |

Page program/erase/verify, SPI NAND and ECC have all been verified on hardware.
Not yet verified: bad-block replacement, 4-byte-address NOR, x2/x4 modes and
long-term stability.

## Version

* Firmware 1.3 / protocol 1.0
* Target: CH552 (CH554 compatible, but USB host mode is not used)

## References

* <https://github.com/ieiao/ch554_sdcc> - CH554/CH552 SDCC support package
* <https://github.com/SoCXin/CH552> - CH552 resources and vendor SDK
* CH552/CH551 datasheet (WCH)

## License

GNU Affero General Public License v3.0, see [`LICENSE`](LICENSE).

Third-party dependencies keep their own licenses: pyusb (BSD-3-Clause),
libusb (LGPL-2.1, dynamically linked), SDCC (compiler; does not affect the
generated firmware).
