/**
 * @file flash_if.h
 * @brief Flash memory interface for DFU
 */
#ifndef FLASH_IF_H
#define FLASH_IF_H

#include <stdint.h>

/* Flash registers. FLASH_BASE is overridable so the host-side test in tools/
 * can point the register window at an ordinary array and link this file for
 * real, rather than stubbing out the address validation it is here to test. */
#ifndef FLASH_BASE
#define FLASH_BASE          0x40022000
#endif
#define FLASH_ACR           (*(volatile uint32_t*)(FLASH_BASE + 0x00))
#define FLASH_KEYR          (*(volatile uint32_t*)(FLASH_BASE + 0x04))
#define FLASH_OPTKEYR       (*(volatile uint32_t*)(FLASH_BASE + 0x08))
#define FLASH_SR            (*(volatile uint32_t*)(FLASH_BASE + 0x0C))
#define FLASH_CR            (*(volatile uint32_t*)(FLASH_BASE + 0x10))
#define FLASH_AR            (*(volatile uint32_t*)(FLASH_BASE + 0x14))

/* Flash status register bits */
#define FLASH_SR_BSY        (1 << 0)
#define FLASH_SR_PGERR      (1 << 2)
#define FLASH_SR_WRPRTERR   (1 << 4)
#define FLASH_SR_EOP        (1 << 5)

/* Flash control register bits */
#define FLASH_CR_PG         (1 << 0)
#define FLASH_CR_PER        (1 << 1)
#define FLASH_CR_MER        (1 << 2)
#define FLASH_CR_OPTPG      (1 << 4)   /* Option byte programming */
#define FLASH_CR_OPTER      (1 << 5)   /* Option byte erase */
#define FLASH_CR_STRT       (1 << 6)
#define FLASH_CR_LOCK       (1 << 7)
#define FLASH_CR_OPTWRE     (1 << 9)   /* Option byte write enable */

/* Option byte addresses */
#define OB_BASE             0x1FFFF800
#define OB_RDP              (*(volatile uint16_t*)(OB_BASE + 0x00))
#define OB_USER             (*(volatile uint16_t*)(OB_BASE + 0x02))
#define OB_DATA0            (*(volatile uint16_t*)(OB_BASE + 0x04))
#define OB_DATA1            (*(volatile uint16_t*)(OB_BASE + 0x06))
#define OB_WRP0             (*(volatile uint16_t*)(OB_BASE + 0x08))
#define OB_WRP1             (*(volatile uint16_t*)(OB_BASE + 0x0A))
#define OB_WRP2             (*(volatile uint16_t*)(OB_BASE + 0x0C))
#define OB_WRP3             (*(volatile uint16_t*)(OB_BASE + 0x0E))

/* Option byte values.
 * Each option byte is stored as value + complement; the hardware computes the
 * complement itself, so only the low byte of a written half-word matters, and
 * only the low byte of a read half-word carries the value. */
#define OB_RDP_UNLOCK       0x00A5     /* Read protection disabled */
#define OB_WRP_NONE         0x00FF     /* No write protection */
#define OB_VALUE(ob)        ((ob) & 0xFF)

/* Flash unlock keys */
#define FLASH_KEY1          0x45670123
#define FLASH_KEY2          0xCDEF89AB

/* Flash page size for GD32F303/STM32F103 */
#define FLASH_PAGE_SIZE     2048

/* Protected address range (bootloader) */
#define BOOTLOADER_START    0x08000000
#define BOOTLOADER_END      0x08002FFF

/* Application address range.
 *
 * The last 2KB page (0x0803F800..0x0803FFFF) is deliberately NOT part of the
 * DFU-writable region. nova-voyager_firmware keeps its settings store there,
 * and its linker already carves the page out (ldscript.ld: FLASH LENGTH 242K,
 * not 244K) so no legitimate image can reach it. Excluding it here as well
 * means the page is safe whatever a DFU host asks for, rather than being safe
 * only because dfu-util happens to erase just the element it is downloading.
 *
 * 121 sectors * 2KB = 247808 bytes, ending at 0x0803F7FF - byte-for-byte the
 * last address that linker will place code at.
 *
 * To hand the full 244KB to DFU instead, set APP_SETTINGS_RESERVE to 0 and
 * change the DfuSe layout string in usb_dfu.c from 121*2Kg to 122*2Kg. The
 * two must always agree; tools/run-tests.sh asserts that they do. */
#define APP_FLASH_START      0x08003000
#define APP_SETTINGS_RESERVE FLASH_PAGE_SIZE   /* one 2KB page, see above */
#define APP_FLASH_END        (0x0803FFFF - APP_SETTINGS_RESERVE)

/* Function prototypes */
void flash_unlock(void);
void flash_lock(void);
int  flash_erase_page(uint32_t address);
int  flash_write(uint32_t address, const uint8_t *data, uint32_t len);
int  flash_is_address_valid(uint32_t address);

/* Clears the WRP option bytes if any are set.
 * Returns 1 if the option bytes were reprogrammed and the caller must reset
 * for them to take effect, 0 if nothing needed doing (or programming failed). */
int  flash_disable_write_protection(void);

#endif /* FLASH_IF_H */
