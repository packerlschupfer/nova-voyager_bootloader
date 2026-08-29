/**
 * @file usb_dfu.h
 * @brief USB DFU class definitions
 */
#ifndef USB_DFU_H
#define USB_DFU_H

#include <stdint.h>

/* DFU Class-Specific Requests */
#define DFU_DETACH          0x00
#define DFU_DNLOAD          0x01
#define DFU_UPLOAD          0x02
#define DFU_GETSTATUS       0x03
#define DFU_CLRSTATUS       0x04
#define DFU_GETSTATE        0x05
#define DFU_ABORT           0x06

/* DFU States */
typedef enum {
    DFU_STATE_APP_IDLE              = 0,
    DFU_STATE_APP_DETACH            = 1,
    DFU_STATE_IDLE                  = 2,
    DFU_STATE_DNLOAD_SYNC           = 3,
    DFU_STATE_DNBUSY                = 4,
    DFU_STATE_DNLOAD_IDLE           = 5,
    DFU_STATE_MANIFEST_SYNC         = 6,
    DFU_STATE_MANIFEST              = 7,
    DFU_STATE_MANIFEST_WAIT_RESET   = 8,
    DFU_STATE_UPLOAD_IDLE           = 9,
    DFU_STATE_ERROR                 = 10
} DFU_State;

/* DFU Status codes */
typedef enum {
    DFU_STATUS_OK               = 0x00,
    DFU_STATUS_ERR_TARGET       = 0x01,
    DFU_STATUS_ERR_FILE         = 0x02,
    DFU_STATUS_ERR_WRITE        = 0x03,
    DFU_STATUS_ERR_ERASE        = 0x04,
    DFU_STATUS_ERR_CHECK_ERASED = 0x05,
    DFU_STATUS_ERR_PROG         = 0x06,
    DFU_STATUS_ERR_VERIFY       = 0x07,
    DFU_STATUS_ERR_ADDRESS      = 0x08,
    DFU_STATUS_ERR_NOTDONE      = 0x09,
    DFU_STATUS_ERR_FIRMWARE     = 0x0A,
    DFU_STATUS_ERR_VENDOR       = 0x0B,
    DFU_STATUS_ERR_USBR         = 0x0C,
    DFU_STATUS_ERR_POR          = 0x0D,
    DFU_STATUS_ERR_UNKNOWN      = 0x0E,
    DFU_STATUS_ERR_STALLEDPKT   = 0x0F
} DFU_Status;

/* DFU Functional descriptor attributes */
#define DFU_ATTR_WILL_DETACH        0x08
#define DFU_ATTR_MANIFESTATION_TOL  0x04
#define DFU_ATTR_CAN_UPLOAD         0x02
#define DFU_ATTR_CAN_DNLOAD         0x01

/* DFU Transfer size - matches dfu-util default */
#define DFU_XFER_SIZE           1024

/* Application start. Must equal APP_FLASH_START in flash_if.h, which owns the
 * flash geometry (page size, and the region end after the settings reserve).
 * usb_dfu.c static-asserts that the two agree.
 *
 * There used to be an APP_END_ADDRESS and a second FLASH_PAGE_SIZE here. Both
 * duplicated flash_if.h and neither was used; APP_END_ADDRESS would have gone
 * silently stale when the region end moved. */
#define APP_START_ADDRESS       0x08003000

/* USB VID/PID */
#define USB_VID                 0x0483  /* STMicroelectronics */
#define USB_PID                 0xDF11  /* DFU in FS mode */

/* DFU context structure */
typedef struct {
    DFU_State   state;
    DFU_Status  status;
    uint32_t    address;        /* Current flash address */
    uint16_t    block_num;      /* Current block number */
    uint16_t    xfer_len;       /* Transfer length */
    uint8_t     manifest_state; /* Manifestation in progress */
} DFU_Context;

/* Function prototypes */
void dfu_init(void);
void dfu_process(void);
int  dfu_handle_request(uint8_t request, uint16_t value, uint16_t length);
void dfu_handle_data(uint8_t *data, uint16_t len);

/* Get current DFU state */
DFU_State dfu_get_state(void);
DFU_Status dfu_get_status(void);

#endif /* USB_DFU_H */
