/* El bootloader propio (plat/bl/bl.h): UN bloque de la NOR en BL_INI (0x44140000..0x4417FFFF), donde entra el cargador de fábrica.
 * Lo que corre DESDE LA NOR es solo la entrada (.bl_entrada: vectores, EMIF y la decisión). El modo flasheo se copia entero a la
 * ITCM (código, en 0) y la DTCM (datos y pilas, en 0x10000, 16 KiB) y corre desde allí: nada se ejecuta desde la NOR mientras
 * está en modo de órdenes. BL_INI, MAIN_INI y MAIN_FIN llegan del Makefile (--defsym), los mismos que ve senew. */
ENTRY(_bl_inicio)
MEMORY
{
    NOR  (rx)  : ORIGIN = BL_INI,     LENGTH = 0x40000
    ITCM (rwx) : ORIGIN = 0x00000000, LENGTH = 0x10000
    DTCM (rw)  : ORIGIN = 0x00010000, LENGTH = 0x4000
}
SECTIONS
{
    .entrada : {
        KEEP(*(.bl_vectores))                    /* +0x00: 8 × ldr pc, [pc, #0x18] y sus 8 direcciones */
        KEEP(*(.bl_emif))                        /* +0x40: el bloque EMIF, 0x70 bytes */
        KEEP(*(.bl_entrada))                     /* +0xB0: la decisión y la copia a las TCM */
        . = ALIGN(4);
    } > NOR
    .itcm : {
        _itcm_ini = .;
        KEEP(*(.bl_vectores_itcm))               /* en 0: los vectores de las excepciones del modo flasheo */
        *(.text .text.*)
        *(.glue_7 .glue_7t .v4_bx)
        *(.rodata .rodata.*)
        . = ALIGN(4);
        _itcm_fin = .;
    } > ITCM AT > NOR
    _itcm_lma = LOADADDR(.itcm);
    .ARM.exidx : { *(.ARM.exidx*) } > ITCM AT > NOR
    .data : {
        _data = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > DTCM AT > NOR
    _data_lma = LOADADDR(.data);
    _bl_fin_nor = _data_lma + (_edata - _data);
    .bss (NOLOAD) : {
        _bss = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > DTCM
    .noinit (NOLOAD) : { *(.noinit) } > DTCM     /* lo que sobrevive a un reinicio del modo flasheo (las excepciones) */
    .pilas (NOLOAD) : {
        . = ALIGN(8);
        _pilas_ini = .;
        . += 0x200;  _pila_exc = .;              /* ABT y UND (y la de IRQ/FIQ, que no se abren) */
        . += 0xC00;  _pila_svc = .;              /* todo lo demás */
    } > DTCM
    ASSERT(_bl_fin_nor <= BL_INI + 0x40000, "el BL no cabe en su bloque")
    ASSERT(MAIN_INI >= BL_INI + 0x40000, "el main pisa el bloque del BL")
    ASSERT(MAIN_FIN <= 0x44400000, "MAIN_FIN pisa el W6FS (0x44400000)")
    ASSERT(_bl_emif == BL_INI + 0x40, "el bloque EMIF tiene que quedar en +0x40, donde lo tiene el main")
    ASSERT(_bl_reset == BL_INI + 0xB0, "la entrada tiene que empezar detras del EMIF")
    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
