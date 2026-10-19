/*==========================================================================*
 * usb.h - USB device core for the CH552 SPI bridge
 *==========================================================================*/

#ifndef __USB_H__
#define __USB_H__

#include <stdint.h>
#include <ch554_usb.h>

/* Bulk endpoints used by the bridge.
 *
 *   EP2 -> 0x82  bulk IN   (device to host)  dual 64 byte buffers
 *   EP3 -> 0x03  bulk OUT  (host to device)  dual 64 byte buffers
 *
 * Endpoints 1 and 4 are disabled.  EP0 carries standard control requests
 * only, so no class or vendor control traffic can disturb the data plane.
 */
#define EP_IN_ADDR 0x82
#define EP_OUT_ADDR 0x03

/* Vendor control request used to report firmware/endpoint state over EP0.
 * Works even if the bulk endpoints are misbehaving. */
#define VENDOR_REQ_STATUS 0x42
#define VENDOR_STATUS_LEN 32

#define MAX_PACKET_SIZE 64
#define EP0_SIZE 64

/* Total length of the configuration descriptor blob: 9 (config) + 9
 * (interface) + 7 (EP IN) + 7 (EP OUT) = 32 bytes. */
#define CFG_TOTAL_LEN 0x20

/* Size of the reply staging area in xRAM.  A single command reply never
 * exceeds 129 bytes (128 readback + status), so 192 is comfortable.
 *
 * Defined in usb.c with an absolute address, so it is declared there and
 * not here: SDCC rejects an extern declaration that is followed by an
 * __at() definition inside the same translation unit. */
#define REPLY_BUF_SIZE 192

void usb_in_arm(uint8_t len);         /* reply of len bytes follows  */
void usb_in_prime(void);              /* send the first chunk       */

/* Streaming IN helpers, used by the bulk read commands.  The caller
 * claims the free half of the dual buffer, fills it, and arms it; the
 * next claim blocks (returns 0) until the SIE has consumed that half. */
uint8_t __xdata *usb_in_claim(void);
void usb_in_send(uint8_t len);

/*---------------------------------------------------------------*
 * Frame handshake between the ISR and the main loop             *
 *                                                               *
 * The ISR cannot run a command handler (it is far too slow), so it  *
 * only records that a frame has arrived and NAKs the endpoint until  *
 * the main loop has consumed it.  The frame stays valid until      *
 * usb_service() re-arms the endpoint, and at most one OUT frame can *
 * be pending at a time, which the NAK guarantees.                *
 *---------------------------------------------------------------*/
extern volatile __idata uint8_t ep3_frame_ready;
extern volatile __idata uint8_t ep3_frame_len;

/* Address of the buffer the SIE just filled. Valid while
 * ep3_frame_ready is 1. */
uint8_t __xdata *usb_out_frame(void);

/* Set by the main loop once it has consumed a frame, so that
 * usb_service() knows the endpoint may be re-armed. */
extern __idata uint8_t ep3_had_frame;

/*---------------------------------------------------------------*
 * Public entry points                                           *
 *---------------------------------------------------------------*/
void usb_init(void);
void usb_service(void);

#endif /* __USB_H__ */
