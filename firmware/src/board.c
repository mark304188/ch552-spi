/*==========================================================================*
 * board.c - board specific pin handling
 *
 * All pin numbers come from board.h; this file only contains the mechanics
 * of driving them.  The two ports that carry auxiliary pins (P1 and P3) are
 * handled with explicit branches because SFRs cannot be addressed through
 * a pointer on the 8051.
 *==========================================================================*/

#include <ch552.h>
#include <stdint.h>

#include "board.h"

/* Apply a (port, bit, mode, level) description.  Returns 0 on success,
 * non-zero if the port is not available. */
static uint8_t gpio_apply(uint8_t port, uint8_t bit, uint8_t mode, uint8_t level)
{
    uint8_t m = (uint8_t)(1u << bit);

    if (port == 1) {
        if (mode == BOARD_GPIO_IN) {
            P1_MOD_OC &= ~m; /* push-pull mode, DIR=0 -> input   */
            P1_DIR_PU &= ~m;
        } else if (mode == BOARD_GPIO_OD) {
            P1_MOD_OC |= m; /* open drain, no pull-up          */
            P1_DIR_PU &= ~m;
            if (level) {
                P1 |= m; /* release                          */
            } else {
                P1 &= ~m; /* pull low                         */
            }
        } else {
            P1_MOD_OC &= ~m; /* push-pull                       */
            P1_DIR_PU |= m;
            if (level) {
                P1 |= m;
            } else {
                P1 &= ~m;
            }
        }
        return 0;
    }

    if (port == 3) {
        if (mode == BOARD_GPIO_IN) {
            P3_MOD_OC &= ~m;
            P3_DIR_PU &= ~m;
        } else if (mode == BOARD_GPIO_OD) {
            P3_MOD_OC |= m;
            P3_DIR_PU &= ~m;
            if (level) {
                P3 |= m;
            } else {
                P3 &= ~m;
            }
        } else {
            P3_MOD_OC &= ~m;
            P3_DIR_PU |= m;
            if (level) {
                P3 |= m;
            } else {
                P3 &= ~m;
            }
        }
        return 0;
    }

    return 1;
}

static uint8_t gpio_sample(uint8_t port, uint8_t bit)
{
    if (port == 1) {
        return (P1 & (uint8_t)(1u << bit)) ? 1 : 0;
    }
    if (port == 3) {
        return (P3 & (uint8_t)(1u << bit)) ? 1 : 0;
    }
    return 0xFF;
}

void board_init(void)
{
    uint8_t i;

    /* Chip select is configured by spi_init() (it owns the SPI pins and
     * re-runs on CMD_SET_SPI_CFG), but release it here as well so the
     * flash never sees a spurious select during power-up. */
    CS_DEASSERT();

    /* Drive every auxiliary pin to its inactive (high) level.  /WP and
     * /HOLD must be high for a flash to be writable and responsive. */
    for (i = 0; i < 4; i++) {
        board_gpio_write(i, 1);
    }
}

uint8_t board_gpio_write(uint8_t index, uint8_t level)
{
    level = level ? 1 : 0;

    switch (index) {
    case 0:
        return gpio_apply(BOARD_GPIO0_PORT, BOARD_GPIO0_BIT,
                          BOARD_GPIO0_MODE, level);
    case 1:
        return gpio_apply(BOARD_GPIO1_PORT, BOARD_GPIO1_BIT,
                          BOARD_GPIO1_MODE, level);
    case 2:
        return gpio_apply(BOARD_GPIO2_PORT, BOARD_GPIO2_BIT,
                          BOARD_GPIO2_MODE, level);
    case 3:
        return gpio_apply(BOARD_GPIO3_PORT, BOARD_GPIO3_BIT,
                          BOARD_GPIO3_MODE, level);
    default:
        return 1;
    }
}

uint8_t board_gpio_read(uint8_t index)
{
    switch (index) {
    case 0:
        return gpio_sample(BOARD_GPIO0_PORT, BOARD_GPIO0_BIT);
    case 1:
        return gpio_sample(BOARD_GPIO1_PORT, BOARD_GPIO1_BIT);
    case 2:
        return gpio_sample(BOARD_GPIO2_PORT, BOARD_GPIO2_BIT);
    case 3:
        return gpio_sample(BOARD_GPIO3_PORT, BOARD_GPIO3_BIT);
    default:
        return 0xFF;
    }
}
