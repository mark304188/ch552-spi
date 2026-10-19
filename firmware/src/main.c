/*==========================================================================*
 * main.c - CH552 USB-SPI bridge entry point
 *
 * Target : CH552 (24 MHz MCS-51 core + USB full-speed device)
 * Build  : SDCC 4.x, see ../Makefile
 *
 * The board acts as a USB-to-SPI bridge aimed at programming SPI NOR and
 * SPI NAND flash.  Everything is optimised for sustained bulk throughput:
 *
 *   - Fsys = 24 MHz, so SPI0 can run at 12 MHz (Fsys/2) and the USB
 *     device clock requirement (Fsys >= 6 MHz) is met with margin.
 *   - Both bulk endpoints are double buffered in xRAM.
 *   - The SPI byte loops are tight enough that the 8051 keeps the SPI
 *     shift engine busy; the USB link, not the CPU, is the bottleneck.
 *   - Flash-paced commands (page program, page read) stream straight
 *     between the USB endpoint buffers and SPI0 with no extra copy.
 *
 * Wiring (defaults, all 3.3 V logic; see firmware/include/board.h to
 * change the chip select or the auxiliary pins):
 *
 *     CH552        SPI flash (NOR or NAND)
 *     ------------------------------------
 *     P1.7  SCK -> SCK
 *     P1.6  MISO<- SO / IO1
 *     P1.5  MOSI-> SI / IO0
 *     P1.4  CS# -> CS#
 *     P3.2  /WP -> /WP       (driven high at boot, CMD_GPIO_WRITE index 0)
 *     P3.4 /HOLD -> /HOLD    (driven high at boot, CMD_GPIO_WRITE index 1)
 *     3V3       -> VCC
 *     GND       -> GND
 *
 * P1.7/P1.6/P1.5 are the SPI0 peripheral's fixed pins.  P1.4 is the SPI0
 * SCS pin, which is a normal GPIO in master mode and is therefore used as
 * a software-driven chip select: that is what lets one command keep CS
 * asserted across a whole flash sequence.
 *
 * P3.2/P3.4 default to the common CH552 programmer layout (/WP and /HOLD).
 * They are push-pull outputs driven high at boot, so a flash is writable
 * even on boards without external pull-ups.
 *==========================================================================*/

#include <ch552.h>
#include <stdint.h>

#include "board.h"
#include "debug.h"
#include "spi.h"
#include "usb.h"
#include "protocol.h"

/* The USB interrupt service routine lives in usb.c.  SDCC emits the
 * 8051 interrupt vector table in the module that defines main(), and it
 * only fills in vectors for ISRs *declared* there, so this prototype (and
 * only this module) must declare it.  Declaring it in usb.h would make
 * every including module emit a second vector table. */
void usb_isr(void) __interrupt(INT_NO_USB) __using(1);

/* Diagnostics shared with the EP0 status request (see usb.c). */
extern volatile __idata uint16_t dbg_tick;
extern __idata uint8_t dbg_phase;
extern volatile __idata uint8_t dbg_cmd;

void main(void)
{
    /* 24 MHz system clock from the internal oscillator / PLL. */
    CfgFsys();
    mDelaymS(5); /* let the internal oscillator settle after the change */

    /* Idle every board-owned pin before USB enumerates, so a flash that is
     * powered together with the bridge never sees a spurious select or
     * reset pulse. */
    board_init();
    spi_init(SPI_CK_DIV4, 0);

    usb_init();

    while (1) {
        if (ep3_frame_ready) {
            uint8_t __xdata *frame = usb_out_frame();
            uint8_t n = ep3_frame_len;

            /* Hand the frame to the protocol layer.  The endpoint is
             * re-armed by usb_service() (or by the streaming command
             * itself); because the OUT buffer is double buffered, the
             * next frame lands in the other half. */
            ep3_frame_ready = 0;
            ep3_had_frame = 1;
            dbg_cmd = frame[0];
            dbg_phase = 1;
            proto_handle(frame, n);
            dbg_phase = 2;
        }

        usb_service();
        dbg_tick++;
        dbg_phase = 0;
    }
}
