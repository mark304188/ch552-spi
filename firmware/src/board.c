/*==========================================================================*
 * board.c - board specific pin handling
 *
 * All pin numbers come from board.h; this file only contains the mechanics
 * of driving them.
 *==========================================================================*/

#include <ch552.h>
#include <stdint.h>

#include "board.h"

/* Map a wire level to the CPU port bit.  The auxiliary pins are driven
 * open-drain, so writing 0 pulls the line low and writing 1 releases it
 * (the target's pull-up brings it high). */
static void gpio_drive(uint8_t bit, uint8_t level)
{
    uint8_t m = (uint8_t)(1u << bit);

    P1_MOD_OC |= m;  /* open drain                            */
    P1_DIR_PU &= ~m; /* no internal pull-up                   */
    if (level) {
        P1 |= m; /* release: transistor off                */
    } else {
        P1 &= ~m; /* assert: pull the line low             */
    }
}

static uint8_t gpio_bit(uint8_t index)
{
    switch (index) {
    case 0:
        return BOARD_GPIO0_BIT;
    case 1:
        return BOARD_GPIO1_BIT;
    case 2:
        return BOARD_GPIO2_BIT;
    case 3:
        return BOARD_GPIO3_BIT;
    default:
        return BOARD_GPIO_NONE;
    }
}

void board_init(void)
{
    uint8_t i;

    /* Chip select is configured by spi_init() (it owns the SPI pins and
     * re-runs on CMD_SET_SPI_CFG), but release it here as well so the
     * flash never sees a spurious select during power-up. */
    CS_DEASSERT();

    /* Release every auxiliary pin. */
    for (i = 0; i < 4; i++) {
        board_gpio_write(i, 1);
    }
}

uint8_t board_gpio_write(uint8_t index, uint8_t level)
{
    uint8_t bit = gpio_bit(index);

    if (bit == BOARD_GPIO_NONE) {
        return 1;
    }
    gpio_drive(bit, level ? 1 : 0);
    return 0;
}

uint8_t board_gpio_read(uint8_t index)
{
    uint8_t bit = gpio_bit(index);

    if (bit == BOARD_GPIO_NONE) {
        return 0xFF;
    }
    return (P1 & (uint8_t)(1u << bit)) ? 1 : 0;
}
