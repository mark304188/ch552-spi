/*==========================================================================*
 * spi.c - CH552 hardware SPI0 master driver
 *
 * Performance design
 * ------------------
 * SPI0 has a 1 byte transmit FIFO and a 1 byte receive FIFO that are
 * physically separate, and writing SPI0_DATA is what starts a transfer.
 * That allows the classic pipelined byte loop:
 *
 *     SPI0_DATA = b;          ; queue the byte
 *     while (!S0_R_FIFO) ;    ; wait for the *previous* byte to land
 *     r = SPI0_DATA;          ; drain it, freeing the FIFO
 *
 * Once the first byte is in flight the CPU can queue the next one before
 * the current byte has finished shifting, so the SCK gap between bytes is
 * only the handful of cycles the loop needs.  At Fsys = 24MHz and
 * SPI_CK_DIV2 (12MHz SCK) the loop is roughly matched to the 16 Fsys
 * cycles a byte takes on the wire, i.e. about 1.4 MB/s of SPI payload.
 *
 * Two rules keep the loop correct:
 *   - the rx FIFO is drained before the next write, otherwise a byte is
 *     lost (S0_IF_OV) and the stream becomes misaligned;
 *   - only one byte is ever written ahead, because the tx FIFO is one
 *     byte deep.
 *
 * The pointer types are explicitly __xdata so SDCC emits direct MOVX
 * accesses.  Generic (3 byte) pointers would call __gptrget/__gptrput for
 * every byte and multiply the per-byte cost by roughly four.
 *==========================================================================*/

#include <ch552.h>
#include <stdint.h>

#include "spi.h"
#include "board.h"

/* Pins owned by SPI0: P1.7 SCK, P1.6 MISO, P1.5 MOSI, plus the software
 * chip select from board.h. */
#define SPI_PIN_MASK (bSCK | bMISO | bMOSI | BOARD_CS_MASK)

/* Base SPI control word for 3-wire master mode with the outputs enabled.
 * bS0_MISO_OE stays 0 because MISO is an input in master mode. */
#define SPI_CTRL_BASE (bS0_SCK_OE | bS0_MOSI_OE)

void spi_init(uint8_t ck_se, uint8_t mode3)
{
    /* Flush the FIFOs and clear the slave-only flag bits. */
    SPI0_CTRL = bS0_CLR_ALL;
    SPI0_STAT = 0xFF; /* write 1 clears the RW flag bits */

    SPI0_SETUP = 0; /* master mode, MSB first, interrupts off */
    SPI0_CK_SE = ck_se;

    /* Mode 0 (idle low) or mode 3 (idle high). */
    if (mode3) {
        SPI0_CTRL = SPI_CTRL_BASE | bS0_MST_CLK;
    } else {
        SPI0_CTRL = SPI_CTRL_BASE;
    }

    /* Push-pull outputs for SCK/MOSI/CS, floating input for MISO (the
     * datasheet asks for MISO without a pull-up). */
    P1_MOD_OC &= ~(uint8_t)SPI_PIN_MASK;
    P1_DIR_PU |= (uint8_t)SPI_PIN_MASK;
    P1_DIR_PU &= ~(uint8_t)bMISO;

    CS_DEASSERT();
}

void spi_flush(void)
{
    SPI0_CTRL |= bS0_CLR_ALL;
    SPI0_CTRL &= ~(uint8_t)bS0_CLR_ALL;
}

uint8_t spi_xfer(uint8_t dat)
{
    SPI0_DATA = dat;
    while (S0_R_FIFO == 0)
        ;
    return SPI0_DATA;
}

void spi_write(const uint8_t __xdata *buf, uint8_t len)
{
    uint8_t n = len;

    if (n == 0) {
        return;
    }
    do {
        SPI0_DATA = *buf++;
        while (S0_R_FIFO == 0)
            ;
        /* Drain the rx FIFO; the received byte is not interesting here. */
        SPI0_DATA;
    } while (--n);
}

void spi_xfer_buf(uint8_t __xdata *buf, uint8_t __xdata *rbuf, uint8_t len)
{
    uint8_t n = len;

    if (n == 0) {
        return;
    }

    if (buf == rbuf) {
        /* In-place exchange: DPTR can stay put for both the read and the
         * write, which is the shortest possible loop body. */
        do {
            SPI0_DATA = *buf;
            while (S0_R_FIFO == 0)
                ;
            *buf++ = SPI0_DATA;
        } while (--n);
        return;
    }

    do {
        /* Queue the byte. The write is accepted while the previous byte
         * is still shifting, which keeps the inter-byte gap minimal. */
        SPI0_DATA = *buf++;

        /* Wait until the previous byte has landed in the rx FIFO. */
        while (S0_R_FIFO == 0)
            ;

        *rbuf++ = SPI0_DATA;
    } while (--n);
}

void spi_read_fill(uint8_t __xdata *dst, uint8_t len)
{
    uint8_t n = len;

    if (n == 0) {
        return;
    }
    do {
        SPI0_DATA = 0xFF; /* dummy byte that generates the SCK burst */
        while (S0_R_FIFO == 0)
            ;
        *dst++ = SPI0_DATA;
    } while (--n);
}
