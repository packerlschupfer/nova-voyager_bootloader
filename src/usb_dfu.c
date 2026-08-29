/**
 * @file usb_dfu.c
 * @brief Minimal USB DFU implementation for bootloader
 */
#include "usb_regs.h"
#include "usb_dfu.h"
#include "flash_if.h"
#include <string.h>

/* PMA buffer layout */
#define BTABLE_OFFSET       0x00
#define EP0_TX_OFFSET       0x40
#define EP0_RX_OFFSET       0x80
#define EP0_MAX_SIZE        64

/* DFU context */
static DFU_Context dfu;

/* Data buffer for DFU transfers */
static uint8_t dfu_buffer[DFU_XFER_SIZE] __attribute__((aligned(4)));
static uint16_t dfu_buffer_len;

/* Pending address set (for SET_ADDRESS) */
static uint8_t pending_address;

/* DfuSe command codes (sent in block 0) */
#define DFUSE_CMD_SET_ADDRESS   0x21
#define DFUSE_CMD_ERASE_PAGE    0x41

/* Pending DfuSe command, decoded from block 0 and executed on the next
 * GETSTATUS. `dfuse_cmd_arg` is the command's own operand and is consumed when
 * the command runs; `dfuse_address_ptr` is the persistent SET_ADDRESS pointer
 * that data blocks are numbered from. Keeping them apart matters: an
 * ERASE_PAGE arriving between data blocks must not relocate the download. */
static uint8_t  dfuse_cmd_pending;
static uint8_t  dfuse_cmd_has_arg;
static uint32_t dfuse_cmd_arg;
static uint32_t dfuse_address_ptr;

/* In-flight control-IN transfer. EP0 moves at most EP0_MAX_SIZE bytes per
 * packet, so anything longer - a 1KB DFU upload block, above all - is fed out
 * one packet at a time from handle_ep0_in(). */
static const uint8_t *ep0_tx_ptr;
static uint16_t ep0_tx_remaining;
static uint16_t ep0_req_len;        /* wLength of the SETUP being answered */
static uint8_t  ep0_tx_need_zlp;

/* Scratch for short replies, so ep0_send() is never handed a pointer to a
 * caller's stack frame. */
static uint8_t ep0_reply[8];

/* USB Descriptors */
static const uint8_t device_descriptor[] = {
    18,                     /* bLength */
    USB_DESC_TYPE_DEVICE,   /* bDescriptorType */
    0x00, 0x02,             /* bcdUSB = 2.00 */
    0x00,                   /* bDeviceClass (defined in interface) */
    0x00,                   /* bDeviceSubClass */
    0x00,                   /* bDeviceProtocol */
    EP0_MAX_SIZE,           /* bMaxPacketSize0 */
    (USB_VID & 0xFF), (USB_VID >> 8),   /* idVendor */
    (USB_PID & 0xFF), (USB_PID >> 8),   /* idProduct */
    0x00, 0x02,             /* bcdDevice = 2.00 */
    1,                      /* iManufacturer */
    2,                      /* iProduct */
    3,                      /* iSerialNumber */
    1                       /* bNumConfigurations */
};

static const uint8_t config_descriptor[] = {
    /* Configuration descriptor */
    9,                      /* bLength */
    USB_DESC_TYPE_CONFIGURATION,
    27, 0,                  /* wTotalLength */
    1,                      /* bNumInterfaces */
    1,                      /* bConfigurationValue */
    0,                      /* iConfiguration */
    0x80,                   /* bmAttributes (bus powered) */
    50,                     /* bMaxPower (100mA) */

    /* Interface descriptor */
    9,                      /* bLength */
    USB_DESC_TYPE_INTERFACE,
    0,                      /* bInterfaceNumber */
    0,                      /* bAlternateSetting */
    0,                      /* bNumEndpoints (control only) */
    0xFE,                   /* bInterfaceClass (Application Specific) */
    0x01,                   /* bInterfaceSubClass (DFU) */
    0x02,                   /* bInterfaceProtocol (DFU mode) */
    4,                      /* iInterface */

    /* DFU Functional descriptor */
    9,                      /* bLength */
    USB_DESC_TYPE_DFU_FUNCTIONAL,
    DFU_ATTR_CAN_DNLOAD | DFU_ATTR_CAN_UPLOAD | DFU_ATTR_MANIFESTATION_TOL,
    0xFF, 0x00,             /* wDetachTimeout (255ms) */
    (DFU_XFER_SIZE & 0xFF), (DFU_XFER_SIZE >> 8), /* wTransferSize */
    0x1A, 0x01              /* bcdDFUVersion (1.1a) */
};

/* String descriptors */
static const uint8_t string0[] = { 4, USB_DESC_TYPE_STRING, 0x09, 0x04 }; /* English */
static const uint8_t string1[] = { 20, USB_DESC_TYPE_STRING,
    'T',0,'e',0,'k',0,'n',0,'a',0,'t',0,'o',0,'o',0,'l',0 };
static const uint8_t string2[] = { 26, USB_DESC_TYPE_STRING,
    'N',0,'o',0,'v',0,'a',0,' ',0,'V',0,'o',0,'y',0,'a',0,'g',0,'e',0,'r',0 };
static const uint8_t string3[] = { 10, USB_DESC_TYPE_STRING,
    '0',0,'0',0,'0',0,'1',0 };
/* DfuSe memory layout string: @Flash/0x08003000/121*2Kg
 * 121 sectors of 2KB each = 242KB, ending at 0x0803F7FF. The last page of
 * flash is left out on purpose - see APP_SETTINGS_RESERVE in flash_if.h.
 *
 * The sector size here must match FLASH_PAGE_SIZE, not just multiply out to
 * the right total. flash_erase_page() aligns any address down to the real 2KB
 * page, so advertising 1KB sectors invited a host to erase 0x08003400 and take
 * 0x08003000 with it. dfu-util erases a whole element before writing any of
 * it, so this never corrupted a download in practice - but it only ever needed
 * a host that interleaved erase and write to destroy the application.
 *
 * 25 chars * 2 bytes + 2 byte header = 52 bytes (fits in one 64 byte packet) */
static const uint8_t string4[] = { 52, USB_DESC_TYPE_STRING,
    '@',0,'F',0,'l',0,'a',0,'s',0,'h',0,'/',0,'0',0,'x',0,'0',0,
    '8',0,'0',0,'0',0,'3',0,'0',0,'0',0,'0',0,'/',0,'1',0,'2',0,
    '1',0,'*',0,'2',0,'K',0,'g',0 };

/* usb_dfu.h and flash_if.h both name the application start; keep them honest. */
_Static_assert(APP_START_ADDRESS == APP_FLASH_START,
               "APP_START_ADDRESS and APP_FLASH_START disagree");

/* Copy data to PMA */
static void pma_write(uint16_t offset, const uint8_t *data, uint16_t len) {
    uint16_t *dst = (uint16_t*)(USB_PMAADDR + offset * 2);
    for (uint16_t i = 0; i < len; i += 2) {
        uint16_t val = data[i];
        if (i + 1 < len) val |= (data[i + 1] << 8);
        *dst++ = val;
        dst++; /* Skip gap */
    }
}

/* Read data from PMA */
static void pma_read(uint16_t offset, uint8_t *data, uint16_t len) {
    uint16_t *src = (uint16_t*)(USB_PMAADDR + offset * 2);
    for (uint16_t i = 0; i < len; i += 2) {
        uint16_t val = *src++;
        src++; /* Skip gap */
        data[i] = val & 0xFF;
        if (i + 1 < len) data[i + 1] = val >> 8;
    }
}

/* Set endpoint TX status */
static void ep_set_tx_status(uint8_t ep, uint16_t status) {
    volatile uint16_t *epr = (volatile uint16_t*)(USB_BASE + ep * 4);
    uint16_t val = *epr;
    /* Toggle bits to reach desired state, preserve invariant bits */
    val = (val & (USB_EP_CTR_RX | USB_EP_TYPE | USB_EP_KIND | USB_EP_CTR_TX | USB_EP_EA))
        | ((val ^ status) & USB_EP_STAT_TX);
    *epr = val;
}

/* Set endpoint RX status */
static void ep_set_rx_status(uint8_t ep, uint16_t status) {
    volatile uint16_t *epr = (volatile uint16_t*)(USB_BASE + ep * 4);
    uint16_t val = *epr;
    val = (val & (USB_EP_CTR_RX | USB_EP_TYPE | USB_EP_KIND | USB_EP_CTR_TX | USB_EP_EA))
        | ((val ^ status) & USB_EP_STAT_RX);
    *epr = val;
}

/* Queue the next packet of the in-flight control-IN transfer. */
static void ep0_tx_chunk(void) {
    uint16_t chunk = ep0_tx_remaining;
    if (chunk > EP0_MAX_SIZE) chunk = EP0_MAX_SIZE;

    pma_write(EP0_TX_OFFSET, ep0_tx_ptr, chunk);
    PMA_WRITE(BTABLE_OFFSET + 2, chunk); /* COUNT_TX */

    ep0_tx_ptr += chunk;
    ep0_tx_remaining -= chunk;

    ep_set_tx_status(0, USB_EP_TX_VALID);
}

/* Send a control-IN data stage of any length. The first packet goes out here;
 * handle_ep0_in() drains the rest as the host asks for them. */
static void ep0_send(const uint8_t *data, uint16_t len) {
    if (len > ep0_req_len) len = ep0_req_len;

    ep0_tx_ptr = data;
    ep0_tx_remaining = len;

    /* A data stage ends on a packet shorter than wMaxPacketSize. If we are
     * returning less than the host asked for but the final packet happens to
     * be exactly full, a terminating zero-length packet is owed. */
    ep0_tx_need_zlp = (len > 0) && (len < ep0_req_len) &&
                      ((len % EP0_MAX_SIZE) == 0);

    ep0_tx_chunk();

    /* Also enable RX for the status stage OUT packet (or the next SETUP) */
    ep_set_rx_status(0, USB_EP_RX_VALID);
}

/* Send zero-length packet */
static void ep0_send_zlp(void) {
    ep0_tx_ptr = 0;
    ep0_tx_remaining = 0;
    ep0_tx_need_zlp = 0;

    PMA_WRITE(BTABLE_OFFSET + 2, 0);
    ep_set_tx_status(0, USB_EP_TX_VALID);
    /* Enable RX for next transaction */
    ep_set_rx_status(0, USB_EP_RX_VALID);
}

/* Stall EP0 - stall both directions */
static void ep0_stall(void) {
    ep0_tx_ptr = 0;
    ep0_tx_remaining = 0;
    ep0_tx_need_zlp = 0;
    ep_set_tx_status(0, USB_EP_TX_STALL);
    /* Keep RX enabled to receive next SETUP */
    ep_set_rx_status(0, USB_EP_RX_VALID);
}

/* Handle GET_DESCRIPTOR */
static void handle_get_descriptor(uint16_t value, uint16_t len) {
    const uint8_t *desc = 0;
    uint16_t desc_len = 0;

    uint8_t type = value >> 8;
    uint8_t index = value & 0xFF;

    switch (type) {
        case USB_DESC_TYPE_DEVICE:
            desc = device_descriptor;
            desc_len = sizeof(device_descriptor);
            break;
        case USB_DESC_TYPE_CONFIGURATION:
            desc = config_descriptor;
            desc_len = sizeof(config_descriptor);
            break;
        case USB_DESC_TYPE_STRING:
            switch (index) {
                case 0: desc = string0; desc_len = sizeof(string0); break;
                case 1: desc = string1; desc_len = sizeof(string1); break;
                case 2: desc = string2; desc_len = sizeof(string2); break;
                case 3: desc = string3; desc_len = sizeof(string3); break;
                case 4: desc = string4; desc_len = sizeof(string4); break;
            }
            break;
    }

    if (desc && desc_len > 0) {
        if (len < desc_len) desc_len = len;
        ep0_send(desc, desc_len);
    } else {
        ep0_stall();
    }
}

/* Handle DFU GETSTATUS */
static void handle_dfu_getstatus(void) {
    uint8_t poll_timeout = 0;

    /* State transitions happen FIRST, then we report the new state */
    if (dfu.state == DFU_STATE_DNLOAD_SYNC) {
        /* Transition to DNBUSY - tell host to wait then poll again */
        dfu.state = DFU_STATE_DNBUSY;
        poll_timeout = 100; /* 100ms for erase/write */
    } else if (dfu.state == DFU_STATE_DNBUSY) {
        /* Execute the pending operation, then go to DNLOAD_IDLE */
        if (dfuse_cmd_pending) {
            uint8_t cmd = dfuse_cmd_pending;
            dfuse_cmd_pending = 0;

            if (cmd == DFUSE_CMD_SET_ADDRESS) {
                if (!dfuse_cmd_has_arg) {
                    dfu.status = DFU_STATUS_ERR_ADDRESS;
                    dfu.state = DFU_STATE_ERROR;
                    goto send_status;
                }
                /* Move the pointer data blocks are numbered from. No flash
                 * operation needed. */
                dfuse_address_ptr = dfuse_cmd_arg;
            } else if (cmd == DFUSE_CMD_ERASE_PAGE) {
                /* A bare 0x41 with no operand means mass erase, which would
                 * take out the bootloader too - refuse it. */
                if (!dfuse_cmd_has_arg || flash_erase_page(dfuse_cmd_arg) != 0) {
                    dfu.status = DFU_STATUS_ERR_ERASE;
                    dfu.state = DFU_STATE_ERROR;
                    goto send_status;
                }
            } else {
                dfu.status = DFU_STATUS_ERR_VENDOR;
                dfu.state = DFU_STATE_ERROR;
                goto send_status;
            }
        } else if (dfu_buffer_len > 0) {
            /* Write data to flash */
            if (flash_write(dfu.address, dfu_buffer, dfu_buffer_len) != 0) {
                dfu.status = DFU_STATUS_ERR_WRITE;
                dfu.state = DFU_STATE_ERROR;
                goto send_status;
            }
            dfu.address += dfu_buffer_len;
            dfu_buffer_len = 0;
        }
        dfu.state = DFU_STATE_DNLOAD_IDLE;
    } else if (dfu.state == DFU_STATE_MANIFEST_SYNC) {
        dfu.state = DFU_STATE_MANIFEST_WAIT_RESET;
    }

send_status:
    ep0_reply[0] = dfu.status;       /* bStatus */
    ep0_reply[1] = poll_timeout;     /* bwPollTimeout[0] */
    ep0_reply[2] = 0;                /* bwPollTimeout[1] */
    ep0_reply[3] = 0;                /* bwPollTimeout[2] */
    ep0_reply[4] = dfu.state;        /* bState - now reflects NEW state */
    ep0_reply[5] = 0;                /* iString */

    ep0_send(ep0_reply, 6);
}

/* Handle DFU GETSTATE */
static void handle_dfu_getstate(void) {
    ep0_reply[0] = dfu.state;
    ep0_send(ep0_reply, 1);
}

/* Handle DFU DNLOAD */
static void handle_dfu_dnload(uint16_t block, uint16_t len) {
    if (len == 0) {
        /* Download complete - enter manifestation */
        if (dfu.state == DFU_STATE_DNLOAD_IDLE) {
            dfu.state = DFU_STATE_MANIFEST_SYNC;
            ep0_send_zlp();
        } else {
            ep0_stall();
        }
        return;
    }

    /* We advertised wTransferSize = DFU_XFER_SIZE. A host that ignores it would
     * overrun dfu_buffer, and the guard in handle_ep0_out() would drop the tail
     * so the completion ACK never fires - wedging DFU until the host times out.
     * Refuse the request instead. */
    if (len > DFU_XFER_SIZE) {
        dfu.status = DFU_STATUS_ERR_UNKNOWN;
        dfu.state = DFU_STATE_ERROR;
        ep0_stall();
        return;
    }

    /* Block 1 is reserved by DfuSe and carries neither a command nor data. It
     * would otherwise fall through to a flash write at a stale dfu.address. */
    if (block == 1) {
        ep0_stall();
        return;
    }

    if (dfu.state == DFU_STATE_IDLE || dfu.state == DFU_STATE_DNLOAD_IDLE) {
        dfu.block_num = block;
        dfu.xfer_len = len;
        dfu_buffer_len = 0; /* Reset buffer for new transfer */

        /* Block 0 = DfuSe command, Block 2+ = data at address pointer + (block-2)*xfer_size */
        if (block >= 2) {
            /* Data block - address is pointer + offset */
            dfu.address = dfuse_address_ptr + (uint32_t)(block - 2) * DFU_XFER_SIZE;
        }
        /* Block 0 will be processed in handle_ep0_out after receiving data */

        /* Prepare to receive data */
        dfu.state = DFU_STATE_DNLOAD_SYNC;
        ep_set_rx_status(0, USB_EP_RX_VALID);
    } else {
        ep0_stall();
    }
}

/* Handle DFU UPLOAD */
static void handle_dfu_upload(uint16_t block, uint16_t len) {
    if (dfu.state != DFU_STATE_IDLE && dfu.state != DFU_STATE_UPLOAD_IDLE) {
        ep0_stall();
        return;
    }

    /* DfuSe reserves blocks 0 and 1 for commands; data blocks start at 2 and
     * are read from the SET_ADDRESS pointer, exactly as downloads are written
     * to it. We expose no upload command set, so blocks below 2 are stalled. */
    if (block < 2) {
        ep0_stall();
        return;
    }

    uint32_t addr = dfuse_address_ptr + (uint32_t)(block - 2) * DFU_XFER_SIZE;

    if (addr < APP_FLASH_START || addr > APP_FLASH_END) {
        /* Past the end of the region - a short packet ends the upload. */
        ep0_send_zlp();
        dfu.state = DFU_STATE_IDLE;
        return;
    }

    /* APP_FLASH_END is inclusive, hence the +1. */
    uint32_t avail = APP_FLASH_END - addr + 1;
    uint16_t read_len = (len < avail) ? len : (uint16_t)avail;

    /* ep0_send() splits this across as many 64-byte packets as it takes. */
    ep0_send((const uint8_t *)addr, read_len);
    dfu.state = DFU_STATE_UPLOAD_IDLE;
}

/* Handle DFU CLRSTATUS */
static void handle_dfu_clrstatus(void) {
    if (dfu.state == DFU_STATE_ERROR) {
        dfu.status = DFU_STATUS_OK;
        dfu.state = DFU_STATE_IDLE;
    }
    ep0_send_zlp();
}

/* Handle DFU ABORT */
static void handle_dfu_abort(void) {
    dfu.status = DFU_STATUS_OK;
    dfu.state = DFU_STATE_IDLE;
    dfu.address = APP_START_ADDRESS;
    dfuse_address_ptr = APP_START_ADDRESS;
    dfuse_cmd_pending = 0;
    dfuse_cmd_has_arg = 0;
    ep0_send_zlp();
}

/* Handle SETUP packet */
static void handle_setup(void) {
    USB_SetupPacket setup;
    pma_read(EP0_RX_OFFSET, (uint8_t*)&setup, 8);

    /* Clear RX CTR */
    USB_EP0R = USB_EP_CTR_TX | USB_EP_CONTROL | 0;

    /* A new SETUP abandons any data stage still in flight, and sets the budget
     * every ep0_send() below is clamped to. */
    ep0_tx_ptr = 0;
    ep0_tx_remaining = 0;
    ep0_tx_need_zlp = 0;
    ep0_req_len = setup.wLength;

    if ((setup.bmRequestType & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
        /* Standard requests */
        switch (setup.bRequest) {
            case USB_REQ_GET_DESCRIPTOR:
                handle_get_descriptor(setup.wValue, setup.wLength);
                break;
            case USB_REQ_SET_ADDRESS:
                pending_address = setup.wValue & 0x7F;
                ep0_send_zlp();
                break;
            case USB_REQ_SET_CONFIGURATION:
                ep0_send_zlp();
                break;
            case USB_REQ_GET_CONFIGURATION:
                ep0_reply[0] = 1;
                ep0_send(ep0_reply, 1);
                break;
            case USB_REQ_GET_STATUS:
                ep0_reply[0] = 0;
                ep0_reply[1] = 0;
                ep0_send(ep0_reply, 2);
                break;
            default:
                ep0_stall();
                break;
        }
    } else if ((setup.bmRequestType & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS) {
        /* DFU class requests */
        switch (setup.bRequest) {
            case DFU_GETSTATUS:
                handle_dfu_getstatus();
                break;
            case DFU_GETSTATE:
                handle_dfu_getstate();
                break;
            case DFU_DNLOAD:
                handle_dfu_dnload(setup.wValue, setup.wLength);
                break;
            case DFU_UPLOAD:
                handle_dfu_upload(setup.wValue, setup.wLength);
                break;
            case DFU_CLRSTATUS:
                handle_dfu_clrstatus();
                break;
            case DFU_ABORT:
                handle_dfu_abort();
                break;
            default:
                ep0_stall();
                break;
        }
    } else {
        ep0_stall();
    }
}

/* Handle EP0 OUT (data received) */
static void handle_ep0_out(void) {
    uint16_t count = PMA_READ(BTABLE_OFFSET + 6) & 0x3FF; /* COUNT_RX */

    if (dfu.state == DFU_STATE_DNLOAD_SYNC && count > 0) {
        /* Append received data to buffer */
        if (dfu_buffer_len + count <= DFU_XFER_SIZE) {
            pma_read(EP0_RX_OFFSET, dfu_buffer + dfu_buffer_len, count);
            dfu_buffer_len += count;
        }

        /* Check if we've received all expected data */
        if (dfu_buffer_len >= dfu.xfer_len) {
            /* Check if this is a DfuSe command (block 0) */
            if (dfu.block_num == 0 && dfu_buffer_len >= 1) {
                dfuse_cmd_pending = dfu_buffer[0];
                dfuse_cmd_has_arg = (dfu_buffer_len >= 5);
                if (dfuse_cmd_has_arg) {
                    /* Extract 32-bit address (little-endian). The top byte must
                     * be widened before shifting or 0xFF << 24 overflows int. */
                    dfuse_cmd_arg = (uint32_t)dfu_buffer[1] |
                                    ((uint32_t)dfu_buffer[2] << 8) |
                                    ((uint32_t)dfu_buffer[3] << 16) |
                                    ((uint32_t)dfu_buffer[4] << 24);
                }
                dfu_buffer_len = 0; /* Don't write command to flash */
            }
            ep0_send_zlp(); /* ACK - transfer complete */
        }
        /* If more data expected, just re-enable RX for next packet */
    }

    /* Clear RX CTR and re-enable reception */
    USB_EP0R = USB_EP_CTR_TX | USB_EP_CONTROL | 0;
    ep_set_rx_status(0, USB_EP_RX_VALID);
}

/* Handle EP0 IN (data sent) */
static void handle_ep0_in(void) {
    /* Clear TX CTR */
    USB_EP0R = USB_EP_CTR_RX | USB_EP_CONTROL | 0;

    /* More of a multi-packet data stage still to go? */
    if (ep0_tx_remaining > 0) {
        ep0_tx_chunk();
        return;
    }

    /* Exactly-full final packet: terminate the stage with a ZLP. */
    if (ep0_tx_need_zlp) {
        ep0_tx_need_zlp = 0;
        PMA_WRITE(BTABLE_OFFSET + 2, 0);
        ep_set_tx_status(0, USB_EP_TX_VALID);
        return;
    }

    /* Transfer complete - a SET_ADDRESS only takes effect now */
    if (pending_address) {
        USB_DADDR = USB_DADDR_EF | pending_address;
        pending_address = 0;
    }
}

/* GPIO registers for USB pins */
#define GPIOA_BASE      0x40010800
#define GPIOA_CRL       (*(volatile uint32_t*)(GPIOA_BASE + 0x00))
#define GPIOA_CRH       (*(volatile uint32_t*)(GPIOA_BASE + 0x04))
#define GPIOA_ODR       (*(volatile uint32_t*)(GPIOA_BASE + 0x0C))
#define GPIOC_BASE      0x40011000
#define GPIOC_CRH       (*(volatile uint32_t*)(GPIOC_BASE + 0x04))
#define GPIOC_ODR       (*(volatile uint32_t*)(GPIOC_BASE + 0x0C))
#define RCC_APB2ENR     (*(volatile uint32_t*)(0x40021018))
#define RCC_APB2ENR_IOPAEN  (1 << 2)
#define RCC_APB2ENR_IOPCEN  (1 << 4)
#define RCC_APB2ENR_AFIOEN  (1 << 0)

/* Initialize USB peripheral */
void usb_init(void) {
    /* Enable GPIOA, GPIOC and AFIO clocks */
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPCEN | RCC_APB2ENR_AFIOEN;

    /* Enable USB clock */
    RCC_APB1ENR |= RCC_APB1ENR_USBEN;

    /* Configure PA4-PA7 to match original bootloader GPIO config exactly
     * PA4: 0x3 = output push-pull 50MHz, HIGH
     * PA5: 0xB = AF output push-pull 50MHz
     * PA6: 0x4 = floating input
     * PA7: 0xB = AF output push-pull 50MHz */
    uint32_t crl = GPIOA_CRL;
    crl &= ~(0xFFFF << 16);  /* Clear PA4-PA7 config */
    crl |= (0xB4B3 << 16);   /* Match original: PA7=B, PA6=4, PA5=B, PA4=3 */
    GPIOA_CRL = crl;
    GPIOA_ODR |= (1 << 4);   /* PA4 HIGH */

    /* Configure PC9 as output open-drain (USB soft-connect?) - from original bootloader
     * PC9: 0x7 = output open-drain 50MHz, LOW
     * Open-drain LOW = actively drive low, which may enable USB pull-up transistor */
    uint32_t crh_c = GPIOC_CRH;
    crh_c &= ~(0xF << 4);    /* Clear PC9 config (bits 7:4) */
    crh_c |= (0x7 << 4);     /* Output open-drain 50MHz */
    GPIOC_CRH = crh_c;
    GPIOC_ODR &= ~(1 << 9);  /* PC9 LOW */

    /* Short delay for pull-up to take effect */
    for (volatile int i = 0; i < 10000; i++);

    /* Configure PA11/PA12 for USB (input - USB peripheral takes over) */
    uint32_t crh = GPIOA_CRH;
    crh &= ~(0xFF << 12);   /* Clear PA11 and PA12 config */
    crh |= (0x44 << 12);    /* Both as floating input (USB peripheral control) */
    GPIOA_CRH = crh;

    /* Force USB reset */
    USB_CNTR = USB_CNTR_FRES | USB_CNTR_PDWN;
    for (volatile int i = 0; i < 1000; i++);

    /* Exit power down */
    USB_CNTR = USB_CNTR_FRES;
    for (volatile int i = 0; i < 1000; i++);

    /* Clear reset */
    USB_CNTR = 0;
    for (volatile int i = 0; i < 1000; i++);

    /* Clear pending interrupts */
    USB_ISTR = 0;

    /* Enable interrupt masks - match original bootloader: 0xBF00 */
    /* CTRM | ERRM | WKUPM | SUSPM | RESETM | SOFM | ESOFM */
    USB_CNTR = 0xBF00;

    /* Pre-configure buffer table and EP0 (will be redone on reset) */
    USB_BTABLE = BTABLE_OFFSET;
    PMA_WRITE(BTABLE_OFFSET + 0, EP0_TX_OFFSET);
    PMA_WRITE(BTABLE_OFFSET + 2, 0);
    PMA_WRITE(BTABLE_OFFSET + 4, EP0_RX_OFFSET);
    PMA_WRITE(BTABLE_OFFSET + 6, 0x8400);

    /* Configure EP0 with proper toggle bit handling
     * CTR bits (rc_w0): write 1 to preserve
     * DTOG bits: write 0 to not toggle
     * STAT bits: write desired XOR current (for initial 00, write desired)
     * Type bits: write directly */
    USB_EP0R = USB_EP_CTR_RX | USB_EP_CTR_TX | USB_EP_CONTROL | USB_EP_RX_VALID | USB_EP_TX_NAK;

    /* Enable USB function - this enables the D+ pull-up on some chips */
    USB_DADDR = USB_DADDR_EF;
}

/* Handle USB reset */
static void usb_reset(void) {
    /* Configure buffer table */
    USB_BTABLE = BTABLE_OFFSET;

    /* Set up EP0 buffer addresses */
    PMA_WRITE(BTABLE_OFFSET + 0, EP0_TX_OFFSET); /* ADDR_TX */
    PMA_WRITE(BTABLE_OFFSET + 2, 0);             /* COUNT_TX */
    PMA_WRITE(BTABLE_OFFSET + 4, EP0_RX_OFFSET); /* ADDR_RX */
    PMA_WRITE(BTABLE_OFFSET + 6, 0x8400);        /* COUNT_RX: BL_SIZE=1, NUM_BLOCK=2 (64 bytes) */

    /* Configure EP0 as control endpoint
     * Include CTR bits (rc_w0) = 1 to preserve them */
    USB_EP0R = USB_EP_CTR_RX | USB_EP_CTR_TX | USB_EP_CONTROL | USB_EP_RX_VALID | USB_EP_TX_NAK;

    /* Enable device with address 0 */
    USB_DADDR = USB_DADDR_EF;

    /* Reset DFU state */
    dfu.state = DFU_STATE_IDLE;
    dfu.status = DFU_STATUS_OK;
    dfu.address = APP_START_ADDRESS;
    dfuse_address_ptr = APP_START_ADDRESS;
    dfuse_cmd_pending = 0;
    dfuse_cmd_has_arg = 0;
    dfu_buffer_len = 0;

    /* Nothing is in flight across a bus reset */
    ep0_tx_ptr = 0;
    ep0_tx_remaining = 0;
    ep0_tx_need_zlp = 0;
    ep0_req_len = 0;
}

/* USB interrupt handler - poll version */
void usb_poll(void) {
    uint16_t istr = USB_ISTR;

    if (istr & USB_ISTR_RESET) {
        USB_ISTR = ~USB_ISTR_RESET;
        usb_reset();
        return;
    }

    if (istr & USB_ISTR_CTR) {
        uint8_t ep = istr & USB_ISTR_EP_ID;

        if (ep == 0) {
            uint16_t epr = USB_EP0R;

            if (epr & USB_EP_CTR_RX) {
                if (epr & USB_EP_SETUP) {
                    handle_setup();
                } else {
                    handle_ep0_out();
                }
            }

            if (epr & USB_EP_CTR_TX) {
                handle_ep0_in();
            }
        }
    }
}

/* Initialize DFU */
void dfu_init(void) {
    dfu.state = DFU_STATE_IDLE;
    dfu.status = DFU_STATUS_OK;
    dfu.address = APP_START_ADDRESS;
    dfu_buffer_len = 0;
    pending_address = 0;
    dfuse_cmd_pending = 0;
    dfuse_cmd_has_arg = 0;
    dfuse_cmd_arg = 0;
    dfuse_address_ptr = APP_START_ADDRESS;
    ep0_tx_ptr = 0;
    ep0_tx_remaining = 0;
    ep0_tx_need_zlp = 0;
    ep0_req_len = 0;

    usb_init();
}

/* Get DFU state */
DFU_State dfu_get_state(void) {
    return dfu.state;
}

/* Get DFU status */
DFU_Status dfu_get_status(void) {
    return dfu.status;
}
