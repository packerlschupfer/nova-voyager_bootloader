/**
 * @file main.c
 * @brief Nova Voyager USB DFU Bootloader
 *
 * Entry conditions for DFU mode:
 * 1. F1 button held at reset (PC10 = LOW). Sampled in main() on every reset
 *    path, not only power-on - a held F1 works across a soft reset too.
 * 2. Magic RAM value at 0x20004FF0 = 0x424C ("BL")
 * 3. No valid application at 0x08003000
 *
 * Otherwise, jump to application.
 */

#include <stdint.h>
#include "usb_dfu.h"
#include "flash_if.h"

/* Supplied by the linker script so the stack top and the DFU magic word have
 * exactly one definition each (see bootloader.ld). */
extern uint32_t _estack;
extern uint32_t _dfu_magic;

#define RAM_BASE        0x20000000
#define APP_ADDRESS     0x08003000

/* RCC registers */
#define RCC_BASE        0x40021000
#define RCC_CR          (*(volatile uint32_t*)(RCC_BASE + 0x00))
#define RCC_CFGR        (*(volatile uint32_t*)(RCC_BASE + 0x04))
#define RCC_APB2ENR     (*(volatile uint32_t*)(RCC_BASE + 0x18))
#define RCC_APB1ENR     (*(volatile uint32_t*)(RCC_BASE + 0x1C))

/* RCC_CR bits */
#define RCC_CR_HSEON    (1 << 16)
#define RCC_CR_HSERDY   (1 << 17)
#define RCC_CR_PLLON    (1 << 24)
#define RCC_CR_PLLRDY   (1 << 25)

/* System clock configuration */
#ifndef USE_120MHZ
#define USE_120MHZ 0  /* Set to 1 for 120MHz, 0 for 72MHz */
#endif

/* Test-only: never start the external oscillator, so the HSE failure path runs
 * on hardware that has a perfectly good crystal. Not for production - see
 * hse_start() below and the env:nova_bootloader_hsefail build. */
#ifndef HSE_FAIL_TEST
#define HSE_FAIL_TEST 0
#endif

/* RCC_CFGR bits */
#define RCC_CFGR_SW_PLL     0x02
#define RCC_CFGR_SWS_PLL    0x08
#define RCC_CFGR_PLLSRC_HSE (1 << 16)
#define RCC_CFGR_PLLMULL9   (7 << 18)   /* PLL x9 = 72MHz from 8MHz HSE */
#define RCC_CFGR_PLLMULL15  (13 << 18)  /* PLL x15 = 120MHz from 8MHz HSE (GD32 only) */
#define RCC_CFGR_PPRE1_DIV2 (4 << 8)    /* APB1 = HCLK/2 = 36MHz @ 72MHz */
#define RCC_CFGR_PPRE1_DIV4 (5 << 8)    /* APB1 = HCLK/4 = 30MHz @ 120MHz */

/* GPIO registers */
#define GPIOC_BASE      0x40011000
#define GPIOC_CRH       (*(volatile uint32_t*)(GPIOC_BASE + 0x04))
#define GPIOC_IDR       (*(volatile uint32_t*)(GPIOC_BASE + 0x08))
#define GPIOC_ODR       (*(volatile uint32_t*)(GPIOC_BASE + 0x0C))

/* APB2 enable bits */
#define RCC_APB2ENR_IOPCEN  (1 << 4)

/* F1 button on PC10 */
#define F1_BUTTON_PIN   (1 << 10)

/* Magic RAM address for software-triggered DFU. Placed by the linker outside
 * .data/.bss so neither startup nor the application can clear it. */
#define DFU_MAGIC_ADDR  (*(volatile uint32_t*)&_dfu_magic)
#define DFU_MAGIC_VALUE 0x424C  /* "BL" */

/* FLASH_ACR (wait states / prefetch) comes from flash_if.h */

/* AIRCR: system reset request */
#define AIRCR           (*(volatile uint32_t*)0xE000ED0C)
#define AIRCR_SYSRESETREQ 0x05FA0004

/* Iterations to wait for an RCC flag before giving up. At the 8MHz HSI this
 * loop is a few milliseconds - far longer than any crystal needs to start. */
#define RCC_TIMEOUT     200000u

/* Function prototypes */
void usb_poll(void);

/* Unused in the 120MHz build: the DFU paths that reset - manifestation, and the
 * reboot after reprogramming option bytes - are both compiled out there. */
static void __attribute__((unused)) system_reset(void) {
    AIRCR = AIRCR_SYSRESETREQ;
    while (1);
}

/* Bounded poll of an RCC flag. Returns 1 if it appeared, 0 on timeout. */
static int rcc_wait(volatile uint32_t *reg, uint32_t mask, uint32_t want) {
    for (uint32_t i = 0; i < RCC_TIMEOUT; i++) {
        if ((*reg & mask) == want)
            return 1;
    }
    return 0;
}

/* Start the external oscillator - or, in a test build, deliberately don't.
 *
 * HSE_FAIL_TEST exists because the failure path below is otherwise unreachable
 * without a physically dead crystal. Leaving HSEON clear means the bounded wait
 * in clock_init() polls a genuinely low HSERDY and genuinely times out: the
 * detection runs for real, it is not bypassed. (HSEON is already clear on entry
 * because a reset clears RCC_CR, but it is cleared explicitly here so the flag
 * behaves the same however this is reached.)
 *
 * What no build flag can establish is that a physically dead crystal drives
 * HSERDY the same way. That is documented behaviour for a stopped oscillator,
 * but an assumption here rather than a measurement. The supportable claim is
 * "fault response verified with HSERDY forced low", never "verified against a
 * dead crystal". */
static void hse_start(void) {
#if HSE_FAIL_TEST
    RCC_CR &= ~RCC_CR_HSEON;
    (void)rcc_wait(&RCC_CR, RCC_CR_HSERDY, 0);   /* wait for ready to fall */
#else
    RCC_CR |= RCC_CR_HSEON;
#endif
}

/* Fall back to the internal 8MHz oscillator after a clock setup failure.
 *
 * CFGR is reset outright so the state handed to the application is the same
 * whichever failure path got here. Without it the bus prescalers depend on how
 * far clock_init() managed to get - a HSE timeout leaves CFGR untouched at /1,
 * while a PLL or switch timeout leaves APB1 at /2 or /4 from the write that
 * already happened, so the app would inherit a 4MHz or 2MHz APB1 with nothing
 * to tell it which. Anything running on the failure path (a console message
 * saying the clock is dead, say) needs that to be predictable.
 *
 * SYSCLK is still HSI on every path that reaches here - none of them completed
 * the switch to PLL - so clearing SW is a no-op rather than a clock change.
 *
 * The application therefore always inherits: HSI at 8MHz, every prescaler /1,
 * zero flash wait states, HSE and PLL both off. */
static void clock_fallback_hsi(void) {
    RCC_CFGR = 0;                               /* SW = HSI, prescalers /1 */
    RCC_CR &= ~(RCC_CR_PLLON | RCC_CR_HSEON);
    FLASH_ACR = 0x30;  /* 0 wait states + prefetch enable, correct at 8MHz */
}

/* Configure system clock - 72MHz or 120MHz based on USE_120MHZ.
 *
 * Returns 1 if the PLL is running and selected, 0 if the external crystal or
 * the PLL failed to come up and the core is still on the 8MHz HSI. A dead
 * crystal must not wedge the one piece of code whose job is recovery, so every
 * wait here is bounded.
 *
 * Assumes the RCC reset state on entry: HSEON and HSERDY clear, PLL off. That
 * holds because the bootloader is only ever reached by a reset, and a reset
 * clears RCC_CR - unlike RCC_CSR's reset-cause flags, which deliberately
 * survive so software can tell POR from pin from soft reset.
 *
 * It would NOT hold for a caller entered by a jump with the PLL already
 * running. PLLMUL is read-only while PLLON is set, so the RCC_CFGR write below
 * would be silently dropped, the core would keep whatever multiplier it
 * inherited, and this function would report success at the wrong frequency.
 * nova-voyager_firmware hit exactly that - it is entered by jump_to_app()
 * rather than by a reset - and had to park on HSI and stop the PLL first.
 * Anything reusing this function outside a reset path needs that prologue. */
static int clock_init(void) {
#if USE_120MHZ
    /* 120MHz mode: HSE * 15 = 120MHz (GD32F303 only) */
    /* Flash wait states: 3 for 96-120MHz */
    FLASH_ACR = 0x33;  /* 3 wait states + prefetch enable */

    /* Enable HSE (8MHz external crystal) */
    hse_start();
    if (!rcc_wait(&RCC_CR, RCC_CR_HSERDY, RCC_CR_HSERDY)) {
        clock_fallback_hsi();
        return 0;
    }

    /* Configure PLL: HSE * 15 = 120MHz, APB1 = /4 (30MHz), APB2 = /1 (120MHz) */
    /* Note: USB won't work at 120MHz (can't divide to 48MHz cleanly) */
    RCC_CFGR = RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLMULL15 | RCC_CFGR_PPRE1_DIV4;

    /* Enable PLL */
    RCC_CR |= RCC_CR_PLLON;
    if (!rcc_wait(&RCC_CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY)) {
        clock_fallback_hsi();
        return 0;
    }

    /* Switch to PLL as system clock */
    RCC_CFGR |= RCC_CFGR_SW_PLL;
    if (!rcc_wait(&RCC_CFGR, 0x0C, RCC_CFGR_SWS_PLL)) {
        clock_fallback_hsi();
        return 0;
    }
    return 1;
#else
    /* 72MHz mode: HSE * 9 = 72MHz (STM32 compatible) */
    /* Flash wait states: 2 for 48-72MHz */
    FLASH_ACR = 0x32;  /* 2 wait states + prefetch enable */

    /* Enable HSE (8MHz external crystal) */
    hse_start();
    if (!rcc_wait(&RCC_CR, RCC_CR_HSERDY, RCC_CR_HSERDY)) {
        clock_fallback_hsi();
        return 0;
    }

    /* Configure PLL: HSE * 9 = 72MHz, APB1 = /2 (36MHz), USB = PLL/1.5 = 48MHz */
    RCC_CFGR = RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;

    /* Enable PLL */
    RCC_CR |= RCC_CR_PLLON;
    if (!rcc_wait(&RCC_CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY)) {
        clock_fallback_hsi();
        return 0;
    }

    /* Switch to PLL as system clock */
    RCC_CFGR |= RCC_CFGR_SW_PLL;
    if (!rcc_wait(&RCC_CFGR, 0x0C, RCC_CFGR_SWS_PLL)) {
        clock_fallback_hsi();
        return 0;
    }
    return 1;
#endif
}

/* Configure GPIO for F1 button */
static void gpio_init(void) {
    /* Enable GPIOC clock */
    RCC_APB2ENR |= RCC_APB2ENR_IOPCEN;

    /* Configure PC10 as input with pull-up */
    /* CRH controls pins 8-15, PC10 is bits [11:8] */
    uint32_t crh = GPIOC_CRH;
    crh &= ~(0xF << 8);    /* Clear PC10 bits */
    crh |= (0x8 << 8);     /* Input with pull-up/pull-down */
    GPIOC_CRH = crh;

    /* Enable pull-up on PC10 */
    GPIOC_ODR |= F1_BUTTON_PIN;
}

/* Check if F1 button is pressed (active low) */
static int f1_button_pressed(void) {
    return (GPIOC_IDR & F1_BUTTON_PIN) == 0;
}

/* Check if magic RAM value is set */
static int magic_ram_set(void) {
    return DFU_MAGIC_ADDR == DFU_MAGIC_VALUE;
}

/* Clear magic RAM value */
static void magic_ram_clear(void) {
    DFU_MAGIC_ADDR = 0;
}

/* Check if valid app exists */
static int app_valid(void) {
    uint32_t sp = *(volatile uint32_t*)APP_ADDRESS;
    uint32_t reset = *(volatile uint32_t*)(APP_ADDRESS + 4);

    /* SP must be in RAM. The upper bound is the real top of the 48KB SRAM, not
     * a round 64KB - an SP in the gap above it would fault on first push. */
    if (sp < RAM_BASE || sp > (uint32_t)&_estack)
        return 0;
    /* Reset must be in app flash */
    if (reset < APP_ADDRESS || reset > 0x08040000)
        return 0;
    return 1;
}

/* Jump to application - pure assembly */
__attribute__((noreturn, naked))
static void jump_to_app(void) {
    __asm volatile (
        /* Disable all interrupts */
        "cpsid i\n"

        /* Set VTOR to application vector table */
        "ldr r0, =0xE000ED08\n"   /* VTOR register address */
        "ldr r1, =0x08003000\n"   /* Application base address */
        "str r1, [r0]\n"          /* Write VTOR */
        "dsb\n"                   /* Data sync barrier */
        "isb\n"                   /* Instruction sync barrier */

        /* Load app SP and reset vector */
        "ldr r0, [r1, #0]\n"
        "ldr r2, [r1, #4]\n"

        /* Set stack and jump */
        "msr msp, r0\n"
        "bx r2\n"
    );
}

/* Nothing here can reach a USB host: boot the application if there is one, and
 * otherwise stop. Stopping is deliberate - a bootloader that resets in a loop
 * destroys the fault state that makes a dead board diagnosable, and the only
 * remaining route in is SWD either way. */
__attribute__((noreturn))
static void boot_app_or_stop(void) {
    if (app_valid())
        jump_to_app();
    while (1);
}

/* Run DFU mode */
static void run_dfu_mode(void) {
#if USE_120MHZ
    /* This build cannot do USB DFU, and says so rather than pretending.
     *
     * Full-speed USB needs 48MHz derived from the PLL, and a 120MHz PLL cannot
     * produce it: 120/1 and 120/1.5 are 120 and 80, neither is 48. Earlier
     * revisions brought the peripheral up anyway, which enumerates a broken
     * device - a host sees a failing 0483:DF11 and reads it as a defective DFU
     * implementation rather than as this build being incapable of DFU by
     * construction. Refusing is both honest and quieter.
     *
     * The practical effect is that F1-hold and the magic word do nothing on the
     * _120 build; it boots the application or stops. That environment exists
     * for ST-Link flashing and development and is documented as not usable as a
     * real bootloader.
     *
     * The USB code links out entirely here, since nothing references it. */
    clock_init();
    boot_app_or_stop();
#else
    /* Initialize clocks for USB */
    if (!clock_init()) {
        /* USB full-speed needs a precise 48MHz that only the HSE-fed PLL can
         * produce, so without the crystal there is no way to enumerate. Booting
         * the application is the only useful thing left; if there isn't one,
         * wait for SWD rather than spin up a USB device nobody can talk to. */
        boot_app_or_stop();
    }

    /* Lift flash write protection if the option bytes have it set. FLASH_WRPR
     * is only reloaded at reset, so a reprogram is worthless until we reboot -
     * re-arm the magic word first so we land straight back in DFU mode. */
    if (flash_disable_write_protection()) {
        DFU_MAGIC_ADDR = DFU_MAGIC_VALUE;
        system_reset();
    }

    /* Initialize USB DFU */
    dfu_init();

    volatile int reset_countdown = 0;

    /* Main DFU loop */
    while (1) {
        usb_poll();

        /* Check for manifestation complete - reset to run new app */
        if (dfu_get_state() == DFU_STATE_MANIFEST_WAIT_RESET) {
            if (reset_countdown == 0) {
                /* Start countdown - allow a few more USB polls for response to complete */
                reset_countdown = 50;
            } else {
                reset_countdown--;
                if (reset_countdown == 1) {
                    system_reset();
                }
            }
        }
    }
#endif /* USE_120MHZ */
}

int main(void) {
    /* Initialize GPIO for button check */
    gpio_init();

    /* Small delay for button debounce */
    for (volatile int i = 0; i < 10000; i++);

    /* Check DFU entry conditions */
    int enter_dfu = 0;

    /* Condition 1: F1 button held */
    if (f1_button_pressed()) {
        enter_dfu = 1;
    }

    /* Condition 2: Magic RAM value set */
    if (magic_ram_set()) {
        magic_ram_clear();
        enter_dfu = 1;
    }

    /* Condition 3: No valid application */
    if (!app_valid()) {
        enter_dfu = 1;
    }

    if (enter_dfu) {
        run_dfu_mode();
    } else {
        /* Bring up the PLL before jumping. If the crystal is dead we hand the
         * application an 8MHz HSI instead of hanging here - its own startup
         * code gets to decide what to do about that. */
        clock_init();
        jump_to_app();
    }

    /* Should never reach here */
    while (1);
}

/* Forward declarations */
void Reset_Handler(void);
void Default_Handler(void);

/* Vector table */
__attribute__((section(".isr_vector")))
const uint32_t vector_table[] = {
    (uint32_t)&_estack,             /* 0x00: Initial Stack Pointer */
    (uint32_t)Reset_Handler,        /* 0x04: Reset */
    (uint32_t)Default_Handler,      /* 0x08: NMI */
    (uint32_t)Default_Handler,      /* 0x0C: HardFault */
    (uint32_t)Default_Handler,      /* 0x10: MemManage */
    (uint32_t)Default_Handler,      /* 0x14: BusFault */
    (uint32_t)Default_Handler,      /* 0x18: UsageFault */
    0, 0, 0, 0,                     /* 0x1C-0x28: Reserved */
    (uint32_t)Default_Handler,      /* 0x2C: SVCall */
    (uint32_t)Default_Handler,      /* 0x30: DebugMon */
    0,                              /* 0x34: Reserved */
    (uint32_t)Default_Handler,      /* 0x38: PendSV */
    (uint32_t)Default_Handler,      /* 0x3C: SysTick */

    /* External interrupts: IRQ 0..59 occupy indices 16..75 (0x40..0x12C).
     * The table is filled out in full so a spurious interrupt lands on
     * Default_Handler instead of running off the end into .text.
     *
     * Nothing is enabled in the NVIC: USB is polled from run_dfu_mode() via
     * usb_poll(), so USB_LP_CAN1_RX0 (IRQ 20, index 36) deliberately has no
     * handler - an ISR calling usb_poll() would re-enter the main loop. */
    [16 ... 75] = (uint32_t)Default_Handler,
};

/* Reset handler */
__attribute__((naked))
void Reset_Handler(void) {
    __asm volatile (
        /* Set stack pointer (from the linker script) */
        "ldr r0, =_estack\n"
        "mov sp, r0\n"

        /* Copy .data section */
        "ldr r0, =_sdata\n"
        "ldr r1, =_edata\n"
        "ldr r2, =_sidata\n"
        "b 2f\n"
        "1: ldr r3, [r2], #4\n"
        "str r3, [r0], #4\n"
        "2: cmp r0, r1\n"
        "blo 1b\n"

        /* Zero .bss section */
        "ldr r0, =_sbss\n"
        "ldr r1, =_ebss\n"
        "mov r2, #0\n"
        "b 4f\n"
        "3: str r2, [r0], #4\n"
        "4: cmp r0, r1\n"
        "blo 3b\n"

        /* Call main */
        "bl main\n"
        "5: b 5b\n"
    );
}

/* Default interrupt handler */
void Default_Handler(void) {
    while (1) {
        __asm volatile ("nop");
    }
}
