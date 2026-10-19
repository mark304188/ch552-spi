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

# Vendor control request (over EP0) that reports firmware/endpoint state.
# It keeps working when the bulk endpoints are dead, so it is used to tell
# "old firmware" apart from "wrong wiring".
VENDOR_REQ_STATUS = 0x42
VENDOR_STATUS_LEN = 32

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

        # 设备已经配置好时不要再发 SET_CONFIGURATION：这条标准请求会把
        # 两端的端点 toggle 归零，旧固件不跟着复位就会和主机错位。固件已
        # 修复（见 usb_bulk_reset），这里仍然跳过这次多余的往返。
        try:
            cfg = dev.get_active_configuration()
        except usb.core.USBError:
            cfg = None
        if cfg is None or cfg.bConfigurationValue != 1:
            try:
                dev.set_configuration()
            except usb.core.USBError:
                pass

        try:
            if dev.is_kernel_driver_active(INTERFACE):
                dev.detach_kernel_driver(INTERFACE)
        except (usb.core.USBError, NotImplementedError):
            # Windows/WinUSB has no kernel driver to detach, and some
            # libusb builds do not implement the call at all.
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

    # ------------------------------------------------------------ 状态查询
    def vendor_status(self):
        """读固件状态（走 EP0，不依赖 bulk）。不支持时返回 None。"""
        try:
            raw = self.dev.ctrl_transfer(0xC0, VENDOR_REQ_STATUS, 0, 0,
                                         VENDOR_STATUS_LEN, TIMEOUT_MS)
        except usb.core.USBError:
            return None
        raw = bytes(raw)
        # The first 16 bytes are the stable part; newer builds append
        # diagnostic counters.  Accept either length.
        if len(raw) < 16 or raw[:3] != b"CSP":
            return None
        if len(raw) < VENDOR_STATUS_LEN:
            raw += bytes(VENDOR_STATUS_LEN - len(raw))
        return raw

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
        print("  - /HOLD (pin 7) 被拉低或悬空：芯片会完全不响应")
        print("    本固件默认把 P3.4 驱高；如果这块板子没接 /HOLD，")
        print("    请用 10k 上拉到 3.3V（读回全 FF 最常见的原因）")
        print("  - SPI 时钟太快（试 --div 8 或 16）")
        return False

    if man == 0x00 and typ == 0x00 and cap == 0x00:
        print("厂商     : 未知 (00 00 00)")
        print()
        print("诊断: 读回全 0x00，MISO 被拉低。可能原因：")
        print("  - MISO 与 GND 短路，或 Flash 未上电")
        print("  - 芯片处于 /HOLD 或掉电状态（检查 pin 7 的上拉和 VCC）")
        return False

    name = MANUFACTURERS.get(man)
    print("厂商     : %s (0x%02X)" % (name or "未知", man))
    print("类型     : 0x%02X" % typ)
    print("容量     : 0x%02X -> %s" % (cap, describe_size(cap)))
    if typ == 0x00:
        print("提示     : type 为 0x00 较罕见，确认没有接错/虚焊")
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
    ap.add_argument("--status", action="store_true",
                    help="只读固件状态（EP0），不做 SPI 操作")
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
    except usb.core.USBError as exc:
        sys.exit("USB 错误: %s" % exc)

    # The EP0 status request tells us whether the chip is running a build
    # that knows about the bulk protocol at all.
    st = br.vendor_status()
    if st is not None:
        print("桥       : 固件 v%d.%d, 协议 v%d"
              % (st[4], st[5], st[3]))
        if args.status or args.verbose:
            tick = st[21] | (st[22] << 8)
            print("寄存器   : UEP2_3_MOD=%02X UEP2_CTRL=%02X UEP3_CTRL=%02X"
                  " UEP3_DMA_L=%02X" % (st[6], st[7], st[8], st[9]))
            print("主循环   : tick=%d phase=%d cmd=0x%02X"
                  % (tick, st[28], st[27]))
            print("中断计数 : isr=%d setup=%d out=%d in=%d"
                  % (st[23], st[26], st[24], st[25]))
            print("端点状态 : frame_ready=%d had_frame=%d frame_len=%d"
                  " rx_half=%d in=0x%02X"
                  % (st[10], st[15], st[16], st[11], st[12]))
            print("回包状态 : reply_len=%d reply_off=%d in_pre_len=%d"
                  % (st[17], st[18], st[19]))
            print("控制     : USB_CTRL=%02X UDEV_CTRL=%02X"
                  % (st[13], st[14]))
        if args.status:
            return 0
    else:
        print("桥       : 未响应固件状态请求（EP0）")
        print("           → 芯片里可能烧的是旧版本固件，请重新烧录当前 build")
        if args.status:
            return 1

    try:
        proto_ver = br.ping()
        br.set_spi(args.div, args.mode == 3)
        fw_major, fw_minor, mode3, div = br.info()
        jdec = br.jedec_id()
    except usb.core.USBError as exc:
        text = str(exc)
        if getattr(exc, "errno", 0) == -7 or "timed out" in text.lower():
            sys.exit("bulk 端点无响应（超时）。\n"
                     "  1) 芯片可能烧的是旧固件：重新烧录 build/ch552_spi_bridge.hex\n"
                     "  2) 也可能是 USB 线/供电问题，换一个端口试试")
        if ("Access" in text or "Permission" in text
                or getattr(exc, "errno", 0) in (-3, 13)):
            sys.exit("USB 权限不足。\n"
                     "  Linux  : 安装 README 里的 udev 规则，或用 sudo 运行\n"
                     "  Windows: 用 Zadig 把接口 0 绑定到 WinUSB")
        sys.exit("USB 错误: %s" % exc)
    except RuntimeError as exc:
        sys.exit(str(exc))

    print("SPI      : mode %d, SCK = %.1f MHz (div=%d)"
          % (3 if mode3 else 0, FSYS_MHZ / div, div))
    print()
    ok = report(jdec)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
