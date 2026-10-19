#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DS35M1GA / DS35Q1GA SPI NAND：ID 探测 + 读写测速（关闭片上 ECC）。

流程：
    复位 -> READ ID -> 关 ECC -> 找连续好块 -> 擦除 -> 整页编程 -> 读回校验

默认走固件加速命令（CMD_NAND_PROGRAM_PAGES / CMD_NAND_READ_PAGES），
--engine host 则退回主机逐页驱动（CMD_STREAM_*），用于对比和排错。

用法：
    python3 host/nand_speedtest.py                  # 4 个块（512 KiB 主区）
    python3 host/nand_speedtest.py --id-only        # 只看 ID 和状态
    python3 host/nand_speedtest.py --engine host    # 主机逐页驱动
    python3 host/nand_speedtest.py --blocks 16 --div 2
    python3 host/nand_speedtest.py --no-erase -y    # 块已擦除时直接写
"""

import argparse
import os
import sys
import time

import usb.core

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bch  # noqa: E402
from nor_id import BACKEND, Bridge, FSYS_MHZ, VID, PID  # noqa: E402
from speedtest import (  # noqa: E402
    DATA_CHUNK, EP_IN, EP_OUT, ST_OK, TIMEOUT_MS, human, rate,
    stream_read, stream_write, u24,
)

try:
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# ------------------------------------------------------------------ 协议常量

CMD_NAND_SET_CFG = 0x25
CMD_NAND_READ_PAGES = 0x26
CMD_NAND_PROGRAM_PAGES = 0x27

# NAND 命令
NAND_RESET = 0xFF
NAND_READ_ID = 0x9F
NAND_WREN = 0x06
NAND_PAGE_READ = 0x13
NAND_READ_CACHE = 0x03
NAND_PROG_LOAD = 0x02
NAND_PROG_EXEC = 0x10
NAND_BLOCK_ERASE = 0xD8
NAND_GET_FEATURE = 0x0F
NAND_SET_FEATURE = 0x1F

FEATURE_STATUS = 0xC0
FEATURE_CFG = 0xB0
FEATURE_LOCK = 0xA0

ST_OIP = 0x01
ST_FAIL = 0x0C  # E_FAIL | P_FAIL
PAGE_SIZE = 2048
OOB_SIZE = 64
PAGE_TOTAL = PAGE_SIZE + OOB_SIZE
PAGES_PER_BLOCK = 64


def row24(row):
    """PAGE READ / EXECUTE / ERASE 的 24 位地址：8 dummy + 16 位行地址。"""
    return bytes((0x00, (row >> 8) & 0xFF, row & 0xFF))


def le16(v):
    return bytes((v & 0xFF, (v >> 8) & 0xFF))


# ------------------------------------------------------------------ NAND 基础操作

def nand_reset(br):
    stream_write(br, bytes([NAND_RESET]), b"")
    time.sleep(0.005)


def nand_read_id(br):
    """READ ID。DS35 在命令后先输出 1 个 dummy 字节，再输出 E5 21；
    直接在返回数据里找厂商 ID，避免依赖固定的偏移。"""
    br.cs(0)
    try:
        rx = br.xfer([NAND_READ_ID] + [0xFF] * 7)
    finally:
        br.cs(1)
    for i in range(1, len(rx) - 1):
        if rx[i] == 0xE5:
            return bytes((rx[i], rx[i + 1]))
    return bytes(rx[1:3])


def nand_get_feature(br, reg):
    """GET FEATURE：0F + 地址，随后第一个字节就是寄存器值。"""
    data = stream_read(br, bytes([NAND_GET_FEATURE, reg]), 3)
    return data[0]


def nand_set_feature(br, reg, value):
    stream_write(br, bytes([NAND_SET_FEATURE, reg, value]), b"")


def nand_wait(br, timeout=1.0):
    """轮询 C0h 的 OIP。返回最终状态。"""
    t0 = time.perf_counter()
    while True:
        st = nand_get_feature(br, FEATURE_STATUS)
        if not (st & ST_OIP):
            return st
        if time.perf_counter() - t0 > timeout:
            raise RuntimeError("NAND 一直忙 (SR=0x%02X)" % st)


def nand_page_read(br, row):
    stream_write(br, bytes([NAND_PAGE_READ]) + row24(row), b"")
    return nand_wait(br)


def nand_read_cache(br, column, count):
    """READ FROM CACHE：命令 + 2 字节列地址 + **1 个 dummy 字节** + 数据。"""
    col = bytes(((column >> 8) & 0xFF, column & 0xFF))
    return stream_read(br, bytes([NAND_READ_CACHE]) + col + b"\xFF", count)


def nand_is_bad_block(br, block):
    """坏块标记：块内第 1/2 页 OOB 第 1 字节不是 0xFF。"""
    for p in (0, 1):
        row = block * PAGES_PER_BLOCK + p
        nand_page_read(br, row)
        if nand_read_cache(br, PAGE_SIZE, 1)[0] != 0xFF:
            return True
    return False


def nand_erase_block(br, block):
    row = block * PAGES_PER_BLOCK
    stream_write(br, bytes([NAND_WREN]), b"")
    stream_write(br, bytes([NAND_BLOCK_ERASE]) + row24(row), b"")
    st = nand_wait(br, timeout=5.0)
    if st & ST_FAIL:
        raise RuntimeError("块 %d 擦除失败 (SR=0x%02X)" % (block, st))
    return st


def nand_program_page(br, row, data):
    """主机逐页：WREN -> PROGRAM LOAD(整页) -> EXECUTE -> 等 OIP。"""
    stream_write(br, bytes([NAND_WREN]), b"")
    stream_write(br, bytes([NAND_PROG_LOAD, 0x00, 0x00]), data)
    stream_write(br, bytes([NAND_PROG_EXEC]) + row24(row), b"")
    st = nand_wait(br)
    if st & ST_FAIL:
        raise RuntimeError("行 0x%04X 编程失败 (SR=0x%02X)" % (row, st))
    return st


# ------------------------------------------------------------------ 固件加速命令

def nand_read_pages_fw(br, row, npages):
    br.send(CMD_NAND_READ_PAGES, u24(row) + le16(npages))
    total = npages * PAGE_TOTAL
    buf = bytearray()
    while len(buf) < total:
        part = br.dev.read(EP_IN, min(DATA_CHUNK, total - len(buf)),
                           TIMEOUT_MS)
        if not part:
            break
        buf += part
    st = br.recv(1)
    if len(buf) != total:
        raise RuntimeError("读回长度不符（%d/%d）" % (len(buf), total))
    if not st or st[0] != ST_OK:
        raise RuntimeError("NAND 读失败 status=0x%02X" % (st[0] if st else 0xFF))
    return bytes(buf)


def nand_program_pages_fw(br, row, npages, data):
    timeout = 5000 + npages * 50
    br.send(CMD_NAND_PROGRAM_PAGES, u24(row) + le16(npages))
    for off in range(0, len(data), DATA_CHUNK):
        br.dev.write(EP_OUT, data[off:off + DATA_CHUNK], TIMEOUT_MS)
    # Bridge.recv 固定 1 s 超时，这里自己读
    buf = bytearray()
    while len(buf) < 4:
        part = br.dev.read(EP_IN, 4 - len(buf), timeout)
        if not part:
            break
        buf += part
    if len(buf) != 4:
        raise RuntimeError("编程回包长度不符（%d）" % len(buf))
    done = buf[1] | (buf[2] << 8) | (buf[3] << 16)
    if buf[0] != ST_OK:
        raise RuntimeError("NAND 编程失败 status=0x%02X，完成 %d 页"
                           % (buf[0], done))
    if done != npages:
        raise RuntimeError("NAND 编程完成页数不符（%d/%d）" % (done, npages))
    return done


# ------------------------------------------------------------------ 数据

def page_pattern(row):
    """主区按行地址生成数据，OOB 填 0xFF（不破坏坏块标记）。"""
    base = (((row & 0xFF) * 0x07) + (((row >> 8) & 0xFF) * 0x13)) & 0xFF
    main = bytes((((i * 0x1D) + base) & 0xFF) for i in range(PAGE_SIZE))
    return main + b"\xFF" * OOB_SIZE


def page_pattern_ecc(row):
    """主区数据 + 每个 512B 段 7 字节 BCH 校验（放在 OOB[8..35]）。

    OOB[0] 留 0xFF，避免把坏块标记写坏。"""
    main = page_pattern(row)[:PAGE_SIZE]
    oob = bytearray(b"\xFF" * OOB_SIZE)
    for i in range(PAGE_SIZE // 512):
        oob[8 + i * 7:8 + (i + 1) * 7] = bch.encode(main[i * 512:(i + 1) * 512])
    return main + bytes(oob)


def verify_ecc_blocks(read_back, npages):
    """逐页逐 512B 段检查 BCH 余式，返回失败段数。"""
    bad = 0
    for p in range(npages):
        base = p * PAGE_TOTAL
        for i in range(PAGE_SIZE // 512):
            sec = read_back[base + i * 512:base + (i + 1) * 512]
            par = read_back[base + PAGE_SIZE + 8 + i * 7:
                            base + PAGE_SIZE + 8 + (i + 1) * 7]
            if not bch.check(sec, par):
                bad += 1
    return bad


def sample_ecc_status(br, start_row, npages, count=4):
    """抽样读几页，返回片上 ECC 状态 SR[5:4] 列表。"""
    res = []
    step = max(1, npages // count)
    for k in range(count):
        row = start_row + k * step
        nand_page_read(br, row)
        st = nand_get_feature(br, FEATURE_STATUS)
        res.append((st >> 4) & 0x03)
    return res


def compare(written, read_back, main_only=True):
    first = -1
    bad = 0
    for i, (a, b) in enumerate(zip(written, read_back)):
        if main_only and (i % PAGE_TOTAL) >= PAGE_SIZE:
            continue
        if a != b:
            if first < 0:
                first = i
            bad += 1
    return first, bad


# ------------------------------------------------------------------ 入口

def find_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=BACKEND)
    if dev is None:
        sys.exit("未找到设备 %04X:%04X" % (VID, PID))
    return dev


def main():
    ap = argparse.ArgumentParser(
        description="DS35M1GA SPI NAND ID 探测与读写测速（无 ECC）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--div", type=int, default=4,
                    help="SPI 分频，SCK = Fsys/div（2=12MHz 4=6MHz）")
    ap.add_argument("--mode", type=int, choices=(0, 3), default=0)
    ap.add_argument("--block", type=int, default=8, help="起始块")
    ap.add_argument("--blocks", type=int, default=4, help="测试块数")
    ap.add_argument("--ecc", choices=("none", "ondie", "host"), default="none",
                    help="none=关 ECC，ondie=片上 ECC，host=主机关 ECC 并算 BCH-4")
    ap.add_argument("--engine", choices=("fw", "host"), default="fw",
                    help="fw=固件加速命令，host=主机逐页驱动")
    ap.add_argument("--id-only", action="store_true", help="只探测 ID/状态")
    ap.add_argument("--no-erase", action="store_true",
                    help="跳过擦除（块已擦除时）")
    ap.add_argument("-y", "--yes", action="store_true", help="跳过确认")
    args = ap.parse_args()

    if args.div < 2:
        sys.exit("--div 最小为 2")
    if args.blocks <= 0 or args.block < 0 or args.block >= 1024:
        sys.exit("--block/--blocks 超出范围")

    br = Bridge(find_device())
    st = br.vendor_status()
    if st is not None:
        print("桥       : 固件 v%d.%d, 协议 v%d" % (st[4], st[5], st[3]))
    br.ping()
    br.set_spi(args.div, args.mode == 3)
    print("SPI      : mode %d, SCK = %.1f MHz (div=%d)"
          % (args.mode, FSYS_MHZ / args.div, args.div))

    # ---------------------------------------------------------- ID / 状态
    nand_reset(br)
    jid = nand_read_id(br)
    manuf = jid[0]
    dev_id = jid[1] if len(jid) > 1 else 0xFF
    names = {
        (0xE5, 0x21): "DS35M1GA (1.8V, 1Gbit)",
        (0xE5, 0x71): "DS35Q1GA (3.3V, 1Gbit)",
    }
    name = names.get((manuf, dev_id), "未知器件")
    print("NAND ID  : %s  ->  %s" % (jid.hex(" ").upper(), name))
    if manuf != 0xE5:
        print("警告     : 厂商 ID 不是 0xE5，后面的时序可能不匹配")
    if (manuf, dev_id) == (0xE5, 0x21):
        print("警告     : 1.8V 器件，确认 VCC 和 IO 电平不要超过 2.0V")

    sr = nand_get_feature(br, FEATURE_STATUS)
    print("状态     : SR=0x%02X  OIP=%d  E_FAIL=%d  P_FAIL=%d  WEL=%d"
          % (sr, sr & 1, (sr >> 2) & 1, (sr >> 3) & 1, (sr >> 1) & 1))

    if args.id_only:
        return 0

    # ---------------------------------------------------------- ECC 设置
    if args.ecc == "ondie":
        nand_set_feature(br, FEATURE_CFG, 0x10)  # ECC on, QE off
    else:
        nand_set_feature(br, FEATURE_CFG, 0x00)  # ECC off, QE off
    cfg = nand_get_feature(br, FEATURE_CFG)
    print("ECC      : %s (B0h=0x%02X, bit4=%d)"
          % ("片上 ECC 开" if args.ecc == "ondie" else "关闭",
             cfg, (cfg >> 4) & 1))
    if (args.ecc == "ondie") != bool(cfg & 0x10):
        print("警告     : ECC 设置与请求不一致")

    # ---------------------------------------------------------- 块保护
    lock = nand_get_feature(br, FEATURE_LOCK)
    if lock & 0x3E:  # BP2:0 + INV + CMP
        print("块保护   : A0h=0x%02X（BP2:0=%d%d%d）-> 清除"
              % (lock, (lock >> 5) & 1, (lock >> 4) & 1, (lock >> 3) & 1))
        nand_set_feature(br, FEATURE_LOCK, 0x00)
        lock = nand_get_feature(br, FEATURE_LOCK)
        if lock & 0x3E:
            print("警告     : 块保护清除失败 (A0h=0x%02X)，检查 BRWD/WP#"
                  % lock)
        else:
            print("           已解锁，全片可写")
    else:
        print("块保护   : A0h=0x%02X（未锁定）" % lock)

    # ---------------------------------------------------------- 几何
    br.send(CMD_NAND_SET_CFG, le16(PAGE_SIZE) + le16(OOB_SIZE) + bytes([0]))
    r = br.recv(1)
    if not r or r[0] != ST_OK:
        sys.exit("设置 NAND 几何失败 (status=0x%02X)" % (r[0] if r else 0xFF))
    print("几何     : %d + %d = %d B/页, %d 页/块"
          % (PAGE_SIZE, OOB_SIZE, PAGE_TOTAL, PAGES_PER_BLOCK))

    # ---------------------------------------------------------- 找连续好块
    good = []
    b = args.block
    while b < 1024 and len(good) < args.blocks:
        if nand_is_bad_block(br, b):
            print("坏块     : %d，跳过" % b)
            good = []  # 要求连续
        else:
            good.append(b)
        b += 1
    if len(good) < args.blocks:
        sys.exit("找不到 %d 个连续好块" % args.blocks)
    start_row = good[0] * PAGES_PER_BLOCK
    npages = len(good) * PAGES_PER_BLOCK
    main_bytes = npages * PAGE_SIZE
    print("测试块   : %s（行 0x%04X 起，%d 页 / %s 主区）"
          % (", ".join(str(x) for x in good), start_row, npages,
             human(main_bytes)))
    print()

    if not args.yes:
        if not sys.stdin.isatty():
            sys.exit("非交互环境：确认要擦写这些块请加 -y")
        if input("擦除并写入这些块？输入 yes: ").strip().lower() != "yes":
            sys.exit("已取消")

    # ---------------------------------------------------------- 擦除
    t_erase = 0.0
    if not args.no_erase:
        t0 = time.perf_counter()
        for blk in good:
            nand_erase_block(br, blk)
        t_erase = time.perf_counter() - t0
        print("[1] 擦除 %d 个块: %.3f s  (%.2f ms/块)"
              % (len(good), t_erase, t_erase * 1000.0 / len(good)))

    # ---------------------------------------------------------- 写入
    t_ecc_enc = 0.0
    if args.ecc == "host":
        t0 = time.perf_counter()
        data = b"".join(page_pattern_ecc(start_row + i) for i in range(npages))
        t_ecc_enc = time.perf_counter() - t0
    else:
        data = b"".join(page_pattern(start_row + i) for i in range(npages))
    t0 = time.perf_counter()
    if args.engine == "fw":
        nand_program_pages_fw(br, start_row, npages, data)
    else:
        for i in range(npages):
            nand_program_page(br, start_row + i,
                              data[i * PAGE_TOTAL:(i + 1) * PAGE_TOTAL])
    t_prog = time.perf_counter() - t0
    print("[2] 编程 %d 页 (%s): %.3f s  (%s)"
          % (npages, args.engine, t_prog, rate(main_bytes, t_prog)))

    # ---------------------------------------------------------- 读回
    t0 = time.perf_counter()
    if args.engine == "fw":
        back = nand_read_pages_fw(br, start_row, npages)
    else:
        back = bytearray()
        for i in range(npages):
            row = start_row + i
            nand_page_read(br, row)
            back += nand_read_cache(br, 0, PAGE_TOTAL)
        back = bytes(back)
    t_read = time.perf_counter() - t0
    print("[3] 读回 %d 页 (%s): %.3f s  (%s)"
          % (npages, args.engine, t_read, rate(main_bytes, t_read)))

    # ---------------------------------------------------------- 校验
    first, bad = compare(data, back)
    ecc_bad = 0
    t_ecc_chk = 0.0
    ecc_status = []
    if args.ecc == "host":
        t0 = time.perf_counter()
        ecc_bad = verify_ecc_blocks(back, npages)
        t_ecc_chk = time.perf_counter() - t0
    elif args.ecc == "ondie":
        ecc_status = sample_ecc_status(br, start_row, npages)

    if bad == 0 and ecc_bad == 0:
        print("[4] 校验: 通过（主区 %s 全部匹配%s）"
              % (human(main_bytes),
                 "，BCH 校验位全部一致" if args.ecc == "host" else ""))
    else:
        print("[4] 校验: 失败，%d 字节不同，首个差异 +0x%X"
              % (bad, first))
        if ecc_bad:
            print("      BCH 校验失败 %d 个 512B 段" % ecc_bad)
        if bad:
            off = max(0, first - 8)
            print("      期望 %s" % data[off:off + 24].hex(" "))
            print("      实际 %s" % back[off:off + 24].hex(" "))
    if args.ecc == "ondie":
        print("      片上 ECC 状态抽样: %s  (%s)"
              % (" ".join("%02d" % x for x in ecc_status),
                 "全部无错" if all(x == 0 for x in ecc_status) else "有纠正/错误"))
    if args.ecc == "host":
        print("      BCH 编码 %.3f s，校验 %.3f s（%d 段 × 512 B）"
              % (t_ecc_enc, t_ecc_chk, npages * 4))

    # ---------------------------------------------------------- 汇总
    print()
    print("汇总")
    print("  芯片     : %s  %s" % (jid.hex(" ").upper(), name))
    print("  SPI      : %.1f MHz (div=%d, mode %d)"
          % (FSYS_MHZ / args.div, args.div, args.mode))
    print("  几何     : %d + %d B/页, %d 页/块, ECC %s"
          % (PAGE_SIZE, OOB_SIZE, PAGES_PER_BLOCK,
             {"none": "关闭", "ondie": "片上", "host": "主机 BCH-4"}[args.ecc]))
    if t_erase:
        print("  擦除     : %.2f ms/块  (%.1f KiB/s)"
              % (t_erase * 1000.0 / len(good),
                 (len(good) * 128.0 * 1024) / t_erase / 1024.0))
    print("  写入     : %s" % rate(main_bytes, t_prog))
    print("  读取     : %s" % rate(main_bytes, t_read))
    print("  校验     : %s" % ("通过" if bad == 0 and ecc_bad == 0 else "失败"))
    if args.ecc == "host":
        print("  ECC 开销 : 编码 %s, 校验 %s"
              % (rate(main_bytes, t_ecc_enc) if t_ecc_enc else "-",
                 rate(main_bytes, t_ecc_chk) if t_ecc_chk else "-"))
    return 0 if bad == 0 and ecc_bad == 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n已中断")
        sys.exit(130)
    except RuntimeError as exc:
        sys.exit("错误: %s" % exc)
    except usb.core.USBError as exc:
        sys.exit("USB 错误: %s" % exc)
