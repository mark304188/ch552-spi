#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""最简 SPI NOR Flash JEDEC ID 探测工具（CH552 USB-SPI 桥）。

发送 0x9F，读回 JEDEC ID，解析厂商与容量，并对"没有芯片 / 接线错误"
给出提示。

用法::

    python3 host/nor_id.py            # 默认 SPI 6 MHz（div=4）
    python3 host/nor_id.py --div 8    # 3 MHz，杜邦线较长时更稳
    python3 host/nor_id.py -v         # 打印收发的原始帧

依赖::

    pip install pyusb

Linux 需要 udev 权限（见 README 的 udev 规则），或直接用 sudo 运行；
Windows 需要先用 Zadig 把接口 0 绑定到 WinUSB。

协议详见 docs/protocol.md。
"""

import argparse
import sys

try:
    import usb.core
    import usb.util
except ImportError:
    sys.exit("缺少 pyusb，请先执行: pip install pyusb")


# ---------------------------------------------------------------- 设备标识
VID, PID = 0x1A86, 0x5507          # 本固件
BOOT_VID, BOOT_PID = 0x4348, 0x55E0  # WCH USB bootloader

EP_OUT = 0x03                      # bulk OUT，主机 -> 设备
EP_IN = 0x82                       # bulk IN，设备 -> 主机
INTERFACE = 0
TIMEOUT_MS = 1000

# ---------------------------------------------------------------- 协议常量
# 帧格式: [cmd][len][payload...]，见 docs/protocol.md
CMD_PING = 0x01
CMD_GET_INFO = 0x02
CMD_SET_SPI_CFG = 0x04
CMD_CS = 0x08
CMD_SPI_XFER = 0x11

ST_OK = 0x00

FSYS_MHZ = 24.0

# JEDEC 厂商 ID（0x9F 返回的第一个字节）
MANUFACTURERS = {
    0x01: "Spansion / Infineon",
    0x0B: "XTX",
    0x1C: "EON",
    0x1F: "Atmel / Adesto",
    0x20: "Micron / ST",
    0x37: "AMIC",
    0x5E: "Zbit",
    0x62: "SANYO",
    0x68: "Boya",
    0x85: "Puya",
    0x89: "Intel",
    0x8C: "ESMT",
    0x9D: "ISSI",
    0xBF: "SST / Microchip",
    0xC2: "Macronix",
    0xC8: "GigaDevice",
    0xEF: "Winbond",
}


class Bridge:
    """USB-SPI 桥的最小客户端：组帧、收发、读 JEDEC ID。"""

    def __init__(self, dev, verbose=False):
        self.dev = dev
        self.verbose = verbose

        try:
            dev.set_configuration()
        except usb.core.USBError:
            # 有些系统在 open 时已经配置过，忽略即可。
            pass

        try:
            usb.util.detach_kernel_driver(dev, INTERFACE)
        except (usb.core.USBError, NotImplementedError):
            pass

        usb.util.claim_interface(dev, INTERFACE)

    # ------------------------------------------------------------ 底层收发
    def send(self, cmd, payload=b""):
        frame = bytes([cmd, len(payload)]) + bytes(payload)
        if self.verbose:
            print("  -> " + frame.hex(" "))
        self.dev.write(EP_OUT, frame, TIMEOUT_MS)

    def recv(self, n):
        """读满 n 字节（设备按 64 字节分帧，短包会让 USB 提前返回）。"""
        buf = bytearray()
        while len(buf) < n:
            chunk = self.dev.read(EP_IN, n - len(buf), TIMEOUT_MS)
            if not chunk:
                break
            buf += bytes(chunk)
        if self.verbose:
            print("  <- " + bytes(buf).hex(" "))
        return bytes(buf)

    # ------------------------------------------------------------ 命令封装
    def ping(self):
        """PING 并校验 magic，确认协议同步；返回固件协议版本。"""
        for _ in range(5):
            self.send(CMD_PING)
            r = self.recv(5)
            if len(r) == 5 and r[0] == ST_OK and r[1:4] == b"CSP":
                return r[4]
        raise RuntimeError("设备无响应或协议不同步，请重新插拔 USB 后重试")

    def info(self):
        """CMD_GET_INFO -> (fw_major, fw_minor, mode3, div)"""
        self.send(CMD_GET_INFO)
        r = self.recv(10)
        if len(r) != 10 or r[0] != ST_OK:
            raise RuntimeError("读取设备信息失败")
        return r[5], r[6], r[7], r[8]

    def set_spi(self, div, mode3):
        self.send(CMD_SET_SPI_CFG, bytes([div & 0xFF, 1 if mode3 else 0]))
        r = self.recv(1)
        if len(r) != 1 or r[0] != ST_OK:
            code = r[0] if r else -1
            raise RuntimeError("设置 SPI 参数失败 (status=%s)" % code)

    def cs(self, level):
        """level=0 拉低选中，1 释放。无回包。"""
        self.send(CMD_CS, bytes([1 if level else 0]))

    def xfer(self, data):
        """全双工交换，返回 MISO 字节。"""
        self.send(CMD_SPI_XFER, bytes(data))
        r = self.recv(len(data) + 1)
        if len(r) != len(data) + 1:
            raise RuntimeError("回包长度不符（期望 %d，收到 %d）"
                               % (len(data) + 1, len(r)))
        if r[0] != ST_OK:
            raise RuntimeError("SPI 传输失败 (status=0x%02X)" % r[0])
        return r[1:]

    # ------------------------------------------------------------ 具体操作
    def jedec_id(self):
        """发送 0x9F 并读回 4 个 ID 字节。"""
        self.cs(0)
        try:
            # 第 1 个字节是发送 0x9F 期间移入的无效数据，忽略。
            rx = self.xfer([0x9F, 0xFF, 0xFF, 0xFF, 0xFF])
        finally:
            self.cs(1)
        return rx[1:]


def describe_size(cap):
    """JEDEC 容量字节是 log2(字节数)。

    标称密度按 1 MB = 1 MiB 换算（16 MB 对应 128 Mbit），和芯片型号的
    标法一致。"""
    if cap >= 32:
        return "厂商自定义编码"
    n = 1 << cap
    mbit = n * 8.0 / (1 << 20)
    if n >= 1 << 20:
        return "%.0f MB (%.0f Mbit)" % (n / (1 << 20), mbit)
    if n >= 1 << 10:
        return "%.0f KB (%.2f Mbit)" % (n / (1 << 10), mbit)
    return "%d B (%.2f Mbit)" % (n, mbit)


def report(jdec):
    man, typ, cap = jdec[0], jdec[1], jdec[2]
    ext = jdec[3] if len(jdec) > 3 else None

    print("JEDEC ID : " + " ".join("%02X" % b for b in jdec))

    if man == 0xFF and typ == 0xFF and cap == 0xFF:
        print("厂商     : 未知 (FF FF FF)")
        print()
        print("诊断: 读回全 0xFF，MOSI/MISO 上没有器件回应。可能原因：")
        print("  - Flash 未供电，或 CS/SCK/MOSI/MISO 接线错误")
        print("  - CS 没有真正拉低（检查 P1.4 与 Flash 的 CS#）")
        print("  - SPI 时钟太快（试 --div 8 或 16）")
        return False

    if man == 0x00 and typ == 0x00 and cap == 0x00:
        print("厂商     : 未知 (00 00 00)")
        print()
        print("诊断: 读回全 0x00，MISO 被拉低。可能原因：")
        print("  - MISO 与 GND 短路，或 Flash 未上电")
        print("  - 上一颗芯片的保持状态（尝试给 Flash 复位）")
        return False

    name = MANUFACTURERS.get(man)
    print("厂商     : %s (0x%02X)" % (name or "未知", man))
    print("类型     : 0x%02X" % typ)
    print("容量     : 0x%02X -> %s" % (cap, describe_size(cap)))
    if ext is not None:
        print("扩展字节 : 0x%02X（厂商自定义，部分型号为设备 ID）" % ext)
    return True


def main():
    ap = argparse.ArgumentParser(
        description="读取 SPI NOR Flash 的 JEDEC ID",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--div", type=int, default=4,
                    help="SPI 分频，SCK = 24MHz/div（2=12MHz 4=6MHz 8=3MHz）")
    ap.add_argument("--mode", type=int, choices=(0, 3), default=0,
                    help="SPI 模式")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="打印原始 USB 帧")
    args = ap.parse_args()

    if args.div < 2:
        sys.exit("--div 最小为 2（SCK = Fsys/2）")

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        if usb.core.find(idVendor=BOOT_VID, idProduct=BOOT_PID) is not None:
            sys.exit("找到的是 WCH bootloader（0x4348:0x55E0），"
                     "说明设备还没运行本固件。")
        sys.exit("未找到设备 %04X:%04X\n"
                 "  Linux  : 检查 udev 规则，或用 sudo 运行\n"
                 "  Windows: 用 Zadig 把接口 0 绑定到 WinUSB\n"
                 "  硬件   : 确认 USB 已连接且固件已烧录" % (VID, PID))

    try:
        br = Bridge(dev, args.verbose)
        proto_ver = br.ping()
        br.set_spi(args.div, args.mode == 3)
        fw_major, fw_minor, mode3, div = br.info()
        jdec = br.jedec_id()
    except usb.core.USBError as exc:
        text = str(exc)
        if ("Access" in text or "Permission" in text
                or getattr(exc, "errno", 0) in (-3, 13)):
            sys.exit("USB 权限不足。\n"
                     "  Linux  : 安装 README 里的 udev 规则，或用 sudo 运行\n"
                     "  Windows: 用 Zadig 把接口 0 绑定到 WinUSB")
        sys.exit("USB 错误: %s" % exc)
    except RuntimeError as exc:
        sys.exit(str(exc))

    print("桥       : 固件 v%d.%d, 协议 v%d" % (fw_major, fw_minor, proto_ver))
    print("SPI      : mode %d, SCK = %.1f MHz (div=%d)"
          % (3 if mode3 else 0, FSYS_MHZ / div, div))
    print()
    ok = report(jdec)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
