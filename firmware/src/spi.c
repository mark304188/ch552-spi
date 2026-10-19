/*==========================================================================*
 * spi.c - CH552 hardware SPI0 master driver
 *
 * Completion detection
 * --------------------
 * The CH552 SPI0 status register exposes two candidate "byte done" bits:
 *
 *   S0_FREE    no shift in progress (documented as the master-mode way to
 *              wait for a byte, and what the vendor SDK uses)
 *   S0_R_FIFO  receive FIFO count
 *
 * S0_R_FIFO turned out to be unreliable in master mode on real hardware:
 * polling it hangs forever, because the FIFO counters are only maintained
 * for slave mode.  The loops below therefore wait on S0_FREE, exactly like
 * CH554SPIMasterWrite/Read in the WCH SDK.
 *
 * Dead-bus protection
 * -------------------
 * A failed SPI transfer must never wedge the bridge, so the first byte of
 * every transfer is waited for with a bounded spin.  If it times out the
 * function gives up and sets spi_bus_ok = 0, which the protocol layer turns
 * into an error reply.  The remaining bytes use the tight unbounded loop,
 * because a bus that clocked the first byte will clock the rest.
 *
 * Cycle budget: one byte at SCK = Fsys/4 is 32 Fsys cycles, and the tight
 * loop costs about 15, so the SPI clock, not the CPU, is the limit.
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

/* Cleared when a transfer had to be abandoned (dead bus). */
uint8_t spi_bus_ok;

/* Wait for the shift engine to go idle.  Returns 0 on timeout. */
static uint8_t spi_wait_idle(void)
{
    uint16_t spins = 2000;

    while (S0_FREE == 0) {
        if (--spins == 0) {
            return 0;
        }
    }
    return 1;
}

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
    spi_bus_ok = 1;
}

void spi_flush(void)
{
    SPI0_CTRL |= bS0_CLR_ALL;
    SPI0_CTRL &= ~(uint8_t)bS0_CLR_ALL;
}

/* Send one byte, discarding the reply, with the dead-bus guard applied to
 * every byte (used for the short command/address prefixes). */
uint8_t spi_put(uint8_t dat)
{
    SPI0_DATA = dat;
    if (!spi_wait_idle()) {
        spi_bus_ok = 0;
        return 0;
    }
    SPI0_DATA; /* drain */
    return 1;
}

uint8_t spi_xfer(uint8_t dat)
{
    SPI0_DATA = dat;
    if (!spi_wait_idle()) {
        spi_bus_ok = 0;
        return 0xFF;
    }
    return SPI0_DATA;
}

void spi_write(const uint8_t __xdata *buf, uint8_t len)
{
    uint8_t n = len;

    if (n == 0) {
        return;
    }

    SPI0_DATA = *buf++;
    if (!spi_wait_idle()) {
        spi_bus_ok = 0; /* bus never clocked: give up */
        return;
    }
    SPI0_DATA; /* drain */

    while (--n) {
        SPI0_DATA = *buf++;
        while (S0_FREE == 0)
            ;
        SPI0_DATA;
    }
}

void spi_xfer_buf(uint8_t __xdata *buf, uint8_t __xdata *rbuf, uint8_t len)
{
    uint8_t n = len;

    if (n == 0) {
        return;
    }

    SPI0_DATA = *buf++;
    if (!spi_wait_idle()) {
        spi_bus_ok = 0;
        return;
    }
    *rbuf++ = SPI0_DATA;

    while (--n) {
        SPI0_DATA = *buf++;
        while (S0_FREE == 0)
            ;
        *rbuf++ = SPI0_DATA;
    }
}

void spi_read_fill(uint8_t __xdata *dst, uint8_t len)
{
    uint8_t n = len;

    if (n == 0) {
        return;
    }

    SPI0_DATA = 0xFF;
    if (!spi_wait_idle()) {
        spi_bus_ok = 0;
        return;
    }
    *dst++ = SPI0_DATA;

    while (--n) {
        SPI0_DATA = 0xFF;
        while (S0_FREE == 0)
            ;
        *dst++ = SPI0_DATA;
    }
}
