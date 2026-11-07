/* Main propio del W580i: imagen cruda en MAIN_INI (NOR, CS0), datos en la SDRAM (0x20000000, 16 MiB).
 * (BL, 2026-10-23) MAIN_INI, MAIN_FIN y BL_INI llegan del Makefile (--defsym): desde v19 el main va en 0x44180000, detrás del
 * bootloader propio (BL_INI = 0x44140000, donde entra el cargador de fábrica), y no puede pasar de MAIN_FIN (0x443C0000).
 * Mapa: docs/hal.md §1.4 (tabla 0x45A8EC4C del original). La ITCM (0, 64 KiB) solo lleva los vectores y las
 * rutinas que retemporizan la memoria; la DTCM (0x10000, 16 KiB) no se usa tras el arranque; la iRAM (0xF3000000) solo
 * guarda, desde R3, el registro de causa del último reinicio (abajo, _reg_reinicio_a/_b) y las cuentas del main de medida. */
ENTRY(_inicio)
MEMORY
{
    FLASH (rx)  : ORIGIN = MAIN_INI, LENGTH = NOR_W6F_INI - MAIN_INI   /* (2026-10-14) hasta 0x44500000 (NOR_W6F_INI; antes 0x44B00000); (BL) el tope de verdad es MAIN_FIN (ASSERT abajo) */
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00E80000   /* la SDRAM (16 MiB) menos la memoria gráfica del final */
    GRAM  (rw)  : ORIGIN = 0x20E80000, LENGTH = 0x00180000   /* donde el original tiene los cuadros que lee el PDICON (hal.md §4.4) */
}
SECTIONS
{
    .text : {
        KEEP(*(.vectores))
        *(.text.arranque)
        *(.text .text.*)
        *(.glue_7 .glue_7t .v4_bx)
        *(.rodata .rodata.*)
        /* Descriptores de las APPS EXTERNAS (docs/sistema.md): cada una deja un `const AppExterna`
         * en .apps_ext y el shell recorre el rango. Sin ninguna, los dos símbolos valen lo mismo. */
        . = ALIGN(4);
        _apps_ext = .;
        KEEP(*(apps_ext))
        _apps_ext_fin = .;
        . = ALIGN(4);
    } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH
    _data_flash = LOADADDR(.data);
    .data : {
        _data = .;
        *(.data .data.*)
        *(.ramfn)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH
    /* El final REAL de la imagen en la flash: lo usa plat/hw_w580.c para no buscar nuestro FS (W6FS) encima del main. */
    _fin_flash = _data_flash + (_edata - _data);
    /* (I1; W6F, auditoría AUD5 M6) NOR_W6F_INI llega del Makefile (--defsym, el mismo valor que ve el C): el main no puede
     * pisar la zona de W6F aunque alguien la mueva con W6F_INI=… y se olvide del LENGTH de arriba. */
    ASSERT(_fin_flash <= NOR_W6F_INI, "el main pisa la zona de W6F (NOR_W6F_INI)")
    ASSERT(ORIGIN(FLASH) + LENGTH(FLASH) <= NOR_W6F_INI, "LENGTH de FLASH pasa de NOR_W6F_INI: corregir hal/enlace.ld")
    /* (BL) el BL de plat/bl/ solo borra y programa [MAIN_INI, MAIN_FIN): un main más largo no se podría grabar por él */
    ASSERT(MAIN_INI == BL_INI || _fin_flash <= MAIN_FIN, "el main pasa de MAIN_FIN: el bootloader no lo podria grabar")
    ASSERT(MAIN_INI == BL_INI || MAIN_INI >= BL_INI + 0x40000, "el main pisa el bloque del bootloader")
    .bss (NOLOAD) : {
        _bss = .;
        /* (R3; propuesta P6 de R2) LA TABLA DE TRADUCCIÓN DE LA MMU, LA PRIMERA DEL .bss (hal/reloj.c, 16 KiB alineados a 16 KiB):
         * antes caía donde quisiera el enlazador y acabó justo DEBAJO de la pila de IRQ: un desborde de esa pila pisaba la
         * tabla (y con la tabla rota no hay ni pantalla de fallo). Aquí queda al principio de la RAM de datos, con todo el
         * .bss entre ella y las pilas. El relleno hasta los 16 KiB es RAM sin usar (menos de 16 KiB), no flash. */
        *(.bss.ttb)
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
    .pilas (NOLOAD) : {
        . = ALIGN(8);
        _pilas_ini = .;
        . += 0x1000;  _pila_irq = .;      /* IRQ: crece hacia abajo, hasta _pilas_ini (allí va un testigo) */
        . += 0x4000;  _pila_svc = .;      /* SVC (todo el sistema): hasta _pila_irq */
        . += 0x1000;  _pila_fallo = .;    /* la de la pantalla de fallo: nadie más la usa */
    } > RAM
    _fin = .;
    /* El cuadro que lee el PDICON, en 0x20E80000: el ÚNICO sitio (con 0x20EA5800) que el original le da nunca
     * (PDICON+50 = 0x00E80000). Hallazgo M7 de la pista R: antes iba en .bss, en un desplazamiento cualquiera, y
     * [sin saber] si el PDICON de verdad lee de cualquier sitio. No se pone a cero en el arranque: lo hace plat. */
    .gram (NOLOAD) : { *(.gram) } > GRAM
    /* (R3; propuesta P3) EL REGISTRO DE CAUSA DEL ÚLTIMO REINICIO (hal/reinicio.c): dos copias de 24 bytes en la iRAM del SoC
     * (0xF3000000, 16 KiB), que el arranque NO borra: hal/arranque.S solo usa de ella 0xF3000000..0xF300020F (pilas
     * provisionales). Lejos de ese tramo y lejos una de otra, por si el cargador de arranque pisa alguna [sin saber]. */
    _reg_reinicio_a = 0xF3003F80;
    _reg_reinicio_b = 0xF3002F80;
    _bl_ini = BL_INI;                         /* (BL) hal/arranque.S (reinicio_caliente) vuelve a entrar por aquí, como tras un reset de verdad */
    _ram_fin = ORIGIN(RAM) + LENGTH(RAM);     /* shell/app_externa.c: hasta dónde llega el bloque de memoria de una app externa */
    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
