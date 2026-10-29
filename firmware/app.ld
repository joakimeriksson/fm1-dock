/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Felucca memory map.
 * Code + constants execute in place from XIP (app.bin offset 0 = 0x02000120),
 * up to fm1-dock's slot table at 0x02030000 (flash 0x34000).
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
    XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x2FEE0
    HX     (rx)  : ORIGIN = 0x02084000, LENGTH = 0xA000   /* flash 0x88000: BLE-MIDI (src/ble/hx.h) */
    RAMTEXT (rwx): ORIGIN = 0x01C00000, LENGTH = 0x4000
    RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 96K
    POOL   (rw)  : ORIGIN = 0x01C20000, LENGTH = 0x10000
    NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D40
    HBGO   (rw)  : ORIGIN = 0x01C7FD40, LENGTH = 0x10    /* fm1-dock's boot slot counter: above a boot firmware's NOINIT */
}
SECTIONS
{
    /* HX (src/ble/hx.h): its header first, its code and constants; its .data loaded behind them, copied by
     * HX itself (crt0 copies only the host's); its .bss, .noinit and .ram_text are the host's sections' */
    .hx : { KEEP(*(.hx_head)) *hx.o(.text* .rodata*) . = ALIGN(4); } > HX
    .hx_data : { . = ALIGN(4); _hx_data_start = .; *hx.o(.data* .sdata*) . = ALIGN(4); _hx_data_end = .; } > RAM AT > HX
    _hx_data_load = LOADADDR(.hx_data);
    _hx_end = LOADADDR(.hx_data) + SIZEOF(.hx_data);
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
    .hbgo (NOLOAD) : { *(.hbgo*) } > HBGO
    _guard0 = 0x01C74000; _ustack_lo = 0x01C74100; _ustack_top = 0x01C7A000;
    _guard1 = 0x01C7A000; _sstack_lo = 0x01C7A100; _sstack_top = 0x01C7C000;
    /DISCARD/ : { *(.comment) *(.note*) }
}
