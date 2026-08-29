/*
 * Linker script for Nova Voyager USB DFU Bootloader
 * GD32F303RCT6 - 256KB Flash, 48KB SRAM, Cortex-M4, 72MHz
 * (Bootloader stays at 72MHz for USB 48MHz requirement)
 *
 * Bootloader: 0x08000000 - 0x08002FFF (12KB)
 * Application: 0x08003000+ (244KB)
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 12K
    RAM (xrw)   : ORIGIN = 0x20000000, LENGTH = 48K
}

/* Entry point */
ENTRY(Reset_Handler)

/* Stack at end of RAM. Referenced by the vector table and Reset_Handler in
   main.c so the stack top has exactly one definition. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/* Magic word the application writes before a soft reset to request DFU mode.
   It lives outside .data/.bss on purpose: the bootloader's startup code must
   not zero it, and it has to survive the reset. The ASSERT after SECTIONS
   keeps .bss from ever growing onto it. */
_dfu_magic = 0x20004FF0;

/* Minimum heap and stack sizes */
_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;

SECTIONS
{
    /* Vector table must be at start of flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    /* Program code */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)

        KEEP(*(.init))
        KEEP(*(.fini))

        . = ALIGN(4);
        _etext = .;
    } >FLASH

    /* Read-only data */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    /* ARM exception handling */
    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } >FLASH

    .ARM :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } >FLASH

    /* Used by startup to initialize data */
    _sidata = LOADADDR(.data);

    /* Initialized data */
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT> FLASH

    /* Uninitialized data (BSS) */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } >RAM

    /* User heap */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } >RAM

    /* Remove debug info */
    /DISCARD/ :
    {
        *(.comment)
        *(.debug*)
    }
}

/* The bootloader must fit its 12KB region and must not collide with the DFU
   magic word. Both are cheap to check here and expensive to discover on a
   bricked board. */
ASSERT(_ebss <= _dfu_magic,
       "bootloader .bss has grown onto the DFU magic word at 0x20004FF0")
