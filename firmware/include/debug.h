/*==========================================================================*
 * debug.h - clock configuration, delays and a minimal UART0 debug console
 *==========================================================================*/

#ifndef __DEBUG_H__
#define __DEBUG_H__

#include <stdint.h>

void CfgFsys(void);
void mDelayuS(uint16_t n);
void mDelaymS(uint16_t n);

void mInitSTDIO(void);
int putchar(int c);
int getchar(void);

/* Enabled with -DDE_PRINTF=1; off by default to keep the code small. */
#ifndef DE_PRINTF
#define DE_PRINTF 0
#endif

#if DE_PRINTF
#define DBG(...) printf(__VA_ARGS__)
#else
#define DBG(...)
#endif

#endif /* __DEBUG_H__ */
