/* SPDX-License-Identifier: GPL-3.0-only */
/* BLUETOOTH, the BLE-MIDI add-on (firmware/dock/hb_module.h "BLUETOOTH", firmware/src/ble/hx.c), linked as a module
 * is (tools/hbmod.py: twice, at two bases, for its relocations).
 *   XIP   the image at BASE: the descriptor (HX's hx_api_t) first, code and constants, .data's load image, .ram_text's
 *   BRAM  .data + .bss (its .noinit too: cleared at boot) at HB_BLE_RAM, the host's 8 KiB for it
 *   BRT   .ram_text at HB_BLE_RT, the last 256 B of the host's RAM for code (the radio's start-up burst,
 *         hal/fm1_ble_rf.h: called through a pointer) */
MEMORY
{
    XIP  (rx)  : ORIGIN = @BASE@, LENGTH = 0x20000
    BRAM (rw)  : ORIGIN = 0x01C18000, LENGTH = 0x2000
    BRT  (rwx) : ORIGIN = 0x01C03F00, LENGTH = 0x100
}
SECTIONS
{
    .text : { KEEP(*(.hb_desc)) *(.text*) *(.rodata*) . = ALIGN(4); _hb_text_end = .; } > XIP
    .data : { . = ALIGN(4); _hb_data_start = .; *(.data*) *(.sdata*) . = ALIGN(4); _hb_data_end = .; } > BRAM AT > XIP
    _hb_data_load = LOADADDR(.data);
    .ram_text : { . = ALIGN(4); _hb_rt_start = .; KEEP(*(.ram_text*)) . = ALIGN(4); } > BRT AT > XIP
    _hb_rt_load = LOADADDR(.ram_text);
    .bss (NOLOAD) : { . = ALIGN(4); *(.bss*) *(.sbss*) *(COMMON) *(.noinit*) . = ALIGN(4); _hb_bss_end = .; } > BRAM
    /DISCARD/ : { *(.comment) *(.note*) }
}
