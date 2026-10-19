#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BCH(4-bit) 参考实现，用于 SPI NAND 主机侧 ECC 测速。

    数据区: 512 字节
    校验区: 7 字节（56 bit）

系统码：parity = (data << 56) mod g(x)，g(x) 是 GF(2^14) 上
α^1、α^3、α^5、α^7 最小多项式的乘积（次数 56），可纠正每 512 字节
最多 4 bit 错误。

说明：
- 片上 ECC 的码型是芯片内部的，校验位存在隐藏 spare 区，和这里不兼容；
  本模块只用于“主机关掉片上 ECC、自己算 ECC”的测速路径。
- 这里实现编码和“校验位是否一致”的检查（余式为 0），不实现纠错译码；
  测速场景写入后立即读回，正常情况下没有错误需要纠正。
"""

M = 14
N = (1 << M) - 1          # 16383
DEG = 4 * M               # 56 位校验
MASK = (1 << DEG) - 1


def _gf_mul(a, b, poly):
    r = 0
    while b:
        if b & 1:
            r ^= a
        b >>= 1
        a <<= 1
        if a & (1 << M):
            a ^= poly
    return r & N


def _gf_pow(a, e, poly):
    r = 1
    while e:
        if e & 1:
            r = _gf_mul(r, a, poly)
        a = _gf_mul(a, a, poly)
        e >>= 1
    return r


def _is_primitive(poly):
    if _gf_pow(2, N, poly) != 1:
        return False
    # 16383 = 3 * 43 * 127
    for q in (3, 43, 127):
        if _gf_pow(2, N // q, poly) == 1:
            return False
    return True


def _find_primitive():
    for low in range(1, 1 << M, 2):
        poly = (1 << M) | low
        if _is_primitive(poly):
            return poly
    raise RuntimeError("no primitive polynomial found")


def _min_poly(i, prim):
    coset = []
    j = i
    while j not in coset:
        coset.append(j)
        j = (j * 2) % N
    poly = [1]
    for j in coset:
        a = _gf_pow(2, j, prim)
        new = [0] * (len(poly) + 1)
        for k, c in enumerate(poly):
            new[k + 1] ^= c
            new[k] ^= _gf_mul(c, a, prim)
        poly = new
    g = 0
    for k, c in enumerate(poly):
        if c not in (0, 1):
            raise RuntimeError("minimal polynomial is not binary")
        if c:
            g |= 1 << k
    return g


def _poly_mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a <<= 1
        b >>= 1
    return r


def _build_generator():
    prim = _find_primitive()
    g = 1
    for i in (1, 3, 5, 7):
        g = _poly_mul(g, _min_poly(i, prim))
    if g.bit_length() - 1 != DEG:
        raise RuntimeError("unexpected generator degree")
    return g


GEN = _build_generator()


def _make_table():
    table = []
    for b in range(256):
        r = b << (DEG - 8)
        for _ in range(8):
            if r & (1 << (DEG - 1)):
                r = ((r << 1) ^ GEN) & MASK
            else:
                r = (r << 1) & MASK
        table.append(r)
    return table


TABLE = _make_table()


def _update(reg, data):
    for b in data:
        reg = ((reg << 8) ^ TABLE[((reg >> (DEG - 8)) ^ b) & 0xFF]) & MASK
    return reg


def encode(data):
    """512 字节 -> 7 字节校验。"""
    if len(data) != 512:
        raise ValueError("BCH block must be 512 bytes")
    reg = _update(0, data)
    return reg.to_bytes(7, "big")


def check(data, parity):
    """校验 data+parity 的余式是否为 0。"""
    if len(data) != 512 or len(parity) != 7:
        raise ValueError("BCH block must be 512+7 bytes")
    return _update(_update(0, data), parity) == 0


if __name__ == "__main__":
    import random
    import time

    rng = random.Random(1)
    data = bytes(rng.getrandbits(8) for _ in range(512))
    par = encode(data)
    print("parity:", par.hex(" "))
    print("check ok:", check(data, par))

    bad = bytearray(data)
    bad[100] ^= 0x01
    print("1 bit 错误检出:", not check(bytes(bad), par))

    bad = bytearray(data)
    bad[10] ^= 0x80
    bad[300] ^= 0x02
    bad[500] ^= 0x40
    bad[511] ^= 0x08
    print("4 bit 错误检出:", not check(bytes(bad), par))

    bad = bytearray(par)
    bad[3] ^= 0x10
    print("校验位错误检出:", not check(data, bytes(bad)))

    n = 2048
    t0 = time.perf_counter()
    for _ in range(n):
        encode(data)
    dt = (time.perf_counter() - t0) / n
    print("encode: %.1f us/512B -> %.1f MiB/s" % (dt * 1e6, 512 / dt / 1048576))
    t0 = time.perf_counter()
    for _ in range(n):
        check(data, par)
    dt = (time.perf_counter() - t0) / n
    print("check : %.1f us/512B -> %.1f MiB/s" % (dt * 1e6, 512 / dt / 1048576))
