/**
 * @file flash_if.c
 * @brief Flash memory interface for DFU bootloader
 */
#include "flash_if.h"

/* Wait for flash operation to complete */
static void flash_wait(void) {
    while (FLASH_SR & FLASH_SR_BSY);
}

/* Unlock flash for programming */
void flash_unlock(void) {
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

/* Lock flash after programming */
void flash_lock(void) {
    FLASH_CR |= FLASH_CR_LOCK;
}

/* Check if address is in valid application range */
int flash_is_address_valid(uint32_t address) {
    /* Reject bootloader area */
    if (address >= BOOTLOADER_START && address <= BOOTLOADER_END)
        return 0;
    /* Must be in application area */
    if (address < APP_FLASH_START || address > APP_FLASH_END)
        return 0;
    return 1;
}

/* Erase a flash page */
int flash_erase_page(uint32_t address) {
    /* Validate address */
    if (!flash_is_address_valid(address))
        return -1;

    /* Align to page boundary */
    address &= ~(FLASH_PAGE_SIZE - 1);

    flash_unlock();
    flash_wait();

    /* Clear any previous errors */
    FLASH_SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;

    /* Set page erase mode */
    FLASH_CR |= FLASH_CR_PER;

    /* Set page address */
    FLASH_AR = address;

    /* Start erase */
    FLASH_CR |= FLASH_CR_STRT;

    /* Wait for completion */
    flash_wait();

    /* Clear page erase mode */
    FLASH_CR &= ~FLASH_CR_PER;

    flash_lock();

    /* Check for errors */
    if (FLASH_SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR))
        return -1;

    return 0;
}

/* True when no page is write-protected. WRP bytes read 0xFF both when erased
 * and when explicitly programmed to "no protection", so only the value byte
 * (not the hardware-generated complement) may be tested. */
static int wrp_is_clear(void) {
    return OB_VALUE(OB_WRP0) == 0xFF && OB_VALUE(OB_WRP1) == 0xFF &&
           OB_VALUE(OB_WRP2) == 0xFF && OB_VALUE(OB_WRP3) == 0xFF;
}

/**
 * Clear the WRP option bytes so DFU can program the application region.
 *
 * Returns 1 if the option bytes were reprogrammed: FLASH_WRPR is only reloaded
 * from them at reset, so the caller must reboot before any write will succeed.
 * Returns 0 if nothing needed doing, or if programming did not take (which
 * keeps a failure from turning into a reset loop).
 */
int flash_disable_write_protection(void) {
    if (wrp_is_clear())
        return 0;

    /* Erasing the option block resets every byte, so snapshot the ones we are
     * not here to change and write them back verbatim. */
    uint16_t rdp   = OB_VALUE(OB_RDP);
    uint16_t user  = OB_VALUE(OB_USER);
    uint16_t data0 = OB_VALUE(OB_DATA0);
    uint16_t data1 = OB_VALUE(OB_DATA1);

    /* Unlock flash */
    flash_unlock();
    flash_wait();

    /* Unlock option bytes */
    FLASH_OPTKEYR = FLASH_KEY1;
    FLASH_OPTKEYR = FLASH_KEY2;

    /* Clear any previous errors */
    FLASH_SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;

    /* Erase option bytes */
    FLASH_CR |= FLASH_CR_OPTER;
    FLASH_CR |= FLASH_CR_STRT;
    flash_wait();
    FLASH_CR &= ~FLASH_CR_OPTER;

    /* Enable option byte programming */
    FLASH_CR |= FLASH_CR_OPTPG;

    /* RDP: the erase above left it at 0xFF, which reads as "protected". Only
     * write the unlock code back if read protection was already off. Taking a
     * device from protected to unprotected triggers an automatic mass erase of
     * main flash at the next reset, and silently destroying the application is
     * the opposite of what a bootloader is for. Leaving RDP erased preserves
     * the level it already had, so no such transition occurs. */
    if (rdp == OB_RDP_UNLOCK) {
        OB_RDP = OB_RDP_UNLOCK;
        flash_wait();
    }

    /* Preserve the user configuration (watchdog mode, stop/standby reset) and
     * the two user data bytes rather than forcing them back to defaults. */
    OB_USER = user;
    flash_wait();
    OB_DATA0 = data0;
    flash_wait();
    OB_DATA1 = data1;
    flash_wait();

    /* The point of the exercise: no write protection on any page. */
    OB_WRP0 = OB_WRP_NONE;
    flash_wait();
    OB_WRP1 = OB_WRP_NONE;
    flash_wait();
    OB_WRP2 = OB_WRP_NONE;
    flash_wait();
    OB_WRP3 = OB_WRP_NONE;
    flash_wait();

    /* Disable option byte programming */
    FLASH_CR &= ~FLASH_CR_OPTPG;

    flash_lock();

    /* Only ask the caller to reset if the new values actually stuck. */
    return wrp_is_clear();
}

/* Write data to flash (must be half-word aligned) */
int flash_write(uint32_t address, const uint8_t *data, uint32_t len) {
    volatile uint16_t *dst = (volatile uint16_t*)address;

    if (len == 0)
        return 0;

    /* Validate address range */
    if (!flash_is_address_valid(address))
        return -1;
    if (!flash_is_address_valid(address + len - 1))
        return -1;

    flash_unlock();
    flash_wait();

    /* Clear any previous errors */
    FLASH_SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;

    /* Enable programming */
    FLASH_CR |= FLASH_CR_PG;

    /* Write half-words. Assembling each one byte-wise avoids assuming `data` is
     * half-word aligned, and pads an odd-length tail with 0xFF (the erased
     * value) instead of reading past the end of the buffer. */
    for (uint32_t i = 0; i < len; i += 2) {
        uint8_t lo = data[i];
        uint8_t hi = (i + 1 < len) ? data[i + 1] : 0xFF;

        *dst++ = (uint16_t)(lo | ((uint16_t)hi << 8));
        flash_wait();

        /* Check for errors */
        if (FLASH_SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) {
            FLASH_CR &= ~FLASH_CR_PG;
            flash_lock();
            return -1;
        }
    }

    /* Disable programming */
    FLASH_CR &= ~FLASH_CR_PG;

    flash_lock();

    return 0;
}
