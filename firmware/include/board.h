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
 *     BOARD_GPIO*       auxiliary pins (/WP, /HOLD, ...)
 *
 * Chip select may be any free P1 pin: P1.0 .. P1.4.  P1.5/6/7 are taken by
 * the SPI peripheral.
 *
 * The auxiliary pin table defaults to the wiring used by the common
 * CH552 SPI-NOR/SPI-NAND programmer boards:
 *
 *     index 0 : P3.2 -> flash /WP    (write protect, active low)
 *     index 1 : P3.4 -> flash /HOLD  (hold,          active low)
 *
 * Both are push-pull outputs and are driven HIGH at power-up, so the flash
 * is writable and responsive even on boards without external pull-ups.
 * Change BOARD_GPIO*_MODE to BOARD_GPIO_OD if the target board already
 * pulls those lines up and the programmer must not drive them hard.
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
 * Auxiliary GPIOs (CMD_GPIO_WRITE / CMD_GPIO_READ)              *
 *                                                              *
 * Each slot is described by a port number (1 or 3), a bit number *
 * and a drive mode.  The index in the command payload selects the *
 * slot, so changing the wiring never changes the host protocol.   *
 *---------------------------------------------------------------*/

/* Drive modes. */
#define BOARD_GPIO_PP 0 /* push-pull output, drives both levels       */
#define BOARD_GPIO_OD 1 /* open drain, releasing needs a pull-up      */
#define BOARD_GPIO_IN 2 /* high impedance input                       */

/* Slot 0: flash /WP on P3.2 */
#define BOARD_GPIO0_PORT 3
#define BOARD_GPIO0_BIT 2
#define BOARD_GPIO0_MODE BOARD_GPIO_PP

/* Slot 1: flash /HOLD on P3.4 */
#define BOARD_GPIO1_PORT 3
#define BOARD_GPIO1_BIT 4
#define BOARD_GPIO1_MODE BOARD_GPIO_PP

/* Slots 2 and 3: unused (port 0xFF). */
#define BOARD_GPIO2_PORT 0xFF
#define BOARD_GPIO2_BIT 0
#define BOARD_GPIO2_MODE BOARD_GPIO_IN

#define BOARD_GPIO3_PORT 0xFF
#define BOARD_GPIO3_BIT 0
#define BOARD_GPIO3_MODE BOARD_GPIO_IN

/*---------------------------------------------------------------*
 * Entry points                                                 *
 *---------------------------------------------------------------*/

/* Put every board-owned pin into its idle state: CS released, auxiliary
 * pins driven to their inactive (high) level.  Called once from main()
 * before USB enumerates. */
void board_init(void);

/* Drive an auxiliary pin.  level 0 = assert (low), 1 = inactive (high or
 * released, depending on the slot's drive mode).  Returns 0 on success,
 * non-zero if the index does not exist. */
uint8_t board_gpio_write(uint8_t index, uint8_t level);

/* Read back an auxiliary pin.  Returns 0/1 in bit 0, or 0xFF if the index
 * does not exist. */
uint8_t board_gpio_read(uint8_t index);

#endif /* __BOARD_H__ */
