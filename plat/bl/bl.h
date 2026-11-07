/* bl.h — EL BOOTLOADER PROPIO de senew (W580i, DB2020), v19. Lo comparten el BL (plat/bl/) y senew (la bandera de entrada), y es
 * el contrato con el anfitrión (herr/puente/pu.py, órdenes «bootloader» y «bl»). Documento: docs/bl.md.
 *
 * DÓNDE. El cargador de fábrica entra SIEMPRE en BL_INI (0x44140000), el primer bloque de 256 KiB del antiguo main: ahí van los 8
 * vectores, el bloque EMIF de +0x40 (el mismo del main: lo lee alguien de fuera) y la decisión. El main de senew va en
 * MAIN_INI..MAIN_FIN (0x44180000..0x443BFFFF, 9 bloques). El BL SOLO borra y programa ese tramo: jamás su propio bloque, ni el
 * W6FS (0x44400000), ni la Memoria (W6F), ni el FS de fábrica, ni el GDFS. La comprobación está en las propias rutinas de la NOR.
 *
 * LA DECISIÓN (bl_arranque.S, desde la NOR, sin tocar nada más):
 *   - la bandera de la iRAM (BL_BANDERA: dos palabras, BL_MAGIA y su complemento) puesta  -> modo flasheo;
 *   - el main sin vectores (la primera palabra no es «ldr pc, [pc, #0x18]» o su reset cae fuera del tramo) -> modo flasheo
 *     (así un flasheo cortado deja el teléfono en el BL: el anfitrión borra primero el bloque de MAIN_INI y graba la región 0 la
 *     última);
 *   - si no, pone la bandera a 0 y salta a MAIN_INI, que hace su arranque de siempre.
 * La bandera NO va en 0xF3002F80: ahí está _reg_reinicio_b (hal/enlace.ld), que hw_reiniciar() escribe justo antes de reiniciar.
 * Va en la misma iRAM, 128 bytes por debajo: lo que sobrevive a sys_reset allí sobrevive aquí.
 *
 * EL MODO FLASHEO corre ENTERO desde la ITCM (el código) y la DTCM (datos y pilas): nada se ejecuta desde la NOR mientras está en
 * modo de órdenes, y no se usa la SDRAM. Sube el reloj como senew (hal/reloj.S, pll_codigo: EMC a 52 MHz de bus y PLL a 208) y
 * habla USB con el MISMO núcleo (api/usb.c, -DUSB_BL: otros descriptores) y el MISMO driver (hal/usb_dc.c) que senew, sin IRQ
 * (usbdc_evento sondea). Al salir baja el reloj a 13 MHz (como reinicio_caliente) y vuelve a entrar por BL_INI.
 *
 * EL PROTOCOLO (peticiones de VENDEDOR al dispositivo por el EP0, como el puente; VID:PID 0x1209:0x0002):
 *   ID   0xD0 IN   BlId (64 B)                                   RES  0xD1 IN   BlRes (32 B)
 *   PUT  0xD2 OUT  wValue = desplazamiento en bl_buf, datos     (nivel 2: fase de datos OUT)
 *   ARG  0xD3..0xD6 OUT sin datos: arg[i] = wValue | wIndex << 16
 *   CUR  0xD7 OUT sin datos: cursor de POKE = wValue             POKE 0xD9 OUT sin datos: bl_buf[cursor] = wValue | wIndex << 16; cursor += 4
 *   GO   0xD8 OUT sin datos: ejecuta la orden wValue & 0xFF con arg[], número de secuencia wValue >> 8
 *   GET  0xDB IN   wValue | wIndex << 16 = dirección, wLength bytes (solo NOR, ITCM, DTCM e iRAM: nada de periféricos)
 * Una orden a la vez: GO la deja PENDIENTE y el bucle la ejecuta (fuera del manejador del USB); el anfitrión sondea RES hasta
 * HECHO con su número de secuencia. Mientras se borra o se programa el USB no se atiende: el anfitrión espera (plazo largo). */
#ifndef BL_H
#define BL_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;

#define BL_VERSION    1
#define BL_BANDERA    0xF3002F00u     /* iRAM: [0] = BL_MAGIA, [1] = ~BL_MAGIA */
#define BL_MAGIA      0x424C4F41u     /* 'BLOA' */
#define BL_VID        0x1209
#define BL_PID        0x0002          /* modo clásico (bandera): el host es pu_bl.py iterando senew */
#define BL_PID_INSTALLER 0x0003       /* modo installer (sin main): el host es la herramienta de primera grabación */
#define BL_BLOQUE     0x40000u        /* bloque de borrado del M18 */
#define BL_REGION     0x400u          /* región de programa (búfer de 512 palabras) */
#define BL_BUF        4096u           /* bl_buf: 4 regiones por orden */
#define BL_VEC_RESET  0xE59FF018u     /* ldr pc, [pc, #0x18]: la primera palabra de un main */

/* bRequest */
enum { BL_R_ID = 0xD0, BL_R_RES = 0xD1, BL_R_PUT = 0xD2, BL_R_ARG0 = 0xD3, BL_R_CUR = 0xD7, BL_R_GO = 0xD8, BL_R_POKE = 0xD9,
       BL_R_GET = 0xDB };
#define BL_N_ARG 4

/* las órdenes (GO).
 *   0..6: las de siempre, numeración densa (BL_OP_N marca el tope del rango clásico, lo usa bl_usb.c para validar).
 *   0xD4..0xD6: las del modo installer. Códigos EXPLÍCITOS (petición del mantenedor: «código 0xD4» para WRITE_PROPIO).
 *   El dispatch en bl_main.c acepta ambas familias por valor; el filtro de bl_usb.c permite las instaladoras solo con BL_INSTALLER. */
enum {
    BL_OP_NADA = 0,   /* eco: r0 = seq */
    BL_OP_WIPE,       /* borra los bloques de MAIN_INI..MAIN_FIN, EL DE MAIN_INI PRIMERO; r0 = bloques borrados */
    BL_OP_WRITE,      /* arg0 = dirección (múltiplo de BL_REGION), arg1 = bytes (múltiplo de BL_REGION, <= BL_BUF), desde bl_buf. Cada
                       * región tiene que estar en blanco; una región toda a 0xFF no se programa. Se relee: r0 = CRC-32 de lo que quedó */
    BL_OP_CRC,        /* arg0 = dirección, arg1 = bytes (las mismas zonas que GET): r0 = CRC-32 */
    BL_OP_SALIR,      /* contesta, suelta el USB, quita la bandera y vuelve a entrar por BL_INI (al main, si tiene vectores) */
#ifdef BL_RAM                         /* (27-oct) solo el BL en RAM lanzado desde el original (docs/senew_en_ram.md): */
    BL_OP_RAMW,       /* arg0 = dirección en la SDRAM (múltiplo de 4), arg1 = bytes (múltiplo de 4, <= BL_BUF), desde bl_buf; r0 = CRC-32 */
    BL_OP_IR,         /* arg0 = dirección de entrada (ARM, en la SDRAM): contesta, suelta el USB y salta. No vuelve */
#endif
    BL_OP_N,
    /* ---- INSTALLER (solo con BL_INSTALLER definido, y entrada = BL_ENTRADA_SIN_MAIN) --------------------------- */
    BL_OP_WRITE_PROPIO = 0xD4,  /* arg0 = dir, arg1 = bytes, desde bl_buf. Igual que BL_OP_WRITE pero el rango permitido se
                                 * amplía a [BL_INI, MAIN_FIN): puede grabar el bloque del propio BL. Para la «vuelta atrás»
                                 * (SENUS graba un R8BE001 entero) o para instalar un BL nuevo durante la primera grabación */
    BL_OP_INSTALADOR_INI = 0xD5,/* arg0 = total declarado en bytes. Pone el estado del instalador a (0 de total) y repinta la
                                 * pantalla con «Instalando...» + barra de 0 %. Cada WRITE/WRITE_PROPIO posterior suma su
                                 * progreso y repinta la barra. Sin pantalla (bl_pantalla.c no se enlazó): no-op */
    BL_OP_LISTO = 0xD6,         /* pinta «Listo. Reinicia.» y bloquea el bucle principal (el anfitrión manda SALIR para
                                 * reset-saltar al main nuevo). Sin pantalla: no-op */
    BL_OP_WIPE_PROPIO = 0xD7    /* arg0 = dirección DE UN BLOQUE DE 256 KiB en [BL_INI, MAIN_FIN) alineada; r0 = bloques (1).
                                 * Hace falta antes de un WRITE_PROPIO que sobreescriba un bloque ya con datos (p. ej. el
                                 * propio bloque del BL). SOLO con BL_INSTALLER (ver dispatch en bl_main). Es seguro borrar
                                 * el bloque del BL porque el BL corre desde ITCM/DTCM: nada se lee de la NOR mientras tanto */
};
enum { BL_OK = 0, BL_E_ORDEN = -1, BL_E_PARAM = -2, BL_E_RANGO = -3, BL_E_NO_BLANCO = -4, BL_E_NOR = -5, BL_E_VERIFICA = -6,
       BL_E_TIEMPO = -7, BL_E_MODO = -8 /* la orden solo vale en modo installer, y el BL está en modo clásico */ };
enum { BL_ST_LIBRE = 0, BL_ST_PENDIENTE, BL_ST_EJECUTANDO, BL_ST_HECHO };
enum { BL_ENTRADA_BANDERA = 1, BL_ENTRADA_SIN_MAIN = 2 };   /* BlId.entrada: por qué está en modo flasheo */

typedef struct {
    u8  estado, seq; u16 op;
    int codigo;                   /* BL_OK o un BL_E_* */
    u32 r0, r1;                   /* resultado; r1 = el SR de la NOR si codigo = BL_E_NOR, o la dirección donde falló */
    u32 ordenes;                  /* hechas desde que arrancó el BL */
    u32 ms;                       /* tiempo desde que arrancó el BL */
    u32 reserva[2];
} BlRes;                          /* 32 bytes */
typedef struct {
    char magia[4];                /* "SNBL" */
    u16  version, entrada;
    u32  bl_ini, main_ini, main_fin, bloque, region;
    u32  buf, buf_tam;
    u32  mhz;                     /* 208 si el PLL enganchó, 13 si no */
    u32  fallos, fallo_pc;        /* excepciones de la CPU desde el último arranque en frío del BL (y la última) */
    char build[16];               /* la compilación: "Oct 23 20:51:07" (mes, día y hora de __DATE__ / __TIME__) */
} BlId;                           /* 64 bytes: cabe en UN paquete del EP0 (nivel 0) */
_Static_assert(sizeof(BlRes) == 32 && sizeof(BlId) == 64, "herr/puente/pu.py usa estos tamaños");
#endif
