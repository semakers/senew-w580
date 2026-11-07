/* El PUENTE de depuración por USB (senew v16): memoria, registros, llamadas a funciones y código nuevo en RAM, SIN regrabar.
 * Documento: docs/puente.md. Herramienta del anfitrión: herr/puente/pu.py. Solo existe con -DPUENTE (compilaciones de
 * DESARROLLO): da control total del teléfono a quien lo enchufe, así que una compilación normal no lleva nada de esto (sin la
 * bandera, api/puente.c queda vacío y el binario sale idéntico byte a byte).
 *
 * POR QUÉ POR EL EP0. El control (EP0) ya funciona en el aparato (la Mac enumera y lee los almacenes MTP), no necesita
 * interfaz ni endpoints nuevos, no cambia ni un byte de los descriptores (el MTP sigue como estaba) y no hay que «reclamar» nada
 * en el anfitrión: son peticiones de VENDEDOR dirigidas al dispositivo. Se recuperan solas: un SETUP nuevo anula cualquier
 * transferencia a medias (hal/usb_dc.h). El coste es la velocidad (algunos cientos de KB/s como mucho), que aquí sobra.
 *
 * EL MODELO. Hay un búfer de intercambio (pu_buf) y UNA orden a la vez:
 *   PUT  (OUT) llena pu_buf desde el anfitrión;   GET (IN) lo lee;
 *   CMD  (OUT) deja una orden (op + 6 palabras);  RES (IN) da su estado y su resultado (el anfitrión sondea hasta «hecho»);
 *   LOG  (IN)  da lo que el sistema escribió con hw_log, desde una posición ABSOLUTA (no se pierde nada si una lectura falla);
 *   ID   (IN)  firma, versión, dónde están el búfer y la zona de código, y el identificador de la compilación.
 * La orden NO se ejecuta dentro del manejador del USB sino en puente_paso(), que llama shell/usb.c desde el bucle principal
 * (la misma vuelta que atiende al MTP), con las IRQ abiertas y el perro estirado. Una excepción de la CPU dentro de una orden
 * (dirección mala, instrucción indefinida) NO tumba el sistema: vuelve al puente con el tipo, el PC y la dirección (el estado
 * 4 de RES). LO QUE NO PUEDE: un acceso que CUELGA EL BUS (un bloque sin reloj: así murió v14) no da excepción; ahí solo queda
 * sacar la pila. */
#ifndef PUENTE_H
#define PUENTE_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;

#define PU_VERSION 1
#define PU_BUF     8192u      /* pu_buf */
#define PU_CODIGO  32768u     /* pu_codigo: donde el anfitrión sube código compilado (alineado a 32 para la caché) */
#define PU_LOG_TAM 8192u      /* potencia de 2 */
#define PU_LOG_MAX 2040u      /* datos por respuesta de LOG (más 8 de cabecera) */

/* bRequest de las peticiones de vendedor (bmRequestType 0x40 = OUT, 0xC0 = IN, al dispositivo). 0x54 es el de MS OS (api/usb.h) */
enum { PU_R_ID = 0xB0, PU_R_PUT = 0xB1, PU_R_GET = 0xB2, PU_R_CMD = 0xB3, PU_R_RES = 0xB4, PU_R_LOG = 0xB5,
       PU_R_ARG0 = 0xC0,     /* 0xC0..0xC5: OUT SIN fase de datos: p[bRequest - 0xC0] = wValue | wIndex << 16 */
       PU_R_GO = 0xC8 };     /* OUT sin datos: ejecuta con los p[] puestos: wValue = op | seq << 8, wIndex = flags */
/*  PUT: wValue = desplazamiento en pu_buf, datos = wLength bytes.      GET: wValue = desplazamiento, wLength = cuántos.
 *  CMD: datos = PuCmd (28 bytes).   RES: IN de PuRes (48 bytes).   ID: IN de PuId (60 bytes).
 *  LOG: wValue = posición absoluta (16 bajos), wIndex = (16 altos), wLength = máximo; IN de {u32 inicio, u32 total, datos…}.
 *
 * TRES NIVELES (el anfitrión elige al conectar, `pu.py info`), porque de lo que hace el EP0 en el aparato solo está probado lo
 * mínimo (SETUP, una fase de datos IN de UN paquete —los descriptores— y el estado): una fase de datos IN de varios paquetes y
 * una fase de datos OUT NO se han visto en silicio.
 *   nivel 0  solo lo probado: respuestas de ≤ 64 bytes (GET y LOG en trozos de 64) y órdenes SIN fase de datos OUT (ARG + GO;
 *            para escribir, PU_OP_POKE);
 *   nivel 1  respuestas de varios paquetes (GET de KiB);
 *   nivel 2  además fase de datos OUT (PUT y CMD de una vez). */

/* las órdenes (PuCmd.op) */
enum {
    PU_OP_NADA = 0,       /* eco: r0 = seq */
    PU_OP_LEER,           /* p0 = dirección, p1 = bytes, p2 = ancho (0 = automático, 1, 2 o 4)  ->  pu_buf[0..p1) */
    PU_OP_ESCRIBIR,       /* p0 = dirección, p1 = bytes, p2 = ancho   <-  pu_buf[0..p1). Limpia la caché de datos e invalida la de instrucciones */
    PU_OP_LLAMAR,         /* p0 = función (bit 0 = Thumb), p1..p4 = r0..r3, p5 = plazo del perro en s (0 = 30)  ->  r0 = lo que devuelve */
    PU_OP_RMW,            /* p0 = dirección, p1 = bits a poner a 0, p2 = bits a poner a 1, p3 = ancho (1, 2, 4)  ->  r0 = antes, r1 = después */
    PU_OP_TECLA,          /* p0 = código K_*, p1 = 0 suelta, 1 pulsa y mantiene, 2 pulsa y suelta a los p2 ms (0 = 120) */
    PU_OP_REINICIAR,      /* hw_reiniciar(): el teléfono arranca otra vez (el USB se va; hay que reconectar) */
    PU_OP_SYNC,           /* p0 = inicio, p1 = fin: baja la caché de datos de ese tramo e invalida la de instrucciones */
    PU_OP_DISPERSA,       /* p0 = cuántas, p1 = ancho: pu_buf lleva p0 direcciones (u32); vuelven p0 valores (u32) en pu_buf */
    PU_OP_POKE,           /* p0 = dirección, p1 = ancho (1, 2, 4) | cuántos (1..4) << 8, p2..p5 = los valores: escribe SIN usar pu_buf (nivel 0: sin fase de datos OUT) */
    PU_OP_N
};
#define PU_F_PELIGRO 0x0001u  /* bandera de PuCmd: permite escribir en la NOR (0x44000000..0x47FFFFFF) */

/* códigos de error (PuRes.codigo; negativos) */
enum { PU_OK = 0, PU_E_ORDEN = -1, PU_E_PARAM = -2, PU_E_ALINEA = -3, PU_E_NOR = -4, PU_E_RAMA = -5 };

typedef struct { u8 op, seq; u16 flags; u32 p[6]; } PuCmd;                       /* 28 bytes */
enum { PU_ST_LIBRE = 0, PU_ST_PENDIENTE, PU_ST_EJECUTANDO, PU_ST_HECHO, PU_ST_FALLO_CPU };   /* PuRes.estado */
typedef struct {
    u8  estado, seq; u16 op;
    int codigo;                  /* PU_OK o un PU_E_* */
    u32 r0, r1;                  /* resultado (LLAMAR: lo que devuelve; RMW: antes y después) */
    u32 bytes;                   /* cuántos bytes útiles dejó en pu_buf */
    u32 falta_tipo, falta_pc, falta_dir, falta_lr, falta_cpsr;   /* excepción de la CPU: 1 indefinida, 3 prebúsqueda, 4 datos */
    u32 ms;                      /* hw_ms() al acabar */
    u32 ordenes;                 /* cuántas órdenes se han hecho desde el arranque */
} PuRes;                         /* 48 bytes */
typedef struct {
    char magia[4];               /* "PUEN" */
    u16  version, flags;
    u32  buf, buf_tam, codigo, codigo_tam;
    u32  log_total, ms;
    char build[28];              /* "PUEN1 " __DATE__ " " __TIME__ de api/puente.c: el anfitrión lo compara con el ELF (símbolo pu_ident) */
} PuId;                          /* 60 bytes: cabe en UN paquete del EP0 (nivel 0) */

_Static_assert(sizeof(PuCmd) == 28 && sizeof(PuRes) == 48 && sizeof(PuId) == 60, "el anfitrión (herr/puente/pu.py) usa estos tamaños");

#ifdef PUENTE
/* ---- lo que usa api/usb.c (peticiones de vendedor por el EP0) ---- */
enum { PU_USB_NO_ES_MIA = 0, PU_USB_IN, PU_USB_OUT, PU_USB_LISTO, PU_USB_STALL };
/* s = los 8 bytes del SETUP. IN: *p y *n = lo que hay que devolver. OUT: *p = dónde dejar los datos (caben *n). LISTO: nada más que el estado. */
int  puente_ep0(const u8 s[8], const u8 **p, u32 *n);
int  puente_ep0_fin(const u8 s[8], const u8 *datos, u32 n);       /* acabó la fase de datos de un OUT: 0 = bien, < 0 = STALL */
/* ---- lo que usa shell/usb.c ---- */
void puente_paso(int en_app);                                    /* una vuelta del bucle: ejecuta la orden pendiente */
/* ---- lo que usan las plataformas ---- */
void pu_log(const char *s);                                      /* plat/hw_w580.c, hw_log: copia al anillo del puente */
int  pu_tecla_sacar(void);                                       /* hw_tecla_evento: la siguiente pulsación inyectada (K_*) o 0 */
int  pu_tecla_abajo(int k);                                      /* hw_tecla_abajo / hw_teclas_mapa: 1 si hay una pulsación inyectada mantenida */
int  puente_fallo_cpu(const u32 *ctx);                           /* excepcion_c: 1 si la excepción era de una orden del puente (NO vuelve: salta a ella) */
extern u8 pu_buf[PU_BUF];
extern u8 pu_codigo[PU_CODIGO];
#endif
#endif
