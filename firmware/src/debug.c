/*==========================================================================*
 * debug.c - clock configuration, delays, minimal UART0 console
 *
 * The delay loops are written the way the vendor SDK does it (busy loop
 * over SAFE_MOD increments) rather than using the timers, because the
 * timers are not touched by this firmware and a software loop keeps the
 * interrupt latency predictable while a bulk SPI transfer is running.
 *==========================================================================*/

#include <ch552.h>
#include <stdint.h>

#include "debug.h"

void CfgFsys(void)
{
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;

/* Supported system clocks and the supply voltage they require on the VCC
 * pin (CH552 datasheet, table 8.2.2):
 *
 *   24 MHz  VCC > 4.4 V      USB bus power (5 V) - the default
 *   16 MHz  VCC > 3.3 V
 *   12 MHz  any VCC          use this when the board runs from 3.3 V
 *    6 MHz  any VCC
 *
 * 32 MHz (SYS_CK_32M, MASK_SYS_CK_SEL = 111) is marked "Reserved,
 * disabled" for the CH552 since datasheet revision V1.1, which lowered
 * the maximum system clock to 24 MHz.  It is only compiled in when
 * ALLOW_OVERCLOCK is defined, and it does not improve end-to-end
 * throughput anyway because the USB full-speed bulk link is the
 * bottleneck. */
#if FREQ_SYS == 32000000
#ifndef ALLOW_OVERCLOCK
#error "32MHz is out of spec for the CH552; build with ALLOW_OVERCLOCK=1 to try it"
#endif
    CLOCK_CFG = (CLOCK_CFG & ~MASK_SYS_CK_SEL) | SYS_CK_32M;
#elif FREQ_SYS == 24000000
    CLOCK_CFG = (CLOCK_CFG & ~MASK_SYS_CK_SEL) | SYS_CK_24M;
#elif FREQ_SYS == 16000000
    CLOCK_CFG = (CLOCK_CFG & ~MASK_SYS_CK_SEL) | SYS_CK_16M;
#elif FREQ_SYS == 12000000
    CLOCK_CFG = (CLOCK_CFG & ~MASK_SYS_CK_SEL) | SYS_CK_12M;
#elif FREQ_SYS == 6000000
    CLOCK_CFG = (CLOCK_CFG & ~MASK_SYS_CK_SEL) | SYS_CK_6M;
#else
#error "FREQ_SYS must be 24/16/12/6MHz (or 32MHz with ALLOW_OVERCLOCK)"
#endif

    SAFE_MOD = 0x00;
}

/* Rough microsecond delay.
 *
 * NOTE: the WCH/Keil SDK uses SAFE_MOD++ as a 2-cycle delay element because
 * Keil emits a single "INC direct".  SDCC does not: it generates
 *     mov a,_SAFE_MOD / inc a / mov _SAFE_MOD,a
 * which costs about 5 Fsys cycles, and the loop adds ~10 more.  The real
 * delay is therefore roughly 2x the nominal value at 24 MHz and ~3x at
 * 6 MHz (the loop overhead does not scale with Fsys).
 *
 * Only used for "wait at least this long" cases (oscillator settle,
 * bootloader entry).  Do not use it for precise bus timing - use a timer
 * or inline NOPs instead. */
void mDelayuS(uint16_t n)
{
    while (n--) {
#if FREQ_SYS >= 32000000
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
#elif FREQ_SYS >= 24000000
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
#elif FREQ_SYS >= 12000000
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
        SAFE_MOD++;
#else
        SAFE_MOD++;
        SAFE_MOD++;
#endif
    }
}

void mDelaymS(uint16_t n)
{
    while (n--) {
        mDelayuS(1000);
    }
}

/*---------------------------------------------------------------*
 * UART0 console (P3.1 TX / P3.0 RX, 115200 8N1 at Fsys = 24 MHz) *
 *                                                               *
 * Only compiled when DE_PRINTF is enabled, so the release build  *
 * does not carry stdio support or touch Timer1.                 *
 *---------------------------------------------------------------*/

#if DE_PRINTF

void mInitSTDIO(void)
{
    SM0 = 0;
    SM1 = 1; /* 8-bit UART, variable baud rate */
    SM2 = 0;
    RCLK = 0;
    TCLK = 0;
    PCON |= SMOD;

    /* Timer1, 8-bit auto-reload on the fast clock (Fsys/4). */
    TMOD = (TMOD & ~(bT1_GATE | bT1_CT | MASK_T1_MOD)) | bT1_M1;
    T2MOD |= bTMR_CLK | bT1_CLK;
    TH1 = (uint8_t)(256 - FREQ_SYS / 32 / 115200);
    TR1 = 1;
    TI = 1;
    REN = 1;
}

int putchar(int c)
{
    while (!TI)
        ;
    TI = 0;
    SBUF = c & 0xFF;
    return c;
}

int getchar(void)
{
    while (!RI)
        ;
    RI = 0;
    return SBUF;
}

#endif /* DE_PRINTF */
