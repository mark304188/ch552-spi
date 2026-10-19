/*--------------------------------------------------------------------------
 * ch552.h
 *
 * Minimal but complete register definition header for the WCH CH552
 * (8-bit enhanced USB MCU, MCS-51 compatible).
 *
 * The definitions below are transcribed from the CH552/CH551 datasheet
 * (V1.x, section 3 "SFR list", 5-16 peripheral chapters) and expressed in
 * SDCC syntax so that the firmware in this project can be built without
 * shipping the vendor SDK.
 *--------------------------------------------------------------------------*/

#ifndef __CH552_H__
#define __CH552_H__

#include <compiler.h>

/*==========================================================================*
 *                              Core SFRs                                  *
 *==========================================================================*/

SFR(PSW, 0xD0); /* program status word */
SBIT(CY, 0xD0, 7);
SBIT(AC, 0xD0, 6);
SBIT(F0, 0xD0, 5);
SBIT(RS1, 0xD0, 4);
SBIT(RS0, 0xD0, 3);
SBIT(OV, 0xD0, 2);
SBIT(F1, 0xD0, 1);
SBIT(P, 0xD0, 0);

SFR(ACC, 0xE0);
SFR(B, 0xF0);
SFR(SP, 0x81);
SFR(DPL, 0x82); /* data pointer low  */
SFR(DPH, 0x83); /* data pointer high */

SFR(SAFE_MOD, 0xA1); /* WriteOnly: writing safe mode / ReadOnly: chip id */
#define CHIP_ID SAFE_MOD

SFR(GLOBAL_CFG, 0xB1); /* global config, Write@SafeMode */
#define bBOOT_LOAD 0x20 /* ReadOnly: boot loader status                    */
#define bSW_RESET 0x10  /* software reset bit, auto clear by hardware      */
#define bCODE_WE 0x08   /* enable flash-ROM program/erase                  */
#define bDATA_WE 0x04   /* enable Data-Flash program/erase                 */
#define bLDO3V3_OFF 0x02 /* disable 5V->3.3V LDO                           */
#define bWDOG_EN 0x01   /* enable watch-dog reset                          */

SFR(PCON, 0x87); /* power control and reset flag */
#define SMOD 0x80
#define bRST_FLAG1 0x20
#define bRST_FLAG0 0x10
#define GF1 0x08
#define GF0 0x04
#define PD 0x02 /* power-down enable bit */

SFR(CLOCK_CFG, 0xB9); /* system clock config, Write@SafeMode */
#define bOSC_EN_INT 0x80
#define bOSC_EN_XT 0x40
#define bWDOG_IF_TO 0x20
#define bROM_CLK_FAST 0x10
#define bRST 0x08
#define MASK_SYS_CK_SEL 0x07

/* Fosc = 24MHz internal, Fpll = Fosc * 4 = 96MHz, Fusb4x = Fpll/2 = 48MHz
 *   Fsys = Fpll/n :  n = 3  -> 32MHz       (SYS_CK_SEL = 0x07)
 *                   n = 4  -> 24MHz       (SYS_CK_SEL = 0x06)
 *                   n = 6  -> 16MHz       (SYS_CK_SEL = 0x05)
 *                   n = 8  -> 12MHz       (SYS_CK_SEL = 0x04)
 *                   n = 16 -> 6MHz        (SYS_CK_SEL = 0x03)
 *                   n = 32 -> 3MHz        (SYS_CK_SEL = 0x02)
 *                   n = 128-> 750KHz      (SYS_CK_SEL = 0x01)
 *                   n = 512-> 187.5KHz    (SYS_CK_SEL = 0x00)            */
#define SYS_CK_32M 0x07
#define SYS_CK_24M 0x06
#define SYS_CK_16M 0x05
#define SYS_CK_12M 0x04
#define SYS_CK_6M 0x03
#define SYS_CK_3M 0x02
#define SYS_CK_750K 0x01
#define SYS_CK_187K 0x00

SFR(WAKE_CTRL, 0xA9); /* wake-up control, Write@SafeMode */
#define bWAK_BY_USB 0x80
#define bWAK_RXD1_LO 0x40
#define bWAK_P1_5_LO 0x20
#define bWAK_P1_4_LO 0x10
#define bWAK_P1_3_LO 0x08
#define bWAK_RST_HI 0x04
#define bWAK_P3_2E_3L 0x02
#define bWAK_RXD0_LO 0x01

SFR(RESET_KEEP, 0xFE);
SFR(WDOG_COUNT, 0xFF);

/*==========================================================================*
 *                          Interrupt control                               *
 *==========================================================================*/

SFR(IE, 0xA8);
SBIT(EA, 0xA8, 7);
SBIT(E_DIS, 0xA8, 6);
SBIT(ET2, 0xA8, 5);
SBIT(ES, 0xA8, 4);
SBIT(ET1, 0xA8, 3);
SBIT(EX1, 0xA8, 2);
SBIT(ET0, 0xA8, 1);
SBIT(EX0, 0xA8, 0);

SFR(IP, 0xB8);
SBIT(PH_FLAG, 0xB8, 7);
SBIT(PL_FLAG, 0xB8, 6);
SBIT(PT2, 0xB8, 5);
SBIT(PS, 0xB8, 4);
SBIT(PT1, 0xB8, 3);
SBIT(PX1, 0xB8, 2);
SBIT(PT0, 0xB8, 1);
SBIT(PX0, 0xB8, 0);

SFR(IE_EX, 0xE8);
SBIT(IE_WDOG, 0xE8, 7);
SBIT(IE_GPIO, 0xE8, 6);
SBIT(IE_PWMX, 0xE8, 5);
SBIT(IE_UART1, 0xE8, 4);
SBIT(IE_ADC, 0xE8, 3);
SBIT(IE_USB, 0xE8, 2);
SBIT(IE_TKEY, 0xE8, 1);
SBIT(IE_SPI0, 0xE8, 0);

SFR(IP_EX, 0xE9);
#define bIP_LEVEL 0x80
#define bIP_GPIO 0x40
#define bIP_PWMX 0x20
#define bIP_UART1 0x10
#define bIP_ADC 0x08
#define bIP_USB 0x04
#define bIP_TKEY 0x02
#define bIP_SPI0 0x01

SFR(GPIO_IE, 0xC7);
#define bIE_IO_EDGE 0x80
#define bIE_RXD1_LO 0x40
#define bIE_P1_5_LO 0x20
#define bIE_P1_4_LO 0x10
#define bIE_P1_3_LO 0x08
#define bIE_RST_HI 0x04
#define bIE_P3_1_LO 0x02
#define bIE_RXD0_LO 0x01

/*==========================================================================*
 *                        Flash / Data-Flash                                *
 *==========================================================================*/

SFR16(ROM_ADDR, 0x84);
SFR(ROM_ADDR_L, 0x84);
SFR(ROM_ADDR_H, 0x85);
SFR16(ROM_DATA, 0x8E);
SFR(ROM_DATA_L, 0x8E);
SFR(ROM_DATA_H, 0x8F);
SFR(ROM_CTRL, 0x86); /* WriteOnly */
#define ROM_STATUS ROM_CTRL /* ReadOnly  */
#define ROM_CMD_WRITE 0x9A
#define ROM_CMD_READ 0x8E
#define bROM_ADDR_OK 0x40
#define bROM_CMD_ERR 0x02

/*==========================================================================*
 *                             I/O ports                                    *
 *==========================================================================*/

SFR(P1, 0x90); /* port 1 input & output */
SBIT(SCK, 0x90, 7); /* SPI0 serial clock    */
SBIT(MISO, 0x90, 6); /* SPI0 data input     */
SBIT(MOSI, 0x90, 5); /* SPI0 data output    */
SBIT(SCS, 0x90, 4); /* SPI0 slave select   */
SFR(P1_MOD_OC, 0x92);
SFR(P1_DIR_PU, 0x93);

#define bSCK 0x80
#define bMISO 0x40
#define bMOSI 0x20
#define bSCS 0x10

SFR(P2, 0xA0); /* internal port, xRAM page select */

SFR(P3, 0xB0); /* port 3 input & output */
SBIT(UDM, 0xB0, 7); /* ReadOnly */
SBIT(UDP, 0xB0, 6); /* ReadOnly */
SBIT(T1, 0xB0, 5);
SBIT(PWM2, 0xB0, 4);
SBIT(RXD1_, 0xB0, 4);
SBIT(T0, 0xB0, 4);
SBIT(INT1, 0xB0, 3);
SBIT(TXD1_, 0xB0, 2);
SBIT(INT0, 0xB0, 2);
SBIT(VBUS1, 0xB0, 2);
SBIT(AIN3, 0xB0, 2);
SBIT(PWM2_, 0xB0, 1);
SBIT(TXD, 0xB0, 1);
SBIT(PWM1_, 0xB0, 0);
SBIT(RXD, 0xB0, 0);
SFR(P3_MOD_OC, 0x96);
SFR(P3_DIR_PU, 0x97);

#define bUDM 0x80
#define bUDP 0x40
#define bT1 0x20
#define bPWM2 0x10
#define bRXD1_ 0x10
#define bT0 0x10
#define bINT1 0x08
#define bTXD1_ 0x04
#define bINT0 0x04
#define bVBUS1 0x04
#define bAIN3 0x04
#define bPWM2_ 0x02
#define bTXD 0x02
#define bPWM1_ 0x01
#define bRXD 0x01

SFR(PIN_FUNC, 0xC6);
#define bUSB_IO_EN 0x80  /* P3.6/P3.7 as USB              */
#define bIO_INT_ACT 0x40 /* ReadOnly                     */
#define bUART1_PIN_X 0x20
#define bUART0_PIN_X 0x10
#define bPWM2_PIN_X 0x08
#define bPWM1_PIN_X 0x04
#define bT2EX_PIN_X 0x02
#define bT2_PIN_X 0x01

SFR(XBUS_AUX, 0xA2);
#define bUART0_TX 0x80 /* ReadOnly */
#define bUART0_RX 0x40 /* ReadOnly */
#define bSAFE_MOD_ACT 0x20 /* ReadOnly */
#define GF2 0x08
#define bDPTR_AUTO_INC 0x04
#define DPS 0x01

/*==========================================================================*
 *                          Timer 0 / 1                                     *
 *==========================================================================*/

SFR(TCON, 0x88);
SBIT(TF1, 0x88, 7);
SBIT(TR1, 0x88, 6);
SBIT(TF0, 0x88, 5);
SBIT(TR0, 0x88, 4);
SBIT(IE1, 0x88, 3);
SBIT(IT1, 0x88, 2);
SBIT(IE0, 0x88, 1);
SBIT(IT0, 0x88, 0);

SFR(TMOD, 0x89);
#define bT1_GATE 0x80
#define bT1_CT 0x40
#define bT1_M1 0x20
#define bT1_M0 0x10
#define MASK_T1_MOD 0x30
#define bT0_GATE 0x08
#define bT0_CT 0x04
#define bT0_M1 0x02
#define bT0_M0 0x01
#define MASK_T0_MOD 0x03

SFR(TL0, 0x8A);
SFR(TL1, 0x8B);
SFR(TH0, 0x8C);
SFR(TH1, 0x8D);

/*==========================================================================*
 *                                UART0                                     *
 *==========================================================================*/

SFR(SCON, 0x98);
SBIT(SM0, 0x98, 7);
SBIT(SM1, 0x98, 6);
SBIT(SM2, 0x98, 5);
SBIT(REN, 0x98, 4);
SBIT(TB8, 0x98, 3);
SBIT(RB8, 0x98, 2);
SBIT(TI, 0x98, 1);
SBIT(RI, 0x98, 0);
SFR(SBUF, 0x99);

/*==========================================================================*
 *                          Timer2 / capture                                *
 *==========================================================================*/

SFR(T2CON, 0xC8);
SBIT(TF2, 0xC8, 7);
SBIT(CAP1F, 0xC8, 7);
SBIT(EXF2, 0xC8, 6);
SBIT(RCLK, 0xC8, 5);
SBIT(TCLK, 0xC8, 4);
SBIT(EXEN2, 0xC8, 3);
SBIT(TR2, 0xC8, 2);
SBIT(C_T2, 0xC8, 1);
SBIT(CP_RL2, 0xC8, 0);

SFR(T2MOD, 0xC9);
#define bTMR_CLK 0x80
#define bT2_CLK 0x40
#define bT1_CLK 0x20
#define bT0_CLK 0x10
#define bT2_CAP_M1 0x08
#define bT2_CAP_M0 0x04
#define T2OE 0x02
#define bT2_CAP1_EN 0x01

SFR16(RCAP2, 0xCA);
SFR(RCAP2L, 0xCA);
SFR(RCAP2H, 0xCB);
SFR16(T2COUNT, 0xCC);
SFR(TL2, 0xCC);
SFR(TH2, 0xCD);
SFR16(T2CAP1, 0xCE);
SFR(T2CAP1L, 0xCE);
SFR(T2CAP1H, 0xCF);

/*==========================================================================*
 *                              PWM1 / PWM2                                 *
 *==========================================================================*/

SFR(PWM_DATA2, 0x9B);
SFR(PWM_DATA1, 0x9C);
SFR(PWM_CTRL, 0x9D);
#define bPWM_IE_END 0x80
#define bPWM2_POLAR 0x40
#define bPWM1_POLAR 0x20
#define bPWM_IF_END 0x10
#define bPWM2_OUT_EN 0x08
#define bPWM1_OUT_EN 0x04
#define bPWM_CLR_ALL 0x02
SFR(PWM_CK_SE, 0x9E);

/*==========================================================================*
 *                          SPI0 (master/slave)                             *
 *==========================================================================*/

SFR(SPI0_STAT, 0xF8);
SBIT(S0_FST_ACT, 0xF8, 7); /* ReadOnly: first byte received (slave)  */
SBIT(S0_IF_OV, 0xF8, 6);   /* FIFO overflow flag                     */
SBIT(S0_IF_FIRST, 0xF8, 5);/* first byte received flag (slave)       */
SBIT(S0_IF_BYTE, 0xF8, 4); /* byte exchanged flag                    */
SBIT(S0_FREE, 0xF8, 3);    /* ReadOnly: SPI idle                    */
SBIT(S0_T_FIFO, 0xF8, 2);  /* ReadOnly: tx FIFO count (0 or 1)      */
SBIT(S0_R_FIFO, 0xF8, 0);  /* ReadOnly: rx FIFO count (0 or 1)      */

SFR(SPI0_DATA, 0xF9); /* read: rx FIFO, write: tx FIFO */
SFR(SPI0_CTRL, 0xFA);
#define bS0_MISO_OE 0x80
#define bS0_MOSI_OE 0x40
#define bS0_SCK_OE 0x20
#define bS0_DATA_DIR 0x10
#define bS0_MST_CLK 0x08
#define bS0_2_WIRE 0x04
#define bS0_CLR_ALL 0x02
#define bS0_AUTO_IF 0x01
SFR(SPI0_CK_SE, 0xFB); /* master clock divider (reset value 0x20) */
#define SPI0_S_PRE SPI0_CK_SE
SFR(SPI0_SETUP, 0xFC);
#define bS0_MODE_SLV 0x80
#define bS0_IE_FIFO_OV 0x40
#define bS0_IE_FIRST 0x20
#define bS0_IE_BYTE 0x10
#define bS0_BIT_ORDER 0x08
#define bS0_SLV_SELT 0x02
#define bS0_SLV_PRELOAD 0x01

/*==========================================================================*
 *                          UART1                                           *
 *==========================================================================*/

SFR(SCON1, 0xC0);
SBIT(U1SM0, 0xC0, 7);
SBIT(U1SMOD, 0xC0, 5);
SBIT(U1REN, 0xC0, 4);
SBIT(U1TB8, 0xC0, 3);
SBIT(U1RB8, 0xC0, 2);
SBIT(U1TI, 0xC0, 1);
SBIT(U1RI, 0xC0, 0);
SFR(SBUF1, 0xC1);
SFR(SBAUD1, 0xC2);

/*==========================================================================*
 *                       ADC / comparator                                   *
 *==========================================================================*/

SFR(ADC_CTRL, 0x80);
SBIT(CMPO, 0x80, 7); /* ReadOnly */
SBIT(CMP_IF, 0x80, 6);
SBIT(ADC_IF, 0x80, 5);
SBIT(ADC_START, 0x80, 4);
SBIT(CMP_CHAN, 0x80, 3);
SBIT(ADC_CHAN1, 0x80, 1);
SBIT(ADC_CHAN0, 0x80, 0);

SFR(ADC_CFG, 0x9A);
#define bADC_EN 0x08
#define bCMP_EN 0x04
#define bADC_CLK 0x01
SFR(ADC_DATA, 0x9F); /* ReadOnly */

/*==========================================================================*
 *                     Touch-key timer                                      *
 *==========================================================================*/

SFR(TKEY_CTRL, 0xC3);
#define bTKC_IF 0x80
#define bTKC_2MS 0x10
#define bTKC_CHAN2 0x04
#define bTKC_CHAN1 0x02
#define bTKC_CHAN0 0x01
SFR16(TKEY_DAT, 0xC4);
SFR(TKEY_DATL, 0xC4);
SFR(TKEY_DATH, 0xC5);

/*==========================================================================*
 *                     USB type-C control                                   *
 *==========================================================================*/

SFR(USB_C_CTRL, 0x91);
#define bVBUS2_PD_EN 0x80
#define bUCC2_PD_EN 0x40
#define bUCC2_PU1_EN 0x20
#define bUCC2_PU0_EN 0x10
#define bVBUS1_PD_EN 0x08
#define bUCC1_PD_EN 0x04
#define bUCC1_PU1_EN 0x02
#define bUCC1_PU0_EN 0x01

/*==========================================================================*
 *                     USB device controller                                *
 *==========================================================================*/

SFR(UDEV_CTRL, 0xD1);
#define bUD_PD_DIS 0x80
#define bUD_DP_PIN 0x20
#define bUD_DM_PIN 0x10
#define bUD_LOW_SPEED 0x04
#define bUD_GP_BIT 0x02
#define bUD_PORT_EN 0x01

SFR(UEP1_CTRL, 0xD2);
#define bUEP_R_TOG 0x80
#define bUEP_T_TOG 0x40
#define bUEP_AUTO_TOG 0x10
#define bUEP_R_RES1 0x08
#define bUEP_R_RES0 0x04
#define MASK_UEP_R_RES 0x0C
#define UEP_R_RES_ACK 0x00
#define UEP_R_RES_TOUT 0x04
#define UEP_R_RES_NAK 0x08
#define UEP_R_RES_STALL 0x0C
#define bUEP_T_RES1 0x02
#define bUEP_T_RES0 0x01
#define MASK_UEP_T_RES 0x03
#define UEP_T_RES_ACK 0x00
#define UEP_T_RES_TOUT 0x01
#define UEP_T_RES_NAK 0x02
#define UEP_T_RES_STALL 0x03

SFR(UEP1_T_LEN, 0xD3);
SFR(UEP2_CTRL, 0xD4);
SFR(UEP2_T_LEN, 0xD5);
SFR(UEP3_CTRL, 0xD6);
SFR(UEP3_T_LEN, 0xD7);

SFR(USB_INT_FG, 0xD8);
SBIT(U_IS_NAK, 0xD8, 7); /* ReadOnly */
SBIT(U_TOG_OK, 0xD8, 6); /* ReadOnly */
SBIT(U_SIE_FREE, 0xD8, 5); /* ReadOnly */
SBIT(UIF_FIFO_OV, 0xD8, 4);
SBIT(UIF_HST_SOF, 0xD8, 3);
SBIT(UIF_SUSPEND, 0xD8, 2);
SBIT(UIF_TRANSFER, 0xD8, 1);
SBIT(UIF_DETECT, 0xD8, 0); /* host mode */
SBIT(UIF_BUS_RST, 0xD8, 0); /* device mode */

SFR(USB_INT_ST, 0xD9); /* ReadOnly */
#define bUIS_IS_NAK 0x80
#define bUIS_TOG_OK 0x40
#define bUIS_TOKEN1 0x20
#define bUIS_TOKEN0 0x10
#define MASK_UIS_TOKEN 0x30
#define UIS_TOKEN_OUT 0x00
#define UIS_TOKEN_SOF 0x10
#define UIS_TOKEN_IN 0x20
#define UIS_TOKEN_SETUP 0x30
#define MASK_UIS_ENDP 0x0F
#define MASK_UIS_H_RES 0x0F

SFR(USB_MIS_ST, 0xDA); /* ReadOnly */
#define bUMS_SOF_PRES 0x80
#define bUMS_SOF_ACT 0x40
#define bUMS_SIE_FREE 0x20
#define bUMS_R_FIFO_RDY 0x10
#define bUMS_BUS_RESET 0x08
#define bUMS_SUSPEND 0x04
#define bUMS_DM_LEVEL 0x02
#define bUMS_DEV_ATTACH 0x01

SFR(USB_RX_LEN, 0xDB); /* ReadOnly */
SFR(UEP0_CTRL, 0xDC);
SFR(UEP0_T_LEN, 0xDD);
SFR(UEP4_CTRL, 0xDE);
SFR(UEP4_T_LEN, 0xDF);

SFR(USB_INT_EN, 0xE1);
#define bUIE_DEV_SOF 0x80
#define bUIE_DEV_NAK 0x40
#define bUIE_FIFO_OV 0x10
#define bUIE_HST_SOF 0x08
#define bUIE_SUSPEND 0x04
#define bUIE_TRANSFER 0x02
#define bUIE_DETECT 0x01 /* host mode    */
#define bUIE_BUS_RST 0x01 /* device mode */

SFR(USB_CTRL, 0xE2);
#define bUC_HOST_MODE 0x80
#define bUC_LOW_SPEED 0x40
#define bUC_DEV_PU_EN 0x20
#define bUC_SYS_CTRL1 0x20
#define bUC_SYS_CTRL0 0x10
#define MASK_UC_SYS_CTRL 0x30
#define bUC_INT_BUSY 0x08
#define bUC_RESET_SIE 0x04
#define bUC_CLR_ALL 0x02
#define bUC_DMA_EN 0x01

SFR(USB_DEV_AD, 0xE3);
#define bUDA_GP_BIT 0x80
#define MASK_USB_ADDR 0x7F

SFR16(UEP2_DMA, 0xE4);
SFR(UEP2_DMA_L, 0xE4);
SFR(UEP2_DMA_H, 0xE5);
SFR16(UEP3_DMA, 0xE6);
SFR(UEP3_DMA_L, 0xE6);
SFR(UEP3_DMA_H, 0xE7);

SFR(UEP4_1_MOD, 0xEA);
#define bUEP1_RX_EN 0x80
#define bUEP1_TX_EN 0x40
#define bUEP1_BUF_MOD 0x10
#define bUEP4_RX_EN 0x08
#define bUEP4_TX_EN 0x04

SFR(UEP2_3_MOD, 0xEB);
#define bUEP3_RX_EN 0x80
#define bUEP3_TX_EN 0x40
#define bUEP3_BUF_MOD 0x10
#define bUEP2_RX_EN 0x08
#define bUEP2_TX_EN 0x04
#define bUEP2_BUF_MOD 0x01

SFR16(UEP0_DMA, 0xEC);
SFR(UEP0_DMA_L, 0xEC);
SFR(UEP0_DMA_H, 0xED);
SFR16(UEP1_DMA, 0xEE);
SFR(UEP1_DMA_L, 0xEE);
SFR(UEP1_DMA_H, 0xEF);

/*==========================================================================*
 *                      Memory / flash layout                               *
 *==========================================================================*/

#define XDATA_RAM_SIZE 0x0400 /* size of the on-chip xRAM (1KB) */
#define ID_CH552 0x52
#define ID_CH551 0x51

#define DATA_FLASH_ADDR 0xC000
#define BOOT_LOAD_ADDR 0x3800
#define ROM_CFG_ADDR 0x3FF8
#define ROM_CHIP_ID_HX 0x3FFA
#define ROM_CHIP_ID_LO 0x3FFC

/*==========================================================================*
 *                    Interrupt vectors / numbers                           *
 *==========================================================================*/

#define INT_ADDR_INT0 0x0003
#define INT_ADDR_TMR0 0x000B
#define INT_ADDR_INT1 0x0013
#define INT_ADDR_TMR1 0x001B
#define INT_ADDR_UART0 0x0023
#define INT_ADDR_TMR2 0x002B
#define INT_ADDR_SPI0 0x0033
#define INT_ADDR_TKEY 0x003B
#define INT_ADDR_USB 0x0043
#define INT_ADDR_ADC 0x004B
#define INT_ADDR_UART1 0x0053
#define INT_ADDR_PWMX 0x005B
#define INT_ADDR_GPIO 0x0063
#define INT_ADDR_WDOG 0x006B

#define INT_NO_INT0 0
#define INT_NO_TMR0 1
#define INT_NO_INT1 2
#define INT_NO_TMR1 3
#define INT_NO_UART0 4
#define INT_NO_TMR2 5
#define INT_NO_SPI0 6
#define INT_NO_TKEY 7
#define INT_NO_USB 8
#define INT_NO_ADC 9
#define INT_NO_UART1 10
#define INT_NO_PWMX 11
#define INT_NO_GPIO 12
#define INT_NO_WDOG 13

#endif /* __CH552_H__ */
