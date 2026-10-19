/*==========================================================================*
 * spi.h - CH552 hardware SPI0 master driver
 *
 * CH552 SPI0 peripheral notes (datasheet V1.x chapter 14):
 *   - 1 byte tx FIFO + 1 byte rx FIFO, physically separated registers
 *   - a write to SPI0_DATA immediately starts a transfer (bS0_DATA_DIR=0)
 *   - S0_FREE==1 means the shift engine is idle (byte boundary gap)
 *   - S0_R_FIFO tells whether the rx FIFO holds a byte that must be drained
 *   - clock = Fsys / SPI0_CK_SE, up to Fsys/2
 *   - CH552 always samples MISO on the rising edge of SCK, only mode 0 and
 *     mode 3 (idle low/high) are available
 *
 * Design goals: minimum cycle count per byte, zero memcpy for bulk
 * transfers, and no per-byte function call overhead.
 *==========================================================================*/

#ifndef __SPI_H__
#define __SPI_H__

#include <stdint.h>

/* SPI clock divider values for SPI0_CK_SE.
 * CH554/CH552 code this value directly as the division factor, so
 * SPI_CK_DIV2 gives the highest possible rate (Fsys/2). */
#define SPI_CK_DIV2 0x02
#define SPI_CK_DIV4 0x04
#define SPI_CK_DIV8 0x08
#define SPI_CK_DIV16 0x10
#define SPI_CK_DIV32 0x20 /* reset value */

/* Chip select macros (CS_ASSERT / CS_DEASSERT) and the pin number are
 * defined in board.h, so wiring the bridge to a different board is a
 * one-line change there.  CS is driven from software so that a single
 * flash sequence can keep it asserted across several commands. */
#include "board.h"

void spi_init(uint8_t ck_se, uint8_t mode3);

/* Discard anything left in the SPI FIFOs.  Called when a transaction is
 * started so an aborted transfer can never misalign the next one. */
void spi_flush(void);

/* Fast single byte exchange: write then drain. Used for short command
 * sequences where the scheduling cost of a loop is larger. */
uint8_t spi_xfer(uint8_t dat);

/* Transmit len bytes from buf. The rx FIFO is drained inline so it can
 * never overflow. */
void spi_write(const uint8_t __xdata *buf, uint8_t len);

/* Exchange len bytes between buf and rbuf. Both may point at the same
 * buffer for in-place operation, which is the common case for bulk
 * programming: the host supplies 0xFF dummy bytes and reads the flash
 * output back into the same buffer. */
void spi_xfer_buf(uint8_t __xdata *buf, uint8_t __xdata *rbuf, uint8_t len);

/* Clock `len` bytes out of MOSI as 0xFF while capturing MISO into dst.
 * This is the flash-read hot path: it avoids reading the source buffer
 * entirely, so only one xdata pointer is live per iteration. */
void spi_read_fill(uint8_t __xdata *dst, uint8_t len);

#endif /* __SPI_H__ */
