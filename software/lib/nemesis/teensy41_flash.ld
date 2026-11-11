/* Teensy 4.1 linker script (Teensyduino 1.59 imxrt1062_t41.ld) with library
   code and the FAUST programs in flash, see the .text.progmem section.
   Use it with: board_build.ldscript = ../lib/nemesis/teensy41_flash.ld */

MEMORY
{
	ITCM (rwx):  ORIGIN = 0x00000000, LENGTH = 512K
	DTCM (rwx):  ORIGIN = 0x20000000, LENGTH = 512K
	RAM (rwx):   ORIGIN = 0x20200000, LENGTH = 512K
	FLASH (rwx): ORIGIN = 0x60000000, LENGTH = 7936K
	ERAM (rwx):  ORIGIN = 0x70000000, LENGTH = 16384K
}

ENTRY(ImageVectorTable)

SECTIONS
{
	.text.headers : {
		KEEP(*(.flashconfig))
		FILL(0xFF)
		. = ORIGIN(FLASH) + 0x1000;
		KEEP(*(.ivt))
		KEEP(*(.bootdata))
		. = ALIGN(1024);
	} > FLASH

	.text.code : {
		KEEP(*(.startup))
		*(.flashmem*)
		. = ALIGN(4);
		KEEP(*(.init))
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;
		__init_array_start = .;
		KEEP (*(.init_array))
		__init_array_end = .;
		. = ALIGN(4);
	} > FLASH

	.text.progmem : {
		*(.progmem*)
		/* NEMESIS: run from flash (through the 32 kB cache) instead of RAM1:
		   C++ and C library code, which is never used in the audio path, and
		   the generated FAUST programs, so that many of them fit in one
		   firmware. libm stays in RAM1, the DSP code calls it every sample. */
		*libstdc++*.a:*(.text*)
		*libsupc++*.a:*(.text*)
		*libc.a:*(.text*)
		*libc_nano.a:*(.text*)
		*libfaust_programs.a:*(.text*)
		/* only used from loop(), never in an interrupt: SD card and display
		   (keeps ITCM at 4 blocks: an odd count made DTCM slower, see
		   CLAUDE.md; SPI and ADC stay, the CV and pot interrupts use them) */
		*libSdFat.a:*(.text*)
		*libSD.a:*(.text*)
		*libSerialFlash.a:*(.text*)
		*libAdafruit*:*(.text*)
		. = ALIGN(4);
	} > FLASH

	/* NEMESIS: the unwind tables of that code would have to stay in ITCM,
	   too far from flash for their 31 bit offsets. They are only used by C++
	   exceptions, which the Teensy build doesn't use (-fno-exceptions). */
	/DISCARD/ : {
		*libstdc++*.a:*(.ARM.exidx* .ARM.extab*)
		*libsupc++*.a:*(.ARM.exidx* .ARM.extab*)
		*libc.a:*(.ARM.exidx* .ARM.extab*)
		*libc_nano.a:*(.ARM.exidx* .ARM.extab*)
		*libfaust_programs.a:*(.ARM.exidx* .ARM.extab*)
		*libSdFat.a:*(.ARM.exidx* .ARM.extab*)
		*libSD.a:*(.ARM.exidx* .ARM.extab*)
		*libSerialFlash.a:*(.ARM.exidx* .ARM.extab*)
		*libAdafruit*:*(.ARM.exidx* .ARM.extab*)
	}

	.text.itcm : {
		. = . + 32; /* MPU to trap NULL pointer deref */
		*(.fastrun)
		*(.text*)
		. = ALIGN(16);
	} > ITCM  AT> FLASH

	.ARM.exidx : {
		__exidx_start = .;
		*(.ARM.exidx* .ARM.extab.text* .gnu.linkonce.armexidx.*)
		__exidx_end = .;
	} > ITCM  AT> FLASH

	.data : {
		*(.endpoint_queue)   
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.rodata*)))
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.data*)))
		KEEP(*(.vectorsram))
	} > DTCM  AT> FLASH

	.bss ALIGN(4) : {
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(COMMON)))
		. = ALIGN(32);
		. = . + 32; /* MPU to trap stack overflow */
	} > DTCM

	.bss.dma (NOLOAD) : {
		*(.hab_log)
		*(.dmabuffers)
		. = ALIGN(32);
	} > RAM

	.bss.extram (NOLOAD) : {
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.externalram)))
		. = ALIGN(32);
	} > ERAM

	.text.csf : {
		FILL(0xFF)
		. = ALIGN(1024);
		KEEP(*(.csf))
		__text_csf_end = .;
	} > FLASH

	_stext = ADDR(.text.itcm);
	_etext = ADDR(.text.itcm) + SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx);
	_stextload = LOADADDR(.text.itcm);

	_sdata = ADDR(.data);
	_edata = ADDR(.data) + SIZEOF(.data);
	_sdataload = LOADADDR(.data);

	_sbss = ADDR(.bss);
	_ebss = ADDR(.bss) + SIZEOF(.bss);

	_heap_start = ADDR(.bss.dma) + SIZEOF(.bss.dma);
	_heap_end = ORIGIN(RAM) + LENGTH(RAM);

	_extram_start = ADDR(.bss.extram);
	_extram_end = ADDR(.bss.extram) + SIZEOF(.bss.extram);

	_itcm_block_count = (SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx) + 0x7FFF) >> 15;
	_flexram_bank_config = 0xAAAAAAAA | ((1 << (_itcm_block_count * 2)) - 1);
	_estack = ORIGIN(DTCM) + ((16 - _itcm_block_count) << 15);

	_flashimagelen = __text_csf_end - ORIGIN(FLASH);
	_teensy_model_identifier = 0x25;

	.debug_info     0 : { *(.debug_info) }
	.debug_abbrev   0 : { *(.debug_abbrev) }
	.debug_line     0 : { *(.debug_line) }
	.debug_frame    0 : { *(.debug_frame) }
	.debug_str      0 : { *(.debug_str) }
	.debug_loc      0 : { *(.debug_loc) }

}
