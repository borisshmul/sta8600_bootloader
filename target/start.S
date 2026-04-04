/*
 * start.S — STA8600 Second-Stage Bootloader Entry Point
 *
 * The STA8600 ROM bootloader loads this image to SYSRAM_BASE (0x20000000)
 * or FLASH_BASE (0x08000000) and jumps to the entry point.
 *
 * Processor: ARM Cortex-A7 (STA8600) running in AArch32 state.
 *
 * AArch32 exception vector table at 0x00 (VBAR set by ROM before handoff):
 *   0x00  Reset
 *   0x04  Undefined Instruction
 *   0x08  SVC
 *   0x0C  Prefetch Abort
 *   0x10  Data Abort
 *   0x14  Reserved
 *   0x18  IRQ
 *   0x1C  FIQ
 */

    .syntax unified
    .arch   armv7-a
    .cpu    cortex-a7

/* -----------------------------------------------------------------------
 * Vector table — must be at load address
 * --------------------------------------------------------------------- */
    .section ".vectors", "ax"
    .global  _vectors
_vectors:
    ldr  pc, =_reset_handler          /* Reset         */
    ldr  pc, =_undef_handler          /* Undef         */
    ldr  pc, =_svc_handler            /* SVC           */
    ldr  pc, =_pabt_handler           /* Prefetch Abort */
    ldr  pc, =_dabt_handler           /* Data Abort    */
    b    .                             /* Reserved      */
    ldr  pc, =_irq_handler            /* IRQ           */
    ldr  pc, =_fiq_handler            /* FIQ           */

/* -----------------------------------------------------------------------
 * Reset handler
 * --------------------------------------------------------------------- */
    .section ".text.reset", "ax"
    .global _reset_handler
    .type   _reset_handler, %function

_reset_handler:
    /* 1. Switch to SVC mode, disable IRQ/FIQ */
    cpsid  if
    mrs    r0, cpsr
    bic    r0, r0, #0x1F
    orr    r0, r0, #0x13            /* SVC mode                            */
    msr    cpsr_c, r0

    /* 2. Invalidate I-cache and branch predictor */
    mov    r0, #0
    mcr    p15, 0, r0, c7, c5, 0   /* ICIALLU — invalidate entire I-cache  */
    mcr    p15, 0, r0, c7, c5, 6   /* BPIALL  — invalidate BP array        */
    isb

    /* 3. Disable MMU and caches (in case ROM left them enabled) */
    mrc    p15, 0, r0, c1, c0, 0   /* Read SCTLR                           */
    bic    r0, r0, #(1 << 0)        /* Clear M (MMU)                        */
    bic    r0, r0, #(1 << 2)        /* Clear C (D-cache)                    */
    bic    r0, r0, #(1 << 12)       /* Clear I (I-cache)                    */
    mcr    p15, 0, r0, c1, c0, 0
    dsb
    isb

    /* 4. Set VBAR to our own vector table */
    ldr    r0, =_vectors
    mcr    p15, 0, r0, c12, c0, 0  /* VBAR                                  */
    isb

    /* 5. Set up SVC stack */
    ldr    sp, =_svc_stack_top

    /* 6. Set up IRQ stack */
    cps    #0x12                    /* IRQ mode                              */
    ldr    sp, =_irq_stack_top
    cps    #0x13                    /* Back to SVC                           */

    /* 7. Zero BSS */
    ldr    r0, =_bss_start
    ldr    r1, =_bss_end
    mov    r2, #0
.Lzero_bss:
    cmp    r0, r1
    bge    .Lbss_done
    str    r2, [r0], #4
    b      .Lzero_bss
.Lbss_done:

    /* 8. Copy data from flash (LMA) to RAM (VMA) */
    ldr    r0, =_data_start
    ldr    r1, =_data_end
    ldr    r2, =_data_load
.Lcopy_data:
    cmp    r0, r1
    bge    .Ldata_done
    ldr    r3, [r2], #4
    str    r3, [r0], #4
    b      .Lcopy_data
.Ldata_done:

    /* 9. Enable I-cache and branch predictor */
    mrc    p15, 0, r0, c1, c0, 0
    orr    r0, r0, #(1 << 12)       /* I-cache enable                       */
    orr    r0, r0, #(1 << 11)       /* Z: branch prediction enable          */
    mcr    p15, 0, r0, c1, c0, 0
    isb

    /* 10. Jump to C entry point */
    bl     bootloader_main
    /* Should never return; loop if it does */
    b      .

/* -----------------------------------------------------------------------
 * Default exception handlers  (can be overridden in C)
 * --------------------------------------------------------------------- */
    .macro  default_handler name
    .global \name
    .type   \name, %function
\name:
    b      .
    .endm

    default_handler _undef_handler
    default_handler _svc_handler
    default_handler _pabt_handler
    default_handler _dabt_handler
    default_handler _irq_handler
    default_handler _fiq_handler

/* -----------------------------------------------------------------------
 * Stack areas  (size configurable; placed in SYSRAM)
 * --------------------------------------------------------------------- */
    .section ".bss.stacks", "aw", %nobits
    .align  3

    .global _svc_stack
_svc_stack:
    .space  4096
    .global _svc_stack_top
_svc_stack_top:

    .global _irq_stack
_irq_stack:
    .space  1024
    .global _irq_stack_top
_irq_stack_top:
