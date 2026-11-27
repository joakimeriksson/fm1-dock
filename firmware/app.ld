/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Felucca memory map.
 * Code + constants execute in place from XIP (app.bin offset 0 = 0x02000120),
 * up to fm1-dock's slot table at 0x02014000 (flash 0x18000); the built-ins right after it.
 * 0x01C00000..0x01C04000 holds .ram_text (code that must run while the flash
 * is busy: the flash driver) and .c1_text (the second core's waiting loop,
 * hal/fm1_cpu1.h); 0x01C04000..0x01C08000 is a module's .ram_text.
 * RAM  : .data/.bss (96 KiB)
 * POOL : big zero-initialised buffers (FX lines, canvases), section .pool, 64 KiB;
 *        0x01C30000..0x01C74000 is the module RAM (firmware/dock/hb_module.h)
 * STACKS with 256-byte guard bands; NOINIT survives resets and UBOOT entry.
 * 0x01C7FD50.. is reserved: boot_info, boot-state mailbox, vector table. */
MEMORY
{
    XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x13EE0
    RAMTEXT (rwx): ORIGIN = 0x01C00000, LENGTH = 0x3F00   /* (its last 256 B: BLUETOOTH's .ram_text, HB_BLE_RT) */
    RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 64K   /* (to 0x01C18000: then BLUETOOTH's RAM, HB_BLE_RAM, 8 KiB, and an
                                                        * effect's, HB_FX_RAM, 24 KiB) */
    POOL   (rw)  : ORIGIN = 0x01C20000, LENGTH = 0x10000
    NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D40
}
SECTIONS
{
    .text : { KEEP(*(.text.crt0)) *(.text*) *(.rodata*) . = ALIGN(4); _etext = .; } > XIP
    .ram_text : { _rt_start = .; KEEP(*(.ram_text*)) . = ALIGN(4); } > RAMTEXT AT > XIP
    _rt_load = LOADADDR(.ram_text);
    .c1_text : { _c1_start = .; KEEP(*(.c1_text*)) . = ALIGN(4); _c1_end = .; } > RAMTEXT AT > XIP
    _c1_load = LOADADDR(.c1_text);
    _rt_end = _c1_end;                  /* the guard's RAM code window covers both (fm1_cpu1.h) */
    .data : { . = ALIGN(4); _data_start = .; *(.data*) *(.sdata*) . = ALIGN(4); _data_end = .; } > RAM AT > XIP
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : { _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    .pool (NOLOAD) : { _pool_start = .; *(.pool*) . = ALIGN(4); _pool_end = .; } > POOL
    .noinit (NOLOAD) : { *(.noinit*) } > NOINIT
    _guard0 = 0x01C74000; _ustack_lo = 0x01C74100; _ustack_top = 0x01C7A000;
    _guard1 = 0x01C7A000; _sstack_lo = 0x01C7A100; _sstack_top = 0x01C7C000;
    /DISCARD/ : { *(.comment) *(.note*) }
}
