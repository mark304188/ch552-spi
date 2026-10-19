/*==========================================================================*
 * ch554_usb.h - USB constants and descriptor structs (SDCC form)
 *
 * Trimmed down to what the vendor-defined device in this project needs.
 *==========================================================================*/

#ifndef __CH554_USB_H__
#define __CH554_USB_H__

/* USB PID */
#define USB_PID_SETUP 0x0D
#define USB_PID_OUT 0x01
#define USB_PID_IN 0x09

/* USB standard device request codes */
#define USB_GET_STATUS 0x00
#define USB_CLEAR_FEATURE 0x01
#define USB_SET_FEATURE 0x03
#define USB_SET_ADDRESS 0x05
#define USB_GET_DESCRIPTOR 0x06
#define USB_SET_DESCRIPTOR 0x07
#define USB_GET_CONFIGURATION 0x08
#define USB_SET_CONFIGURATION 0x09
#define USB_GET_INTERFACE 0x0A
#define USB_SET_INTERFACE 0x0B
#define USB_SYNCH_FRAME 0x0C

/* Bit define for USB request type */
#define USB_REQ_TYP_IN 0x80
#define USB_REQ_TYP_OUT 0x00
#define USB_REQ_TYP_MASK 0x60
#define USB_REQ_TYP_STANDARD 0x00
#define USB_REQ_TYP_CLASS 0x20
#define USB_REQ_TYP_VENDOR 0x40
#define USB_REQ_TYP_RESERVED 0x60
#define USB_REQ_RECIP_MASK 0x1F
#define USB_REQ_RECIP_DEVICE 0x00
#define USB_REQ_RECIP_INTERF 0x01
#define USB_REQ_RECIP_ENDP 0x02
#define USB_REQ_RECIP_OTHER 0x03

/* USB descriptor type */
#define USB_DESCR_TYP_DEVICE 0x01
#define USB_DESCR_TYP_CONFIG 0x02
#define USB_DESCR_TYP_STRING 0x03
#define USB_DESCR_TYP_INTERF 0x04
#define USB_DESCR_TYP_ENDP 0x05

/* USB endpoint type and attributes */
#define USB_ENDP_DIR_MASK 0x80
#define USB_ENDP_ADDR_MASK 0x0F
#define USB_ENDP_TYPE_MASK 0x03
#define USB_ENDP_TYPE_CTRL 0x00
#define USB_ENDP_TYPE_ISOCH 0x01
#define USB_ENDP_TYPE_BULK 0x02
#define USB_ENDP_TYPE_INTER 0x03

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

typedef struct _USB_SETUP_REQ {
    uint8_t bRequestType;
    uint8_t bRequest;
    uint8_t wValueL;
    uint8_t wValueH;
    uint8_t wIndexL;
    uint8_t wIndexH;
    uint8_t wLengthL;
    uint8_t wLengthH;
} USB_SETUP_REQ, *PUSB_SETUP_REQ;

#endif /* __CH554_USB_H__ */
