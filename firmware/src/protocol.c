/*==========================================================================*
 * protocol.c - command dispatch for the CH552 SPI bridge
 *
 * See protocol.h for the wire format.  This file is the only place that
 * knows about flash-style transactions; the USB and SPI layers below it
 * are generic.
 *
 * Throughput notes
 * ----------------
 * The streaming commands are the ones that matter for programming:
 *
 *   page program : CMD_STREAM_WRITE [len=256][pre=02 A2 A1 A0]
 *                  -> one command frame + four 64 byte data frames
 *   page read    : CMD_STREAM_READ  [len=256][pre=03 A2 A1 A0]
 *                  -> one command frame + four 64 byte IN frames
 *
 * Prefix bytes are clocked out by the device, so the host never has to
 * build a separate "command" frame for the opcode and address.  That is
 * what keeps a page transfer at the theoretical USB frame count.
 *==========================================================================*/

#include <ch552.h>
#include <stdint.h>

#include "board.h"
#include "spi.h"
#include "usb.h"
#include "protocol.h"

void mDelaymS(uint16_t n);

/* The reply staging buffer lives in usb.c with an absolute xRAM address.
 * It is declared here rather than in a header because SDCC rejects an
 * extern declaration that is followed by an __at() definition inside the
 * same translation unit, and usb.c is where it is defined. */
extern uint8_t __xdata usb_reply_buf[];

/*---------------------------------------------------------------*
 * Configuration state                                          *
 *---------------------------------------------------------------*/

static __idata uint8_t spi_div = SPI_CK_DIV4; /* 6 MHz at Fsys = 24 MHz */
static __idata uint8_t spi_mode3;
static __idata uint8_t cs_held; /* CS asserted by CMD_CS, stream must not release it */

/*---------------------------------------------------------------*
 * Reply construction                                           *
 *---------------------------------------------------------------*/

static void reply_begin(uint8_t total, uint8_t status)
{
    usb_in_arm(total);
    usb_reply_buf[0] = status;
}

static void reply_commit(void)
{
    usb_in_prime();
}

static void reply_one(uint8_t status)
{
    reply_begin(1, status);
    reply_commit();
}

/*---------------------------------------------------------------*
 * Streaming helpers                                            *
 *---------------------------------------------------------------*/

/* Current raw OUT data frame and the prefix bytes of the running stream.
 * Both are file-scope typed pointers/arrays on purpose: this keeps the hot
 * path free of generic (3 byte) pointers, which would otherwise call the
 * __gptrget/__gptrput helpers for every access. */
static uint8_t __xdata *stream_frame;
static __idata uint8_t stream_pre[MAX_PREFIX];

/* Blocking wait for the next raw OUT data frame.  usb_service() has to be
 * called here because the main loop is inside this command. */
static uint8_t stream_wait_frame(void)
{
    uint16_t spins = 0;

    while (!ep3_frame_ready) {
        usb_service();
        if (++spins == 0) {
            return 0; /* host stopped sending: give up */
        }
    }
    stream_frame = usb_out_frame();
    return ep3_frame_len;
}

static void stream_release_frame(void)
{
    ep3_frame_ready = 0;
    ep3_had_frame = 1;
    usb_service();
}

/* Clock the prefix bytes out (opcode / address) from internal RAM. */
static void stream_prefix(uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        if (!spi_put(stream_pre[i])) {
            return; /* bus did not clock; spi_bus_ok is cleared */
        }
    }
}

/* Clock `total` bytes from the host out to the SPI bus. */
static uint8_t stream_write(uint32_t total)
{
    uint8_t n;

    while (total) {
        n = stream_wait_frame();
        if (n == 0) {
            return ST_TIMEOUT;
        }
        if (total < n) {
            n = (uint8_t)total;
        }
        spi_write(stream_frame, n);
        total -= n;
        stream_release_frame();
    }
    return ST_OK;
}

/* Clock `total` bytes in from the SPI bus and stream them to the host. */
static uint8_t stream_read(uint32_t total)
{
    uint8_t __xdata *buf;
    uint8_t n;
    uint16_t spins;

    while (total) {
        spins = 0;
        do {
            buf = usb_in_claim();
            if (buf) {
                break;
            }
            usb_service();
        } while (++spins != 0);

        if (!buf) {
            return ST_TIMEOUT;
        }

        n = total > MAX_PACKET_SIZE ? MAX_PACKET_SIZE : (uint8_t)total;
        spi_read_fill(buf, n);
        usb_in_send(n);
        total -= n;
    }
    return ST_OK;
}

/* Full-duplex stream: host bytes are shifted out while the slave response
 * is shifted in and returned in the same order. */
static uint8_t stream_xfer(uint32_t total)
{
    uint8_t __xdata *in;
    uint8_t n;
    uint16_t spins;

    while (total) {
        n = stream_wait_frame();
        if (n == 0) {
            return ST_TIMEOUT;
        }
        if (total < n) {
            n = (uint8_t)total;
        }

        spins = 0;
        while (!(in = usb_in_claim())) {
            usb_service();
            if (++spins == 0) {
                return ST_TIMEOUT;
            }
        }

        spi_xfer_buf(stream_frame, in, n);
        usb_in_send(n);
        total -= n;
        stream_release_frame();
    }
    return ST_OK;
}

/* Run one streaming command.
 *
 *   dir: 0 = write, 1 = read, 2 = full duplex
 *
 * CS is asserted only if the host has not already asserted it with
 * CMD_CS, and released only in that case, so multi-phase flash sequences
 * can share one chip-select window. */
static uint8_t run_stream(uint8_t __xdata *p, uint8_t len, uint8_t dir)
{
    uint32_t total;
    uint8_t pre_len;
    uint8_t status;
    uint8_t own_cs;
    uint8_t i;

    if (len < 4) {
        return ST_PARAM;
    }

    total = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    pre_len = p[3];
    if (pre_len > MAX_PREFIX || (uint8_t)(4 + pre_len) > len) {
        return ST_PARAM;
    }

    /* Copy the prefix out of the USB frame before anything can re-arm
     * the OUT endpoint: usb_in_arm() below calls usb_service(), which
     * hands the frame buffer back to the SIE. */
    for (i = 0; i < pre_len; i++) {
        stream_pre[i] = p[4 + i];
    }

    own_cs = !cs_held;
    if (own_cs) {
        spi_flush();
        CS_ASSERT();
    }

    /* Drop any reply left over from the previous command so it cannot
     * leak into the streamed data. */
    usb_in_arm(0);

    stream_prefix(pre_len);

    switch (dir) {
    case 0:
        status = stream_write(total);
        break;
    case 1:
        status = stream_read(total);
        break;
    default:
        status = stream_xfer(total);
        break;
    }

    if (!spi_bus_ok && status == ST_OK) {
        status = ST_ERR; /* SPI never clocked: report it to the host */
    }

    if (own_cs) {
        CS_DEASSERT();
    }
    return status;
}

/*---------------------------------------------------------------*
 * Flash accelerators                                            *
 *                                                               *
 * The generic CMD_STREAM_* path lets the host drive every flash  *
 * command, which costs 3..5 USB round trips per page.  The       *
 * commands below move the per-page loop into the device so the   *
 * host can stream one large block instead.                       *
 *                                                               *
 *   NOR : WREN + PP(256 B) + poll WIP, 4 KiB sector erase        *
 *   NAND: PAGE READ + poll OIP + READ FROM CACHE,                *
 *         WREN + PROGRAM LOAD + EXECUTE + poll OIP + E/P_FAIL    *
 *                                                               *
 * The OOB area is transferred verbatim: on-die ECC (enable it    *
 * with SET FEATURE 1F/B0) and host-computed ECC both work.       *
 *                                                               *
 * All loop state lives in xdata: internal RAM is needed for the  *
 * stack, and SDCC allocates non-reentrant locals statically.     *
 *---------------------------------------------------------------*/

static __xdata uint32_t acc_addr;   /* current flash byte / page    */
static __xdata uint32_t acc_total;  /* bytes / pages still to do    */
static __xdata uint32_t acc_left;   /* bytes left in this page      */
static __xdata uint32_t acc_done;   /* units completed (reply)      */
static __xdata uint32_t acc_budget; /* poll budget                  */

/* Pending OUT frame.  A page boundary can fall inside a USB frame when
 * the start address is not 64 byte aligned, so the frame is held until
 * every byte of it has been shifted out. */
static __xdata uint8_t pend_off;
static __xdata uint8_t pend_len;

static uint8_t out_fill(void)
{
    if (pend_len) {
        return 1;
    }
    pend_len = stream_wait_frame();
    pend_off = 0;
    return pend_len != 0;
}

static void out_consume(uint8_t n)
{
    pend_off += n;
    pend_len -= n;
    if (!pend_len) {
        stream_release_frame();
    }
}

/* Give the held frame back to the SIE (error paths). */
static void out_drop(void)
{
    if (pend_len) {
        pend_len = 0;
        stream_release_frame();
    }
}

static void reply_done(uint8_t status)
{
    reply_begin(4, status);
    usb_reply_buf[1] = (uint8_t)acc_done;
    usb_reply_buf[2] = (uint8_t)(acc_done >> 8);
    usb_reply_buf[3] = (uint8_t)(acc_done >> 16);
    reply_commit();
}

/*---------------------------------------------------------------*
 * NOR: page program / sector erase                              *
 *---------------------------------------------------------------*/

static uint8_t nor_sr(void)
{
    uint8_t v;

    CS_ASSERT();
    spi_put(0x05);
    v = spi_xfer(0xFF);
    CS_DEASSERT();
    return v;
}

static void nor_wren(void)
{
    CS_ASSERT();
    spi_put(0x06);
    CS_DEASSERT();
}

/* Wait for WIP to rise (a few parts need some microseconds after CS
 * rises) and then to fall.  Returns 0 if it never clears. */
static uint8_t nor_wait_ready(void)
{
    uint16_t spins;

    for (spins = 0; spins < 2000; spins++) {
        if (nor_sr() & 0x01) {
            break;
        }
    }
    acc_budget = 1000000UL;
    while (acc_budget--) {
        if (!(nor_sr() & 0x01)) {
            return 1;
        }
    }
    return 0;
}

/* Program acc_total bytes starting at acc_addr.  Pages are never crossed. */
static uint8_t nor_program(void)
{
    uint8_t n;

    while (acc_total) {
        acc_left = 256UL - (acc_addr & 0xFFUL);
        if (acc_total < acc_left) {
            acc_left = acc_total;
        }

        nor_wren();
        CS_ASSERT();
        spi_put(0x02);
        spi_put((uint8_t)(acc_addr >> 16));
        spi_put((uint8_t)(acc_addr >> 8));
        spi_put((uint8_t)acc_addr);

        while (acc_left) {
            if (!out_fill()) {
                CS_DEASSERT();
                out_drop();
                return ST_TIMEOUT;
            }
            n = pend_len;
            if (acc_left < (uint32_t)n) {
                n = (uint8_t)acc_left;
            }
            spi_write(stream_frame + pend_off, n);
            out_consume(n);
            acc_left -= n;
            acc_total -= n;
            acc_addr += n;
            acc_done += n;
        }
        CS_DEASSERT();

        if (!nor_wait_ready()) {
            return ST_BUSY;
        }
    }
    return spi_bus_ok ? ST_OK : ST_ERR;
}

static uint8_t nor_erase(void)
{
    while (acc_total) {
        nor_wren();
        CS_ASSERT();
        spi_put(0x20);
        spi_put((uint8_t)(acc_addr >> 16));
        spi_put((uint8_t)(acc_addr >> 8));
        spi_put((uint8_t)acc_addr);
        CS_DEASSERT();

        if (!nor_wait_ready()) {
            return ST_BUSY;
        }
        acc_addr += 4096UL;
        acc_total -= 4096UL;
        acc_done++;
    }
    return spi_bus_ok ? ST_OK : ST_ERR;
}

/*---------------------------------------------------------------*
 * NAND: sequential page read / program                          *
 *---------------------------------------------------------------*/

static __xdata uint16_t nand_page_size = 2048;
static __xdata uint16_t nand_oob_size = 64;
static __xdata uint8_t nand_flags; /* bit0: READ FROM CACHE uses 0x0B */

#define NAND_FEATURE_STATUS 0xC0
#define NAND_ST_OIP 0x01
#define NAND_ST_FAIL 0x0C /* E_FAIL | P_FAIL */

static uint8_t nand_feature(uint8_t reg)
{
    uint8_t v;

    CS_ASSERT();
    spi_put(0x0F);
    spi_put(reg);
    v = spi_xfer(0xFF);
    CS_DEASSERT();
    return v;
}

/* Returns the status register once OIP clears, 0xFF on timeout. */
static uint8_t nand_wait_ready(void)
{
    uint16_t spins;
    uint8_t st;

    for (spins = 0; spins < 2000; spins++) {
        st = nand_feature(NAND_FEATURE_STATUS);
        if (st & NAND_ST_OIP) {
            break;
        }
    }
    acc_budget = 2000000UL;
    while (acc_budget--) {
        st = nand_feature(NAND_FEATURE_STATUS);
        if (!(st & NAND_ST_OIP)) {
            return st;
        }
    }
    return 0xFF;
}

/* 0x13 PAGE READ or 0x10 PROGRAM EXECUTE: opcode + 24 bit page address. */
static void nand_cmd_page(uint8_t cmd, uint32_t page)
{
    CS_ASSERT();
    spi_put(cmd);
    spi_put((uint8_t)(page >> 16));
    spi_put((uint8_t)(page >> 8));
    spi_put((uint8_t)page);
    CS_DEASSERT();
}

static uint8_t nand_read_pages(void)
{
    uint8_t st;

    while (acc_total) {
        nand_cmd_page(0x13, acc_addr);
        st = nand_wait_ready();
        if (st == 0xFF || (st & NAND_ST_FAIL)) {
            return ST_ERR;
        }

        CS_ASSERT();
        /* READ FROM CACHE: opcode + 2 byte column + 1 dummy byte.  The
         * first byte clocked after the column is 00h; data follows. */
        if (nand_flags & 0x01) {
            spi_put(0x0B);
        } else {
            spi_put(0x03);
        }
        spi_put(0x00);
        spi_put(0x00);
        spi_put(0xFF); /* dummy */
        st = stream_read((uint16_t)(nand_page_size + nand_oob_size));
        CS_DEASSERT();
        if (st != ST_OK) {
            return st;
        }
        acc_addr++;
        acc_total--;
    }
    return spi_bus_ok ? ST_OK : ST_ERR;
}

static uint8_t nand_program_pages(void)
{
    uint8_t n;

    while (acc_total) {
        CS_ASSERT();
        spi_put(0x06); /* WREN */
        CS_DEASSERT();

        CS_ASSERT();
        spi_put(0x02); /* PROGRAM LOAD, column 0 */
        spi_put(0x00);
        spi_put(0x00);

        acc_left = (uint16_t)(nand_page_size + nand_oob_size);
        while (acc_left) {
            if (!out_fill()) {
                CS_DEASSERT();
                out_drop();
                return ST_TIMEOUT;
            }
            n = pend_len;
            if (acc_left < (uint32_t)n) {
                n = (uint8_t)acc_left;
            }
            spi_write(stream_frame + pend_off, n);
            out_consume(n);
            acc_left -= n;
        }
        CS_DEASSERT();

        nand_cmd_page(0x10, acc_addr);
        if (nand_wait_ready() & NAND_ST_FAIL) {
            return ST_ERR;
        }
        acc_addr++;
        acc_total--;
        acc_done++;
    }
    return spi_bus_ok ? ST_OK : ST_ERR;
}

/*---------------------------------------------------------------*
 * Accelerator entry points                                      *
 *---------------------------------------------------------------*/

static uint8_t cmd_nor_program(uint8_t __xdata *p, uint8_t len)
{
    acc_done = 0;
    if (len < 6) {
        return ST_PARAM;
    }
    if (cs_held) {
        return ST_BUSY;
    }
    acc_addr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    acc_total = (uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16);
    if (acc_total == 0) {
        return ST_PARAM;
    }
    out_drop();
    return nor_program();
}

static uint8_t cmd_nor_erase(uint8_t __xdata *p, uint8_t len)
{
    acc_done = 0;
    if (len < 6) {
        return ST_PARAM;
    }
    if (cs_held) {
        return ST_BUSY;
    }
    acc_addr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    acc_total = (uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16);
    if (acc_total == 0 || (acc_addr & 0xFFFUL) || (acc_total & 0xFFFUL)) {
        return ST_PARAM;
    }
    return nor_erase();
}

static uint8_t cmd_nand_read(uint8_t __xdata *p, uint8_t len)
{
    if (len < 5) {
        return ST_PARAM;
    }
    if (cs_held) {
        return ST_BUSY;
    }
    acc_addr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    acc_total = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
    if (acc_total == 0) {
        return ST_PARAM;
    }
    usb_in_arm(0);
    return nand_read_pages();
}

static uint8_t cmd_nand_program(uint8_t __xdata *p, uint8_t len)
{
    acc_done = 0;
    if (len < 5) {
        return ST_PARAM;
    }
    if (cs_held) {
        return ST_BUSY;
    }
    acc_addr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    acc_total = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
    if (acc_total == 0) {
        return ST_PARAM;
    }
    out_drop();
    return nand_program_pages();
}

static void cmd_nand_set_cfg(uint8_t __xdata *p, uint8_t len)
{
    uint16_t pg, oob;

    if (len < 5) {
        reply_one(ST_PARAM);
        return;
    }
    pg = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    oob = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
    if (pg < 256 || pg > 4096 || oob > 512) {
        reply_one(ST_PARAM);
        return;
    }
    nand_page_size = pg;
    nand_oob_size = oob;
    nand_flags = p[4];
    reply_one(ST_OK);
}

/*---------------------------------------------------------------*
 * Control commands                                             *
 *---------------------------------------------------------------*/

static void cmd_ping(void)
{
    reply_begin(PING_REPLY_LEN, ST_OK);
    usb_reply_buf[1] = PING_MAGIC0;
    usb_reply_buf[2] = PING_MAGIC1;
    usb_reply_buf[3] = PING_MAGIC2;
    usb_reply_buf[4] = PROTO_VERSION;
    reply_commit();
}

static void cmd_get_info(void)
{
    reply_begin(INFO_LEN, ST_OK);
    usb_reply_buf[1] = 0x86; /* idVendor  low  (WCH) */
    usb_reply_buf[2] = 0x1A; /* idVendor  high       */
    usb_reply_buf[3] = 0x07; /* idProduct low        */
    usb_reply_buf[4] = 0x55; /* idProduct high       */
    usb_reply_buf[5] = FW_VER_MAJOR;
    usb_reply_buf[6] = FW_VER_MINOR;
    usb_reply_buf[7] = spi_mode3;
    usb_reply_buf[8] = spi_div;
    usb_reply_buf[9] = 0;
    reply_commit();
}

static void cmd_get_caps(void)
{
    uint16_t caps;
    uint8_t i;

    caps = CAP_STREAM_MODE | CAP_CS_CONTROL | CAP_DUAL_BUFFER |
           CAP_XFER | CAP_SPI_CFG | CAP_GPIO |
           CAP_NOR_ACCEL | CAP_NAND_ACCEL | CAP_ERASE_ACCEL;

    reply_begin(CAPS_LEN, ST_OK);
    usb_reply_buf[1] = (uint8_t)(caps & 0xFF);
    usb_reply_buf[2] = (uint8_t)(caps >> 8);
    usb_reply_buf[3] = MAX_PACKET_SIZE;
    usb_reply_buf[4] = MAX_PREFIX;
    usb_reply_buf[5] = PROTO_VERSION;
    for (i = 6; i < CAPS_LEN; i++) {
        usb_reply_buf[i] = 0;
    }
    reply_commit();
}

static void cmd_set_spi_cfg(uint8_t __xdata *p, uint8_t n)
{
    if (n < 2) {
        reply_one(ST_PARAM);
        return;
    }
    spi_div = p[0];
    if (spi_div < 2) {
        spi_div = 2; /* Fsys/1 is not a legal master divider */
    }
    spi_mode3 = p[1] ? 1 : 0;
    spi_init(spi_div, spi_mode3);
    reply_one(ST_OK);
}

static void cmd_get_spi_cfg(void)
{
    reply_begin(3, ST_OK);
    usb_reply_buf[1] = spi_div;
    usb_reply_buf[2] = spi_mode3;
    reply_commit();
}

static void cmd_cs(uint8_t level)
{
    if (level) {
        CS_DEASSERT();
        cs_held = 0;
    } else {
        spi_flush();
        CS_ASSERT();
        cs_held = 1;
    }
    /* No reply: the host pipelines these around data transfers. */
}

/*---------------------------------------------------------------*
 * Buffered SPI commands (payload carried in the header frame)   *
 *---------------------------------------------------------------*/

static void cmd_spi_write(uint8_t __xdata *p, uint8_t n)
{
    /* Documented as a silent command: never reply, so a bad length can
     * never desynchronise the reply stream. */
    if (n) {
        spi_write(p, n);
    }
}

static void cmd_spi_xfer(uint8_t __xdata *p, uint8_t n)
{
    uint8_t i;

    if (n == 0) {
        reply_one(ST_LEN);
        return;
    }
    for (i = 0; i < n; i++) {
        usb_reply_buf[1 + i] = p[i];
    }
    spi_xfer_buf(usb_reply_buf + 1, usb_reply_buf + 1, n);
    reply_begin((uint8_t)(n + 1), spi_bus_ok ? ST_OK : ST_ERR);
    reply_commit();
}

static void cmd_spi_read(uint8_t n)
{
    if (n == 0) {
        reply_one(ST_LEN);
        return;
    }
    spi_read_fill(usb_reply_buf + 1, n);
    reply_begin((uint8_t)(n + 1), spi_bus_ok ? ST_OK : ST_ERR);
    reply_commit();
}

/*---------------------------------------------------------------*
 * GPIO / board control                                          *
 *---------------------------------------------------------------*/

static void cmd_gpio_write(uint8_t __xdata *p, uint8_t n)
{
    if (n < 2 || board_gpio_write(p[0], p[1])) {
        reply_one(ST_PARAM);
        return;
    }
    reply_one(ST_OK);
}

static void cmd_gpio_read(uint8_t __xdata *p, uint8_t n)
{
    uint8_t v;

    if (n < 1) {
        reply_one(ST_PARAM);
        return;
    }
    v = board_gpio_read(p[0]);
    if (v == 0xFF) {
        reply_one(ST_PARAM);
        return;
    }
    reply_begin(2, ST_OK);
    usb_reply_buf[1] = v;
    reply_commit();
}

/*---------------------------------------------------------------*
 * Dispatcher                                                   *
 *---------------------------------------------------------------*/

void proto_handle(uint8_t __xdata *frame, uint8_t n)
{
    uint8_t cmd, len, status;
    uint8_t __xdata *p;

    if (n < 2) {
        reply_one(ST_LEN);
        return;
    }

    cmd = frame[0];
    len = frame[1];
    p = frame + 2;
    spi_bus_ok = 1;

    if (len > (uint8_t)(n - 2)) {
        reply_one(ST_LEN);
        return;
    }

    switch (cmd) {
    case CMD_PING:
        cmd_ping();
        break;

    case CMD_GET_INFO:
        cmd_get_info();
        break;

    case CMD_GET_CAPS:
        cmd_get_caps();
        break;

    case CMD_SET_SPI_CFG:
        cmd_set_spi_cfg(p, len);
        break;

    case CMD_GET_SPI_CFG:
        cmd_get_spi_cfg();
        break;

    case CMD_CS:
        /* Documented as silent (it sits in the page-program hot path):
         * never reply, even for a malformed frame. */
        if (len >= 1) {
            cmd_cs(p[0]);
        }
        break;

    case CMD_SPI_WRITE:
        cmd_spi_write(p, len);
        break;

    case CMD_SPI_XFER:
        cmd_spi_xfer(p, len);
        break;

    case CMD_SPI_READ:
        if (len < 1) {
            reply_one(ST_PARAM);
        } else {
            cmd_spi_read(p[0]);
        }
        break;

    case CMD_STREAM_WRITE:
        status = run_stream(p, len, 0);
        reply_one(status);
        break;

    case CMD_STREAM_READ:
        status = run_stream(p, len, 1);
        reply_one(status);
        break;

    case CMD_STREAM_XFER:
        status = run_stream(p, len, 2);
        reply_one(status);
        break;

    case CMD_NOR_PROGRAM:
        reply_done(cmd_nor_program(p, len));
        break;

    case CMD_NOR_ERASE:
        reply_done(cmd_nor_erase(p, len));
        break;

    case CMD_NAND_SET_CFG:
        cmd_nand_set_cfg(p, len);
        break;

    case CMD_NAND_READ_PAGES:
        reply_one(cmd_nand_read(p, len));
        break;

    case CMD_NAND_PROGRAM_PAGES:
        reply_done(cmd_nand_program(p, len));
        break;

    case CMD_GPIO_WRITE:
        cmd_gpio_write(p, len);
        break;

    case CMD_GPIO_READ:
        cmd_gpio_read(p, len);
        break;

    case CMD_BOOTLOADER:
        /* Drop the USB port and hand control to the WCH bootloader. */
        proto_reset();
        EA = 0;
        USB_INT_EN = 0;
        USB_CTRL = bUC_RESET_SIE;
        mDelaymS(20);
#if defined(BOOTLOADER_ENTRY)
        ((void (*)(void))BOOTLOADER_ENTRY)();
#endif
        while (1)
            ;
        break;

    default:
        reply_one(ST_UNKNOWN);
        break;
    }
}

void proto_reset(void)
{
    cs_held = 0;
    /* Deliberately leave the SPI pins alone: a USB bus reset must not
     * drop CS on a flash that may be in the middle of a program cycle. */
}
