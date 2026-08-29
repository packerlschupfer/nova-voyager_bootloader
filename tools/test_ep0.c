/*
 * Host-side test for the EP0 control-IN packetisation and the DFU upload
 * address math.
 *
 * Compiles src/usb_dfu.c natively with the USB register block and the packet
 * memory redirected to ordinary arrays, and the application flash region mmap'd
 * at its real address, so what runs here is the shipping source rather than a
 * reimplementation. Including the .c gives access to its statics.
 *
 *   cc -I include -DUSB_BASE=... -DUSB_PMAADDR=... tools/test_ep0.c && ./a.out
 *
 * See tools/run-tests.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

/* Backing store for the redirected register / PMA windows. The PMA is 16-bit
 * words in 32-bit slots, so it needs twice the addressable offset range. */
uint8_t usb_mock[0x100];
uint8_t pma_mock[0x800];
uint8_t rcc_mock[0x100];
uint8_t flash_mock[0x100];   /* FLASH_SR reads 0, so flash_wait() never spins */

#include "../src/usb_dfu.c"

/* FLASH_SR's error and BSY bits are rc_w1 on the part: hardware sets them,
 * software clears them by writing 1. A plain memory mock cannot express that,
 * so flash_if.c's "clear previous errors" write would read straight back as
 * PGERR|WRPRTERR and every write would report failure. Model the register as
 * a slot that is zero on every access, which is the correct behaviour when
 * there is no flash controller to raise an error or go busy.
 *
 * flash_if.h is already included (via usb_dfu.c) and include-guarded, so this
 * definition survives into flash_if.c below. */
static uint32_t flash_sr_scratch;
static volatile uint32_t *flash_sr_slot(void) {
    flash_sr_scratch = 0;
    return &flash_sr_scratch;
}
#undef FLASH_SR
#define FLASH_SR (*flash_sr_slot())

/* Compiled in rather than stubbed, so the address validation and the erase
 * page alignment under test are the shipping ones. Included rather than
 * linked so it sees the redirected FLASH_BASE window. */
#include "../src/flash_if.c"

static int failures;

static void check(int cond, const char *what) {
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

/* Collect every packet the device queues for one control-IN data stage. */
static uint8_t captured[8192];
static size_t  captured_len;
static int     captured_packets;

static void drain(void) {
    captured_len = 0;
    captured_packets = 0;

    for (int guard = 0; guard < 256; guard++) {
        uint16_t count = PMA_READ(BTABLE_OFFSET + 2); /* COUNT_TX */
        pma_read(EP0_TX_OFFSET, captured + captured_len, count);
        captured_len += count;
        captured_packets++;

        if (ep0_tx_remaining == 0 && !ep0_tx_need_zlp)
            return;

        handle_ep0_in();    /* host ACKed; queue the next packet */
    }
    check(0, "drain terminated");
}

/* ---- EP0 packetisation --------------------------------------------------- */

static uint8_t payload[4096];

static void test_packetisation(void) {
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 7 + 3);

    puts("EP0 control-IN packetisation");

    /* The case that was broken: a full 1KB DFU upload block. */
    ep0_req_len = 1024;
    ep0_send(payload, 1024);
    drain();
    check(captured_len == 1024, "1024 requested, 1024 sent (was 64 before)");
    check(captured_packets == 16, "1024 bytes spans 16 packets of 64");
    check(memcmp(captured, payload, 1024) == 0, "1024-byte payload arrives intact");

    /* Short final packet terminates the stage; no ZLP owed. */
    ep0_req_len = 1024;
    ep0_send(payload, 100);
    drain();
    check(captured_len == 100, "100 of 1024 requested -> 100 sent");
    check(captured_packets == 2, "100 bytes spans 2 packets (64 + 36)");
    check(memcmp(captured, payload, 100) == 0, "short payload arrives intact");

    /* Exact multiple of the packet size, less than asked for: needs a ZLP. */
    ep0_req_len = 1024;
    ep0_send(payload, 512);
    drain();
    check(captured_len == 512, "512 of 1024 requested -> 512 sent");
    check(captured_packets == 9, "512 bytes spans 8 full packets + a ZLP");

    /* Exactly satisfying the request needs no terminating ZLP. */
    ep0_req_len = 512;
    ep0_send(payload, 512);
    drain();
    check(captured_packets == 8, "512 of 512 requested -> 8 packets, no ZLP");

    /* Never send more than the host asked for. */
    ep0_req_len = 8;
    ep0_send(payload, 18);
    drain();
    check(captured_len == 8, "18-byte descriptor clamped to an 8-byte wLength");

    /* A 6-byte GETSTATUS reply still works. */
    ep0_req_len = 6;
    ep0_send(payload, 6);
    drain();
    check(captured_len == 6 && captured_packets == 1, "6-byte GETSTATUS is one packet");
}

/* ---- DFU upload address math --------------------------------------------- */

#define FLASH_LEN (APP_FLASH_END - APP_FLASH_START + 1)

static uint8_t *flash;

/* Map the application region at its real address so flash_write() in the
 * shipping flash_if.c writes where it actually would on the part. */
static int map_flash(void) {
    flash = mmap((void *)APP_FLASH_START, FLASH_LEN, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return flash == (uint8_t *)APP_FLASH_START;
}

static void fill_flash(void) {
    for (size_t i = 0; i < FLASH_LEN; i++)
        flash[i] = (uint8_t)(i * 31 + 11);
}

static void test_upload(void) {
    puts("\nDFU upload addressing");
    fill_flash();

    /* Block 2 is the first data block and reads from the address pointer. */
    dfu.state = DFU_STATE_IDLE;
    dfuse_address_ptr = APP_FLASH_START;
    ep0_req_len = 1024;
    handle_dfu_upload(2, 1024);
    drain();
    check(captured_len == 1024 && memcmp(captured, flash, 1024) == 0,
          "block 2 reads the first 1024 bytes of the app region");

    /* Block n reads from pointer + (n-2)*1024, not n*1024. */
    dfu.state = DFU_STATE_IDLE;
    ep0_req_len = 1024;
    handle_dfu_upload(5, 1024);
    drain();
    check(captured_len == 1024 && memcmp(captured, flash + 3 * 1024, 1024) == 0,
          "block 5 reads at pointer + 3KB");

    /* SET_ADDRESS moves the base the blocks are numbered from. */
    dfu.state = DFU_STATE_IDLE;
    dfuse_address_ptr = APP_FLASH_START + 0x8000;
    ep0_req_len = 1024;
    handle_dfu_upload(2, 1024);
    drain();
    check(captured_len == 1024 && memcmp(captured, flash + 0x8000, 1024) == 0,
          "block 2 follows the SET_ADDRESS pointer");

    /* A partial trailing block is clipped to the inclusive end of the region. */
    dfu.state = DFU_STATE_IDLE;
    dfuse_address_ptr = APP_FLASH_END - 255;   /* 256 bytes left */
    ep0_req_len = 1024;
    handle_dfu_upload(2, 1024);
    drain();
    check(captured_len == 256, "trailing partial block yields the last 256 bytes");
    check(memcmp(captured, flash + FLASH_LEN - 256, 256) == 0,
          "trailing partial block includes the final byte of flash");

    /* Reading past the end ends the upload rather than running off. */
    dfu.state = DFU_STATE_IDLE;
    dfuse_address_ptr = APP_FLASH_END - 255;
    ep0_req_len = 1024;
    handle_dfu_upload(3, 1024);
    drain();
    check(captured_len == 0, "block past the end of flash returns a ZLP");
    check(dfu.state == DFU_STATE_IDLE, "upload past the end returns to dfuIDLE");

    /* Blocks 0 and 1 are reserved and must not be served as data. */
    for (uint16_t b = 0; b < 2; b++) {
        dfu.state = DFU_STATE_IDLE;
        ep0_req_len = 1024;
        ep0_tx_remaining = 0;
        ep0_tx_need_zlp = 0;
        handle_dfu_upload(b, 1024);
        check(ep0_tx_remaining == 0,
              b == 0 ? "block 0 is stalled, not served as data"
                     : "block 1 is stalled, not served as data");
    }
}

/* ---- Descriptor self-consistency ------------------------------------------
 *
 * A descriptor whose declared length disagrees with its actual size is served
 * truncated or over-claimed, and the symptom is easy to miss: string2 declared
 * 24 for a 26-byte descriptor, so the product name enumerated as "Nova Voyage"
 * with the final character silently dropped.
 */

struct desc_case { const char *name; const uint8_t *data; size_t size; };

static void check_string_desc(uint8_t index, const char *name,
                              const uint8_t *data, size_t size) {
    char what[96];

    snprintf(what, sizeof what, "%s declares its real length", name);
    check(data[0] == size, what);

    /* Served in full to a host asking for everything. */
    ep0_req_len = 255;
    handle_get_descriptor((uint16_t)((USB_DESC_TYPE_STRING << 8) | index), 255);
    drain();
    snprintf(what, sizeof what, "%s is served in full (%zu bytes)", name, size);
    check(captured_len == size && memcmp(captured, data, size) == 0, what);

    /* And to the common host pattern of reading bLength, then asking for
     * exactly that many bytes. This is where an under-declared length costs
     * real characters: Linux trims to buf[0], so a 26-byte descriptor claiming
     * 24 loses its final UTF-16 character. */
    ep0_req_len = data[0];
    handle_get_descriptor((uint16_t)((USB_DESC_TYPE_STRING << 8) | index), data[0]);
    drain();
    snprintf(what, sizeof what, "%s survives a bLength-sized request", name);
    check(captured_len == size && memcmp(captured, data, size) == 0, what);
}

static void test_descriptors(void) {
    puts("\nDescriptor self-consistency");

    check(device_descriptor[0] == sizeof(device_descriptor),
          "device descriptor declares its real length");

    /* A configuration descriptor declares 9 for itself; wTotalLength covers
     * the interface and DFU functional descriptors that follow it. */
    check(config_descriptor[0] == 9, "config descriptor header is 9 bytes");
    check((config_descriptor[2] | (config_descriptor[3] << 8))
              == sizeof(config_descriptor),
          "config wTotalLength covers interface + DFU functional");

    check_string_desc(0, "string0 (langid)", string0, sizeof(string0));
    check_string_desc(1, "string1 (manufacturer)", string1, sizeof(string1));
    check_string_desc(2, "string2 (product)", string2, sizeof(string2));
    check_string_desc(3, "string3 (serial)", string3, sizeof(string3));
    check_string_desc(4, "string4 (DfuSe layout)", string4, sizeof(string4));

}

/* ---- Advertised geometry vs the real region --------------------------------
 *
 * The DfuSe layout string is a promise to the host about where it may write
 * and how coarsely it must erase. Two ways it has already been wrong: it
 * claimed 1KB sectors while the erase page is 2KB, and it claimed a region
 * reaching 0x0803FFFF while the last page is reserved. Both are silent from
 * the device side, so assert the promise against the constants that enforce it.
 */
static void test_layout_matches_region(void) {
    puts("\nAdvertised layout vs actual region");

    /* Decode the UTF-16LE descriptor back to ASCII. */
    char layout[64];
    size_t n = 0;
    for (size_t i = 2; i + 1 < sizeof(string4) && n < sizeof(layout) - 1; i += 2)
        layout[n++] = (char)string4[i];
    layout[n] = '\0';

    unsigned base = 0, sectors = 0, ksize = 0;
    char unit = 0;
    int got = sscanf(layout, "@Flash/0x%x/%u*%u%c", &base, &sectors, &ksize, &unit);

    printf("  layout string: %s\n", layout);
    check(got == 4, "layout string parses");
    check(base == APP_FLASH_START, "layout base == APP_FLASH_START");
    check(unit == 'K', "layout sector unit is K");

    /* Sector size must be the real erase page, or a host that interleaves
     * erase and write can wipe a sector it just programmed. */
    check(ksize * 1024u == FLASH_PAGE_SIZE,
          "advertised sector size == FLASH_PAGE_SIZE");

    /* And the region must stop where APP_FLASH_END does, so the reserved
     * settings page at 0x0803F800 stays out of reach of any host. */
    uint32_t advertised = (uint32_t)sectors * ksize * 1024u;
    uint32_t actual = APP_FLASH_END - APP_FLASH_START + 1;
    printf("  advertised %u bytes, region %u bytes\n", advertised, actual);
    check(advertised == actual, "advertised size == APP_FLASH_END region");
    check(APP_FLASH_END < 0x0803F800,
          "settings page 0x0803F800 is outside the DFU region");

    /* Emitted for run-tests.sh to check the README against. A full-region read
     * needs an explicit length, and that length has now drifted twice when the
     * region moved - once in the README, once in a note. */
    printf("DOCS-DFUSE-ADDRESS: 0x%08X:%u\n", APP_FLASH_START, actual);
}

/* ---- DFU download path -----------------------------------------------------
 *
 * The upload path is covered above and on hardware; the download path is the
 * one that writes flash, and until now nothing exercised it. These drive the
 * real DfuSe sequence - DNLOAD setup, OUT data in 64-byte packets, then the
 * two GETSTATUS polls that move DNLOAD_SYNC -> DNBUSY -> DNLOAD_IDLE and
 * perform the operation - against the shipping usb_dfu.c and flash_if.c.
 */

static void reset_dfu(void) {
    dfu.state = DFU_STATE_IDLE;
    dfu.status = DFU_STATUS_OK;
    dfu.address = APP_START_ADDRESS;
    dfuse_address_ptr = APP_START_ADDRESS;
    dfuse_cmd_pending = 0;
    dfuse_cmd_has_arg = 0;
    dfu_buffer_len = 0;
    ep0_tx_ptr = 0;
    ep0_tx_remaining = 0;
    ep0_tx_need_zlp = 0;
    ep0_req_len = 0;
}

/* A DNLOAD SETUP, with the preamble handle_setup() would have run. */
static void host_dnload(uint16_t block, uint16_t len) {
    ep0_tx_ptr = 0;
    ep0_tx_remaining = 0;
    ep0_tx_need_zlp = 0;
    ep0_req_len = len;
    handle_dfu_dnload(block, len);
}

/* OUT data, delivered as the bus would: EP0_MAX_SIZE per packet. */
static void host_data(const uint8_t *data, uint16_t len) {
    uint16_t sent = 0;
    while (sent < len) {
        uint16_t chunk = (uint16_t)(len - sent);
        if (chunk > EP0_MAX_SIZE) chunk = EP0_MAX_SIZE;
        pma_write(EP0_RX_OFFSET, data + sent, chunk);
        PMA_WRITE(BTABLE_OFFSET + 6, chunk);   /* COUNT_RX */
        handle_ep0_out();
        sent = (uint16_t)(sent + chunk);
    }
}

/* One GETSTATUS round trip; returns the bState the device reported. */
static uint8_t host_getstatus(void) {
    ep0_req_len = 6;
    handle_dfu_getstatus();
    drain();
    return captured_len >= 5 ? captured[4] : 0xFF;
}

static uint8_t last_poll_timeout(void) { return captured[1]; }
static uint8_t last_status(void)       { return captured[0]; }

/* Block 0 command: opcode plus optional 32-bit little-endian operand. */
static uint8_t host_command(uint8_t opcode, int with_addr, uint32_t addr) {
    uint8_t cmd[5];
    uint16_t len = with_addr ? 5 : 1;
    cmd[0] = opcode;
    cmd[1] = (uint8_t)(addr);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr >> 16);
    cmd[4] = (uint8_t)(addr >> 24);

    host_dnload(0, len);
    host_data(cmd, len);
    host_getstatus();          /* DNLOAD_SYNC -> DNBUSY */
    return host_getstatus();   /* DNBUSY -> execute */
}

static uint8_t host_data_block(uint16_t block, const uint8_t *data, uint16_t len) {
    host_dnload(block, len);
    host_data(data, len);
    host_getstatus();
    return host_getstatus();
}

static void test_download(void) {
    uint8_t buf[DFU_XFER_SIZE];
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 13 + 5);

    puts("\nDFU download path");

    /* --- the sequence a host actually uses --------------------------------- */
    reset_dfu();
    fill_flash();
    check(host_command(DFUSE_CMD_SET_ADDRESS, 1, APP_FLASH_START) == DFU_STATE_DNLOAD_IDLE,
          "SET_ADDRESS completes to dfuDNLOAD-IDLE");

    check(host_data_block(2, buf, DFU_XFER_SIZE) == DFU_STATE_DNLOAD_IDLE,
          "block 2 completes to dfuDNLOAD-IDLE");
    check(memcmp(flash, buf, DFU_XFER_SIZE) == 0,
          "block 2 lands at the address pointer");

    check(host_data_block(3, buf, DFU_XFER_SIZE) == DFU_STATE_DNLOAD_IDLE,
          "block 3 completes");
    check(memcmp(flash + DFU_XFER_SIZE, buf, DFU_XFER_SIZE) == 0,
          "block 3 lands one transfer further on");

    /* --- the regression the dfuse_cmd_arg split exists to prevent ---------- */
    reset_dfu();
    fill_flash();
    host_command(DFUSE_CMD_SET_ADDRESS, 1, APP_FLASH_START);
    host_data_block(2, buf, DFU_XFER_SIZE);

    /* An ERASE_PAGE arriving mid-download must not move the address pointer.
     * With the operand and the pointer sharing one variable, block 3 below
     * would land at 0x08020000 + 1024 instead of the base + 1024. */
    host_command(DFUSE_CMD_ERASE_PAGE, 1, APP_FLASH_START + 0x1D000);
    host_data_block(3, buf, DFU_XFER_SIZE);

    check(memcmp(flash + DFU_XFER_SIZE, buf, DFU_XFER_SIZE) == 0,
          "block 3 after an interleaved ERASE_PAGE still lands at base + 1KB");
    check(memcmp(flash + 0x1D000 + DFU_XFER_SIZE, buf, DFU_XFER_SIZE) != 0,
          "block 3 did NOT follow the erase address");

    /* --- multi-packet OUT accumulation ------------------------------------- */
    reset_dfu();
    fill_flash();
    host_command(DFUSE_CMD_SET_ADDRESS, 1, APP_FLASH_START);
    host_data_block(2, buf, DFU_XFER_SIZE);
    check(memcmp(flash, buf, DFU_XFER_SIZE) == 0,
          "1KB reassembled intact from 16 OUT packets");

    /* --- GETSTATUS sequencing ---------------------------------------------- */
    reset_dfu();
    fill_flash();
    host_command(DFUSE_CMD_SET_ADDRESS, 1, APP_FLASH_START);
    host_dnload(2, DFU_XFER_SIZE);
    host_data(buf, DFU_XFER_SIZE);
    check(host_getstatus() == DFU_STATE_DNBUSY, "first GETSTATUS reports dfuDNBUSY");
    check(last_poll_timeout() == 100, "dfuDNBUSY carries a 100ms bwPollTimeout");
    check(memcmp(flash, buf, DFU_XFER_SIZE) != 0,
          "nothing written yet at dfuDNBUSY - the host must poll again");
    check(host_getstatus() == DFU_STATE_DNLOAD_IDLE, "second GETSTATUS completes the write");
    check(memcmp(flash, buf, DFU_XFER_SIZE) == 0, "write happened on the second poll");

    /* --- refusals ----------------------------------------------------------- */
    reset_dfu();
    check(host_command(DFUSE_CMD_ERASE_PAGE, 0, 0) == DFU_STATE_ERROR,
          "bare ERASE_PAGE (mass erase) is refused");
    check(last_status() == DFU_STATUS_ERR_ERASE, "  ...reported as errErase");

    reset_dfu();
    check(host_command(DFUSE_CMD_SET_ADDRESS, 0, 0) == DFU_STATE_ERROR,
          "SET_ADDRESS with no operand is refused");
    check(last_status() == DFU_STATUS_ERR_ADDRESS, "  ...reported as errAddress");

    reset_dfu();
    check(host_command(0x99, 1, APP_FLASH_START) == DFU_STATE_ERROR,
          "unknown DfuSe command is refused");
    check(last_status() == DFU_STATUS_ERR_VENDOR, "  ...reported as errVendor");

    /* Erase inside the bootloader region: rejected by flash_is_address_valid,
     * which is the real guard in the shipping flash_if.c. */
    reset_dfu();
    check(host_command(DFUSE_CMD_ERASE_PAGE, 1, BOOTLOADER_START) == DFU_STATE_ERROR,
          "ERASE_PAGE inside the bootloader region is refused");
    check(last_status() == DFU_STATUS_ERR_ERASE, "  ...reported as errErase");

    /* And a write aimed there, via a SET_ADDRESS pointing at the bootloader. */
    reset_dfu();
    host_command(DFUSE_CMD_SET_ADDRESS, 1, BOOTLOADER_START);
    check(host_data_block(2, buf, DFU_XFER_SIZE) == DFU_STATE_ERROR,
          "write aimed at the bootloader region is refused");
    check(last_status() == DFU_STATUS_ERR_WRITE, "  ...reported as errWrite");

    /* The reserved settings page is outside APP_FLASH_END, so a write there
     * must be refused too - the structural half of the carve-out. */
    reset_dfu();
    host_command(DFUSE_CMD_SET_ADDRESS, 1, 0x0803F800);
    check(host_data_block(2, buf, DFU_XFER_SIZE) == DFU_STATE_ERROR,
          "write aimed at the reserved settings page is refused");

    /* --- guards that stall rather than error -------------------------------- */
    reset_dfu();
    host_command(DFUSE_CMD_SET_ADDRESS, 1, APP_FLASH_START);
    fill_flash();
    host_dnload(1, 64);
    check(dfu.state != DFU_STATE_DNLOAD_SYNC,
          "reserved block 1 is stalled, not accepted as data");

    reset_dfu();
    host_dnload(2, DFU_XFER_SIZE + 1);
    check(dfu.state == DFU_STATE_ERROR,
          "a wLength above wTransferSize is refused, not silently truncated");

    /* --- completion --------------------------------------------------------- */
    reset_dfu();
    host_command(DFUSE_CMD_SET_ADDRESS, 1, APP_FLASH_START);
    host_data_block(2, buf, DFU_XFER_SIZE);
    host_dnload(0, 0);          /* zero-length DNLOAD ends the download */
    check(dfu.state == DFU_STATE_MANIFEST_SYNC, "empty DNLOAD enters dfuMANIFEST-SYNC");
    check(host_getstatus() == DFU_STATE_MANIFEST_WAIT_RESET,
          "GETSTATUS then reports dfuMANIFEST-WAIT-RESET");
}

int main(void) {
    if (!map_flash()) {
        puts("could not map the flash region at its real address - cannot run");
        return 1;
    }

    test_packetisation();
    test_descriptors();
    test_layout_matches_region();
    test_upload();
    test_download();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
