/*==========================================================================*
 * usb.c - USB device core for the CH552
 *
 * xRAM layout (1KB on-chip, 0x0000-0x03FF).  All buffers are 64 byte
 * aligned, which is what the SIE expects for the dual-buffer modes.
 *
 *   0x0000  ep0_buf[64]        EP0 IN/OUT (shared single buffer)
 *   0x0040  ep3_buf[128]       EP3 OUT 0x03, dual 64B buffers
 *   0x00C0  ep2_buf[128]       EP2 IN  0x82, dual 64B buffers
 *   0x0140  usb_reply_buf[192] staging for multi-frame IN replies
 *   0x0200 ......              free for the C compiler's xdata
 *
 * Endpoint modes:
 *   UEP2_3_MOD = bUEP2_TX_EN | bUEP2_BUF_MOD   -> EP2 dual IN
 *              | bUEP3_RX_EN | bUEP3_BUF_MOD   -> EP3 dual OUT
 *
 * Double buffering is what makes the bridge fast: the SIE can be moving
 * one 64 byte half while the firmware fills the other.  Using it requires
 * the firmware to pick the half explicitly, so both endpoints run with
 * bUEP_AUTO_TOG cleared and the data toggle is flipped by hand.  That is
 * deterministic: the half selected for a transfer is exactly the toggle
 * value programmed right before the transfer is armed.
 *
 *   - IN : write half tx_half, arm with bUEP_T_TOG = tx_half, then flip
 *          tx_half.  The SIE transmits the half we just filled.
 *   - OUT: arm with bUEP_R_TOG = rx_half, so the next frame lands in
 *          rx_half.  After processing, flip rx_half and re-arm.
 *==========================================================================*/

#include <ch552.h>
#include <stdint.h>

#include "ch554_usb.h"
#include "usb.h"
#include "protocol.h"

/*---------------------------------------------------------------*
 * Buffers                                                      *
 *---------------------------------------------------------------*/
uint8_t __xdata __at(0x0000) ep0_buf[64];
uint8_t __xdata __at(0x0040) ep3_buf[2 * MAX_PACKET_SIZE];
uint8_t __xdata __at(0x00C0) ep2_buf[2 * MAX_PACKET_SIZE];
uint8_t __xdata __at(0x0140) usb_reply_buf[REPLY_BUF_SIZE];

/*---------------------------------------------------------------*
 * USB descriptors                                              *
 *---------------------------------------------------------------*/

/* Class/subclass/protocol are "per interface" (0x00) so the
 * vendor-specific interface descriptor carries the vendor class code.
 * Linux then exposes the device through usbfs/libusb with no driver
 * bound; the same descriptors are what WinUSB / libusbK match on
 * Windows. */
__code uint8_t dev_desc[] = {
    0x12, 0x01,     /* bLength, bDescriptorType               */
    0x00, 0x02,     /* bcdUSB 2.00                            */
    0x00,           /* bDeviceClass     : defined per interface */
    0x00,           /* bDeviceSubClass                        */
    0x00,           /* bDeviceProtocol                        */
    EP0_SIZE,       /* bMaxPacketSize0                        */
    0x86, 0x1A,     /* idVendor  (WCH)                        */
    0x07, 0x55,     /* idProduct : "U" + 0x55                 */
    0x00, 0x01,     /* bcdDevice 1.00                         */
    0x01,           /* iManufacturer                          */
    0x02,           /* iProduct                               */
    0x03,           /* iSerialNumber                          */
    0x01            /* bNumConfigurations                     */
};

__code uint8_t cfg_desc[] = {
    /* configuration descriptor */
    0x09, 0x02,
    CFG_TOTAL_LEN, 0x00, /* wTotalLength                        */
    0x01,                /* bNumInterfaces                      */
    0x01,                /* bConfigurationValue                 */
    0x00,                /* iConfiguration                      */
    0x80,                /* bmAttributes: bus powered           */
    0x32,                /* bMaxPower: 100mA                    */

    /* interface 0, vendor specific */
    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00,

    /* EP2 IN, bulk, 64 bytes */
    0x07, 0x05, EP_IN_ADDR, 0x02, MAX_PACKET_SIZE, 0x00, 0x00,

    /* EP3 OUT, bulk, 64 bytes */
    0x07, 0x05, EP_OUT_ADDR, 0x02, MAX_PACKET_SIZE, 0x00, 0x00
};

__code uint8_t lang_desc[] = {0x04, 0x03, 0x09, 0x04};
__code uint8_t manu_desc[] = {0x0C, 0x03, 'w', 0, 'c', 0, 'h', 0, '5', 0, '5', 0};
__code uint8_t prod_desc[] = {
    0x14, 0x03,
    'c', 0, 'h', 0, '5', 0, '5', 0, '2', 0, '-', 0, 's', 0, 'p', 0, 'i', 0
};
__code uint8_t seri_desc[] = {0x0A, 0x03, '0', 0, '0', 0, '0', 0, '1', 0};

/*---------------------------------------------------------------*
 * State                                                        *
 *---------------------------------------------------------------*/

volatile __idata uint8_t ep3_frame_ready;
volatile __idata uint8_t ep3_frame_len;
__idata uint8_t ep3_had_frame;

/* Half of the dual buffer the next transfer will use. */
static volatile __idata uint8_t rx_half;
static __idata uint8_t tx_half;

/* Address of the half of the OUT buffer the SIE just filled. */
static uint8_t __xdata *ep3_out_ptr;

/* Diagnostics exposed through the EP0 status request.  They answer the
 * "is the main loop alive and how often does the ISR fire" question
 * without needing a UART. */
volatile __idata uint16_t dbg_tick; /* incremented by the main loop */
static volatile __idata uint8_t dbg_isr;   /* total USB interrupts     */
static volatile __idata uint8_t dbg_out;   /* EP3 OUT events           */
static volatile __idata uint8_t dbg_in;    /* EP2 IN events            */
static volatile __idata uint8_t dbg_setup; /* EP0 SETUP events         */
static volatile __idata uint8_t dbg_rxlen; /* last USB_RX_LEN          */
volatile __idata uint8_t dbg_cmd;          /* last command dispatched  */
__idata uint8_t dbg_phase;                 /* main loop progress marker */

static __idata uint8_t dev_config;
static __idata uint8_t dev_addr_pending;
static __idata uint8_t reply_len; /* total bytes queued for the IN endpoint */
static __idata uint8_t reply_off; /* bytes already handed to the SIE       */

uint8_t __xdata *usb_out_frame(void)
{
    return ep3_out_ptr;
}

/* Forward declarations: usb_in_arm() drains in-flight chunks by calling
 * usb_service(), which in turn feeds the IN endpoint. */
void usb_service(void);
static void usb_in_next_chunk(void);

/*---------------------------------------------------------------*
 * Bulk IN (EP2)                                                *
 *                                                              *
 * Two 64 byte halves.  The firmware fills one half while the SIE *
 * transmits the other, so the payload is ready before the host   *
 * asks for the next packet.  Only one half is ever armed at a   *
 * time (the SIE has a single T_LEN/T_TOG register pair); the    *
 * other one is pre-filled and armed by the ISR as soon as the   *
 * in-flight half completes.                                     *
 *---------------------------------------------------------------*/

/* in_half   : half that the next arm will use
 * in_armed  : a half is armed / being transmitted
 * in_pre_len: length of the pre-filled half, 0 when there is none   */
/* volatile because the USB ISR updates them while the main loop polls
 * them in a spin loop (see usb_in_arm / usb_in_claim). */
static volatile __idata uint8_t in_half;
static volatile __idata uint8_t in_armed;
static volatile __idata uint8_t in_pre_len;

/* Bounded spin while the SIE still owns the last IN packet.
 *
 * usb_service() is needed here because a prefilled half is only armed from
 * there, and because it keeps the OUT endpoint re-armed. */
uint8_t usb_in_wait_idle(uint16_t budget)
{
    while (in_armed || in_pre_len) {
        usb_service();
        if (--budget == 0) {
            return 0;
        }
    }
    return 1;
}

/* Prepare a new reply of `len` bytes. */
void usb_in_arm(uint8_t len)
{
    /* A command is starting.  Let any packet still on the wire finish
     * before the buffers are recycled: arming a new chunk while the old one
     * is in flight abandons it AND advances the data toggle, so the next
     * packet carries the wrong PID and the host reports EIO.
     *
     * This is the normal path for streamed reads, where the status byte is
     * queued immediately after the data chunks were armed. */
    if (!usb_in_wait_idle(8000) && in_armed) {
        /* The host stopped reading.  Reuse the half (and therefore the
         * toggle) of the packet that was never taken, so the next transfer
         * is still in phase with what the host expects. */
        in_half ^= 1;
    }

    EA = 0;
    in_armed = 0;
    in_pre_len = 0;
    UEP2_CTRL = (UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
    EA = 1;

    reply_len = len;
    reply_off = 0;
}

/* Fast xdata -> xdata copy.  The library memcpy uses generic (3 byte)
 * pointers and costs about 25 cycles per byte; this one is ~14. */
static void xcopy(uint8_t __xdata *dst, const uint8_t __xdata *src, uint8_t n)
{
    if (n == 0) {
        return;
    }
    do {
        *dst++ = *src++;
    } while (--n);
}

/* Arm half `h` of the dual IN buffer with `n` bytes.  Written as a macro
 * rather than a function on purpose: it runs in both the main loop and the
 * USB ISR, and a function with parameters would share its static parameter
 * area between the two contexts. */
#define USB_IN_ARM(h, n)                                                   \
    do {                                                                   \
        UEP2_T_LEN = (n);                                                  \
        UEP2_CTRL = (UEP2_CTRL & ~(bUEP_T_TOG | MASK_UEP_T_RES)) |         \
                    ((h) ? bUEP_T_TOG : 0) | UEP_T_RES_ACK;                \
    } while (0)

/* Move the next request chunk into the free half and either arm it or
 * leave it pre-filled for the ISR to arm.
 *
 * The arm/prefill decision races with the IN-complete interrupt, which
 * flips in_half and clears in_armed.  The critical section makes the
 * decision atomic; the kick at the top recovers if the interrupt landed
 * between a prefill and the next arming. */
static void usb_in_next_chunk(void)
{
    uint8_t n;
    uint8_t __xdata *dst;

    if (in_pre_len && !in_armed) {
        /* Pre-filled but idle: the interrupt arrived in between. */
        EA = 0;
        USB_IN_ARM(in_half, in_pre_len);
        in_half ^= 1;
        in_armed = 1;
        in_pre_len = 0;
        EA = 1;
    }

    if (reply_off >= reply_len || in_pre_len) {
        return;
    }

    n = reply_len - reply_off;
    if (n > MAX_PACKET_SIZE) {
        n = MAX_PACKET_SIZE;
    }

    dst = ep2_buf + (in_half ? MAX_PACKET_SIZE : 0);
    xcopy(dst, usb_reply_buf + reply_off, n);
    reply_off += n;

    EA = 0;
    if (in_armed) {
        /* The SIE is busy with the other half: leave this one ready. */
        in_pre_len = n;
    } else {
        USB_IN_ARM(in_half, n);
        in_half ^= 1;
        in_armed = 1;
    }
    EA = 1;
}

/* Arm the first chunk of a freshly built reply. */
void usb_in_prime(void)
{
    usb_in_next_chunk();
}

/* Claim the free half for a streaming transfer.  Returns 0 while the SIE
 * still owns both halves (the caller should retry). */
uint8_t __xdata *usb_in_claim(void)
{
    if (in_pre_len) {
        return 0; /* the pre-filled half has not been armed yet */
    }
    return ep2_buf + (in_half ? MAX_PACKET_SIZE : 0);
}

/* Arm the half returned by usb_in_claim() with `len` valid bytes. */
void usb_in_send(uint8_t len)
{
    EA = 0;
    if (in_armed) {
        in_pre_len = len;
    } else {
        USB_IN_ARM(in_half, len);
        in_half ^= 1;
        in_armed = 1;
    }
    EA = 1;
}

/*---------------------------------------------------------------*
 * Bulk OUT (EP3)                                               *
 *---------------------------------------------------------------*/

static void usb_out_rearm(void)
{
    UEP3_CTRL = (UEP3_CTRL & ~(bUEP_R_TOG | MASK_UEP_R_RES)) |
                (rx_half ? bUEP_R_TOG : 0) | UEP_R_RES_ACK;
}

/*---------------------------------------------------------------*
 * Endpoint 0                                                   *
 *---------------------------------------------------------------*/

static __idata uint8_t ep0_req;
static __idata uint8_t ep0_rem;
static __code const uint8_t *ep0_rom_src;

#define UsbSetupBuf ((struct _USB_SETUP_REQ __xdata *)ep0_buf)

/* Vendor status request (bRequest = 0x42, bmRequestType = 0xC0).
 *
 * It travels over EP0, so it still works when the bulk endpoints are not
 * answering - which is exactly the case when the chip is running an older
 * or incompatible build.  Host tools use it to confirm the firmware
 * version before blaming the wiring. */
static void ep0_fill_status(void)
{
    uint8_t i;

    for (i = 0; i < VENDOR_STATUS_LEN; i++) {
        ep0_buf[i] = 0;
    }
    ep0_buf[0] = 'C';
    ep0_buf[1] = 'S';
    ep0_buf[2] = 'P';
    ep0_buf[3] = PROTO_VERSION;
    ep0_buf[4] = 1; /* firmware major, keep in sync with protocol.c */
    ep0_buf[5] = 0; /* firmware minor */
    ep0_buf[6] = UEP2_3_MOD;
    ep0_buf[7] = UEP2_CTRL;
    ep0_buf[8] = UEP3_CTRL;
    ep0_buf[9] = UEP3_DMA_L;
    ep0_buf[10] = ep3_frame_ready;
    ep0_buf[11] = rx_half;
    ep0_buf[12] = (uint8_t)((in_half & 0x01) |
                            ((in_armed & 0x01) << 1) |
                            ((in_pre_len ? 1 : 0) << 2));
    ep0_buf[13] = USB_CTRL;
    ep0_buf[14] = UDEV_CTRL;
    ep0_buf[15] = ep3_had_frame;
    ep0_buf[16] = ep3_frame_len;
    ep0_buf[17] = reply_len;
    ep0_buf[18] = reply_off;
    ep0_buf[19] = in_pre_len;
    ep0_buf[20] = dbg_rxlen;
    ep0_buf[21] = (uint8_t)dbg_tick;
    ep0_buf[22] = (uint8_t)(dbg_tick >> 8);
    ep0_buf[23] = dbg_isr;
    ep0_buf[24] = dbg_out;
    ep0_buf[25] = dbg_in;
    ep0_buf[26] = dbg_setup;
    ep0_buf[27] = dbg_cmd;
    ep0_buf[28] = dbg_phase;
}

/* USB 2.0 9.1.1.5: SET_CONFIGURATION puts every endpoint back to DATA0
 * and cancels any pending transfer.  The host does exactly that, so the
 * device has to follow: otherwise the first bulk packet after the host
 * re-selects the configuration (every libusb open does) is seen as a
 * toggle error and silently dropped, and the bridge looks dead until the
 * next replug. */
static void usb_bulk_reset(void)
{
    rx_half = 0;
    in_half = 0;
    in_armed = 0;
    in_pre_len = 0;
    ep3_frame_ready = 0;
    ep3_had_frame = 0;
    ep3_frame_len = 0;
    ep3_out_ptr = ep3_buf;
    reply_len = 0;
    reply_off = 0;

    UEP2_CTRL = UEP_T_RES_NAK;
    UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
}

static void ep0_setup(void)
{
    uint8_t len, req;

    if (USB_RX_LEN != 8) {
        UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL |
                    UEP_T_RES_STALL;
        return;
    }

    ep0_req = UsbSetupBuf->bRequest;
    /* wLength is 16 bit.  A request larger than 255 is treated as "as
     * much as the descriptor holds", which is what every host does when
     * it asks for a descriptor of unknown size. */
    ep0_rem = UsbSetupBuf->wLengthH ? 0xFF : UsbSetupBuf->wLengthL;
    req = UsbSetupBuf->bRequest;
    len = 0;

    if ((UsbSetupBuf->bRequestType & USB_REQ_TYP_MASK) == USB_REQ_TYP_VENDOR) {
        if (req != VENDOR_REQ_STATUS) {
            goto stall;
        }
        ep0_fill_status();
        len = VENDOR_STATUS_LEN;
        /* Clamp to what the host asked for; otherwise the reply would
         * carry whatever is left in ep0_buf from the previous descriptor
         * transfer. */
        if (ep0_rem > len) {
            ep0_rem = len;
        }
        len = ep0_rem >= EP0_SIZE ? EP0_SIZE : ep0_rem;
        UEP0_T_LEN = len;
        UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
        return;
    }

    if ((UsbSetupBuf->bRequestType & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD) {
        goto stall;
    }

    switch (req) {
    case USB_GET_DESCRIPTOR:
        switch (UsbSetupBuf->wValueH) {
        case 0x01:
            ep0_rom_src = dev_desc;
            len = sizeof(dev_desc);
            break;
        case 0x02:
            ep0_rom_src = cfg_desc;
            len = sizeof(cfg_desc);
            break;
        case 0x03:
            switch (UsbSetupBuf->wValueL) {
            case 0x00:
                ep0_rom_src = lang_desc;
                len = sizeof(lang_desc);
                break;
            case 0x01:
                ep0_rom_src = manu_desc;
                len = sizeof(manu_desc);
                break;
            case 0x02:
                ep0_rom_src = prod_desc;
                len = sizeof(prod_desc);
                break;
            default:
                ep0_rom_src = seri_desc;
                len = sizeof(seri_desc);
                break;
            }
            break;
        default:
            goto stall;
        }
        if (ep0_rem > len) {
            ep0_rem = len;
        }
        len = ep0_rem >= EP0_SIZE ? EP0_SIZE : ep0_rem;
        {
            /* Code -> xdata copy, written out here instead of calling
             * memcpy(): the ISR must not use any library routine whose
             * static parameter block could be shared with the main
             * loop's call tree. */
            uint8_t i;
            for (i = 0; i < len; i++) {
                ep0_buf[i] = ep0_rom_src[i];
            }
        }
        ep0_rem -= len;
        ep0_rom_src += len;
        break;

    case USB_SET_ADDRESS:
        /* Applied when the status stage completes. */
        dev_addr_pending = UsbSetupBuf->wValueL;
        break;

    case USB_GET_CONFIGURATION:
        ep0_buf[0] = dev_config;
        len = ep0_rem >= 1 ? 1 : 0;
        break;

    case USB_SET_CONFIGURATION:
        dev_config = UsbSetupBuf->wValueL;
        usb_bulk_reset();
        break;

    case USB_SET_INTERFACE:
        usb_bulk_reset();
        break;

    case USB_GET_STATUS:
        ep0_buf[0] = 0x00;
        ep0_buf[1] = 0x00;
        len = ep0_rem >= 2 ? 2 : ep0_rem;
        break;

    default:
        goto stall;
    }

    UEP0_T_LEN = len;
    /* EP0 does not support automatic toggling; DATA1 is used for the
     * first packet of every control transfer. */
    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
    return;

stall:
    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
}

static void ep0_in_done(void)
{
    uint8_t len;

    if (ep0_req == USB_GET_DESCRIPTOR && ep0_rem) {
        uint8_t i;
        len = ep0_rem >= EP0_SIZE ? EP0_SIZE : ep0_rem;
        for (i = 0; i < len; i++) {
            ep0_buf[i] = ep0_rom_src[i];
        }
        ep0_rem -= len;
        ep0_rom_src += len;
        UEP0_T_LEN = len;
        UEP0_CTRL = UEP0_CTRL ^ bUEP_T_TOG;
        return;
    }

    if (ep0_req == USB_SET_ADDRESS) {
        USB_DEV_AD = (USB_DEV_AD & bUDA_GP_BIT) | dev_addr_pending;
    }
    UEP0_T_LEN = 0;
    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
}

static void ep0_out_done(void)
{
    UEP0_T_LEN = 0;
    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
}

/*---------------------------------------------------------------*
 * USB interrupt                                                *
 *---------------------------------------------------------------*/

void usb_isr(void) __interrupt(INT_NO_USB) __using(1)
{
    uint8_t token;

    dbg_isr++;

    if (UIF_TRANSFER) {
        token = USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP);

        switch (token) {
        case UIS_TOKEN_SETUP | 0:
            dbg_setup++;
            ep0_setup();
            break;

        case UIS_TOKEN_IN | 0:
            ep0_in_done();
            break;

        case UIS_TOKEN_OUT | 0:
            ep0_out_done();
            break;

        case UIS_TOKEN_OUT | 3:
            dbg_out++;
            if (U_TOG_OK) {
                dbg_rxlen = USB_RX_LEN;
                ep3_out_ptr = ep3_buf + (rx_half ? MAX_PACKET_SIZE : 0);
                ep3_frame_len = USB_RX_LEN;
                ep3_frame_ready = 1;
                /* Hold the endpoint until the main loop has consumed the
                 * frame, so the SIE cannot overwrite it. */
                UEP3_CTRL = (UEP3_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_NAK;
            }
            /* A toggle mismatch is a retransmission that was already
             * accepted: keep ACK armed (so the host can advance) and drop
             * the duplicate. */
            break;

        case UIS_TOKEN_IN | 2:
            dbg_in++;
            in_armed = 0;
            if (in_pre_len) {
                /* The other half was filled while this one was on the
                 * wire: arm it without touching the data. */
                USB_IN_ARM(in_half, in_pre_len);
                in_half ^= 1;
                in_armed = 1;
                in_pre_len = 0;
            } else {
                UEP2_CTRL = (UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
            }
            break;

        default:
            break;
        }

        UIF_TRANSFER = 0;
    }

    if (UIF_BUS_RST) {
        proto_reset();
        dev_config = 0;
        usb_bulk_reset();

        UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        USB_DEV_AD = 0x00;

        UIF_SUSPEND = 0;
        UIF_TRANSFER = 0;
        UIF_BUS_RST = 0;
    }

    if (UIF_SUSPEND) {
        /* Do not power down: a bridge must be able to resume instantly
         * and must never drop a flash that is mid-program. */
        UIF_SUSPEND = 0;
    }
}

/*---------------------------------------------------------------*
 * Init / main-loop service                                     *
 *---------------------------------------------------------------*/

void usb_init(void)
{
    /* Make sure P3.6/P3.7 are the USB pins (reset value already does). */
    PIN_FUNC |= bUSB_IO_EN;

    USB_CTRL = 0x00;
    USB_CTRL &= ~bUC_HOST_MODE;
    USB_CTRL |= bUC_DEV_PU_EN | bUC_INT_BUSY | bUC_DMA_EN;
    USB_DEV_AD = 0x00;

    USB_CTRL &= ~bUC_LOW_SPEED;
    UDEV_CTRL &= ~bUD_LOW_SPEED; /* full speed, 12 Mbit/s */
    UDEV_CTRL = bUD_PD_DIS;
    UDEV_CTRL |= bUD_PORT_EN;

    rx_half = 0;
    in_half = 0;
    in_armed = 0;
    in_pre_len = 0;
    reply_len = 0;
    reply_off = 0;
    ep3_frame_ready = 0;
    ep3_out_ptr = ep3_buf;

    UEP0_DMA = (uint16_t)ep0_buf;
    UEP2_DMA = (uint16_t)ep2_buf;
    UEP3_DMA = (uint16_t)ep3_buf;

    /* EP1 and EP4 disabled; EP2 dual IN buffer; EP3 dual OUT buffer. */
    UEP4_1_MOD = 0x00;
    UEP2_3_MOD = bUEP2_TX_EN | bUEP2_BUF_MOD | bUEP3_RX_EN | bUEP3_BUF_MOD;

    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    UEP2_CTRL = UEP_T_RES_NAK;  /* manual toggle, see the file header  */
    UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;

    UEP0_T_LEN = 0;
    UEP2_T_LEN = 0;

    USB_INT_FG = 0xFF;
    USB_INT_EN = bUIE_TRANSFER | bUIE_SUSPEND | bUIE_BUS_RST;
    IE_USB = 1;
    EA = 1;
}

void usb_service(void)
{
    /* Keep the IN endpoint flowing when the reply spans more than one
     * USB frame: fill the idle half while the other is on the wire. */
    usb_in_next_chunk();

    /* Re-arm the OUT endpoint once the main loop has consumed the frame. */
    if (ep3_frame_ready == 0 && ep3_had_frame) {
        ep3_had_frame = 0;
        rx_half ^= 1; /* the next frame goes into the other half */
        usb_out_rearm();
    }
}
