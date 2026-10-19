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

#define FW_VER_MAJOR 1
#define FW_VER_MINOR 0

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
           CAP_XFER | CAP_SPI_CFG | CAP_GPIO;

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
