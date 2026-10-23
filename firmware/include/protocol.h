/*==========================================================================*
 * protocol.h - USB <-> SPI wire protocol for the CH552 SPI bridge
 *
 * Transport
 * ---------
 * One vendor-specific interface; no driver is needed on Linux (usbfs /
 * libusb).  Two bulk endpoints, both double buffered inside the CH552:
 *
 *   OUT 0x03   64 byte frames, host -> device
 *   IN  0x82   64 byte frames, device -> host
 *
 * Request frame (exactly one USB frame)
 * -------------------------------------
 *   +-----+-----+-----------------------------+
 *   | cmd | len | payload[len]                |
 *   +-----+-----+-----------------------------+
 *
 *   len + 2 <= 64.  For the streaming commands the payload is a small
 *   descriptor: a 24-bit byte count plus up to 8 "prefix" bytes that the
 *   device clocks out before the data phase.  The data itself travels in
 *   the following raw USB frames.
 *
 * Reply frame (one or more USB frames)
 * ------------------------------------
 *   +--------+---------------------------+
 *   | status | data[...]                 |
 *   +--------+---------------------------+
 *
 * Data commands that cannot fail (write, CS control, configuration)
 * produce no reply at all: the transfer is complete once the host's next
 * OUT transfer is accepted.  That keeps a flash page-program loop at one
 * USB transaction per data frame instead of two.
 *==========================================================================*/

#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>

/*---------------------------------------------------------------*
 * Status codes (first byte of every reply frame)                *
 *---------------------------------------------------------------*/
#define ST_OK 0x00      /* command completed                        */
#define ST_ERR 0x01     /* generic error                            */
#define ST_BUSY 0x02    /* peripheral busy                          */
#define ST_PARAM 0x03   /* bad parameter or unsupported value       */
#define ST_LEN 0x04     /* frame length does not match the header   */
#define ST_TIMEOUT 0x05 /* host stopped producing/consuming data    */
#define ST_UNKNOWN 0xFF /* unknown command                          */

/*---------------------------------------------------------------*
 * Commands                                                      *
 *---------------------------------------------------------------*/

/* Identification and configuration. */
#define CMD_PING 0x01        /* -> PING_REPLY_LEN bytes            */
#define CMD_GET_INFO 0x02    /* -> INFO_LEN bytes                  */
#define CMD_GET_CAPS 0x03    /* -> CAPS_LEN bytes                  */
#define CMD_SET_SPI_CFG 0x04 /* payload [div][mode3] -> 1 byte      */
#define CMD_GET_SPI_CFG 0x05 /* -> 3 bytes                         */

/* Chip-select control.  CS is software driven so a multi-command flash
 * sequence can keep it asserted for its whole duration. */
#define CMD_CS 0x08 /* payload [level]  0 = assert, 1 = release */

/* Small buffered transfers, payload carried in the same USB frame. */
#define CMD_SPI_WRITE 0x10 /* payload [data...]  no reply         */
#define CMD_SPI_XFER 0x11  /* payload [data...] -> len+1 bytes    */
#define CMD_SPI_READ 0x12  /* payload [n]       -> n+1 bytes      */

/* Streaming transfers.
 *
 *   payload = [len0][len1][len2][pre_len][pre0..pre7]
 *
 *   len     number of data bytes moved in the data phase (24 bit)
 *   pre_len number of prefix bytes clocked out first (0..8)
 *   pre     e.g. opcode + 24-bit address, or opcode + column address
 *
 * The device asserts CS (unless it is already asserted by CMD_CS),
 * clocks the prefix out, then moves `len` bytes:
 *
 *   CMD_STREAM_WRITE  host -> device data, e.g. page program
 *   CMD_STREAM_READ   device -> host data, e.g. page read
 *   CMD_STREAM_XFER   full duplex, e.g. read with dummy write phase
 *
 * A single status byte is returned when the transfer completes. */
#define CMD_STREAM_WRITE 0x20
#define CMD_STREAM_READ 0x21
#define CMD_STREAM_XFER 0x22

/* Flash accelerators: the device runs the per-page loop itself, so the
 * host streams a whole block with one command instead of 3..5 USB round
 * trips per page.  All of them drive CS themselves and must not be mixed
 * with CMD_CS (they reply ST_BUSY if CS is held).
 *
 *   CMD_NOR_PROGRAM      [addr24][len24] + data
 *                        -> [status][done24]   bytes programmed
 *   CMD_NOR_ERASE        [addr24][len24]       4 KiB aligned
 *                        -> [status][done24]   sectors erased
 *   CMD_NAND_SET_CFG     [page16][oob16][flags] -> 1 byte
 *                        flags bit0: READ FROM CACHE uses 0x0B (1 dummy)
 *   CMD_NAND_READ_PAGES  [page24][npages16]
 *                        -> npages*(page+oob) bytes + 1 byte status
 *   CMD_NAND_PROGRAM_PAGES [page24][npages16] + data
 *                        -> [status][done24]   pages programmed
 *
 * The NAND commands transfer the OOB area verbatim, so both on-die ECC
 * and host-computed ECC work: enable on-die ECC with SET FEATURE
 * (0x1F, 0xB0) through CMD_STREAM_WRITE, or fill the OOB yourself. */
#define CMD_NOR_PROGRAM 0x23
#define CMD_NOR_ERASE 0x24
#define CMD_NAND_SET_CFG 0x25
#define CMD_NAND_READ_PAGES 0x26
#define CMD_NAND_PROGRAM_PAGES 0x27

/* Board control. */
#define CMD_GPIO_WRITE 0x30 /* payload [pin][value]  (no reply)    */
#define CMD_GPIO_READ 0x31  /* payload [pin] -> 2 bytes            */

/*---------------------------------------------------------------*
 * Reply payload layouts                                         *
 *---------------------------------------------------------------*/
#define PING_REPLY_LEN 5 /* status, 'C', 'S', 'P', protocol version */
#define INFO_LEN 10      /* status, vid lo/hi, pid lo/hi, fw major,
                          * fw minor, mode3, divider, reserved         */
#define CAPS_LEN 16      /* status, caps lo/hi, max packet size, ...   */

/* Capability bits (little-endian 16-bit field at reply offset 1). */
#define CAP_STREAM_MODE 0x0001 /* CMD_STREAM_*                    */
#define CAP_CS_CONTROL 0x0002  /* software chip select            */
#define CAP_DUAL_BUFFER 0x0004 /* EP2/EP3 double buffering        */
#define CAP_XFER 0x0008        /* CMD_SPI_XFER / CMD_SPI_READ     */
#define CAP_SPI_CFG 0x0010     /* runtime divider / mode changes  */
#define CAP_GPIO 0x0020        /* CMD_GPIO_*                      */
#define CAP_NOR_ACCEL 0x0040   /* CMD_NOR_PROGRAM / CMD_NOR_ERASE */
#define CAP_NAND_ACCEL 0x0080  /* CMD_NAND_READ / PROGRAM_PAGES   */
#define CAP_ERASE_ACCEL 0x0100 /* CMD_NOR_ERASE                   */

/* Protocol version.  Bumped whenever the frame layout changes. */
#define PROTO_VERSION 0x01

/* Firmware version, reported by CMD_GET_INFO and the EP0 status query. */
#define FW_VER_MAJOR 1
#define FW_VER_MINOR 3

/* Magic bytes returned by CMD_PING: ASCII "CSP". */
#define PING_MAGIC0 'C'
#define PING_MAGIC1 'S'
#define PING_MAGIC2 'P'

/* Maximum number of prefix bytes a stream command may carry. */
#define MAX_PREFIX 8

/* GPIO pin indices used by CMD_GPIO_*.
 *
 * The index maps to a physical pin through the BOARD_GPIO* table in
 * board.h.  value 0 = assert (drive low), 1 = inactive (drive high, or
 * release on open-drain slots).  With the default board mapping: */
#define PIN_WP 0   /* P3.2 -> flash /WP   0 = write protected */
#define PIN_HOLD 1 /* P3.4 -> flash /HOLD 0 = hold (chip idle) */

/*---------------------------------------------------------------*
 * Entry points                                                  *
 *---------------------------------------------------------------*/

/* Handle one command frame in place.  Runs in main-loop context and may
 * block for the duration of a streaming transfer. */
void proto_handle(uint8_t __xdata *frame, uint8_t n);

/* Reset protocol state (USB bus reset). */
void proto_reset(void);

#endif /* __PROTOCOL_H__ */
