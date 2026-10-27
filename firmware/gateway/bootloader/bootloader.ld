/*
 * Linker script of the Gateway bootloader: the 16 KB bootloader partition at the reset vector
 * (docs/hld/flash-partition-layout.md 5.1). Its stack and data live in SRAM1; the application
 * re-initialises all of RAM, so nothing is handed over through it.
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

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } >RAM

  /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
