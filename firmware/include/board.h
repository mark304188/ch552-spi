/*==========================================================================*
 * board.h - board specific pin assignment
 *
 * The SPI0 data/clock pins are **fixed by the chip** and cannot be moved:
 *
 *     P1.7  SCK
 *     P1.6  MISO
 *     P1.5  MOSI
 *
 * Everything else is a plain GPIO, so this file is the single place to
 * change when adapting the firmware to a different board:
 *
 *     BOARD_CS_BIT      chip select (default P1.4, the SPI0 SCS pin,
 *                       which is a normal GPIO in master mode)
 *     BOARD_GPIO*_BIT   auxiliary pins exposed through CMD_GPIO_*
 *
 * Chip select may be any free P1 pin: P1.0 .. P1.4.  P1.5/6/7 are taken by
 * the SPI peripheral.  All auxiliary GPIOs are driven open-drain, which is
 * the safe way to drive a target reset line (the target's pull-up defines
 * the inactive level, and there is no contention with another driver).
 *==========================================================================*/

#ifndef __BOARD_H__
#define __BOARD_H__

#include <ch552.h>

/*---------------------------------------------------------------*
 * Chip select                                                  *
 *---------------------------------------------------------------*/

#define BOARD_CS_BIT 4 /* P1.4 */

/* Declare the pin as a bit-addressable symbol so CS toggling is a single
 * CLR/SETB instruction instead of a read-modify-write on the port. */
SBIT(board_cs, 0x90, BOARD_CS_BIT);

#define BOARD_CS_MASK ((uint8_t)(1u << BOARD_CS_BIT))

/* Flash chip select is active low.  Swap the two macros if a board needs
 * an active-high enable instead. */
#define CS_ASSERT() (board_cs = 0)
#define CS_DEASSERT() (board_cs = 1)

/*---------------------------------------------------------------*
 * Auxiliary GPIOs (CMD_GPIO_WRITE / CMD_GPIO_READ)             *
 *                                                              *
 * The index in the command payload selects the entry, so pin 0 is *
 * BOARD_GPIO0_BIT.  Use 0xFF for an unused slot.                *
 *---------------------------------------------------------------*/

#define BOARD_GPIO0_BIT 3    /* P1.3: target /RESET */
#define BOARD_GPIO1_BIT 0xFF /* unused */
#define BOARD_GPIO2_BIT 0xFF /* unused */
#define BOARD_GPIO3_BIT 0xFF /* unused */

#define BOARD_GPIO_NONE 0xFF

/*---------------------------------------------------------------*
 * Entry points                                                 *
 *---------------------------------------------------------------*/

/* Put every board-owned pin into its idle state (CS released, auxiliary
 * pins released).  Called once from main() before USB enumerates. */
void board_init(void);

/* Drive an auxiliary pin: level 0 asserts (pulls low), 1 releases it.
 * `index` is the value used on the wire by CMD_GPIO_*. Returns 0 on
 * success, non-zero if the index does not exist. */
uint8_t board_gpio_write(uint8_t index, uint8_t level);

/* Read back an auxiliary pin.  Returns 0/1 in bit 0, or 0xFF if the index
 * does not exist. */
uint8_t board_gpio_read(uint8_t index);

#endif /* __BOARD_H__ */
