/**
 * @file usb_regs.h
 * @brief USB peripheral register definitions for STM32F1/GD32F303
 */
#ifndef USB_REGS_H
#define USB_REGS_H

#include <stdint.h>

/* USB Peripheral base address.
 * Overridable so the host-side test in tools/ can point the register and packet
 * memory windows at ordinary arrays and exercise the shipping source. */
#ifndef USB_BASE
#define USB_BASE            0x40005C00
#endif
#ifndef USB_PMAADDR
#define USB_PMAADDR         0x40006000  /* Packet Memory Area */
#endif

/* USB Registers */
#define USB_EP0R            (*(volatile uint16_t*)(USB_BASE + 0x00))
#define USB_EP1R            (*(volatile uint16_t*)(USB_BASE + 0x04))
#define USB_EP2R            (*(volatile uint16_t*)(USB_BASE + 0x08))
#define USB_EP3R            (*(volatile uint16_t*)(USB_BASE + 0x0C))
#define USB_CNTR            (*(volatile uint16_t*)(USB_BASE + 0x40))
#define USB_ISTR            (*(volatile uint16_t*)(USB_BASE + 0x44))
#define USB_FNR             (*(volatile uint16_t*)(USB_BASE + 0x48))
#define USB_DADDR           (*(volatile uint16_t*)(USB_BASE + 0x4C))
#define USB_BTABLE          (*(volatile uint16_t*)(USB_BASE + 0x50))

/* Endpoint register bits */
#define USB_EP_CTR_RX       0x8000
#define USB_EP_DTOG_RX      0x4000
#define USB_EP_STAT_RX      0x3000
#define USB_EP_SETUP        0x0800
#define USB_EP_TYPE         0x0600
#define USB_EP_KIND         0x0100
#define USB_EP_CTR_TX       0x0080
#define USB_EP_DTOG_TX      0x0040
#define USB_EP_STAT_TX      0x0030
#define USB_EP_EA           0x000F

/* Endpoint status values */
#define USB_EP_RX_DIS       0x0000
#define USB_EP_RX_STALL     0x1000
#define USB_EP_RX_NAK       0x2000
#define USB_EP_RX_VALID     0x3000

#define USB_EP_TX_DIS       0x0000
#define USB_EP_TX_STALL     0x0010
#define USB_EP_TX_NAK       0x0020
#define USB_EP_TX_VALID     0x0030

/* Endpoint types */
#define USB_EP_BULK         0x0000
#define USB_EP_CONTROL      0x0200
#define USB_EP_ISOCHRONOUS  0x0400
#define USB_EP_INTERRUPT    0x0600

/* Control register bits */
#define USB_CNTR_CTRM       0x8000  /* Correct transfer interrupt mask */
#define USB_CNTR_PMAOVRM    0x4000  /* PMA overflow interrupt mask */
#define USB_CNTR_ERRM       0x2000  /* Error interrupt mask */
#define USB_CNTR_WKUPM      0x1000  /* Wakeup interrupt mask */
#define USB_CNTR_SUSPM      0x0800  /* Suspend interrupt mask */
#define USB_CNTR_RESETM     0x0400  /* Reset interrupt mask */
#define USB_CNTR_SOFM       0x0200  /* Start of frame interrupt mask */
#define USB_CNTR_ESOFM      0x0100  /* Expected start of frame mask */
#define USB_CNTR_RESUME     0x0010  /* Resume request */
#define USB_CNTR_FSUSP      0x0008  /* Force suspend */
#define USB_CNTR_LP_MODE    0x0004  /* Low-power mode */
#define USB_CNTR_PDWN       0x0002  /* Power down */
#define USB_CNTR_FRES       0x0001  /* Force USB reset */

/* Interrupt status register bits */
#define USB_ISTR_CTR        0x8000  /* Correct transfer */
#define USB_ISTR_PMAOVR     0x4000  /* PMA overflow */
#define USB_ISTR_ERR        0x2000  /* Error */
#define USB_ISTR_WKUP       0x1000  /* Wakeup */
#define USB_ISTR_SUSP       0x0800  /* Suspend */
#define USB_ISTR_RESET      0x0400  /* USB reset */
#define USB_ISTR_SOF        0x0200  /* Start of frame */
#define USB_ISTR_ESOF       0x0100  /* Expected start of frame */
#define USB_ISTR_DIR        0x0010  /* Direction of transaction */
#define USB_ISTR_EP_ID      0x000F  /* Endpoint identifier */

/* Device address register bits */
#define USB_DADDR_EF        0x0080  /* Enable function */
#define USB_DADDR_ADD       0x007F  /* Device address */

/* PMA buffer table entry (in PMA memory, needs x2 offset) */
typedef struct {
    uint16_t ADDR_TX;
    uint16_t reserved0;
    uint16_t COUNT_TX;
    uint16_t reserved1;
    uint16_t ADDR_RX;
    uint16_t reserved2;
    uint16_t COUNT_RX;
    uint16_t reserved3;
} USB_BufferTableEntry;

/* PMA access macros (PMA is 16-bit wide with gaps) */
#define PMA_ADDR(offset)    (USB_PMAADDR + ((offset) * 2))
#define PMA_READ(offset)    (*(volatile uint16_t*)PMA_ADDR(offset))
#define PMA_WRITE(offset, val) (*(volatile uint16_t*)PMA_ADDR(offset) = (val))

/* Buffer table access (BTABLE register contains offset in PMA) */
#define BTABLE_ADDR_TX(ep)  PMA_READ(USB_BTABLE + (ep) * 8 + 0)
#define BTABLE_COUNT_TX(ep) PMA_READ(USB_BTABLE + (ep) * 8 + 2)
#define BTABLE_ADDR_RX(ep)  PMA_READ(USB_BTABLE + (ep) * 8 + 4)
#define BTABLE_COUNT_RX(ep) PMA_READ(USB_BTABLE + (ep) * 8 + 6)

#define SET_BTABLE_ADDR_TX(ep, addr)  PMA_WRITE(USB_BTABLE + (ep) * 8 + 0, addr)
#define SET_BTABLE_COUNT_TX(ep, cnt)  PMA_WRITE(USB_BTABLE + (ep) * 8 + 2, cnt)
#define SET_BTABLE_ADDR_RX(ep, addr)  PMA_WRITE(USB_BTABLE + (ep) * 8 + 4, addr)
#define SET_BTABLE_COUNT_RX(ep, cnt)  PMA_WRITE(USB_BTABLE + (ep) * 8 + 6, cnt)

/* RCC registers for USB clock */
#ifndef RCC_BASE
#define RCC_BASE            0x40021000
#endif
#define RCC_APB1ENR         (*(volatile uint32_t*)(RCC_BASE + 0x1C))
#define RCC_APB1ENR_USBEN   (1 << 23)

/* Standard USB request types */
#define USB_REQ_TYPE_STANDARD   0x00
#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_TYPE_VENDOR     0x40
#define USB_REQ_TYPE_MASK       0x60

#define USB_REQ_RECIPIENT_DEVICE    0x00
#define USB_REQ_RECIPIENT_INTERFACE 0x01
#define USB_REQ_RECIPIENT_ENDPOINT  0x02
#define USB_REQ_RECIPIENT_MASK      0x1F

/* Standard USB requests */
#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09
#define USB_REQ_GET_INTERFACE       0x0A
#define USB_REQ_SET_INTERFACE       0x0B
#define USB_REQ_SYNCH_FRAME         0x0C

/* Descriptor types */
#define USB_DESC_TYPE_DEVICE        0x01
#define USB_DESC_TYPE_CONFIGURATION 0x02
#define USB_DESC_TYPE_STRING        0x03
#define USB_DESC_TYPE_INTERFACE     0x04
#define USB_DESC_TYPE_ENDPOINT      0x05
#define USB_DESC_TYPE_DFU_FUNCTIONAL 0x21

/* Setup packet structure */
typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} USB_SetupPacket;

#endif /* USB_REGS_H */
