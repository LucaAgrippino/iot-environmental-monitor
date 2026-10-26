/*
 * Linker script of the Slot-A boot stub: 16 KB at the start of the flash, where the
 * bootloader will one day live (docs/hld/flash-partition-layout.md 5.1).
 */

ENTRY(boot_reset_handler)

_estack = 0x20018000; /* end of the 96 KB SRAM1 */

MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 16K
}

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } >FLASH

  .text :
  {
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
