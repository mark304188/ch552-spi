#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SPI NOR 写入 / 读回速度测试（CH552 USB-SPI 桥）。

流程：擦除 -> 逐页编程 -> 读回校验，每个阶段单独计时，最后给出速度汇总。
默认使用流式命令（CMD_STREAM_*），也就是实际烧录时走的路径。

用法示例：

    python3 host/speedtest.py                 # 64 KiB @ 0x000000，6 MHz
    python3 host/speedtest.py --div 2         # 12 MHz
    python3 host/speedtest.py --size 1M -y    # 1 MiB，跳过确认
    python3 host/speedtest.py --no-write      # 只测读取速度
    python3 host/speedtest.py --no-erase -y   # 空白区域直接编程

注意：擦除和写入会覆盖目标区域，默认会要求确认。
"""

import argparse
import os
import sys
import time

import usb.core

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nor_id import BACKEND, Bridge, FSYS_MHZ, VID, PID  # noqa: E402

# 中文输出在 Windows 重定向时也需要 UTF-8（nor_id 已处理 stdout，
# 这里保证 stderr 一致）。
try:
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# ------------------------------------------------------------------ 协议常量

EP_OUT = 0x03
EP_IN = 0x82
TIMEOUT_MS = 5000

CMD_STREAM_WRITE = 0x20
CMD_STREAM_READ = 0x21

MAX_PACKET = 64
# 数据阶段一次提交给 libusb 的字节数。设备按 64 字节逐包处理，但一个大的
# URB 可以让主机控制器在同一帧里连续重试，比每 64 字节调用一次快很多。
DATA_CHUNK = 16384

# NOR 命令
NOR_WREN = 0x06
NOR_RDSR = 0x05
NOR_READ = 0x03
NOR_PP = 0x02
NOR_SE = 0x20  # 4 KiB 扇区擦除

SECTOR = 4096
PAGE = 256

ST_OK = 0x00


# ------------------------------------------------------------------ 小工具

def u24(value):
    return bytes((value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF))


def parse_size(text):
    """接受 4096 / 4K / 64K / 1M / 0x1000。"""
    t = text.strip().upper().replace("_", "")
    mult = 1
    if t.endswith("K"):
        mult, t = 1024, t[:-1]
    elif t.endswith("M"):
        mult, t = 1024 * 1024, t[:-1]
    value = int(t, 0)
    return value * mult


def human(nbytes):
    if nbytes >= 1024 * 1024:
        return "%.2f MiB" % (nbytes / (1024.0 * 1024))
    return "%.1f KiB" % (nbytes / 1024.0)


def rate(nbytes, seconds):
    if seconds <= 0:
        return "-"
    return "%.1f KiB/s" % (nbytes / 1024.0 / seconds)


def make_pattern(addr, size, kind, seed=0x5A):
    """生成写入数据。

    counter：以地址为起点的伪随机置换（每个 256 字节块内 0..255 各出现一次），
    不同页/不同位置数据不同，便于发现地址错位与位粘连。
    """
    if kind == "ff":
        return b"\xFF" * size
    if kind == "zero":
        return b"\x00" * size
    if kind == "random":
        import random
        rng = random.Random(seed)
        block = bytes(rng.getrandbits(8) for _ in range(256))
        return (block * (size // 256 + 1))[:size]

    base = ((addr & 0xFF) + ((addr >> 8) & 0xFF) + ((addr >> 16) & 0xFF)) & 0xFF
    return bytes(((i * 0x1D) + base) & 0xFF for i in range(size))


# ------------------------------------------------------------------ 流式命令

def _check_status(st, what):
    if st != ST_OK:
        raise RuntimeError("%s 失败 (status=0x%02X)" % (what, st))


def stream_write(br, prefix, data, chunk=DATA_CHUNK):
    """CMD_STREAM_WRITE：一条命令 + 裸数据帧，整个传输期间 CS 由设备保持。"""
    payload = u24(len(data)) + bytes([len(prefix)]) + bytes(prefix)
    br.send(CMD_STREAM_WRITE, payload)

    for off in range(0, len(data), chunk):
        br.dev.write(EP_OUT, data[off:off + chunk], TIMEOUT_MS)

    st = br.recv(1)
    _check_status(st[0] if st else 0xFF, "流式写入")


def stream_read(br, prefix, count, chunk=DATA_CHUNK):
    """CMD_STREAM_READ：先 count 字节数据，最后 1 字节状态。"""
    payload = u24(count) + bytes([len(prefix)]) + bytes(prefix)
    br.send(CMD_STREAM_READ, payload)

    buf = bytearray()
    while len(buf) < count:
        part = br.dev.read(EP_IN, min(chunk, count - len(buf)), TIMEOUT_MS)
        if not part:
            break
        buf += part

    st = br.recv(1)
    _check_status(st[0] if st else 0xFF, "流式读取")
    if len(buf) != count:
        raise RuntimeError("读取长度不符（期望 %d，收到 %d）" % (count, len(buf)))
    return bytes(buf)


# ------------------------------------------------------------------ NOR 操作

def read_status(br):
    """RDSR。发 0x05 的那个字节 MISO 无效，真实状态在第二个字节。"""
    data = stream_read(br, bytes([NOR_RDSR]), 2)
    return data[1]


def wait_ready(br, timeout=5.0):
    """轮询 WIP，返回 (状态, 耗时, 轮询次数)。"""
    t0 = time.perf_counter()
    polls = 0
    while True:
        sr = read_status(br)
        polls += 1
        if not (sr & 0x01):
            return sr, time.perf_counter() - t0, polls
        if time.perf_counter() - t0 > timeout:
            raise RuntimeError("Flash 一直忙 (SR=0x%02X)，检查供电与 /WP" % sr)


def erase_sector(br, addr):
    """4 KiB 扇区擦除，等到 WIP 清零。返回 (issue+wait 耗时, busy 耗时)。

    NOR 的擦除/编程命令只有在 WEL=1 时才执行，所以每条擦除命令前都要
    先发 WREN（0x06）并让 CS 拉高，让芯片锁存写使能。"""
    t0 = time.perf_counter()
    stream_write(br, bytes([NOR_WREN]), b"")
    stream_write(br, bytes([NOR_SE]) + u24(addr), b"")
    t_issue = time.perf_counter() - t0
    _sr, t_busy, _polls = wait_ready(br)
    return t_issue + t_busy, t_busy


def program_page(br, addr, data):
    """页编程：WREN -> PP -> 等 WIP。

    返回 (发送耗时, 等待耗时)。发送耗时里包含 CS 释放前的全部 SPI 移出，
    也就是桥 + USB 的实际写入开销；等待耗时是 Flash 自己的 tPP。
    """
    stream_write(br, bytes([NOR_WREN]), b"")
    t0 = time.perf_counter()
    stream_write(br, bytes([NOR_PP]) + u24(addr), data)
    t_send = time.perf_counter() - t0
    _sr, t_busy, _polls = wait_ready(br)
    return t_send, t_busy


def read_region(br, addr, size):
    t0 = time.perf_counter()
    data = stream_read(br, bytes([NOR_READ]) + u24(addr), size)
    return data, time.perf_counter() - t0


# ------------------------------------------------------------------ 校验

def compare(written, read_back):
    """返回 (是否一致, 首个差异偏移, 差异字节数)。

    bytes 比较在 C 层完成，先按块定位，只有确实不一致时才逐字节统计，
    这样 4 MiB 的校验也只需要几毫秒。"""
    if written == read_back:
        return True, -1, 0

    first = -1
    bad = 0
    block = 4096
    for off in range(0, len(written), block):
        a = written[off:off + block]
        b = read_back[off:off + block]
        if a == b:
            continue
        for j in range(len(a)):
            if a[j] != b[j]:
                if first < 0:
                    first = off + j
                bad += 1
    return False, first, bad


# ------------------------------------------------------------------ 入口

def find_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=BACKEND)
    if dev is None:
        sys.exit("未找到设备 %04X:%04X\n"
                 "  Linux  : 检查 udev 规则，或用 sudo 运行\n"
                 "  Windows: 用 Zadig 把接口 0 绑定到 WinUSB\n"
                 "  硬件   : 确认 USB 已连接且固件已烧录" % (VID, PID))
    return dev


def confirm(args, addr, size):
    if args.yes:
        return
    if not sys.stdin.isatty():
        sys.exit("非交互环境：确认要覆盖 0x%06X..0x%06X 请加 --yes"
                 % (addr, addr + size - 1))
    print()
    print("警告：将擦除并写入 0x%06X..0x%06X（%s），原有数据会丢失。"
          % (addr, addr + size - 1, human(size)))
    answer = input("继续？输入 yes: ").strip().lower()
    if answer != "yes":
        sys.exit("已取消")


def main():
    ap = argparse.ArgumentParser(
        description="SPI NOR 写入/读回速度测试",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--addr", default="0", help="起始地址（需 4 KiB 对齐）")
    ap.add_argument("--size", default="64K",
                    help="测试大小，支持 4K/64K/1M 或字节数")
    ap.add_argument("--div", type=int, default=4,
                    help="SPI 分频，SCK = Fsys/div（2=12MHz 4=6MHz 8=3MHz）")
    ap.add_argument("--mode", type=int, choices=(0, 3), default=0,
                    help="SPI 模式")
    ap.add_argument("--pattern", choices=("counter", "random", "zero", "ff"),
                    default="counter", help="写入数据模式")
    ap.add_argument("--seed", type=int, default=0x5A, help="random 模式种子")
    ap.add_argument("--chunk", type=int, default=DATA_CHUNK,
                    help="数据阶段每 URB 字节数")
    ap.add_argument("--no-erase", action="store_true",
                    help="跳过擦除（仅当目标区域已擦除）")
    ap.add_argument("--no-write", action="store_true",
                    help="只测读取速度，不擦除/不写入")
    ap.add_argument("--erase-only", action="store_true",
                    help="只测擦除速度")
    ap.add_argument("-y", "--yes", action="store_true", help="跳过确认")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="打印每条命令的原始帧")
    args = ap.parse_args()

    addr = int(args.addr, 0)
    size = parse_size(args.size)

    if args.div < 2:
        sys.exit("--div 最小为 2（SCK = Fsys/2）")
    if addr % SECTOR:
        sys.exit("--addr 必须 4 KiB 对齐")
    if size <= 0 or size % SECTOR:
        sys.exit("--size 必须是 4 KiB 的整数倍")
    if not args.no_write and size % PAGE:
        sys.exit("--size 必须是 256 字节的整数倍")

    dev = find_device()
    try:
        br = Bridge(dev, args.verbose)
    except (usb.core.USBError, RuntimeError) as exc:
        sys.exit("USB 错误: %s" % exc)

    st = br.vendor_status()
    if st is not None:
        print("桥       : 固件 v%d.%d, 协议 v%d" % (st[4], st[5], st[3]))
    else:
        print("桥       : 未响应状态请求（可能是旧固件）")

    try:
        br.ping()
        br.set_spi(args.div, args.mode == 3)
        fw_major, fw_minor, mode3, div = br.info()
        jdec = br.jedec_id()
    except (usb.core.USBError, RuntimeError) as exc:
        sys.exit("通信失败: %s" % exc)

    cap = jdec[2]
    chip_size = 1 << cap if cap < 32 else 0
    manuf = {0xEF: "Winbond", 0xC2: "Macronix", 0x20: "Micron",
             0x9D: "ISSI", 0x1F: "Adesto", 0xC8: "GigaDevice",
             0x01: "Spansion", 0xBF: "SST"}
    print("芯片     : JEDEC %s  %s  容量 %s"
          % (jdec.hex(" ").upper(), manuf.get(jdec[0], "未知厂商"),
             human(chip_size) if chip_size else "未知"))
    print("SPI      : mode %d, SCK = %.1f MHz (div=%d)"
          % (3 if mode3 else 0, FSYS_MHZ / div, div))

    if chip_size and addr + size > chip_size:
        sys.exit("测试范围超出芯片容量（%s）" % human(chip_size))
    if addr % SECTOR or size % SECTOR:
        sys.exit("地址/长度必须 4 KiB 对齐")

    sr = read_status(br)
    if sr & 0x01:
        sys.exit("芯片忙 (SR=0x%02X)，稍后重试" % sr)
    if sr & 0x3C:
        print("警告     : 状态寄存器保护位非零 (SR=0x%02X)，写入可能失败" % sr)

    # 状态轮询的成本决定了 "写入(实测)" 与 "写入(桥)" 的差距：每次 WIP
    # 查询都是 1 次命令 OUT + 1~2 次 IN 往返。
    t0 = time.perf_counter()
    for _ in range(5):
        read_status(br)
    poll_ms = (time.perf_counter() - t0) * 1000.0 / 5

    sectors = size // SECTOR
    pages = size // PAGE
    print("测试范围 : 0x%06X - 0x%06X (%s), %d 扇区 / %d 页"
          % (addr, addr + size - 1, human(size), sectors, pages))
    print("状态轮询 : %.2f ms/次（WIP 等待的时间分辨率）" % poll_ms)
    print()

    if args.erase_only or not args.no_write:
        confirm(args, addr, size)

    # ---------------------------------------------------------- 擦除
    t_erase = t_erase_busy = 0.0
    if not args.no_write and not args.no_erase:
        print("[1] 擦除 %d 个 4 KiB 扇区 ..." % sectors)
        t0 = time.perf_counter()
        for i in range(sectors):
            _total, busy = erase_sector(br, addr + i * SECTOR)
            t_erase_busy += busy
            if sectors >= 8 and (i + 1) % max(1, sectors // 8) == 0:
                print("\r    扇区 %d/%d" % (i + 1, sectors), end="", flush=True)
        t_erase = time.perf_counter() - t0
        print("\r    完成，用时 %.3f s%s" % (t_erase, " " * 12))
        print("    平均 %.1f ms/扇区, %s"
              % (t_erase * 1000.0 / sectors, rate(size, t_erase)))
        print()

    # ---------------------------------------------------------- 写入
    t_send = t_busy = 0.0
    data = None
    if not args.no_write and not args.erase_only:
        data = make_pattern(addr, size, args.pattern, args.seed)
        print("[2] 编程 %d 页 x %d B (%s 模式) ..."
              % (pages, PAGE, args.pattern))
        t0 = time.perf_counter()
        for i in range(pages):
            off = i * PAGE
            send, busy = program_page(br, addr + off, data[off:off + PAGE])
            t_send += send
            t_busy += busy
            if pages >= 16 and (i + 1) % max(1, pages // 8) == 0:
                print("\r    页 %d/%d" % (i + 1, pages), end="", flush=True)
        t_total = time.perf_counter() - t0
        print("\r    完成%s" % (" " * 16))
        print("    发送/SPI : %.3f s  (%s)  <- 桥 + USB 的写入开销"
              % (t_send, rate(size, t_send)))
        print("    等待 tPP : %.3f s  (平均 %.2f ms/页)"
              % (t_busy, t_busy * 1000.0 / pages))
        print("    合计     : %.3f s  (%s, 平均 %.2f ms/页)"
              % (t_total, rate(size, t_total), t_total * 1000.0 / pages))
        print()

    # ---------------------------------------------------------- 读回
    t_read = 0.0
    read_back = None
    if not args.erase_only:
        print("[3] 读回 %s ..." % human(size))
        read_back, t_read = read_region(br, addr, size)
        print("    用时 %.3f s  (%s)" % (t_read, rate(size, t_read)))
        print()

    # ---------------------------------------------------------- 校验
    ok = True
    verified = data is not None and read_back is not None
    if verified:
        print("[4] 校验 %s ..." % human(size))
        ok, first, bad = compare(data, read_back)
        if ok:
            if args.pattern == "ff":
                print("    一致（全 0xFF，注意：也可能是写入没生效）")
            else:
                print("    一致，%d 字节全部匹配" % size)
        else:
            print("    不一致：%d 个字节不同，首个差异在 +0x%X" % (bad, first))
            off = max(0, first - 8)
            print("      期望 %s" % data[off:off + 24].hex(" "))
            print("      实际 %s" % read_back[off:off + 24].hex(" "))
        print()

    # ---------------------------------------------------------- 汇总
    print("汇总")
    print("  芯片     : %s  %s" % (jdec.hex(" ").upper(), human(chip_size)))
    print("  SPI      : %.1f MHz (div=%d, mode %d)"
          % (FSYS_MHZ / div, div, 3 if mode3 else 0))
    if t_erase:
        print("  擦除     : %s  (%.1f ms/扇区)"
              % (rate(size, t_erase), t_erase * 1000.0 / sectors))
    if t_send:
        print("  写入(桥) : %s  (%.2f ms/页, 不含 Flash tPP)"
              % (rate(size, t_send), t_send * 1000.0 / pages))
        print("  写入(实测) : %s  (含 Flash tPP)"
              % rate(size, t_send + t_busy))
    if t_read:
        print("  读取     : %s" % rate(size, t_read))
    if verified:
        print("  校验     : %s" % ("通过" if ok else "失败"))

    return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n已中断")
        sys.exit(130)
    except RuntimeError as exc:
        sys.exit("错误: %s" % exc)
    except usb.core.USBError as exc:
        sys.exit("USB 错误: %s\n（如果是超时，先运行 python3 host/nor_id.py --status）"
                 % exc)
