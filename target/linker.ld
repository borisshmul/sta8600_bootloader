/*
 * linker.ld — STA8600 Second-Stage Bootloader Linker Script
 *
 * Memory layout:
 *   ROM         0x00000000  256 KB  (ST ROM bootloader, read-only)
 *   SYSRAM      0x20000000  512 KB  (BL2 data + stacks + heap)
 *   FLASH       0x08000000    4 MB  (BL2 code at 0x08000000, App at 0x08010000)
 *   OTP         0x1FFF0000    4 KB  (OTP / eFuse, written once)
 *   SEC_FLASH   0x0C000000        (TrustZone secure flash)
 *   SEC_RAM     0x30000000        (TrustZone secure RAM)
 *
 * BL2 is linked to execute from internal flash starting at 0x08000000.
 * If the ROM bootloader loads BL2 to SYSRAM first (XIP not available),
 * change LOAD_BASE to 0x20000000 and update load_addr accordingly.
 */

LOAD_BASE    = 0x08000000;   /* physical load address (LMA) */
EXEC_BASE    = 0x08000000;   /* virtual execution address (VMA) */
DATA_RAM     = 0x20000000;   /* where .data lives at runtime   */
SYSRAM_SIZE  = 0x00080000;   /* 512 KB                         */

MEMORY
{
    FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 64K   /* BL2 code region   */
    SYSRAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 512K  /* BL2 data region   */
}

ENTRY(_reset_handler)

SECTIONS
{
    /* ------------------------------------------------------------------ */
    /* Vector table — must be at the very start of flash                  */
    /* ------------------------------------------------------------------ */
    .vectors EXEC_BASE :
    {
        KEEP(*(.vectors))
    } > FLASH

    /* ------------------------------------------------------------------ */
    /* Read-only code and data                                             */
    /* ------------------------------------------------------------------ */
    .text :
    {
        *(.text.reset)
        *(.text*)
        *(.rodata*)
    } > FLASH

    /* Record the LMA of .data (used by startup to copy it to RAM) */
    _data_load = LOADADDR(.data);

    /* ------------------------------------------------------------------ */
    /* Initialised data — stored in FLASH, copied to SYSRAM by startup    */
    /* ------------------------------------------------------------------ */
    .data : AT (ADDR(.text) + SIZEOF(.text))
    {
        _data_start = .;
        *(.data*)
        _data_end = .;
    } > SYSRAM

    /* ------------------------------------------------------------------ */
    /* BSS — zero-initialised, lives only in SYSRAM                       */
    /* ------------------------------------------------------------------ */
    .bss :
    {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        *(.bss.stacks)    /* stack areas declared in start.S */
        _bss_end = .;
    } > SYSRAM

    /* ------------------------------------------------------------------ */
    /* Heap (optional; grows up from _bss_end)                            */
    /* ------------------------------------------------------------------ */
    .heap :
    {
        _heap_start = .;
        . = . + 8192;     /* 8 KB heap — adjust as needed */
        _heap_end   = .;
    } > SYSRAM

    /* Safety check: ensure we don't overflow SYSRAM */
    ASSERT(_heap_end <= 0x20000000 + SYSRAM_SIZE,
           "BL2: SYSRAM overflow — reduce BSS, heap, or stacks")

    /* Discard debugging sections to keep BL2 small */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.ARM.attributes)
    }
}
