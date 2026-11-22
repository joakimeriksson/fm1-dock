/* SPDX-License-Identifier: GPL-3.0-only */
/* A fm1-dock effect (firmware/dock/hb_module.h, "Effects"), linked as a module is (tools/hbmod.py: twice, at two
 * bases, for its relocations).
 *   XIP   the image at BASE: the descriptor (an hb_fx_t) first, code and constants, then .data's load image
 *   FRAM  .data + .bss at HB_FX_RAM: the effect's own RAM, 24 KiB of the host's (a synth keeps the module RAM)
 * No .ram_text: the RAM for code is the synth's. */
MEMORY
{
    XIP  (rx)  : ORIGIN = @BASE@, LENGTH = 0x20000
    FRAM (rw)  : ORIGIN = 0x01C1A000, LENGTH = 0x6000
}
SECTIONS
{
    .text : { KEEP(*(.hb_desc)) *(.text*) *(.rodata*) . = ALIGN(4); _hb_text_end = .; } > XIP
    .data : { . = ALIGN(4); _hb_data_start = .; *(.data*) *(.sdata*) . = ALIGN(4); _hb_data_end = .; } > FRAM AT > XIP
    _hb_data_load = LOADADDR(.data);
    _hb_rt_start = 0x01C04000;                  /* (none: tools/hbmod.py finds it empty) */
    _hb_rt_load = _hb_data_load;
    .bss (NOLOAD) : { . = ALIGN(4); *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _hb_bss_end = .; } > FRAM
    /DISCARD/ : { *(.comment) *(.note*) *(.ram_text*) }
}
