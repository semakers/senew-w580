/* La API de hardware (api/hw.h) en el teléfono: W580i / DB2020, sobre los controladores de hal/.
 * Es plat/hw_w600.c del sistema del W600 adaptado: lo que cambia está en docs/sistema.md. */
#include "hal.h"
#include "hw.h"
#include "ui.h"                                    /* solo por los códigos K_* */
#include "diseno.h"                                /* (estética) la letra en uso en la pantalla de fallo */
#include "almacen.h"
#include "usuario.h"
#include "w6f.h"                                   /* (I1) el puerto de la Memoria (api/hw_w6f.c) */
#include "w6f_sistema.h"
void cg_fallo_cpu(const u32 *ctx); int cg_perro_irq(void);   /* (C) shell/cargador.c: las apps cargables */
#include "bat.h"
#include "carga.h"                                 /* (CARGA) la carga por USB en números y decisiones */
#include "placa.h"
#include "enc.h"
#include "m18.h"                                   /* (R2) solo m18_fis(): dónde está en el chip cada byte lógico, para cotejar el espejo */                                   /* (R2) en qué señal de la tecla de encendido se cree (pulsación larga) */                                   /* (S7) la batería en números: rectas, umbrales, la decisión del arranque */

#ifndef HW_VERSION
#define HW_VERSION HW_VERSION_SISTEMA
#endif
extern char _inicio[], _data_flash[], _data[], _edata[], _fin[];
static u8 hay_pll, hay_mmu, hay_serie;
static signed char lcd_estado;                     /* 0 = bien; < 0 = lcd_init no pudo */
static u32 id_nor;
static HwArranque arr = { HW_ARR_BIEN, -1, -1, -1 };   /* (S7) cómo fue la comprobación temprana de la batería */
static int e23 = -1; static u32 e23_ms; static u8 e23_hay;   /* (S7) el registro 0x23 del PMU (cargador, USB, batería presente), guardado */
void hwo_fs_estado(HwFsEstado *e);

/* ---- (S7) LA FLASH: PROTECCIÓN, FORMATO PENDIENTE Y ENVOLTURAS (docs/sistema.md §15.3) -------------------------------------
 * Tres cosas que antes no había, todas AQUÍ (hal/nor.c no se toca):
 *  1. ANTES de la primera escritura se decide si se puede escribir: el binario lleva escritura (NOR_ESCRITURA), el chip es el
 *     esperado (plat/nor_guarda.c: id 0089 887E y CFI de 64 MiB / 256 KiB / 1 KiB / órdenes 0x0200), la geometría de hal/nor.c
 *     cuadra, y no es el modo seguro. Si algo falla: `proteccion` != 0 y NADA escribe — ni el almacén ni Mis archivos (se montan
 *     de solo lectura si ya hay volumen), ni se da formato; y como segundo cerrojo las cuatro envolturas de abajo se niegan.
 *  2. NUNCA se da formato a una zona que no esté EN BLANCO (todo 0xFF en la flash física) o con NUESTRA firma (un volumen que se
 *     monta de solo lectura). En un aparato, 0x45B00000..0x45DFFFFF trae el final del main original: esa zona queda sin montar y
 *     `pendiente` lo apunta; el shell PREGUNTA (shell/shell.c) y solo con el sí se borra y formatea (hw_formatear).
 *  3. Las envolturas avisan al perro de software (cada operación de flash es progreso legítimo) y marcan `nor_en_curso`, para
 *     que ni el perro ni el apagado forzado corten una operación a medias. */
static AlmPuerto nv_puerto;
static signed char nv_hay;                         /* 1 = montado */
static u8 proteccion, pendiente, confirmada, propia;   /* propia: (AUD2 B4) de las pendientes, las que llevan NUESTRA marca */       /* HW_PROT_* · HW_ZONA_* sin formato y sin permiso · HW_ZONA_* que el usuario ha mandado formatear */
static u8 modo_seguro, rescate;
static volatile u8 nor_en_curso;
/* (R3, P3) LA CAUSA DEL REINICIO: lo que el arranque anterior dejó apuntado (hal/reinicio.c) y lo que este apuntará si acaba mal. */
static RstInfo rst;                                /* del arranque ANTERIOR */
static u8 seguro_auto;                             /* 1 = modo seguro puesto SOLO: 3 arranques seguidos no llegaron al bucle */
static u8 pan_causa; static u32 pan_pc, pan_lr;    /* lo que apunta panico_cuerpo */
#define RST_ARRANQUES_MALOS 3
#define RST_BUCLE_VIVO_S 5
#include "tel.h"                                   /* (P600, paso 6) plat/tel/: el perro y la pulsación larga */
#include "bl/bl.h"                                 /* (Fase 3b) solo la bandera de entrada al bootloader: BL_BANDERA, BL_MAGIA */
#ifdef PUENTE
#include "puente.h"                                /* (v16) el puente de depuración por USB (docs/puente.md) */
#endif
int hw_w6f_formatear_zona(int ajena); int hw_w6f_ajena(void);   /* (I1) api/hw_w6f.c */
#if NOR_ESCRITURA
/* (R2; auditoría del sistema, A3) EL ESPEJO DE LA NOR, VIGILADO. hal/nor.c lee y decide contra una copia en RAM de 1,5 MiB
 * (el mayor blanco de la RAM para un puntero perdido, de casa o de una app externa: no hay protección de memoria). Lo que
 * hay en el chip se puede leer siempre (mapeado; cada byte lógico l del bloque está en el físico m18_fis(l): hal/m18.h), así
 * que el espejo se COTEJA con el chip sin tocar hal/nor.c:
 *  - antes de PROGRAMAR: el tramo de destino (del que m18_programar decide qué bits bajar) y, si los datos de origen salen
 *    del propio espejo (una limpieza que copia de un bloque a otro), también ese tramo;
 *  - antes de BORRAR un bloque: el espejo ENTERO (la decisión de borrar la tomó quien leyó el espejo; ~1,5 MiB de cotejo
 *    frente a un borrado de 0,2–1 s);
 *  - y de fondo, un bloque por segundo desde el bucle (espejo_paso): todo el espejo cada 12 s.
 * Si algo no casa: NO se escribe —ni esa operación ni ninguna más hasta reiniciar (HW_PROT_ESPEJO)—, se dice por la UART y
 * el shell lo avisa. El espejo se vuelve a cargar del chip en el arranque siguiente; el chip no se ha tocado.
 * LÍMITE: los datos que el FS haya copiado ANTES a un búfer suyo desde un espejo ya corrupto no se ven aquí (los frenan las
 * sumas de W6U y del almacén). Hacer el espejo de solo lectura con la MMU exige tocar hal/nor.c y el enlace: propuesta P5. */
#define ESPEJO_BLOQUES ((NOR_ESPEJO_FIN - NOR_ESPEJO_INI) / M18_BLOQUE)   /* (I1) el tramo CON espejo, no todo lo escribible: W6F no tiene espejo */
static u32 esp_cotejos, esp_fallos, esp_dir_mal;
static int espejo_casa(u32 g, u32 d, u32 n)        /* bloque global g (0..11), tramo lógico [d, d + n): 1 = el espejo dice lo que hay en el chip */
{
    const u8 *e = nor_zona(0) + g * M18_LOGICO, *f = (const u8 *)(size_t)(NOR_ESPEJO_INI + g * M18_BLOQUE);
    u32 l = d & ~15u, fin = (d + n + 15u) & ~15u;
    if (fin > M18_LOGICO) fin = M18_LOGICO;
    esp_cotejos++;
    for (; l < fin; l += 16) {
        const u32 *a = (const u32 *)(const void *)(e + l), *b = (const u32 *)(const void *)(f + 2 * l);
        if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3]) { esp_dir_mal = g * M18_LOGICO + l; return 0; }
    }
    return 1;
}
static void espejo_corrupto(const char *cuando)
{
    if (!(proteccion & HW_PROT_ESPEJO)) {
        esp_fallos++;
        proteccion |= HW_PROT_ESPEJO;
        hw_log("nor: *** EL ESPEJO EN RAM NO COINCIDE CON EL CHIP ("); hw_log(cuando); hw_log(", byte logico "); uart_hex(UART0, esp_dir_mal);
        hw_log("): memoria corrompida. NO SE ESCRIBE NADA MAS hasta reiniciar ***\n");
    }
}
static int espejo_antes(u32 g, u32 d, const void *p, u32 n)   /* 0 = adelante; < 0 = no se programa */
{
    const u8 *o = p, *e0 = nor_zona(0);
    if (g >= ESPEJO_BLOQUES || !espejo_casa(g, d, n)) { espejo_corrupto("destino de un programa"); return -NOR_E_VERIFICA; }
    if (o >= e0 && o < e0 + ESPEJO_BLOQUES * M18_LOGICO) {
        u32 de = (u32)(o - e0), k = 0;
        while (k < n) {                            /* el origen puede cruzar de un bloque lógico al siguiente */
            u32 gb = (de + k) / M18_LOGICO, db = (de + k) % M18_LOGICO, t = M18_LOGICO - db;
            if (t > n - k) t = n - k;
            if (gb >= ESPEJO_BLOQUES || !espejo_casa(gb, db, t)) { espejo_corrupto("origen de una copia"); return -NOR_E_VERIFICA; }
            k += t;
        }
    }
    return 0;
}
static int espejo_entero(const char *cuando)
{
    u32 g;
    for (g = 0; g < ESPEJO_BLOQUES; g++) if (!espejo_casa(g, 0, M18_LOGICO)) { espejo_corrupto(cuando); return -NOR_E_VERIFICA; }
    return 0;
}
static void espejo_paso(void)                      /* desde el bucle: un bloque por segundo */
{
    static u32 visto, g;
#ifdef PRUEBA_ESPEJO_CORRUPTO                      /* (R2) PRUEBA: a los 12 s del arranque, un «puntero perdido» pisa ese byte del espejo */
    { static u8 hecho; if (!hecho && tics > 3000u) { hecho = 1; ((volatile u8 *)(size_t)nor_zona(0))[PRUEBA_ESPEJO_CORRUPTO] ^= 0x10; hw_log("PRUEBA_ESPEJO_CORRUPTO: un byte del espejo pisado a proposito\n"); } }
#endif
    if (!nor_zona_bloque() || (proteccion & HW_PROT_ESPEJO) || nor_en_curso || tics - visto < 250u) return;
    visto = tics;
    if (!espejo_casa(g, 0, M18_LOGICO)) espejo_corrupto("ronda de fondo");
    if (++g >= ESPEJO_BLOQUES) g = 0;
}
/* (R2; auditoría del sistema, M4) hw_ms() NO PIERDE EL TIEMPO DE LA NOR. Programar una región son hasta 512 programas de una
 * palabra con las interrupciones cortadas: el tic no late y `tics` se atrasa (copiando 700 KB, del orden de un minuto con los
 * tiempos de un M18 real [inferido]). Aquí —sin tocar hal/nor.c— cada operación se mide con el RTC (1/128 s, el único reloj
 * que no para; como hw_dormir) y lo que el tic no contó se le suma. El grano de 7,8 ms se acumula entre operaciones, así que
 * no hay sesgo; si el RTC no corre o da un salto, no se inventa nada (tope de 30 s por operación). */
static u32 nor_perdido_us, nor_repuestos;
static u32 nor_t0, nor_k0;
static void nor_reloj_antes(void) { nor_t0 = rtc_128(); nor_k0 = tics; }
static void nor_reloj_despues(void)
{
    u32 t1 = rtc_128(), dt = (t1 >= nor_t0 ? t1 : t1 + 3600u * 128u) - nor_t0, contado = (tics - nor_k0) * 4000u, real, n, e;
    if (dt > 30u * 128u) return;
    real = dt * 7812u + dt / 2u;                   /* 1/128 s = 7812,5 us */
    if (real <= contado) return;
    nor_perdido_us += real - contado;
    n = nor_perdido_us / 4000u;
    if (!n) return;
    nor_perdido_us -= n * 4000u;
    e = irq_cerrar(); tics += n; if (!e) irq_abrir();
    nor_repuestos += n;
}
#define G_OP(cotejo, op) do { int r; if (cotejo) return -NOR_E_VERIFICA; nor_en_curso = 1; nor_reloj_antes(); r = (op); nor_reloj_despues(); nor_en_curso = 0; tel_perro_prorroga(); tel_tras_nor(); return r; } while (0)
static int g_programar(u32 b, u32 d, const void *p, u32 n) { if (proteccion) return -NOR_E_DESHABILITADA; G_OP(espejo_antes(b, d, p, n), nor_programar(b, d, p, n)); }
static int g_borrar(u32 b) { if (proteccion && !rescate) return -NOR_E_DESHABILITADA; G_OP(!rescate && espejo_entero("antes de un borrado"), nor_borrar(b)); }
static int g_usr_programar(u32 b, u32 d, const void *p, u32 n) { if (proteccion) return -NOR_E_DESHABILITADA; G_OP(espejo_antes(nor_zona_bloques() + b, d, p, n), nor_usr_programar(b, d, p, n)); }
static int g_usr_borrar(u32 b) { if (proteccion && !rescate) return -NOR_E_DESHABILITADA; G_OP(!rescate && espejo_entero("antes de un borrado"), nor_usr_borrar(b)); }
#else
static void espejo_paso(void) { }
static int g_programar(u32 b, u32 d, const void *p, u32 n) { (void)b; (void)d; (void)p; (void)n; return -NOR_E_DESHABILITADA; }
static int g_borrar(u32 b) { (void)b; return -NOR_E_DESHABILITADA; }
#define g_usr_programar g_programar
#define g_usr_borrar g_borrar
#endif
static int en_blanco(u32 ini, u32 fin)             /* la flash FÍSICA de esa zona, entera a 0xFF (las dos mitades de cada segmento) */
{
    const u32 *p = (const u32 *)ini, *f = (const u32 *)fin;
    while (p < f) if (*p++ != 0xFFFFFFFFu) return 0;
    return 1;
}
static void proteccion_decidir(void)
{
    const NorChip *g = nor_chip();                 /* (R3, P1) UNA sola lectura del chip, en la HAL: geometría (se lee) y permiso (se escribe) */
    int r = nor_escr_init();                       /* geometría de la zona según la tabla CFI del chip (hal/nor.c) */
    proteccion = 0;
    if (!NOR_ESCRITURA) proteccion |= HW_PROT_COMPILADO;
    if (g->que == NOR_CHIP_ID) proteccion |= HW_PROT_CHIP;
    if (g->que == NOR_CHIP_CFI || !nor_zona_bloque() || (r && r != -NOR_E_DESHABILITADA)) proteccion |= HW_PROT_CFI;
    if (modo_seguro) proteccion |= HW_PROT_SEGURO;
    if (g->ocupado) hw_log("nor: *** el chip seguia OCUPADO en el tope al arrancar: no se le manda nada (sin zonas, SIN ESCRITURA) ***\n");
    if (g->suspendido_ajeno) hw_log("nor: *** hay un borrado SUSPENDIDO que no se puede atribuir a nuestras zonas: NO se toca (SIN ESCRITURA) ***\n");
    if (g->suspendidos) { hw_log("nor: *** habia "); uart_dec(UART0, g->suspendidos); hw_log(g->saneo_lento ? " borrado SUSPENDIDO de antes del reinicio: reanudado y terminado (tardo mas que el tope)\n" : " borrado SUSPENDIDO de antes del reinicio: reanudado y terminado\n"); }
    hw_log("nor: id "); uart_hex(UART0, g->id); hw_log(", CFI: 2^"); uart_dec(UART0, g->cfi_tam); hw_log(" bytes, ");
    uart_dec(UART0, g->cfi_bloques); hw_log(" bloques de "); uart_dec(UART0, g->cfi_bloque >> 10); hw_log(" KiB, bufer 2^"); uart_dec(UART0, g->cfi_bufer); hw_log("\n");
    if (!proteccion) { hw_log("nor: chip reconocido; se puede escribir en las zonas propias\n"); return; }
    hw_log("nor: *** MODO SIN ESCRITURA ***:");
    if (proteccion & HW_PROT_COMPILADO) hw_log(" binario sin escritura;");
    if (proteccion & HW_PROT_CHIP) hw_log(" el chip NO es el esperado (0089 887E);");
    if (proteccion & HW_PROT_CFI) { hw_log(" la tabla CFI no cuadra (palabra "); uart_hex(UART0, g->cfi_mal); hw_log(");"); }
    if (proteccion & HW_PROT_SEGURO) hw_log(" modo seguro;");
    hw_log(" nada se borra ni se programa\n");
}
static void nv_iniciar(void)
{
    if (!nor_zona_bloque()) { hw_log("nv: sin geometria de la NOR: sin almacen\n"); return; }
    nv_puerto = (AlmPuerto){ nor_zona_bloques(), nor_zona_bloque(), nor_zona, g_programar, g_borrar };
    if (modo_seguro) { hw_log("nv: MODO SEGURO: el almacen NO se lee (ajustes de fabrica)\n"); return; }
    nv_hay = alm_montar(&nv_puerto, 0) == ALM_OK;  /* primero SIN escribir: ¿hay un almacén nuestro? */
    if (nv_hay) {
        if (!proteccion) nv_hay = alm_montar(&nv_puerto, 1) == ALM_OK;
        hw_log(!nv_hay ? "nv: no se pudo montar\n" : proteccion ? "nv: almacen montado, SOLO LECTURA\n" : "nv: almacen montado\n");
        return;
    }
    if (proteccion) { hw_log("nv: no hay almacen y la flash esta protegida: sin almacen\n"); return; }
    if (!en_blanco(NOR_ZONA_INI, NOR_ZONA_FIN) && !(confirmada & HW_ZONA_AJUSTES)) {
        pendiente |= HW_ZONA_AJUSTES;
        if (alm_firma(&nv_puerto)) { propia |= HW_ZONA_AJUSTES; hw_log("nv: la zona 0x45B00000 lleva NUESTRA marca pero NO monta (danada o a medio formatear): NO se formatea sin permiso (sin almacen)\n"); }
        else
        hw_log("nv: la zona 0x45B00000 NO esta en blanco ni es nuestra: NO se formatea sin permiso (sin almacen)\n");
        return;
    }
    nv_hay = alm_montar(&nv_puerto, 1) == ALM_OK;  /* en blanco (o con permiso): formato */
    hw_log(nv_hay ? "nv: almacen montado (zona en blanco: formato)\n" : "nv: no se pudo montar\n");
}
static int clave_vedada(u16 clave);
/* (2026-10-25, música sólida) LA VEDA DE LA FLASH. Programar o borrar la NOR mientras suena música la estropea: el ARM se queda dentro de la
 * operación (un borrado, cientos de ms) sin dar datos al DSP y, según se midió con las migas, el propio DSP pierde ritmo mientras tanto
 * (memoria dsp-mp3-resuelto-migas-flash-2026-10-25). El servicio de música (shell/musica.c) pone la veda mientras SUENA y la quita en el silencio
 * (pausa, parada, fin); hwo_flash_veda(0) escribe entonces lo aplazado. Mientras dura, hw_guardar/hw_olvidar NO escriben: el valor se queda
 * en RAM (VP_N claves de hasta VP_TAM bytes; hw_cargar lo devuelve como si estuviera guardado) y la recolección de la Memoria (plat/main_w580.c)
 * espera. Lo que no cabe se escribe igual (y se cuenta en hw_veda_rotas), como los ficheros que el usuario o una app guardan a propósito. Al
 * apagar o reiniciar se escribe todo lo aplazado (hw_apagar, hw_reiniciar). */
#define VP_N 8
#define VP_TAM 512
static struct { u16 clave; u8 uso, olvidar; u16 n; u8 d[VP_TAM]; } vp[VP_N];
static u8 veda;
volatile u32 hw_veda_aplazadas, hw_veda_rotas, hw_veda_vaciadas;
extern volatile u8 nor_veda;                       /* hal/nor.c: cuenta las operaciones de la NOR que caen dentro de la veda (nor_veda_ops) */
static int vp_buscar(u16 clave) { int i; for (i = 0; i < VP_N; i++) if (vp[i].uso && vp[i].clave == clave) return i; return -1; }
static int vp_poner(u16 clave, const void *d, u32 n, int olvidar)
{
    int i = vp_buscar(clave);
    if (n > VP_TAM) return -1;
    if (i < 0) for (i = 0; i < VP_N && vp[i].uso; i++) ;
    if (i >= VP_N) return -1;
    vp[i].clave = clave; vp[i].uso = 1; vp[i].olvidar = (u8)!!olvidar; vp[i].n = (u16)n;
    if (n) memcpy(vp[i].d, d, n);
    hw_veda_aplazadas++;
    return 0;
}
static void vp_vaciar(void)
{
    int i;
    for (i = 0; i < VP_N; i++) {
        if (!vp[i].uso) continue;
        vp[i].uso = 0;
        if (vp[i].olvidar) alm_borrar(vp[i].clave); else alm_guardar(vp[i].clave, vp[i].d, vp[i].n);
        hw_veda_vaciadas++;
    }
}
/* (2026-10-25) RESPIRAR: lo llaman los trabajos largos del bucle (las esperas de los guiones del panel, hal/lcd.c) para que la música no se
 * quede sin datos mientras tanto. En el original no hace falta: el audio va en procesos OSE de prioridad 15 que desalojan a la interfaz
 * (docs/re_musica_segundo_plano.md); senew no tiene procesos y lo hace así, a mano, donde se sabe que el bucle se para. */
void hw_mp3_respirar(void) __attribute__((weak));
void hw_respirar(void) { if (hw_mp3_respirar) hw_mp3_respirar(); }
void hwo_flash_veda(int si)
{
    if (!!si == veda) return;
    veda = (u8)!!si; nor_veda = veda;
    if (!veda) vp_vaciar();
}
int hw_flash_vedada(void) { return veda; }
int hw_cargar(u16 clave, void *buf, u32 max)
{
    int r, i = vp_buscar(clave);
    if (i >= 0) {                                  /* aplazado por la veda: vale lo último que se pidió guardar */
        if (vp[i].olvidar) return HW_NO_DISPONIBLE;
        memcpy(buf, vp[i].d, vp[i].n < max ? vp[i].n : max);
        return vp[i].n;
    }
    r = nv_hay ? alm_cargar(clave, buf, max) : -1;
    return r < 0 ? HW_NO_DISPONIBLE : r;
}
int hw_guardar(u16 clave, const void *d, u32 n)
{
    if (!nv_hay || !NOR_ESCRITURA || proteccion) return HW_NO_DISPONIBLE;
    if (clave_vedada(clave)) return HW_ERROR;
    if (n > ALM_MAX_VALOR) return HW_ERROR;
    if (veda) { if (!vp_poner(clave, d, n, 0)) return HW_OK; hw_veda_rotas++; }
    return alm_guardar(clave, d, n) == ALM_OK ? HW_OK : HW_ERROR;
}
int hw_olvidar(u16 clave)
{
    if (!nv_hay || !NOR_ESCRITURA || proteccion) return HW_NO_DISPONIBLE;
    if (clave_vedada(clave)) return HW_ERROR;
    if (veda) { if (!vp_poner(clave, NULL, 0, 1)) return HW_OK; hw_veda_rotas++; }
    return alm_borrar(clave) == ALM_OK ? HW_OK : HW_ERROR;
}
void hw_nv_estado(HwNvEstado *e)
{
    AlmEstado a;
    alm_estado(&a);
    *e = (HwNvEstado){ .montado = nv_hay > 0, .escribible = nv_hay > 0 && !a.solo_lectura && NOR_ESCRITURA && !proteccion, .motivo = nor_escr_motivo(),
        .bloques = a.bloques, .tam_bloque = a.tam_bloque, .usado = a.usado, .vivos = a.vivos, .claves = a.claves, .generacion = a.generacion,
        .borrados = nor_cta.borrados, .programas = nor_cta.programas, .suspensiones = nor_cta.suspensiones, .errores = a.errores + nor_cta.errores };
}
int hw_modo_seguro(void) { return modo_seguro; }
/* (R3, P3) Lo que el shell necesita saber del arranque anterior (api/hw.h). */
void hw_reinicio(HwReinicio *r)
{
    static const u8 de[] = { [RST_DESCONOCIDA] = HW_RST_DESCONOCIDA, [RST_SIN_APUNTAR] = HW_RST_SIN_APUNTAR, [RST_PERRO] = HW_RST_PERRO, [RST_EXCEPCION] = HW_RST_EXCEPCION,
                             [RST_PANICO] = HW_RST_PANICO, [RST_PILA] = HW_RST_PILA, [RST_FORZADO] = HW_RST_FORZADO, [RST_PEDIDO] = HW_RST_NORMAL, [RST_APAGADO] = HW_RST_NORMAL };
    *r = (HwReinicio){ .causa = de[rst.causa <= RST_APAGADO ? rst.causa : 0], .seguro_auto = seguro_auto, .sin_bucle = rst.sin_bucle, .pc = rst.pc, .lr = rst.lr, .arranques = rst.arranques };
}
/* El bucle principal (plat/main_w580.c) lo llama en cada vuelta: cuando lleva RST_BUCLE_VIVO_S segundos dando vueltas, este
 * arranque «llegó al bucle» y la cuenta de arranques fallidos vuelve a 0. */
void plat_bucle(void)
{
    static u32 desde; static u8 dicho, empezado;
    if (dicho) return;
    if (!empezado) { empezado = 1; desde = tics; return; }
    if (tics - desde >= RST_BUCLE_VIVO_S * 250u) { dicho = 1; rst_bucle_vivo(); hw_log("arranque: el bucle lleva 5 s vivo: arranque BUENO (la cuenta de fallidos, a 0)\n"); }
}
int hw_nor_proteccion(void) { return proteccion; }
int hw_formato_pendiente(void) { return pendiente | (hw_w6f_ajena() ? HW_ZONA_MEMORIA : 0); }   /* (I1) y la Memoria, si es ajena */
int hw_formato_propio(void) { return propia & pendiente; }   /* (AUD2 B4) */

/* ---- la zona de ficheros de usuario (W6U): api/usuario.c sobre NOR_USR_INI..NOR_USR_FIN (hal/nor.c) --------
 * El puerto es el del almacén con otra base. Sin escritura (flash protegida) se monta en solo lectura si ya hay un
 * volumen; si no lo hay, no hay zona. (S7) Y si la zona no está en blanco ni es un volumen nuestro, tampoco se
 * formatea: queda pendiente de que el usuario lo confirme. */
static AlmPuerto usr_puerto;
const AlmPuerto *hwo_usr_puerto(int *puede_escribir)
{
    int puede = !proteccion && NOR_ESCRITURA;
    if (!nor_zona_bloque() || nor_usr_bloques() < 3) return 0;
    if (modo_seguro) { hw_log("usr: MODO SEGURO: Mis archivos NO se monta (ni se lee)\n"); return 0; }   /* (R2; auditoría de S7, M7) un volumen que cuelgue el montaje no puede colgar también el modo seguro */
    usr_puerto = (AlmPuerto){ nor_usr_bloques(), nor_zona_bloque(), nor_usr, g_usr_programar, g_usr_borrar };
    if (puede && !(confirmada & HW_ZONA_USUARIO) && !en_blanco(NOR_USR_INI, NOR_USR_FIN) && usr_montar(&usr_puerto, 0) != USR_OK) {
        pendiente |= HW_ZONA_USUARIO;
        if (usr_firma(&usr_puerto)) { propia |= HW_ZONA_USUARIO; hw_log("usr: la zona 0x45C00000 es un volumen NUESTRO que NO monta (danado): NO se formatea sin permiso\n"); }
        else
        hw_log("usr: la zona 0x45C00000 NO esta en blanco ni es nuestra: NO se formatea sin permiso\n");
        puede = 0;
    }
    *puede_escribir = puede;
    return &usr_puerto;
}
void hwo_usr_tras_escribir(void) { }

/* ---- (I1) LA MEMORIA (W6F, api/hw_w6f.c): el puerto sobre la zona de W6F de hal/nor.c (modo objeto, SIN espejo) ----------
 * Las mismas guardas que las otras zonas (fs2/INTEGRACION.md §3): con `proteccion` (chip inesperado, CFI, modo seguro, espejo
 * corrupto, binario sin escritura) se niegan; marcan nor_en_curso, ponen al día el reloj y prorrogan al perro (G_OP). Sin
 * cotejo de espejo: W6F no tiene, y lo que lee lo comprueba con sus CRC. Si una envoltura veta una orden, W6F ve un fallo de
 * flash: la operación da W6F_E_FLASH (y lo de «solo lectura» por eso se levanta solo cuando la flash vuelve a dejarse: N1). */
#if NOR_ESCRITURA
static int g_w6f_programar(u32 b, u32 pg, const u8 *d) { if (proteccion) return -NOR_E_DESHABILITADA; G_OP(0, nor_w6f_programar(b, pg, d)); }
static int g_w6f_borrar(u32 b) { if (proteccion) return -NOR_E_DESHABILITADA; G_OP(0, nor_w6f_borrar(b)); }
#else
static int g_w6f_programar(u32 b, u32 pg, const u8 *d) { (void)b; (void)pg; (void)d; return -NOR_E_DESHABILITADA; }
static int g_w6f_borrar(u32 b) { (void)b; return -NOR_E_DESHABILITADA; }
#endif
/* (AUD5 M5) el puerto comprueba sus rangos él mismo: fuera de la zona no se lee ni un byte, lo pida quien lo pida */
static int w6f_p_leer(void *c, uint32_t b, uint32_t pg, uint32_t off, void *buf, uint32_t n)
{
    (void)c;
    if (b >= nor_w6f_bloques() || pg >= W6F_SIS_PPB || off > W6F_SIS_PAG || n > W6F_SIS_PAG - off) return -1;
    memcpy(buf, nor_w6f(b) + pg * W6F_SIS_PAG + off, n);
    return 0;
}
static int w6f_p_prog(void *c, uint32_t b, uint32_t pg, const uint8_t *d) { (void)c; return pg < W6F_SIS_PPB ? g_w6f_programar(b, pg, d) : -1; }
static int w6f_p_borrar(void *c, uint32_t b) { (void)c; return g_w6f_borrar(b); }
static W6fFlash w6f_fl = { 0, W6F_FL_NOR_OBJETO, 0, W6F_SIS_PAG, W6F_SIS_PPB, w6f_p_leer, w6f_p_prog, w6f_p_borrar, 0, 0 };
const W6fFlash *hwo_w6f_flash(int *puede_escribir)
{
    if (!nor_w6f_bloques()) return 0;              /* sin geometría (chip ocupado, sin CFI…): ni se lee */
    if (modo_seguro) { hw_log("w6f: MODO SEGURO: la Memoria NO se monta (ni se lee)\n"); return 0; }   /* como Mis archivos (R2, M7) */
    w6f_fl.n_bloques = nor_w6f_bloques();
    *puede_escribir = !proteccion && NOR_ESCRITURA;
    return &w6f_fl;
}
void hwo_w6f_tras_escribir(void) { }
int hwo_w6f_fallo_de_prueba(void) { return 0; }

/* El usuario ha dicho que sí: borrar la zona entera (bloque a bloque) y darle formato. */
int hw_formatear(int zonas)
{
    u32 b; int mal = 0;
    int memoria = (zonas & HW_ZONA_MEMORIA) && hw_w6f_ajena();
    if (proteccion || !nor_zona_bloque()) return HW_NO_DISPONIBLE;
    zonas &= pendiente;
    if (zonas & HW_ZONA_AJUSTES) {
        hw_log("nv: formato de la zona de ajustes, CONFIRMADO por el usuario\n");
        for (b = 0; b < nor_zona_bloques(); b++) { hw_progreso(b, nor_zona_bloques()); if (g_borrar(b)) mal = 1; }
        confirmada |= HW_ZONA_AJUSTES; pendiente &= (u8)~HW_ZONA_AJUSTES;
        nv_hay = alm_montar(&nv_puerto, 1) == ALM_OK;
        if (!nv_hay) mal = 1;
    }
    if (zonas & HW_ZONA_USUARIO) {
        hw_log("usr: formato de Mis archivos, CONFIRMADO por el usuario\n");
        for (b = 0; b < nor_usr_bloques(); b++) { hw_progreso(b, nor_usr_bloques()); if (g_usr_borrar(b)) mal = 1; }
        confirmada |= HW_ZONA_USUARIO; pendiente &= (u8)~HW_ZONA_USUARIO;
        if (hw_usr_iniciar() != HW_OK) mal = 1;
    }
    if (memoria && hw_w6f_formatear_zona(1) != HW_OK) mal = 1;   /* (I1) la Memoria ajena: 64 borrados, con hw_progreso */
    return mal ? HW_ERROR : HW_OK;
}
/* EL RESCATE DEL MODO SEGURO: un ajuste guardado que cuelga el arranque normal no se puede arreglar sin escribir. Esto borra
 * el almacén de ajustes ENTERO (4 bloques), y nada más; solo si el chip es el esperado y solo pedido por el usuario. */
int hw_borrar_ajustes(void)
{
    u32 b; int mal = 0;
    if (!modo_seguro || !NOR_ESCRITURA || !nor_zona_bloque() || (proteccion & ~HW_PROT_SEGURO)) return HW_NO_DISPONIBLE;   /* (R2) solo en modo seguro: antes lo limitaba solo el menú */
    hw_log("nv: BORRADO del almacen de ajustes pedido por el usuario\n");
    rescate = 1;
    for (b = 0; b < nor_zona_bloques(); b++) { hw_progreso(b, nor_zona_bloques()); if (g_borrar(b)) mal = 1; }
    rescate = 0;
    nv_hay = 0;
    return mal ? HW_ERROR : HW_OK;
}
/* (R2; auditoría de S7, M7) EL SEGUNDO RESCATE: si lo que cuelga el arranque normal es el volumen de Mis archivos, el modo
 * seguro (que ya no lo monta) puede borrarlo entero: sus 8 bloques y nada más (el rango lo comprueba además hal/nor.c). */
int hw_borrar_usuario(void)
{
    u32 b; int mal = 0;
    /* (R2; auditoría de datos, A1) TAMBIÉN EN MODO NORMAL Y CON EL VOLUMEN MONTADO: es la SALIDA cuando Mis archivos se queda
     * atascado (ni escribir ni borrar) y hw_formatear no vale, porque solo actúa sobre zonas sin montar. No pasa por el FS:
     * borra los bloques en crudo, así que funciona esté como esté el volumen; después se le da formato (modo normal). */
    if (!NOR_ESCRITURA || !nor_zona_bloque() || nor_usr_bloques() < 3 || (proteccion & ~HW_PROT_SEGURO) || (proteccion && !modo_seguro)) return HW_NO_DISPONIBLE;
    hw_log(modo_seguro ? "usr: BORRADO de Mis archivos pedido por el usuario (modo seguro)\n" : "usr: VACIADO de Mis archivos pedido por el usuario\n");
    hw_usr_cancelar();
    rescate = 1;
    for (b = 0; b < nor_usr_bloques(); b++) { hw_progreso(b, nor_usr_bloques()); if (g_usr_borrar(b)) mal = 1; }
    rescate = 0;
    if (!modo_seguro) { confirmada |= HW_ZONA_USUARIO; pendiente &= (u8)~HW_ZONA_USUARIO; if (hw_usr_iniciar() != HW_OK) mal = 1; }
    return mal ? HW_ERROR : HW_OK;
}

/* ---- robustez: testigos de las pilas ---------------------------------------------------------------
 * Las dos pilas (IRQ y SVC) se siembran con un patrón al arrancar y el tic mira 4 veces por segundo
 * hasta dónde han bajado: así hw_salud() puede decir cuánto hueco libre se ha visto como mínimo, y un
 * desbordamiento se caza ANTES de que corrompa el montículo (que está justo debajo). El barrido va con
 * grano de 256 bytes para no gastar casi nada dentro de la interrupción (16 + 64 lecturas). */
extern char _pilas_ini[], _pila_irq[], _pila_svc[];
#define TESTIGO 0x57A1C0DEu
static u32 pila_irq_min = 0xFFFFFFFF, pila_svc_min = 0xFFFFFFFF;
static u32 cta_excepciones, cta_dormidas, cta_desp_tecla, cta_desp_tapa, cta_ms_dormido;
/* Vibrar: PMU 0xA0 continuo (hal/i2c.c pmu_vibrador; [medido] que mueve el motor). Lo apaga el bucle (el I²C no se toca
 * desde la interrupción): vibra_paso() va en hw_tecla_evento y en hw_dormir, así que dura lo pedido ± una vuelta. */
static u32 vibra_hasta;
static void vibra_paso(void) { if (vibra_hasta && (int)(hw_ms() - vibra_hasta) >= 0) { vibra_hasta = 0; pmu_vibrador(0); } }
static u32 desliz_cambios;
static u8 en_panico;
static u8 chip_calla;                              /* el chip de LED no contesta (ver LUCES, más abajo) */
static void luz_paso(void);                        /* el paso siguiente de la rampa de luces, si toca */
static int luz_en_rampa(void) __attribute__((unused));
static void luz_ya(int fase);
static void dz_paso(void);                         /* (S5) el teclado del deslizador sigue al sensor */
#define LUZ_PULSO_PMU_MS 460

static void pilas_sembrar(void)
{
    u32 *p, sp;
    for (p = (u32 *)_pilas_ini; p < (u32 *)_pila_irq; p++) *p = TESTIGO;   /* IRQ: todavía no ha habido ninguna */
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    for (p = (u32 *)_pila_irq; p + 64 < (u32 *)sp; p++) *p = TESTIGO;      /* SVC: hasta 256 bytes por debajo de aquí */
}
static u32 pila_libre(char *fondo, char *tope)     /* bytes de patrón intacto desde el fondo (grano de 256) */
{
    char *p = fondo;
    while (p + 256 <= tope && *(volatile u32 *)p == TESTIGO) p += 256;
    return (u32)(p - fondo);
}
/* ---- (S7) EL PERRO DE SOFTWARE Y LA PULSACIÓN LARGA, DESDE LA INTERRUPCIÓN DEL TIC (docs/sistema.md §15.5, §15.6) ---------------
 * El perro de HARDWARE (F9010000) lo desarma la llave del arranque y de su periodo y su alimentación no se sabe nada: NO se arma.
 * Esto es otra cosa: la IRQ del tic (cada 64 tics = 256 ms) mira cuánto hace que el bucle principal dio señales.
 *  - hw_perro_latido(): el bucle del sistema en cada vuelta, y app_latido() por las apps externas. La primera llamada lo ARMA
 *    (el arranque —montar el FS, decodificar el fondo— no cuenta hasta entonces... pero sí desde que plat_init acaba: ver abajo).
 *  - sin latido en `perro_plazo` segundos (10; 30 con una app externa en pantalla) -> MUERDE.
 *  - cada operación de flash acabada da PRÓRROGA (un borrado tarda ~1 s; compactar Mis archivos, varios), pero sin latido del
 *    bucle en PERRO_TOPE_S (60 s) muerde igual: un bucle infinito de escrituras no se libra.
 *  - con una operación de flash EN CURSO no se muerde ni se apaga a la fuerza: se espera a que acabe (las rutinas de la NOR van
 *    con las IRQ cortadas, así que esta interrupción solo entra con el chip en modo lectura; pero un borrado SUSPENDIDO quedaría
 *    pendiente en el chip). Con tope: 20 s más.
 *  - en reposo profundo el tic no late y el perro tampoco cuenta; en la pantalla de fallo, apagado.
 * AL MORDER (no se vuelve al bucle): gancho del tic fuera, tono callado, y el I²C SE TOMA desde aquí —la transacción que el
 * bucle tuviera a medias se abandona: cada transferencia empieza con un reset del bloque (hal/i2c.c), y se reintenta hasta 3
 * veces por si el PMU se hubiera quedado a medio byte— para dejar la corriente de carga a 0 (0x61 <- 0x10, 0x63 <- 0) y el
 * vibrador parado (0xA0 <- 0); después la PANTALLA DE FALLO con el PC interrumpido (de la pila de la IRQ: arranque.S guarda
 * {r0-r3, r12, lr} y lr es el PC) y el LR del bucle (leído pasando un instante a modo SVC), y REINICIO a los 15 s o con una
 * tecla. LO QUE NO PUEDE: sacar de un cuelgue con las interrupciones cortadas (ahí no hay tic). [emulador]; en el aparato, sin probar.
 *
 * LA TECLA DE ENCENDIDO se mira aquí por la celda 30 del teclado (bits 27..31 de FB010000: la foto de las teclas a masa; una
 * LECTURA pura, sin barrer, que no estorba al barrido del bucle). El camino del PMU (0x23 bit 0) NO se puede mirar desde la
 * interrupción (es I²C); con la interfaz viva lo mira el shell, y con la interfaz colgada el perro acaba en la pantalla de
 * fallo, que sí lo sondea. Pulsada 8 s seguidos: hw_encendido_largo() = 1 (el shell apaga con orden: guarda y se despide).
 * Pulsada 11 s (nadie ha apagado: la interfaz no responde): APAGADO FORZADO desde aquí, con el I²C tomado igual que el perro:
 * carga fuera, vibrador y luces fuera, 0x41 <- 0 y fuera la retención (3 intentos); y si sigue con corriente (cargador puesto,
 * [sin saber]) REINICIA, que es lo que hace el original con cargador. */
/* (P600, paso 6) el perro y la pulsación larga son COMUNES: plat/tel/perro.c. Aquí queda lo que solo sabe esta placa (tel_*). */
static u8 kbd_listo;
static u8 tono_listo;                               /* (lo usan también el perro y la pantalla de fallo, más abajo) */
extern void (*volatile tic_gancho)(void);
extern void (*volatile tic_audio)(void);       /* (2026-10-26) hal/timer.c: el alimentador del DSP (hal/mp3_w580.c) */
extern void (*volatile tic_camara)(void);      /* (1.9) hal/timer.c: la vigilancia del cuadro de la cámara (shell/camara_sis.c) */
extern void (*volatile tic_micro)(void);       /* (1.10) hal/timer.c: el micrófono (shell/micro_sis.c) */
static void panico_cuerpo(void *arg);
static const char *pan_titulo, *pan_detalle;
static void hex8(char *s, u32 v);
void hwo_tarjeta_iniciar(void); void hwo_tarjeta_apagar(void);   /* (pista T) plat/tarjeta_w580.c */
int  hw_tarjeta_paso(void);                        /* (pista T) api/tarjeta.c: la memoria externa late con el bucle (y con una app en pantalla) */
void tel_latido_placa(void) { espejo_paso(); hw_tarjeta_paso(); }
int  tel_kbd_masa(int *nivel, int *foto) { if (!kbd_listo) return -1; kbd_masa(nivel, foto); return 0; }
int  tel_nor_en_curso(void) { return nor_en_curso; }
int  tel_en_panico(void) { return en_panico; }
int  tel_pmu_tecla(void)                           /* 0x23 bit 0 AHORA, con el I²C tomado (3 intentos) */
{
    u8 v = 0; int i, leido = -1;
    for (i = 0; i < 3 && leido; i++) { i2c_revivir(0); leido = pmu_leer(0x23, &v); }
    return leido ? -1 : (v & 1);
}
void tel_sin_bucle(void)                           /* lo común del perro y del apagado forzado: callar y dejar el PMU seguro, desde la IRQ */
{
    int i;
    en_panico = 1;
    tic_gancho = 0;                                /* la música o la melodía de una app ya no laten */
    tic_audio = 0;                                 /* (2026-10-26) ni el alimentador del DSP */
    tic_camara = 0;                                /* (1.9) ni la vigilancia de la cámara */
    tic_micro = 0;                                 /* (1.10) ni el micrófono */
    tic_tono_hasta = 0; if (tono_listo) tono_fin();
    pmu_carga_fuera();                              /* (R2) 3 intentos, cada uno levanta el veto del bus, y RELEÍDO */
    vibra_hasta = 0;
    for (i = 0; i < 3; i++) { i2c_revivir(0); if (!pmu_vibrador(0)) break; }
    i2c_revivir(1);
}
void tel_fallo(const char *titulo, const char *detalle, int causa, u32 pc, u32 lr, int reiniciar)
{
    pan_causa = causa == TEL_FALLO_PERRO ? RST_PERRO : RST_PANICO; pan_pc = pc; pan_lr = lr;
    pan_titulo = titulo; pan_detalle = detalle;
    en_pila_de_fallo(panico_cuerpo, (void *)(size_t)(reiniciar ? 1 : 0));
    for (;;) ;
}
void tel_apagado_forzado(void)
{
    int i;
    luz_ya(HW_LUZ_APAGADA);
    lcd_apagar();
    rst_apuntar(RST_APAGADO, 0, 0);                /* (R3) un apagado no es un arranque fallido */
    for (i = 0; i < 3; i++) { i2c_revivir(0); pmu_apagar(); espera_ms(500); }
    hw_log("apagado forzado: SIGO VIVO (cargador?): reinicio\n");
    rst_apuntar(RST_FORZADO, 0, 0);
    sys_reset();
    for (;;) ;
}
void tic_vigia(void)                               /* lo llama el tic cada 64 tics (256 ms) */
{
    u32 a = pila_libre(_pilas_ini, _pila_irq), b = pila_libre(_pila_irq, _pila_svc);
    if (a < pila_irq_min) pila_irq_min = a;
    if (b < pila_svc_min) pila_svc_min = b;
    /* (R2; auditoría del sistema, M3) antes solo se apuntaba. Debajo de la pila de IRQ están el anillo de la UART y la TABLA DE
     * LA MMU: con menos de 512 bytes de patrón intacto en cualquiera de las dos se para AQUÍ, en la pila de fallo, antes de que
     * el desborde pise nada (el grano del testigo es de 256 bytes: «< 512» es 0 o 256). */
    if ((a < 512 || b < 512) && !en_panico) {
        tel_sin_bucle();
        pan_causa = RST_PILA;
        pan_titulo = a < 512 ? "PILA DE INTERRUPCIONES CASI LLENA" : "PILA DEL SISTEMA CASI LLENA";
        pan_detalle = "Quedaban menos de 512 bytes libres. Se para antes de que el desborde pise la memoria de debajo.";
        en_pila_de_fallo(panico_cuerpo, (void *)(size_t)(modo_seguro ? 0 : 1));
        for (;;) ;
    }
    tel_perro_vigia();
}
void hw_salud(HwSalud *s)
{
    s->excepciones = cta_excepciones;
    s->pila_svc_libre = pila_svc_min == 0xFFFFFFFF ? 0 : pila_svc_min;
    s->pila_irq_libre = pila_irq_min == 0xFFFFFFFF ? 0 : pila_irq_min;
    s->dormidas = cta_dormidas; s->despertares_tecla = cta_desp_tecla;
    s->despertares_tapa = cta_desp_tapa; s->ms_dormido = cta_ms_dormido;
    s->esperas_cortadas = espera_cortadas;
}

/* ---- arranque y sistema -------------------------------------------------------------------------- */
/* El cuadro que lee el PDICON: RGB565 en el orden natural del ARM. El framework pinta en ui_pantalla con el
 * byte alto primero (así lo quería el panel del W600 por DMA); aquí se da la vuelta a los bytes de las filas
 * que cambian y el PDICON manda esa banda desde esta copia. */
static u16 lcd_cuadro[PLACA_ANCHO * PLACA_ALTO] __attribute__((aligned(32), section(".gram")));   /* en 0x20E80000, como el original (hal/enlace.ld) */

/* (S7) LA BATERÍA ANTES DE ENCENDER NADA, como el arranque temprano del original (pmu_startup_battery_check, 0x449CDB04;
 * docs/bateria.md §4.4 y §8.A3): lee 0x23 y la batería «en frío» (0x70 <- 0x6A, 0x71 sin pulso), con el PMU aún sin su lista.
 *  - cargador o USB puestos: seguir (el shell abre el aviso «este sistema NO carga»);
 *  - sin nada enchufado y 2850 <= mV < 3400 (lectura 6..56): NO ARRANCAR. Señal breve que no depende de la pantalla —tres
 *    toques del vibrador, A1 <- 0C / A0 <- 20 / A0 <- 00, lo mismo que escribe el original al encender—, corriente de carga a 0
 *    (0x61 <- 0x10, como él en ese mismo sitio) y apagar como él: 0x41 <- 0 y fuera la retención (F9050058 bit 0). Si sigue con
 *    corriente (tecla aún pulsada, cargador recién puesto… [sin saber]) se insiste una vez por segundo, a oscuras, para siempre;
 *  - lectura por debajo de 2850 mV (no creíble) o PMU mudo: seguir, como el original, con una bandera que se ve en Sistema.
 * El original, en vez de vibrar, enciende 5 s las salidas de corriente 0x36/0x37 ([inferido] un LED): aquí no, porque
 * [sin saber] qué LED son. Los umbrales son los suyos; la recta es la de fábrica, no la de ESE aparato. */
static void bateria_al_arrancar(void)
{
    u8 v = 0; int l, i, que;
    arr.estado23 = pmu_leer(0x23, &v) ? -1 : v;
    l = arr.lectura = pmu_bateria();
    arr.mv = l < 0 ? -1 : bat_mv_frio(l);
    que = bat_arranque_decidir(arr.estado23, l);
    arr.como = que == BAT_ARR_APAGAR ? HW_ARR_BIEN : que;      /* (BAT_ARR_* y HW_ARR_* valen lo mismo en lo que se devuelve) */
    hw_log("bateria al arrancar: lectura "); uart_dec(UART0, l < 0 ? 0u : (u32)l);
    hw_log(l < 0 ? " (el PMU no contesta)" : " = "); if (l >= 0) { uart_dec(UART0, (u32)arr.mv); hw_log(" mV"); }
    hw_log(", estado 23 = "); uart_hex(UART0, (u32)arr.estado23 & 0xFFu);
    if (arr.estado23 >= 0) { e23 = arr.estado23; e23_hay = 1; enc_pmu(&tel_enc, arr.estado23 & 1); }
    /* (R2; auditoría de S7, A4) LA CARGA A 0, LO PRIMERO Y EN TODOS LOS CAMINOS (antes: solo en el de batería baja; con el
     * cargador puesto —que es cuando importa— la primera escritura de 0x61 era la de la lista, 0,5 s después y tras 0x60). */
    if (pmu_carga_fuera()) { hw_log(" [CARGA A 0 SIN CONFIRMAR: 61 = "); uart_hex(UART0, (u32)pmu_carga.r61 & 0xFFu); hw_log(", 63 = "); uart_hex(UART0, (u32)pmu_carga.r63 & 0xFFu); hw_log("]"); }
    else hw_log(" [carga a 0 confirmada: 61 = 10, 63 = 00]");
    if (que == BAT_ARR_SEGUIR) { hw_log(": bien\n"); return; }
    if (que == BAT_ARR_CARGADOR) { hw_log(": hay CARGADOR o USB (la bateria se carga solo por USB configurado: api/carga.h)\n"); return; }
    if (que == BAT_ARR_NO_CREIBLE) { hw_log(": lectura NO CREIBLE (< 2850 mV o sin PMU); se sigue sin fiarse\n"); return; }
    hw_log(": BATERIA BAJA (< 3400 mV) sin cargador: NO se arranca. Tres toques de vibrador y apagar\n");
    for (i = 0; i < 3; i++) { pmu_vibrador(1); espera_ms(75); pmu_vibrador(0); espera_ms(175); }
    rst_apuntar(RST_APAGADO, 0, 0);                /* (R3) no arrancar por batería baja es un apagado ordenado: no cuenta como arranque fallido */
    for (;;) {                                     /* a oscuras: ni luz, ni panel, ni lista del PMU */
        pmu_apagar();                              /* 0x41 <- 0 y F9050058 &= ~1, como el original */
        espera_ms(1000);
        hw_log("bateria baja: sigo con corriente; insisto en apagar (41 <- 0)\n");
    }
}

#ifdef SENEW_RAM                                   /* (27-oct) etapas en la iRAM (hal/traza_iram.c): hasta dónde llegó un arranque en RAM que se colgó */
void tz_etapa(u32 n);
#define ETAPA(n) tz_etapa(n)
#else
#define ETAPA(n) ((void)0)
#endif
void hw_iniciar(void)
{
    ETAPA(10);
    pilas_sembrar();
    ETAPA(11);
#ifndef SIN_PLL
    hay_pll = (u8)reloj_208();                     /* como el original: sube ANTES de inicializar SYSCON (hal.md §2.2) */
#endif
    ETAPA(12);
#ifndef SENEW_CAMARA                                /* (27-oct) variante de DEPURACIÓN: senew-RAM sin tocar SYSCON ni GPIO, para heredar el sensor de la cámara encendido y configurado por el ORIGINAL (volcar sus registros) */
    syscon_init();
#endif
    ETAPA(13);
    id_nor = nor_id();                             /* antes de la caché de datos (hal/nor.c) */
    ETAPA(14);
#ifndef SIN_MMU
    mmu_cache_init(); hay_mmu = 1;                 /* MMU 1:1 + caché de datos e instrucciones */
#endif
    ETAPA(15);
#ifndef SENEW_CAMARA
    gpio_init();                                   /* antes de la UART: la detección de cable lee P0.0 */
#endif
    ETAPA(16);
    hay_serie = (u8)uart_init(UART0);
    ETAPA(17);
    hw_log(hay_pll ? "reloj: PLL enganchado, 208 MHz\n" : "reloj: 13 MHz\n");
    {   /* (R3, P3) POR QUÉ SE REINICIÓ: lo que dejó apuntado el arranque anterior, antes de hacer nada más */
        static const char *const que[] = { "sin registro (arranque en frio, o la memoria no sobrevivio): causa DESCONOCIDA", "registro valido pero NADIE apunto la causa (reset de hardware?)",
            "el PERRO (el sistema no respondia)", "una EXCEPCION de la CPU", "un PANICO del sistema", "una PILA casi llena", "un APAGADO FORZADO que no apago", "reinicio pedido", "apagado ordenado" };
        rst_arranque(&rst);
        hw_log("arranque: el anterior acabo por: "); hw_log(que[rst.causa <= RST_APAGADO ? rst.causa : 0]);
        if (rst.causa == RST_PERRO || rst.causa == RST_EXCEPCION) { hw_log(" (pc = "); uart_hex(UART0, rst.pc); hw_log(", lr = "); uart_hex(UART0, rst.lr); hw_log(")"); }
        hw_log("; arranques seguidos que no llegaron al bucle: "); uart_dec(UART0, rst.sin_bucle); hw_log("\n");
        if (rst.valido && rst.sin_bucle >= RST_ARRANQUES_MALOS) {
            seguro_auto = 1;
            hw_log("*** MODO SEGURO AUTOMATICO: los 3 ultimos arranques no llegaron al bucle\n");
        }
    }
    /* (S4) LA LUZ, EN EL ORDEN DEL ORIGINAL (pista E5, con el chip de LED contestando; docs/sistema.md §10.1):
     *  1. el pulso de la luz del PMU del arranque temprano (0x45 <- 0x50, 0x43 <- 0x80; el original, de 0,12 a 0,58 s);
     *  2. la lista de inicio del PMU, que la apaga (0x43 <- 0x04) y no vuelve a encenderse;
     *  3. el inicio del chip de LED (toda su sombra);  4. el panel;  5. la luz del logo, que ya es del chip. */
    bateria_al_arrancar();                         /* (S7) ANTES de encender nada: con la batería baja y nada enchufado, de aquí no se pasa */
    if (pmu_luz_pantalla(100)) hw_log("luz: el PMU no contesta por I2C\n");
    memset(lcd_cuadro, 0, sizeof lcd_cuadro);      /* .gram no se pone a cero en el arranque (lcd_enviar baja la caché de lo que manda) */
#ifndef MEDIR
    tic_init();
    espera_ms(LUZ_PULSO_PMU_MS);                   /* lo que dura el pulso en el original */
#endif
    rtc_init();
    if (pmu_encendido()) hw_log("pmu: no contesta\n");
    if (!pmu_carga.confirmada) { hw_log("pmu: *** LA CARGA A 0 NO SE HA PODIDO CONFIRMAR (61 = "); uart_hex(UART0, (u32)pmu_carga.r61 & 0xFFu); hw_log(", 63 = "); uart_hex(UART0, (u32)pmu_carga.r63 & 0xFFu); hw_log("): NO ENCHUFAR EL CARGADOR ***\n"); }
    else hw_log("pmu: carga a 0 confirmada tras la lista (61 = 10, 63 = 00)\n");   /* la lista de inicio del PMU del original (apaga la luz del PMU); después, fuera la retención */
    chip_calla = ledc_init() != 0;
    hw_log(chip_calla ? "luz: el chip de LED (I2C 1, 0x40) no contesta\n" : "luz: chip de LED (I2C 1, 0x40) iniciado\n");
#ifdef MEDIR
    lcd_estado = (signed char)lcd_init(lcd_cuadro);
#else
    lcd_estado = (signed char)lcd_init(lcd_cuadro);   /* detecta el panel y le manda un cuadro negro antes de encenderlo (init_seq del original) */
    if (lcd_estado) hw_log("lcd: el bloque grafico no contesta (sin pantalla)\n");
    else {
        hw_log("lcd: panel iniciado, 240x320\n");
        hw_log("lcd: descriptor "); hw_log(lcd_panel_nombre());
        hw_log(lcd_panel_r28() < 0 ? " (el registro 0x0028 no se pudo leer)\n" : (lcd_panel_r28() & 0xF) == 0 ? " (R28 & 0xF = 0)\n" : (lcd_panel_r28() & 0xF) == 0xA ? " (R28 & 0xF = A)\n" : (lcd_panel_r28() & 0xF) == 0xE ? " (R28 & 0xF = E)\n" : " (R28 no casa con ninguno)\n");
    }
    kbd_init();
    {   /* (S7) MODO SEGURO: la tecla «C» (celda 8) pulsada AHORA —dos barridos separados 30 ms, los dos con ella—. Se mira una vez. */
        u8 m1[7], m2[7];
        kbd_listo = 1;
        kbd_mapa(m1); espera_ms(30); kbd_mapa(m2);
        modo_seguro = (u8)((m1[1] & m2[1] & 0x08) != 0);
        if (modo_seguro) seguro_auto = 0;          /* lo ha pedido una persona: no es el automático */
        else if (seguro_auto) modo_seguro = 1;     /* (R3, P3) tres arranques seguidos sin llegar al bucle */
        if (modo_seguro) hw_log(seguro_auto ? "*** MODO SEGURO (AUTOMATICO): sin escritura en la NOR, sin leer ajustes, sin apps externas\n"
                                            : "*** MODO SEGURO (tecla C pulsada al arrancar): sin escritura en la NOR, sin leer ajustes, sin apps externas\n");
    }
    kbd_irq_init();                                /* IRQ 60: saca al sistema del reposo profundo al pulsar */
    uart_rx_irq_init();
#endif
    hw_luz_fase(HW_LUZ_LOGO);                      /* la primera luz de verdad: la 2 del chip, al nivel del logo */
#ifndef MEDIR                                      /* (R3) en las medidas no hay interrupciones ni teclado: el controlador y el teclado tienen el reloj parado (el estricto lo decía) */
    gpio_deslizador_irq_init();                    /* IRQ 67: el deslizador saca al sistema del reposo profundo */
    pmu_irq_init();                                /* IRQ 37: la tecla de encendido por el PMU */
#endif
    proteccion_decidir();                          /* (S7) ANTES de cualquier escritura: ¿es el chip esperado?, ¿modo seguro? */
    nv_iniciar();
    hw_usr_iniciar();                              /* la zona de ficheros de usuario (W6U) */
    hw_w6f_iniciar();                              /* (I1) la Memoria (W6F): solo monta; nunca da formato aquí */
#ifndef MEDIR
    if (!modo_seguro) hwo_tarjeta_iniciar();       /* (pista T) la Memory Stick: plat/tarjeta_w580.c. La detecta y la monta el primer latido.
                                                    * (AUD8 M1) En modo seguro NO: ni el regulador del PMU, ni la IRQ 76, ni la FAT; la salida de
                                                    * emergencia no puede caer por lo mismo que el arranque normal */
    else hw_log("tarjeta: MODO SEGURO: la Memory Stick NO se inicia (ni se mira)\n");
#endif
    hw_perro_latido();                             /* (S7) el perro de software queda armado: si el resto del arranque (montar el FS, los
                                                    * ajustes, decodificar el fondo) se cuelga, a los 10 s salta la pantalla de fallo */
}

/* «wait for interrupt» del ARM926 (MCR p15,0,Rd,c7,c0,4). [emulador]; en el teléfono, sin probar. */
__attribute__((target("arm"), noinline)) void hw_reposo(void)
{
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c0, 4" ::: "r0");
}
/* REPOSO PROFUNDO (hal.md §6). El tic de 4 ms deja de latir: el temporizador 1 se pone a un solo plazo largo
 * y se espera con WFI. Al volver se repone el tic y se pone hw_ms() al día con lo que diga el RTC, que es el
 * único reloj que no se para. Despierta el temporizador como tarde, y antes que él cualquier IRQ abierta:
 * teclado (60), alarma del RTC (36) o serie (46). A diferencia del W600, aquí NO se cierran puertas de reloj
 * antes del WFI: [sin saber] cuáles cierra el original del DB2020 al dormir (FUN_4423667c deja +1C y +38; no
 * se ha seguido). [emulador]; en un teléfono, sin probar. */
u32 hw_dormir(u32 max_ms)
{
#ifdef MEDIR
    extern u32 medir_ms; medir_ms += max_ms; return max_ms;
#else
    u32 t0, t1, dt, ms, k0, p0;
    if (tic_tono_hasta) { hw_reposo(); return 0; } /* un tono con plazo lo apaga el TIC: dormir a fondo lo dejaría clavado */
    if (max_ms > 1900) max_ms = 1900;              /* el comparador son 16 bits de cuentas de 32 768 Hz */
    if (max_ms < 20) max_ms = 20;
    kbd_destapar();                                /* (AUD6 B5) si la guarda de ráfaga dejó tapada la IRQ 60, una tecla no despertaría */
    vibra_paso();
    if (vibra_hasta) { hw_reposo(); return 0; }    /* vibrando: lo apaga el bucle, no se duerme a fondo */
    luz_paso();
    if (luz_en_rampa()) { hw_reposo(); return 0; } /* una rampa de luces a medias: sus pasos van por el tic */
    k0 = kbd_irqs; p0 = desliz_irqs;
    irq_cerrar();                                  /* el montaje del plazo largo y las puertas, sin que entre nada */
    t0 = rtc_128();
    tic_largo(max_ms * 32768u / 1000u);
    cta_dormidas++;
    irq_abrir();                                   /* queda una ventana de dos instrucciones: si entra ahí una IRQ,
                                                      el WFI espera a la siguiente y como tarde al temporizador */
    hw_reposo();                                   /* WFI */
    irq_cerrar();
    tic_normal();
    t1 = rtc_128();
    dt = (t1 >= t0 ? t1 : t1 + 3600u * 128u) - t0; /* 1/128 s; el RTC da la vuelta cada hora */
    ms = dt * 1000u / 128u;
    if (ms > max_ms + 200) ms = max_ms;            /* si el RTC no corre (chip sin poner), no inventes tiempo */
    tics += ms / 4;                                /* hw_ms() al día: el tic no ha latido mientras dormíamos */
    irq_abrir();
    cta_ms_dormido += ms;
    if (kbd_irqs != k0) cta_desp_tecla++;
    if (desliz_irqs != p0) cta_desp_tapa++;
    return ms;
#endif
}

/* ---- pantalla de fallo -----------------------------------------------------------------------------
 * Un fallo sin salida (excepción de la CPU, montículo o pila agotados) no puede confiar en el framework
 * ni en el montículo: esto pinta a mano sobre el cuadro estático con las primitivas del lienzo (que solo
 * gastan pila), lo manda también por la UART, y espera una tecla para reiniciar. No vuelve. */
static const u32 *pan_ctx;
static void hex8(char *s, u32 v) { int i; for (i = 7; i >= 0; i--) { u32 d = v & 15; s[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v >>= 4; } s[8] = 0; }
static const char *const pan_exc[8] = { "", "INSTRUCCION INDEFINIDA", "SWI", "ABORTO DE PREBUSQUEDA",
                                        "ABORTO DE DATOS", "EXCEPCION RESERVADA", "", "FIQ" };
static const char *const pan_reg[8] = { "r12", "sp ", "lr ", "pc ", "cpsr", "tipo", "DFSR", "FAR " };

static void panico_cuerpo(void *arg)
{
    static char lin[48];
    Lienzo l = { ui_pantalla, { 0, 0, UI_ANCHO, UI_ALTO } };
    const Color rojo = RGB(128, 0, 0), blanco = RGB(255, 255, 255), amar = RGB(255, 208, 64);
    int y, i;
    tic_en_marcha = 0;                             /* el tic ya no late (espera_ms va por el temporizador 2) */
    rst_apuntar(pan_causa ? pan_causa : RST_PANICO, pan_pc, pan_lr);   /* (R3, P3) lo primero: el arranque siguiente sabrá por qué */
    hw_log("\n*** FALLO: "); hw_log(pan_titulo); hw_log(" / "); hw_log(pan_detalle ? pan_detalle : ""); hw_log("\n");
    if (pan_ctx) {
        for (i = 0; i < 17; i++) { hex8(lin, pan_ctx[i]); hw_log(lin); hw_log((i & 3) == 3 ? "\n" : " "); }
        hw_log("\n");
    }
    tic_gancho = 0; tic_audio = 0; tic_camara = 0; tic_micro = 0; tic_tono_hasta = 0; if (tono_listo) tono_fin();   /* (S7) nada sigue sonando ni latiendo (2026-10-26: ni el alimentador del DSP; 1.9: ni la cámara) */
    pmu_carga_fuera();                              /* (S7) el sistema se queda sin bucle: que no quede ninguna consigna de carga (0x61 <- 0x10, 0x63 <- 0).
                                                    * (R2; M5 de la auditoría de S7) con el veto del bus levantado, 3 intentos y releído */
    vibra_hasta = 0; i2c_revivir(0); pmu_vibrador(0);              /* (S7) ni el vibrador (antes solo lo apagaba quien pasara por hw_tecla_evento) */
    luz_ya(HW_LUZ_VIVA);                           /* por si el fallo llega con la pantalla dormida o atenuada (sin rampas: no hay tic) */
    lcd_despertar(0);
    {   /* (estética) la letra en uso */
    const int pq = F_ESTILO(DIS_T_PEQUENA), ti = F_ESTILO(DIS_T_TITULO), hp = dis_alto_texto(1, pq, NULL), ppq = dis_paso_linea(pq), pl = dis_paso_linea(0);
    li_rect(&l, 0, 0, UI_ANCHO, UI_ALTO, rojo);
    /* (estética, 26-oct) con la letra en uso: la banda, los detalles, los registros y el pie en la talla PEQUEÑA (caben más líneas) y
     * los pasos de línea de la letra (antes 10 px y posiciones fijas) */
    li_rect(&l, 0, 0, UI_ANCHO, hp + 4, RGB(200, 0, 0));
    li_texto_f(&l, 4, 2, "FALLO DEL SISTEMA", blanco, 1, pq);
    y = hp + 8;
    /* el título a escala 2 si cabe; si no, a 1: un título cortado en la pantalla de fallo sería la
     * peor manera de enterarse de lo que ha pasado */
    if (li_ancho_texto_f(pan_titulo, 1, ti) <= UI_ANCHO - 8) { li_texto_f(&l, 4, y, pan_titulo, amar, 1, ti); y += dis_alto_texto(1, ti, pan_titulo) + 4; }
    else {
        const char *t = pan_titulo;
        while (*t && y < hp + 8 + 3 * pl) {
            int n = li_partir(t, UI_ANCHO - 8, 1, 0);
            li_texto_n(&l, 4, y, t, n, amar, 1, 0);
            t += n; while (*t == ' ' || *t == '\n') t++;
            y += pl;
        }
        y += 4;
    }
    if (pan_detalle && pan_detalle[0]) {
        const char *d = pan_detalle;
        while (*d && y + ppq <= UI_ALTO - 3 * ppq - 4 - (pan_ctx ? 2 * ppq : 0)) {   /* a trozos: la línea se parte sola; deja sitio a 2 registros y al pie */
            int n = li_partir(d, UI_ANCHO - 8, 1, pq);
            li_texto_n(&l, 4, y, d, n, blanco, 1, pq);
            d += n; while (*d == ' ' || *d == '\n') d++;
            y += ppq;
        }
    }
    if (pan_ctx) {
        y += 4;
        for (i = 0; i < 8 && y + ppq <= UI_ALTO - 3 * ppq - 4; i++) {
            u32 v = pan_ctx[i < 1 ? 12 : 12 + i];  /* r12, sp, lr, pc, cpsr, tipo, DFSR, FAR */
            li_texto_f(&l, 4, y, pan_reg[i], amar, 1, pq);
            hex8(lin, v);
            li_texto_f(&l, 8 + li_ancho_texto_f("DFSR", 1, pq) + 6, y, lin, blanco, 1, pq);
            y += ppq;
        }
    }
    li_texto_f(&l, 4, UI_ALTO - 3 * ppq - 2, arg ? "Reinicio en 15 s (o con una tecla)" : "Una tecla reinicia el telefono", blanco, 1, pq);
    li_texto_f(&l, 4, UI_ALTO - 2 * ppq - 2, "Encendido 3 s: APAGAR", blanco, 1, pq);
    li_texto_f(&l, 4, UI_ALTO - ppq - 2, HW_VERSION, RGB(255, 160, 160), 1, pq);
    hw_pantalla_volcar(ui_pantalla, 0, UI_ALTO);
    }
    hw_log("(pantalla de fallo pintada; una tecla reinicia)\n");
    espera_ms(400);                                /* no contar la tecla que venía pulsada */
    {   /* el tic está parado: sondeo puro. (S7) La tecla de ENCENDIDO mantenida 3 s APAGA (por la celda 30 o por el PMU, 0x23 bit 0,
         * que aquí sí se puede leer: el I²C es nuestro); cualquier otra tecla —o soltar la de encendido antes— reinicia; y si el
         * fallo vino del perro (arg), reinicio solo a los 15 s: un cuelgue sin nadie delante no deja el teléfono encendido en rojo. */
        /* (R2; auditoría de S7) B7: no se acepta NINGUNA tecla (ni la de encendido) hasta haberlo visto TODO SUELTO una vez: con
         * la C del modo seguro aún pulsada, una tecla atascada o la foto de la tecla de encendido clavada, la pantalla se iba
         * sin poder leerse. M2: aunque nadie pulse, la batería se vigila (cada 10 s, como en marcha: 0x77, o 0x6A si no vale;
         * dos lecturas creíbles seguidas por debajo del corte APAGAN —no en modo seguro—) y la luz baja al minuto. */
        u32 vueltas = 0, enc_v = 0, bajas = 0; u8 v23 = 0, suelto = 0;
        for (;;) {
            u8 m[7]; int n = kbd_mapa(m), e;
            if (!(vueltas % 10)) { i2c_revivir(0); if (pmu_leer(0x23, &v23)) v23 = 0; }
            if (n == 0) suelto = 1;                      /* el TECLADO entero suelto, visto aquí */
            /* la de encendido: por el PMU (nivel; vale si se le vio suelta alguna vez desde el arranque: api/enc.h) o por el
             * teclado (solo tras verlo suelto aquí). Así quien llega manteniéndola porque la interfaz no respondía —y solo
             * llega por el PMU— apaga sin tener que soltarla, y una señal clavada no apaga nada. */
            e = (!(v23 & 1) && tel_enc.pmu_suelta) || ((m[6] & 1) && suelto);   /* (2026-10-20) PMU bit 0 invertido */
            if (e) { if (++enc_v >= 150) break; }        /* 150 vueltas de 20 ms */
            else if (enc_v) break;                       /* soltó la de encendido antes de tiempo: reinicia */
            else if (suelto && n > 0) break;             /* otra tecla (pulsada DESPUÉS de verlo todo suelto): reinicia */
            if (arg && vueltas >= 750) break;
            if (vueltas == 3000) luz_ya(HW_LUZ_ATENUADA);
            if (vueltas && !(vueltas % 500)) {
                int c = pmu_vbat_auto(), mv;
                if (c == 0x00 || c == 0xFF) { c = pmu_bateria(); mv = c < 0 ? -1 : bat_mv_frio(c); } else mv = c < 0 ? -1 : bat_mv_marcha(c);
                if (mv >= HW_BAT_MV_NO_CREIBLE && mv < HW_BAT_MV_CORTE) bajas++; else if (mv >= 0) bajas = 0;
                if (bajas >= 2 && !modo_seguro) { hw_log("fallo: bateria por debajo del corte: APAGAR\n"); enc_v = 150; break; }
            }
            espera_ms(20); vueltas++;
        }
        if (enc_v >= 150) {
            hw_log("fallo: encendido mantenido 3 s: APAGAR\n");
            luz_ya(HW_LUZ_APAGADA); lcd_apagar();
            for (vueltas = 0; vueltas < 3; vueltas++) { i2c_revivir(0); pmu_apagar(); espera_ms(500); }
            hw_log("fallo: sigo vivo tras apagar: reinicio\n");   /* (la causa apuntada sigue siendo la del fallo) */
        }
    }
    sys_reset();
    for (;;) ;
}
void hw_panico(const char *titulo, const char *detalle)
{
    if (en_panico) sys_reset();                    /* un fallo dentro del fallo: reinicia y no lo intentes otra vez */
    en_panico = 1;
    pan_titulo = titulo; pan_detalle = detalle; pan_ctx = 0;
    en_pila_de_fallo(panico_cuerpo, 0);            /* la pila normal puede estar rota: se usa la de fallo */
    for (;;) ;
}
/* La llama arranque.S desde cualquier excepción que no sea IRQ, ya con la pila de fallo puesta. */
void excepcion_c(const u32 *ctx)
{
    static char det[64];
    u32 tipo = ctx[17];
    cta_excepciones++;
#ifdef PUENTE
    if (!en_panico && !nor_en_curso) puente_fallo_cpu(ctx);   /* (v16) una orden del puente (dirección mala...): vuelve a ella con el tipo y el PC; si no era suya, devuelve 0 y sigue */
#endif
    if (!en_panico && !nor_en_curso) cg_fallo_cpu(ctx);   /* (C) con código de una app CARGABLE en curso: se cierra ELLA y esto no vuelve (shell/cargador.c) */
    if (en_panico) sys_reset();
    en_panico = 1;
    hex8(det, ctx[15]);                            /* «pc = XXXXXXXX» */
    det[8] = ' '; det[9] = 'p'; det[10] = 'c'; det[11] = 0;
    pan_causa = RST_EXCEPCION; pan_pc = ctx[15]; pan_lr = ctx[14];
    pan_titulo = tipo < 8 ? pan_exc[tipo] : "EXCEPCION";
    pan_detalle = det; pan_ctx = ctx;
    panico_cuerpo(0);
    for (;;) ;
}

int  hw_reiniciar(void)
{
    veda = 0; nor_veda = 0; vp_vaciar();           /* (2026-10-25) lo aplazado por la veda de la música, antes de irse */
    hwo_tarjeta_apagar();                          /* (28-oct) la M2: lo escrito, CERRADO y sin tensión antes de irse (el PMU no se reinicia con la
                                                      CPU, pero el que arranque después le quitará la tensión sin cerrar: docs/tarjeta_escritura.md §10) */
    rst_apuntar(RST_PEDIDO, 0, 0);
    pmu_carga_fuera();             /* (CARGA) el PMU NO se reinicia con la CPU: sin esto 0x63 seguiría cargando sin nadie (p. ej. en el bootloader, grabando) */
    sys_reset();
    return HW_OK;
}
/* APAGAR (docs/bateria.md §5.5, §8.A6). Como el original sin cargador: luces fuera, guion «shutdown» del panel, 0x41 <- 0 y
 * fuera la retención. Si a los 500 ms sigue habiendo corriente, el teléfono NO se ha apagado: [sin saber] si 0x41 <- 0 apaga con
 * el cargador o el USB puestos (el original ni lo intenta: reinicia en «solo carga»). (S7) En ese caso YA NO se reescribe la lista
 * del PMU ni se enciende nada: se devuelve HW_ERROR con el panel y las luces apagados, y el shell decide (shell/energia.c). */
int  hw_apagar(void)
{
    veda = 0; nor_veda = 0; vp_vaciar();           /* (2026-10-25) lo aplazado por la veda de la música, antes de irse */
    hwo_tarjeta_apagar();                          /* (pista T) la tarjeta, sin tensión antes de apagar */
    hw_luz_fase(HW_LUZ_APAGADA);
    hw_tono_fin();
    vibra_hasta = 0; pmu_vibrador(0);
    lcd_apagar();                                  /* guion «shutdown» del panel (shutdown_seq) */
    hw_log("apagar: 41 <- 0 y fuera la retencion\n");
    rst_apuntar(RST_APAGADO, 0, 0);                /* (R3) apagado ordenado */
    {   /* (R2; auditoría de S7, A1) si la ESCRITURA no sale (bus vetado, PMU a medias), se repite hasta 3 veces levantando el veto */
        int i; for (i = 0; i < 3; i++) { i2c_revivir(0); if (!pmu_apagar()) break; hw_log("apagar: la escritura de 41 NO ha salido; otra vez\n"); }
    }
    espera_ms(500);                                /* si de verdad se apaga, aquí ya no hay corriente */
    hw_log("apagar: SIGO VIVO a los 500 ms (no se reescribe nada en el PMU)\n");
    return HW_ERROR;
}
#ifdef PUENTE
#ifdef SENEW_RAM
void tz_log(const char *s);                                      /* hal/traza_iram.c: el rastro en la iRAM, que sobrevive a un reinicio del perro */
void hw_log(const char *s) { uart_puts(UART0, s); pu_log(s); tz_log(s); }
#else
void hw_log(const char *s) { uart_puts(UART0, s); pu_log(s); }   /* (v16) además, al anillo que el puente lee por USB */
#endif
#else
void hw_log(const char *s) { uart_puts(UART0, s); }
#endif
u32  hw_cuenta(void) { return R32(0xF901001C); }

void hw_info(HwInfo *i)
{
    HwFsEstado f;
    hwo_fs_estado(&f);
    i->version = HW_VERSION; i->plataforma = "W580i (DB2020)";
    i->cpu_mhz = reloj_mhz; i->pll = hay_pll; i->mmu = hay_mmu;
    i->nor_id = id_nor; i->nand_id = 0; (void)f;
    i->panel = lcd_estado ? 0 : lcd_panel_nombre(); i->panel_r28 = lcd_panel_r28();
    i->ram_total = 16u << 20; i->ram_estatica = (u32)(_fin - _data);
    i->tam_codigo = (u32)(_data_flash - _inicio) + (u32)(_edata - _data);
    i->irq_serie = uart_rx_irqs; i->irq_alarma = rtc_alarmas;
}

/* ---- pantalla y luces ------------------------------------------------------------------------------ */
/* (estética, giro) el cuadro lógico ocupa los mismos píxeles en las cuatro orientaciones; girado, su paso de fila es su ancho y el
 * PDICON lo manda al panel con el código y la esquina de cada giro (medidos en el aparato: docs/estetica.md §7) */
static u8 giro_pant;
static const u8 giro_codigo[4] = { 0, 5, 3, 6 };
#define GIRO_AN(g) ((g) & 1 ? PLACA_ALTO : PLACA_ANCHO)
#define GIRO_AL(g) ((g) & 1 ? PLACA_ANCHO : PLACA_ALTO)
static u32 giro_esquina(int g) { return g == 1 ? (u32)(GIRO_AL(g) - 1) << 16 : g == 2 ? (u32)(GIRO_AN(g) - 1) | (u32)(GIRO_AL(g) - 1) << 16 : g == 3 ? (u32)(GIRO_AN(g) - 1) : 0; }
int hw_pantalla_girar(int g) { giro_pant = (u8)(g & 3); return HW_OK; }
void hw_pantalla_volcar(const u16 *c, int y0, int filas)
{
    const int an = GIRO_AN(giro_pant), al = GIRO_AL(giro_pant);
    const u16 *o = c + y0 * an; u16 *d = lcd_cuadro + y0 * an;
    int n = filas * an;
    if (y0 < 0 || filas <= 0 || y0 + filas > al) return;
    if ((((u32)o | (u32)d) & 3) == 0) {                /* (pista V) de dos en dos píxeles */
        const u32 *o4 = (const u32 *)(const void *)o; u32 *d4 = (u32 *)(void *)d;
        u32 m = 0x00FF00FFu;
        __asm__("" : "+r"(m));                         /* (pista O) la máscara, en un registro y opaca: si el compilador la ve constante hace bswap + ror (7 por palabra); así son 5 */
#define GIRA(a) ((((a) >> 8) & m) | (((a) & m) << 8))
        for (; n >= 16; n -= 16, o4 += 8, d4 += 8) {
            u32 a = o4[0], b = o4[1]; d4[0] = GIRA(a); d4[1] = GIRA(b);
            a = o4[2]; b = o4[3]; d4[2] = GIRA(a); d4[3] = GIRA(b);
            a = o4[4]; b = o4[5]; d4[4] = GIRA(a); d4[5] = GIRA(b);
            a = o4[6]; b = o4[7]; d4[6] = GIRA(a); d4[7] = GIRA(b);
        }
        for (; n >= 2; n -= 2) { const u32 a = *o4++; *d4++ = GIRA(a); }
#undef GIRA
        o = (const u16 *)(const void *)o4; d = (u16 *)(void *)d4;
    }
    while (n--) { u16 v = *o++; *d++ = (u16)((v >> 8) | (v << 8)); }
    if (giro_pant) lcd_enviar_girado(lcd_cuadro, (u32)an, giro_codigo[giro_pant], giro_esquina(giro_pant));   /* (giro) el cuadro entero, girado por el PDICON */
    else lcd_enviar(lcd_cuadro, (u32)y0, (u32)filas);   /* el PDICON no ve la caché: lcd_enviar baja a la SDRAM esas filas antes de mandarlas (hal/lcd.c) */
    luz_paso();                                    /* una app a pantalla completa no pasa por hw_tecla_evento: que las rampas no se queden a medias */
    vibra_paso();                                  /* (R2) ni el vibrador encendido hasta que salga (tope de 2 s) */
}

/* LUCES (pista S4): COMO EL ORIGINAL. docs/sistema.md §10.1.
 * Qué hay (hal.md §4.6 y §12; DD_drv_Backlight.c) y qué hace el original con cada cosa [observado en el emulador con el
 * chip de LED contestando, pista E5: work/emu/trazas/e5_c2led_s.*, y cotejado aquí con su pseudo-C]:
 *   a) PMU 0x43 bit 7 + 0x45: un pulso en el arranque temprano (0,12–0,58 s), antes de encender el panel. Su lista de
 *      inicio del PMU la apaga y NO vuelve;
 *   b) los dos PWM del GPIO (la «luz 0» de DD_drv_Backlight): no los enciende NUNCA;
 *   c) el chip de LED del I²C 1, 0x40: TODA la luz en marcha. La luz 2 (registro 0x2F) es la única encendida con el
 *      logo y la que se atenúa sin apagarse: [inferido, fuerte] la de la pantalla. Las luces 4 y 1 (0x09, 0x0A) y los
 *      dos grupos de tres entran al llegar al standby y se apagan al atenuar: [inferido] teclado y demás.
 *   [sin saber] qué LED cuelga de cada salida y qué corriente es cada nivel: el emulador es un banco de registros.
 * Lo que se escribe en cada FASE (hw_luz_fase) es lo del original, en su orden y con sus rampas:
 *   LOGO      luz 2: elevador (0x22 <- 0x84, 0x21 <- 0x9A, 0x00 <- 0x08), campo (0x02 <- 0x40), nivel 0x2F <- 0x84;
 *   VIVA      luz 4: 2, 4, 5 cada 50 ms; 100 ms; luz 1: 2, 4, 5; 100 ms; los dos grupos a FF, FF, FF; y la luz 2 al
 *             nivel del brillo de 0x10 en 0x10 cada 50 ms (el original: 84, 74, …, 34, 2F; al despertar 06, 16, 26, 2F);
 *   ATENUADA  la luz 2 baja a 39/47 de su nivel (2F -> 2C, 29, 27) y las luces 1 y 4 a 4; a los 3 s (el original: 2,9 y
 *             5,0 s en las dos veces que se le vio) los grupos fuera y, cada 50 ms, la luz 2 de 3 en 3 hasta 0x06 y las
 *             luces 1 y 4 de 1 en 1 hasta apagarse;
 *   DORMIDA   (el panel duerme) la luz se queda como en ATENUADA: 0x2F = 0x06. Así lo deja el original los 12 s que se
 *             le ha visto con el panel dormido; [sin saber] si más tarde la apaga. LUZ_DORMIDA_COMO_EL_ORIGINAL = 0 la apaga;
 *   APAGADA   todo fuera por el camino de FUN_4529c810 / FUN_4529c9ec [pseudo-C: no se ha visto ejecutar un apagado
 *             con el chip presente].
 * EL BRILLO de Ajustes (hw_luz(0, pct)) se reparte DENTRO del rango del original, nunca por encima de su máximo:
 *   1..50 % -> 0x06..0x2F (50 %, el de fábrica, es el nivel de su standby) y 50..100 % -> 0x2F..0x84 (el de su logo).
 * PMU y PWM se quedan APAGADOS en marcha, como en el original: encender lo que él nunca enciende es justo el riesgo
 * que vigila el emulador estricto. «Todas las candidatas a la vez» queda solo como DIAGNÓSTICO explícito: el main del
 * hito 6 (las recorre de una en una) y compilar con -DLUCES_TODAS=1 (además de lo de arriba, PMU y PWM encendidos).
 * Si el chip no contesta (el emulador por omisión; o un fallo nuestro del I²C 1 en un aparato) no se enciende NADA más:
 * se dice por la UART y Ajustes lo enseña; se le vuelve a preguntar cada vez que la pantalla despierta.
 * Las rampas no bloquean: hw_luz_fase() monta un guion de pasos y luz_paso() —desde hw_tecla_evento, hw_teclas_mapa,
 * hw_pantalla_volcar y hw_dormir; nunca desde una interrupción, que el I²C va por sondeo— da los que toquen. */
#ifndef LUZ_DORMIDA_COMO_EL_ORIGINAL
#define LUZ_DORMIDA_COMO_EL_ORIGINAL 1
#endif
#define LUZ_A_MANO_MS 5000                         /* lo que dura una luz encendida o apagada a mano desde Ajustes */
#define LZ_MINIMO  0x06                            /* 0x2F atenuado */
#define LZ_STANDBY 0x2F                            /* 0x2F en el standby del original = nuestro 50 % */
#define LZ_LOGO    LEDC_MAX_PANTALLA               /* 0x2F con el logo = nuestro 100 % */
enum { LZ_LUZ, LZ_GRUPO, LZ_ESPERA };              /* LZ_LUZ: a = luz, b = nivel · LZ_GRUPO: a = grupo, b = nivel de los tres · LZ_ESPERA: a + 256 b ms */
static struct { u8 op, a, b; } lz_guion[120];
static u8 lz_n, lz_pc;
static u8 lz_fase = 0xFF, lz_brillo = 50;
static u32 lz_hasta;                               /* hw_ms() hasta el que espera el guion (0 = no espera) */
/* (S5) El color de las laterales en uso normal (niveles de registro R, G, B; hw_luz_color). De fábrica blanco: lo del
 * original. Y si un EFECTO tiene las luces (hw_luz_efecto): entonces la fase no da pasos y el motor (api/luces.c) manda. */
static u8 lz_color[3] = { 0xFF, 0xFF, 0xFF };
static u8 lz_efecto;
static u8 dz_act, dz_obj;                          /* el teclado del deslizador: lo escrito y a dónde va (más abajo) */
static u32 lz_fase_ms;                             /* hw_ms() del último cambio de fase (el teclado del deslizador mide desde ahí) */
static u32 pwm_t0, pwm_rampas;                     /* cuándo arrancó la última rampa del PWM del chip (para decir cómo «se ve»: hw_luz_leer) */
extern u32 ledc_rampas;
static void rgb_sello(void) { if (ledc_rampas != pwm_rampas) { pwm_rampas = ledc_rampas; pwm_t0 = hw_ms(); } }
static const u8 luz_del_chip[HW_LUCES] = { 0, 1, 2, 4 };
static const u8 luz_reg_nivel[HW_LUCES] = { 0, 0x0A, 0x2F, 0x09 };

static void lz_pon(u8 op, u32 a, u32 b)
{
    if (lz_n >= sizeof lz_guion / sizeof lz_guion[0]) return;
    lz_guion[lz_n].op = op; lz_guion[lz_n].a = (u8)a; lz_guion[lz_n].b = (u8)b; lz_n++;
}
static void lz_espera(u32 ms) { lz_pon(LZ_ESPERA, ms & 0xFF, ms >> 8); }
static int lz_sombra(u8 reg) { int v = ledc_sombra_de(reg); return v < 0 ? 0 : v; }
static int lz_nivel(int pct)                       /* brillo 1..100 -> registro 0x2F, dentro del rango del original */
{
    if (pct <= 50) return LZ_MINIMO + (LZ_STANDBY - LZ_MINIMO) * pct / 50;
    return LZ_STANDBY + ((int)LZ_LOGO - LZ_STANDBY) * (pct - 50) / 50;
}
static void lz_pantalla_a(int *p, int destino, int paso)   /* rampa de la luz 2: el primer paso ya, los demás cada 50 ms */
{
    while (*p != destino) {
        if (*p < destino) *p = *p + paso >= destino ? destino : *p + paso;
        else *p = *p - paso <= destino ? destino : *p - paso;
        lz_pon(LZ_LUZ, 2, (u32)*p);
        if (*p != destino) lz_espera(50);
    }
}
static void lz_teclado_sube(u8 luz, int k)         /* 2, 4, 5 cada 50 ms, y 100 ms */
{
    while (k < (int)LEDC_MAX_TECLADO) {
        k = k + 2 > (int)LEDC_MAX_TECLADO ? (int)LEDC_MAX_TECLADO : k + 2;
        lz_pon(LZ_LUZ, luz, (u32)k);
        lz_espera(k < (int)LEDC_MAX_TECLADO ? 50 : 100);
    }
    while (k > (int)LEDC_MAX_TECLADO) {            /* (S5) un efecto la dejó por encima del standby (hasta 0x10): baja de 2 en 2 */
        k = k - 2 < (int)LEDC_MAX_TECLADO ? (int)LEDC_MAX_TECLADO : k - 2;
        lz_pon(LZ_LUZ, luz, (u32)k);
        lz_espera(50);
    }
}
static void lz_al_minimo(int p, int k1, int k4)    /* la segunda mitad del atenuado: grupos fuera y todo abajo, cada 50 ms */
{
    int paso = (p - LZ_MINIMO + 10) / 11;
    if (paso < 3) paso = 3;
    lz_pon(LZ_GRUPO, 0, 0); lz_pon(LZ_GRUPO, 1, 0);
    while (p > LZ_MINIMO || k1 > 0 || k4 > 0) {
        if (p > LZ_MINIMO) { p = p - paso < LZ_MINIMO ? LZ_MINIMO : p - paso; lz_pon(LZ_LUZ, 2, (u32)p); }
        if (k1 > 0) lz_pon(LZ_LUZ, 1, (u32)--k1);
        if (k4 > 0) lz_pon(LZ_LUZ, 4, (u32)--k4);
        if (p > LZ_MINIMO || k1 > 0 || k4 > 0) lz_espera(50);
    }
}
static void lz_montar(int fase, u32 antes_ms)      /* el guion para llegar a esa fase DESDE LO QUE HAY ESCRITO en el chip (tras esperar antes_ms) */
{
    int p = lz_sombra(0x2F), k4 = lz_sombra(0x09), k1 = lz_sombra(0x0A), d, paso = 3;
    lz_n = lz_pc = 0; lz_hasta = 0;
    if (antes_ms) lz_espera(antes_ms);
    switch (fase) {
    case HW_LUZ_LOGO:
        lz_pon(LZ_LUZ, 2, LZ_LOGO);
        break;
    case HW_LUZ_VIVA:
        lz_teclado_sube(4, k4);
        lz_teclado_sube(1, k1);
        lz_pon(LZ_GRUPO, 0, 0xFF); lz_pon(LZ_GRUPO, 1, 0xFF);
        lz_pantalla_a(&p, lz_nivel(lz_brillo), 0x10);
        break;
    case HW_LUZ_ATENUADA:
        d = p * 39 / 47; if (d < LZ_MINIMO) d = LZ_MINIMO;
        if (p > d) {
            paso = (p - d + 2) / 3; if (paso < 3) paso = 3;
            p = p - paso < d ? d : p - paso;
            lz_pon(LZ_LUZ, 2, (u32)p);
        }
        if (k1 > 4) { k1 = 4; lz_pon(LZ_LUZ, 1, 4); }
        if (k4 > 4) { k4 = 4; lz_pon(LZ_LUZ, 4, 4); }
        if (p > d) { lz_espera(50); lz_pantalla_a(&p, d, paso); }
        lz_espera(3000);
        lz_al_minimo(p, k1, k4);
        break;
    case HW_LUZ_DORMIDA:
        if (LUZ_DORMIDA_COMO_EL_ORIGINAL) { lz_al_minimo(p, k1, k4); break; }
        /* si no, todo fuera: */
        /* fallthrough */
    default:                                       /* HW_LUZ_APAGADA */
        lz_pon(LZ_GRUPO, 0, 0); lz_pon(LZ_GRUPO, 1, 0);
        lz_pon(LZ_LUZ, 1, 0); lz_pon(LZ_LUZ, 4, 0); lz_pon(LZ_LUZ, 2, 0);
        break;
    }
}
static void lz_callar(void)
{
    chip_calla = 1; lz_n = lz_pc = 0; lz_hasta = 0;
    hw_log("luz: el chip de LED ha dejado de contestar\n");
}
static int luz_en_rampa(void) { return lz_pc < lz_n || dz_act != dz_obj; }
static void lz_dar(int sin_esperas)                /* da los pasos que toquen: hasta la primera espera, o todos */
{
    while (lz_pc < lz_n) {
        int r;
        if (lz_hasta) {
            if (!sin_esperas && (int)(hw_ms() - lz_hasta) < 0) return;
            lz_hasta = 0;
        }
        if (lz_guion[lz_pc].op == LZ_ESPERA) {
            lz_hasta = hw_ms() + lz_guion[lz_pc].a + 256u * lz_guion[lz_pc].b; if (!lz_hasta) lz_hasta = 1;
            lz_pc++;
            continue;
        }
        if (lz_guion[lz_pc].op == LZ_LUZ) r = ledc_luz(lz_guion[lz_pc].a, lz_guion[lz_pc].b);
        else if (lz_guion[lz_pc].b && (lz_color[0] || lz_color[1] || lz_color[2]))        /* (S5) la terna, a SU color (blanco de fábrica = lo de S4) */
            r = ledc_grupo_fundir(lz_guion[lz_pc].a, lz_color[0], lz_color[1], lz_color[2], 2, 0);
        else r = ledc_grupo_apagar(lz_guion[lz_pc].a);
        rgb_sello();
        lz_pc++;
        if (r) { lz_callar(); return; }
    }
    lz_hasta = 0;
}
static void luz_paso(void) { if (lz_pc < lz_n) lz_dar(0); dz_paso(); }
static void luz_otras(int si)                      /* DIAGNÓSTICO (-DLUCES_TODAS=1): las candidatas que el original no usa en marcha */
{
#ifdef LUCES_TODAS
    pmu_luz_pantalla(si ? 100 : 0); pwm_luz_dd(si ? 100 : 0);
#else
    (void)si;
#endif
}
static void luz_ya(int fase)                       /* esa fase entera AHORA, sin rampas (pantalla de fallo, apagado) */
{
    lz_fase = (u8)fase; lz_fase_ms = hw_ms();
    lz_efecto = 0;                                 /* (S5) si había un efecto, pierde las luces */
    if (fase == HW_LUZ_APAGADA && dz_act) { dz_act = dz_obj = 0; pwm_luz_dd_nivel(0); }   /* (S5) y el teclado del deslizador, fuera ya */
    luz_otras(fase == HW_LUZ_LOGO || fase == HW_LUZ_VIVA || fase == HW_LUZ_ATENUADA);
    if (chip_calla) return;
    lz_montar(fase, 0);
    lz_dar(1);
}
void hw_luz_fase(int fase)
{
    if (fase < HW_LUZ_APAGADA || fase > HW_LUZ_DORMIDA) return;
    if (fase == lz_fase) return;                   /* el shell la pide cada vez que hay actividad: que no reinicie una rampa en marcha */
    if (fase == HW_LUZ_APAGADA || en_panico) { luz_ya(fase); return; }
    if (chip_calla && fase == HW_LUZ_VIVA && lz_fase != HW_LUZ_LOGO) chip_calla = ledc_init() != 0;   /* al despertar se le vuelve a preguntar */
    lz_fase = (u8)fase; lz_fase_ms = hw_ms();
    luz_otras(fase != HW_LUZ_DORMIDA);
    if (lz_efecto) {                               /* (S5) un efecto tiene las luces: sigue si la pantalla sigue viva; si no, las pierde */
        if (fase == HW_LUZ_VIVA || fase == HW_LUZ_LOGO) return;
        lz_efecto = 0;
    }
    if (chip_calla) return;
    lz_montar(fase, 0);
    lz_dar(0);
}
int hw_luz_fase_actual(void) { return lz_fase == 0xFF ? HW_LUZ_APAGADA : lz_fase; }
int hw_luz(int n, int pct)
{
    if (n < 0 || n >= HW_LUCES || pct < 0 || pct > 100) return HW_ERROR;
    if (n == 0) {
        if (!pct) hw_luz_fase(HW_LUZ_APAGADA);     /* como siempre: 0 = fuera */
        else {
            int cambia = pct != lz_brillo;
            lz_brillo = (u8)pct;
            if (lz_fase == HW_LUZ_APAGADA || lz_fase == 0xFF) hw_luz_fase(HW_LUZ_VIVA);
            else if (cambia && lz_fase == HW_LUZ_VIVA && !chip_calla && !lz_efecto) { lz_montar(HW_LUZ_VIVA, 0); lz_dar(0); }
        }
        return chip_calla ? HW_ERROR : HW_OK;
    }
    /* 1..3, a mano (Ajustes, diagnóstico): la luz 1, 2 o 4 del chip al máximo del original, o apagada, DURANTE 5 s:
     * después la fase en curso se vuelve a montar sola (apagar a mano la luz 2 no puede dejar la pantalla a oscuras). */
    if (lz_efecto) return HW_ERROR;                /* (S5) con un efecto en marcha las luces son suyas */
    chip_calla = 0;                                /* pedida a mano: se vuelve a intentar */
    lz_n = lz_pc = 0; lz_hasta = 0;
    if (ledc_luz(luz_del_chip[n], pct ? (n == 2 ? LZ_LOGO : LEDC_MAX_TECLADO) : 0)) { chip_calla = 1; return HW_ERROR; }
    if (lz_fase != 0xFF) { lz_montar(lz_fase, LUZ_A_MANO_MS); lz_dar(0); }
    return HW_OK;
}
int hw_luz_estado(int n)
{
    if (n < 0 || n >= HW_LUCES) return HW_ERROR;
    if (chip_calla) return HW_ERROR;               /* el chip de LED no contesta */
    if (n == 0) return lz_brillo;
    return lz_sombra(luz_reg_nivel[n]) ? 100 : 0;
}

/* ---- (S5) LAS SEIS LUCES UNA A UNA, PARA EL MOTOR DE EFECTOS (api/hw.h; api/luces.c) ------------------------------
 * ledid del original -> luz de su controlador (FUN_4529e530): 0 -> 2 (pantalla, 0x2F), 1 -> 0 (los dos PWM del GPIO),
 * 2 -> 1 (0x0A), 3 -> 4 (0x09). Solo valen mientras un efecto tiene las luces; el orden de las escrituras es el de
 * hal/i2c.c (elevador, campo, nivel) y los topes, los del código del original (0x84, 0x10; 0xFF en el PWM y las ternas). */
static const u8 led_del_chip[HW_LEDS] = { 2, 0, 1, 4 };
static const u8 led_reg_nivel[HW_LEDS] = { 0x2F, 0, 0x0A, 0x09 };
static const u8 led_max[HW_LEDS] = { LEDC_MAX_PANTALLA, 0xFF, LEDC_TOPE_TECLADO, LEDC_TOPE_TECLADO };
/* EL TECLADO DEL DESLIZADOR (ledid 1, los dos PWM del GPIO a la vez; [inferido, fuerte] que sea eso: docs/luces.md §3).
 * Como el original (FUN_45390f74 y power_up.ble): con el deslizador ABIERTO va con los otros teclados —sube en 400 ms al
 * encender, baja al 30 % al atenuar y a los 3 s se apaga, de 2 s en 2 s—; CERRADO, apagado de golpe. Va por pasos de 50 ms
 * como los fundidos del original (31 niveles al subir, 6 al bajar) y no depende del chip de LED. Los niveles son el 36 % y
 * el 30 % de 0xFF: lo que les toca a los otros teclados; NINGÚN valor suyo se ha visto ejecutar al original. */
#define DZ_VIVA (0xFF * 36 / 100)
#define DZ_ATEN (0xFF * 30 / 100)
static u32 dz_t __attribute__((unused)), dz_escrituras;
static void dz_paso(void)
{
#ifndef LUCES_TODAS                                /* (con el diagnóstico los PWM van encendidos a tope: no se tocan) */
    u32 t = hw_ms();
    int cerrado;
    if (en_panico) return;
    if (dz_t && (int)(t - dz_t) < 0) return;
    dz_t = t + 50; if (!dz_t) dz_t = 1;
    cerrado = gpio_deslizador() == DESLIZADOR_CERRADO_ES;
    if (lz_efecto) {                               /* con un efecto manda el motor; pero CERRADO esta luz no luce nunca (FUN_45390f74) */
        if (cerrado && dz_act) { dz_act = dz_obj = 0; pwm_luz_dd_nivel(0); dz_escrituras++; }
        return;
    }
    dz_obj = (u8)(cerrado ? 0 : lz_fase == HW_LUZ_VIVA ? DZ_VIVA : lz_fase == HW_LUZ_ATENUADA && t - lz_fase_ms < 3000 ? DZ_ATEN : 0);
    if (dz_act == dz_obj) return;
    if (cerrado || lz_fase == HW_LUZ_APAGADA || lz_fase == 0xFF) dz_act = dz_obj;
    else if (dz_act < dz_obj) dz_act = (u8)(dz_obj - dz_act > 31 ? dz_act + 31 : dz_obj);
    else dz_act = (u8)(dz_act - dz_obj > 6 ? dz_act - 6 : dz_obj);
    pwm_luz_dd_nivel(dz_act); dz_escrituras++;
#endif
}
int hw_luz_salida_max(int led) { return led < 0 || led >= HW_LEDS ? 0 : led_max[led]; }
int hw_luz_salida_nivel(int led)
{
    if (led < 0 || led >= HW_LEDS) return 0;
    return led == HW_LED_TECLADO_DESL ? dz_act : lz_sombra(led_reg_nivel[led]);
}
int hw_luz_salida(int led, int nivel)
{
    if (led < 0 || led >= HW_LEDS || nivel < 0) return HW_ERROR;
    if (!lz_efecto) return HW_NO_DISPONIBLE;
    if (nivel > led_max[led]) nivel = led_max[led];
    if (led == HW_LED_TECLADO_DESL) {
        if (nivel && gpio_deslizador() == DESLIZADOR_CERRADO_ES) nivel = 0;   /* solo con el deslizador abierto, lo pida quien lo pida */
        if (dz_act != nivel) { dz_act = dz_obj = (u8)nivel; pwm_luz_dd_nivel((u32)nivel); dz_escrituras++; }
        return HW_OK;
    }
    if (chip_calla) return HW_ERROR;
    if (ledc_luz(led_del_chip[led], (u32)nivel)) { lz_callar(); return HW_ERROR; }
    return HW_OK;
}
/* Las ternas: con un efecto, o con la pantalla viva (el color, el latido). Dormido o atenuado del todo, nada: cero tráfico. */
static int rgb_vale(void) { return !chip_calla && !en_panico && (lz_efecto || lz_fase == HW_LUZ_VIVA); }
static int rgb_hecho(int r) { rgb_sello(); if (r) { lz_callar(); return HW_ERROR; } return HW_OK; }
int hw_luz_rgb_fundir(int t, int r, int g, int b, int vel, int bajar)
{
    if (t < 0 || t > 1 || r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255 || (!r && !g && !b)) return HW_ERROR;
    if (!rgb_vale()) return chip_calla ? HW_ERROR : HW_NO_DISPONIBLE;
    return rgb_hecho(ledc_grupo_fundir(t, (u8)r, (u8)g, (u8)b, (u8)(vel & 7), bajar));
}
int hw_luz_rgb_fijo(int t, int r, int g, int b, int modo)
{
    if (t < 0 || t > 1 || r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255 || (!r && !g && !b) || modo < 1 || modo > 3) return HW_ERROR;
    if (!rgb_vale()) return chip_calla ? HW_ERROR : HW_NO_DISPONIBLE;
    return rgb_hecho(ledc_grupo_fijo(t, (u8)r, (u8)g, (u8)b, (u8)modo));
}
int hw_luz_rgb_apagar(int t)
{
    if (t < 0 || t > 1) return HW_ERROR;
    if (chip_calla || en_panico) return HW_ERROR;
    return rgb_hecho(ledc_grupo_apagar(t));        /* (si ya está apagada no escribe nada) */
}
int hw_luz_efecto(int si)
{
    if (si == 1) {
        if (chip_calla || en_panico || (lz_fase != HW_LUZ_VIVA && lz_fase != HW_LUZ_LOGO)) return 0;
        lz_efecto = 1;
        lz_n = lz_pc = 0; lz_hasta = 0;            /* el guion de la fase, donde estuviera, se deja: al devolver las luces se monta otra vez */
    } else if (si == 0 && lz_efecto) {
        lz_efecto = 0;
        if (lz_fase != 0xFF && !chip_calla && !en_panico) { lz_montar(lz_fase, 0); lz_dar(0); }
    }
    return lz_efecto;
}
void hw_luz_color(int r, int g, int b)
{
    int t;
    if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255) return;
    if (lz_color[0] == r && lz_color[1] == g && lz_color[2] == b) return;
    lz_color[0] = (u8)r; lz_color[1] = (u8)g; lz_color[2] = (u8)b;
    if (lz_efecto || !rgb_vale()) return;          /* se pondrá al montar la fase */
    for (t = 0; t < 2; t++) {                      /* como FUN_45391b30 (el naranja del Walkman): las dos ternas, fundidas a ese color */
        if (rgb_hecho(r || g || b ? ledc_grupo_fundir(t, (u8)r, (u8)g, (u8)b, 2, 0) : ledc_grupo_apagar(t))) return;
    }
}
void hw_luz_leer(HwLuces *e)
{
    extern u32 lu_pwm_modelo(u32 vel, int sentido, u32 ms);
    int t, c, s16 = lz_sombra(0x16), s00 = lz_sombra(0x00), sent = s16 & 2 ? 1 : s16 & 4 ? 2 : 0;
    memset(e, 0, sizeof *e);
    e->presente = (u8)!chip_calla; e->fase = (u8)hw_luz_fase_actual(); e->efecto = lz_efecto;
    e->nivel[HW_LED_PANTALLA] = (u8)((s00 & 8) && (lz_sombra(0x02) & 0xC0) ? lz_sombra(0x2F) : 0);
    e->nivel[HW_LED_TECLADO_1] = (u8)((s00 & 8) && (lz_sombra(0x01) & 0x03) ? lz_sombra(0x09) : 0);
    e->nivel[HW_LED_TECLADO_2] = (u8)((s00 & 8) && (lz_sombra(0x01) & 0x0C) ? lz_sombra(0x0A) : 0);
    e->nivel[HW_LED_TECLADO_DESL] = dz_act;
    e->pwm = (u8)(sent ? lu_pwm_modelo((u32)(s16 >> 3) & 7, sent, hw_ms() - pwm_t0) : (u32)lz_sombra(0x17));
    for (t = 0; t < 2; t++) for (c = 0; c < 3; c++) {
        int campo = (lz_sombra(t ? 0x04 : 0x02) >> (2 * c)) & 3, n = lz_sombra((u8)((t ? 0x13 : 0x0B) + c));
        e->rgb_nivel[t][c] = (u8)n;
        e->rgb[t][c] = (u8)(!(s00 & 4) || !campo ? 0 : campo == 2 ? n * e->pwm / 255 : n);
    }
    e->escrituras = ledc_cta.escrituras + dz_escrituras;
}

/* ---- teclas ---------------------------------------------------------------------------------------- */
static u8 mapa_ahora[7];
static u32 mapa_tic = 0xFFFFFFFF;
/* La tecla de ENCENDIDO: además de por la matriz (celda 30, a masa) llega por el PMU (suceso por la IRQ 37 y
 * estado en 0x23 bit 0), que es por donde la da el emulador de la pista E. Mientras está pulsada se relee el
 * estado cada medio segundo, por si el suceso de soltarla no llegara. */
static u8 enc_abajo; static u32 enc_tic;
static void barrer(void)
{
    u32 t = tics; u8 v;
    if (t - mapa_tic < 5) return;                  /* cada 20 ms: hace de antirrebote */
    mapa_tic = t;
#ifndef MEDIR                                      /* (R3) en las medidas las teclas las pone el guion; el bloque del teclado está sin reloj */
    kbd_mapa(mapa_ahora);                          /* m[columna] bit fila: celda = columna*5 + fila; m[6] = a masa */
#endif
    if (enc_abajo && t - enc_tic >= 125) pmu_pendiente = 1;
    if (pmu_sucesos(&v) > 0) { enc_abajo = v & 1; enc_tic = t; e23 = v; e23_ms = hw_ms(); e23_hay = 1; enc_pmu(&tel_enc, v & 1); }   /* (R2) solo si 0x23 se leyó de verdad */   /* (S7) y el cargador, el USB y la batería, al día */
    if (enc_abajo) mapa_ahora[6] |= 0x01;
}
int hw_teclas_mapa(u8 col[7])
{
    int f, c, n = 0;
    luz_paso();                                    /* las apps que leen las teclas en crudo no pasan por hw_tecla_evento */
    vibra_paso();                                  /* (R2; auditoría del sistema) el vibrador solo se apagaba desde hw_tecla_evento y hw_dormir */
    barrer();
    for (f = 0; f < 7; f++) { col[f] = mapa_ahora[f]; for (c = 0; c < 5; c++) if (col[f] & (1u << c)) n++; }
#ifdef PUENTE
    for (f = 0; f < 35; f++) {                     /* (v16) las pulsaciones que mantiene el puente, en la matriz cruda (los juegos la leen así) */
        int k = hw_celda_tecla(f);
        if (k && pu_tecla_abajo(k) && !(col[f / 5] & (1u << (f % 5)))) { col[f / 5] |= (u8)(1u << (f % 5)); n++; }
    }
#endif
    return n;
}
int hw_tecla_abajo(int k)
{
    int c;
#ifdef PUENTE
    if (pu_tecla_abajo(k)) return 1;               /* (v16) mantenida por el puente */
#endif
    barrer();
    for (c = 0; c < 35; c++) if (hw_celda_tecla(c) == k && (mapa_ahora[c / 5] & (1u << (c % 5)))) return 1;
    return 0;
}
int hw_tecla_evento(void)
{
    static u8 antes[7], cola[8], n_cola;
    static u32 visto = 0xFFFFFFFF, desde;
    static int rep = -1;                           /* celda que se repite si se mantiene */
    int f, c, k;
    vibra_paso();
    luz_paso();
#ifdef PUENTE
    if ((k = pu_tecla_sacar()) != 0) return k;     /* (v16) una pulsación inyectada por el puente: la primera */
#endif
    if (n_cola) { k = cola[0]; for (f = 1; f < n_cola; f++) cola[f - 1] = cola[f]; n_cola--; return k; }
    if (mapa_tic == visto && tics - mapa_tic < 5) return 0;
    barrer();
    if (mapa_tic == visto) return 0;
    visto = mapa_tic;
    for (f = 0; f < 7; f++) {
        u8 nuevas = (u8)(mapa_ahora[f] & ~antes[f]);
        for (c = 0; c < 5; c++) if (nuevas & (1u << c)) {
            k = hw_celda_tecla(f * 5 + c);
            if (k && n_cola < 8) cola[n_cola++] = (u8)k;
            rep = f * 5 + c; desde = tics;
        }
        antes[f] = mapa_ahora[f];
    }
    if (rep >= 0) {
        if (!(mapa_ahora[rep / 5] & (1u << (rep % 5)))) rep = -1;
        else {
            k = hw_celda_tecla(rep);
            if (((k >= K_ARRIBA && k <= K_DER) || k == K_C) && tics - desde >= 100 && n_cola < 8) {
                desde = tics - 70;                 /* repetición: 400 ms, luego cada 120 */
                cola[n_cola++] = (u8)k;
            }
        }
    }
    if (n_cola) { k = cola[0]; for (f = 1; f < n_cola; f++) cola[f - 1] = cola[f]; n_cola--; return k; }
    return 0;
}

/* ---- tiempo ------------------------------------------------------------------------------------------ */
#ifdef MEDIR
u32 medir_ms;                                      /* en las medidas el tiempo lo pone el guion */
u32 hw_ms(void) { return medir_ms; }
#else
u32 hw_ms(void) { return tics * 4; }
#endif
int hw_rtc_leer(HwFecha *f)
{
    rtc_t t; int r = rtc_get(&t);
    f->anio = t.anio; f->mes = t.mes; f->dia = t.dia; f->hora = t.hora; f->min = t.min; f->seg = t.seg;
#ifdef MEDIR                                       /* (pista O) en las medidas la HORA también la pone el guion: 10:10:00 + medir_ms; así las huellas no dependen de cuántas instrucciones lleva el emulador */
    { u32 s = 10 * 3600 + 10 * 60 + medir_ms / 1000; f->hora = (u8)(s / 3600 % 24); f->min = (u8)(s / 60 % 60); f->seg = (u8)(s % 60); }
#endif
    return r ? HW_ERROR : HW_OK;
}
int hw_rtc_alarma(int hora, int min)               /* alarma diaria del chip, en la hora DEL RTC; hora < 0 la quita */
{
#ifndef MEDIR
    if (hora > 23 || min < 0 || min > 59) return HW_ERROR;
    rtc_alarma(hora, min);
#endif
    return HW_OK;
}
int hw_rtc_alarma_salto(void) { static u32 vistas; u32 a = rtc_alarmas; if (a == vistas) return 0; vistas = a; return 1; }
int hw_rtc_poner(const HwFecha *f)
{
    rtc_t t = { f->anio, f->mes, f->dia, f->hora, f->min, f->seg };
    if (f->anio < 1970 || f->anio > 2099 || f->mes < 1 || f->mes > 12 || f->dia < 1 || f->dia > hw_dias_mes(f->anio, f->mes) ||
        f->hora > 23 || f->min > 59 || f->seg > 59) return HW_ERROR;
    rtc_set(&t);
    return HW_OK;
}

/* ---- ficheros ----------------------------------------------------------------------------------------
 * El FS de FÁBRICA del W580 («SEMC FS», todo en la NOR: 0x45E00000..0x47DFFFFF; docs/fs.md) se lee con
 * api/fsfab.c, SOLO LECTURA, directamente de la flash (la MMU la tiene mapeada con caché). Si la flash no lo trae
 * (la de prueba que hace herr/hacer_flash.py sin --fabrica está a 0xFF ahí) no hay cabeceras y contesta «no hay».
 * Esa zona y el GDFS (0x47E00000) no se escriben NUNCA: las rutinas de hal/nor.c comprueban el rango ellas mismas. */
#include "fsfab.h"
#define FSFAB_INI 0x45E00000u
#define FSFAB_FIN 0x47E00000u
static signed char fs_hay;                         /* 0 = sin mirar, 1 = sí, -1 = no */
int hwo_fs_listo(void)
{
    if (!fs_hay) {
        fs_hay = ff_montar((const u8 *)(size_t)FSFAB_INI, FSFAB_FIN - FSFAB_INI) > 0 ? 1 : -1;
        hw_log(fs_hay > 0 ? "fs: FS de fabrica montado\n" : "fs: no hay FS de fabrica\n");
    }
    return fs_hay > 0;
}
int hwo_fs_listar(const char *dir, u32 desde, HwEntrada *e, u32 max) { return hwo_fs_listo() ? ff_listar(dir, desde, e, max) : HW_ERROR; }
int hwo_fs_contar(const char *dir) { return hwo_fs_listar(dir, 0, 0, 0); }
u32 hwo_fs_tam(const char *ruta) { return hwo_fs_listo() ? ff_tam(ruta) : 0; }
u32 hwo_fs_leer(const char *ruta, u8 *buf, u32 max) { return hwo_fs_listo() ? ff_leer(ruta, buf, max) : 0; }
u32 hwo_fs_leer_desde(const char *ruta, u32 desde, u8 *buf, u32 max) { return hwo_fs_listo() ? ff_leer_desde(ruta, desde, buf, max) : 0; }
void hwo_fs_estado(HwFsEstado *e)
{
    const FfEstado *f = ff_estado();
    memset(e, 0, sizeof *e);
    e->montado = fs_hay > 0; e->bloques = f->validos; e->cabeceras = f->cabeceras; e->lecturas_nor = f->lecturas;
    e->errores = f->cabeceras_malas + f->desconocidos + f->fuera;
}

/* Dónde puede estar NUESTRO sistema de ficheros (W6FS): detrás del main, en el primer megabyte libre tras el
 * final REAL de la imagen y nunca antes de 0x44400000, hasta donde empiezan las zonas escribibles (hal.h).
 * (I1) Y, si ese megabyte ya no cabe entero por debajo de la Memoria (un main grande: con una app externa, 8,8 MiB, el megabyte
 * siguiente es 0x44B00000, donde empieza W6F), un segundo sitio: el primer BLOQUE (256 KiB) libre tras el main.
 * herr/hacer_flash.py y herr/hacer_aparato.py lo ponen con la misma regla. */
extern char _fin_flash[];
#define W6FS_MIN 0x44400000u
const u8 *hwo_w6fs(int i, u32 *max)
{
    u32 tras;
    if (i > 1) return 0;
    tras = ((u32)(size_t)_fin_flash + 0xFFFFFu) & ~0xFFFFFu;
    if (tras < W6FS_MIN) tras = W6FS_MIN;
    if (i == 0) {
        if (tras + (1u << 20) > NOR_ESCR_INI) return 0;      /* el main se ha comido el megabyte: el sitio 1 */
    } else {
        if (tras + (1u << 20) <= NOR_ESCR_INI) return 0;     /* cabía el megabyte: no hay sitio 1 */
        tras = ((u32)(size_t)_fin_flash + 0x3FFFFu) & ~0x3FFFFu;
        if (tras < W6FS_MIN || tras >= NOR_ESCR_INI) return 0;
    }
    *max = NOR_ESCR_INI - tras;
    return (const u8 *)(size_t)tras;
}

/* ---- energía, GPIO, I²C ------------------------------------------------------------------------------- */
/* (S7) LA BATERÍA EN MARCHA, como la mide el original (HAL_SYSPWR, cada 10 s; docs/bateria.md §5.4, §8.A4): el registro 0x77 del
 * PMU («medida automática 9», lectura de registro, sin pulso) por la recta por defecto de su GDFS, mV = 2755 + 10,843 x cuenta
 * (la más baja de las tres rectas: la conservadora). Si 0x77 da 00 o FF (no vale), la lectura «en frío» del arranque (0x6A) por
 * la suya; la pantalla de Sistema dice cuál se usa. [sin saber] cuál vale en el aparato con nuestro arranque, y la calibración
 * de ESE teléfono. La lectura se guarda 5 s (la barra de estado la pide varias veces por segundo; quien vigila sondea cada 10). */
static int bat_cuenta = -1; static u8 bat_fuente, bat_hay; static u32 bat_cuando;
static int bat_cruda(void)
{
    if (!bat_hay || hw_ms() - bat_cuando >= 5000u) {
        int c = pmu_vbat_auto();
        bat_fuente = 0;
        if (c == 0x00 || c == 0xFF) { c = pmu_bateria(); bat_fuente = 1; }
        bat_cuenta = c; bat_cuando = hw_ms(); bat_hay = 1;
    }
    return bat_cuenta;
}
int hw_bateria_mv(void)
{
    int a = bat_cruda();
    if (a < 0) return HW_NO_DISPONIBLE;
    return bat_fuente ? bat_mv_frio(a) : bat_mv_marcha(a);
}
int hw_bateria_pct(void)
{
    int mv = hw_bateria_mv();
    return mv < 0 ? mv : bat_pct(mv);
}
int hw_bateria_fuente(void) { return bat_cruda() < 0 ? HW_NO_DISPONIBLE : bat_fuente; }
/* El registro 0x23 (bit 3 cargador, bit 7 USB, bit 6 batería NO conectada, bit 0 tecla de encendido): se guarda lo que da
 * cada suceso del PMU (barrer) y, por si el suceso no llegara, se relee una vez por segundo. */
static int estado23(void)
{
    u8 v;
    if (!e23_hay || hw_ms() - e23_ms >= 1000u) { e23 = pmu_leer(0x23, &v) ? -1 : v; e23_ms = hw_ms(); e23_hay = 1; if (e23 >= 0) enc_pmu(&tel_enc, !(e23 & 1)); }   /* (2026-10-20, hito 7 en bueno) bit 0 = 1 SUELTA, 0 PULSADA (modelo E tenía polaridad invertida) */
    return e23;
}
int hw_bateria_presente(void) { int e = estado23(); return e < 0 ? HW_NO_DISPONIBLE : !(e & 0x40); }
void hw_arranque(HwArranque *a) { *a = arr; }
/* ---- (CARGA, 25-oct-2026) LA CARGA POR USB (api/carga.h, docs/bateria.md §12, work/bitacora_CARGA.md) ------------------------------
 * Una vez por segundo: 0x23 (fresco), 0x63 releído, los BORNES (0x75 y 0x77, el mayor de los válidos —0x77 da 00 a ratos en el aparato—
 * por la recta de la placa), la batería por BDATA cada minuto (en dos mitades: abrir y, al paso siguiente, convertir), el suceso 0x21
 * bit 7 acumulado por pmu_sucesos; carga_paso() decide y aquí se escribe 0x63 como el original (pmu_carga_usb). Si el bucle se para,
 * el chip corta solo a 4,1 V, y el perro de software y la pantalla de fallo ponen 0x63 a 0 (tel_sin_bucle, tel_fallo). */
static Carga carga_m; static u8 carga_lista, carga_fallo, bdata_abierta, fg_puesto;
volatile u8 carga_veto;                            /* (CARGA) != 0 = PROHIBIDA: el freno de mano por el puente (`pu.py w32 carga_veto 1`, ancho 1), para las pruebas */
static u32 carga_t, bdata_t; static int bdata_c = -1, carga_mv = -1, carga_r63 = -1, carga_usb_ma; static short bat_temp_dc = CARGA_SIN_TEMP; static u8 bat_tipo = 0xFF;
static long carga_fg; static u8 carga_fg_ok;
static int bornes_mv(void)                         /* la recta de siempre (bat_mv_marcha): la del GDFS de ESTE teléfono (unidad 0x0009) da ~9 mV
                                                     * menos a 4,0 V, así que los topes caen un poco antes: del lado prudente */
{
    u8 a, b; int m = -1;
    if (!pmu_leer(0x75, &a) && a != 0x00 && a != 0xFF) m = a;
    if (!pmu_leer(0x77, &b) && b != 0x00 && b != 0xFF && b > m) m = b;
    return m < 0 ? CARGA_SIN_MV : bat_mv_marcha(m);
}
static void carga_soltar(void)                     /* alguien ha puesto la carga a 0 por fuera: que la máquina lo sepa (no es un corte del chip) */
{
    if (carga_m.estado == CARGA_CARGANDO) { carga_m.estado = CARGA_MIDIENDO; carga_m.icod = 0; }
}
int hw_carga_paso(int usb_ma)
{
    CargaEntrada e; CargaSalida o; u8 v; int ant;
    u32 t = hw_ms();
    if (!carga_lista) { carga_iniciar(&carga_m); carga_lista = 1; }
    if (carga_t && t - carga_t < 1000u) return carga_m.estado;
    carga_t = t ? t : 1;
    memset(&e, 0, sizeof e);
    e.ms = t; e.prohibida = (u8)(!placa()->bat.carga_permitida || modo_seguro || carga_veto);
    if (pmu_leer(0x23, &v)) e.pmu_ok = 0;
    else { e.pmu_ok = 1; e23 = v; e23_ms = t; e23_hay = 1; e.usb = (u8)((v >> 7) & 1); e.cargador = (u8)((v >> 3) & 1); e.quitada = (u8)((v >> 6) & 1); }
    if (e.pmu_ok && (e.usb || e.cargador) && !e.quitada) {            /* la batería por BDATA: al enchufar y luego cada minuto */
        if (!fg_puesto && !pmu_fg_encender()) fg_puesto = 1;          /* el medidor, como el original al ver USB */
        if (bdata_abierta && t - bdata_t >= 400u) {
            bdata_c = pmu_bdata_cerrar(); bdata_abierta = 0; bdata_t = t;
            if (bdata_c < 0) { bat_temp_dc = CARGA_SIN_TEMP; }
            else { int mv = carga_bdata_mv(bdata_c); bat_tipo = carga_tipo(mv); bat_temp_dc = (short)carga_temp_dc(mv); }
        } else if (!bdata_abierta && (bat_tipo == 0xFF || t - bdata_t >= 60000u) && !pmu_bdata_abrir()) { bdata_abierta = 1; bdata_t = t; }
    } else {
        if (bdata_abierta) { pmu_bdata_cerrar(); bdata_abierta = 0; }
        bat_tipo = 0xFF; bat_temp_dc = CARGA_SIN_TEMP;                /* la próxima enchufada se mide de nuevo antes de cargar */
    }
    e.usb_ma = (unsigned short)(usb_ma > 0 ? usb_ma : 0);
    e.mv = (short)(carga_mv = bornes_mv());
    e.temp_dc = bat_temp_dc; e.tipo = bat_tipo;
    e.r63 = (short)(carga_r63 = pmu_leer(0x63, &v) ? -1 : v);
    e.suceso_techo = (u8)(pmu_ev21_tomar(0x80) != 0);
    e.fallo_escritura = carga_fallo; carga_fallo = 0;
    carga_usb_ma = e.usb_ma;
    ant = carga_m.estado;
    o = carga_paso(&carga_m, &e);
    if (o.escribir) {
        int r = pmu_carga_usb(o.vcod, o.escribir == 1 ? o.icod : 0);
        carga_fallo = (u8)(r < 0);
        if (r >= 0) carga_r63 = r;
        if (r < 0) hw_log("carga: NO se pudo escribir 0x63\n");
    }
    carga_fg_ok = (u8)(fg_puesto && !pmu_fg_leer(3, &carga_fg));
    if (carga_m.estado != ant || o.escribir == 1) {
        static const char *const nom[CARGA_N_ESTADOS] = { "sin fuente", "USB sin permiso", "midiendo", "CARGANDO", "COMPLETA", "pausa por temperatura",
                                                          "DETENIDA", "cargador de pared (no se usa)", "prohibida" };
        char h[9];                                 /* (los números por hw_log, no por uart_hex: así salen también en `pu.py log`) */
        hw_log("carga: "); hw_log(nom[carga_m.estado < CARGA_N_ESTADOS ? carga_m.estado : 0]);
        hw_log(" motivo "); hex8(h, carga_m.motivo); hw_log(h); hw_log(" mV "); hex8(h, (u32)carga_mv); hw_log(h);
        hw_log(" tdc "); hex8(h, (u32)(int)bat_temp_dc); hw_log(h); hw_log(" 63 "); hex8(h, (u32)carga_r63); hw_log(h);
        if (o.escribir) { hw_log(o.escribir == 1 ? " -> corriente " : " -> PARAR "); hex8(h, o.icod); hw_log(h); }
        hw_log("\n");
    }
    return carga_m.estado;
}
void hw_carga_estado(HwCarga *c)
{
    *c = (HwCarga){ .estado = carga_lista ? carga_m.estado : CARGA_SIN_FUENTE, .motivo = carga_m.motivo, .mv = carga_mv, .temp_dc = bat_temp_dc, .tipo = bat_tipo,
                    .bdata = bdata_c, .icod = carga_m.icod, .pedida = carga_m.pedida, .r63 = carga_r63, .ms_cargando = carga_m.ms_cargando,
                    .techos = carga_m.techos, .escalones = carga_m.escalones, .usb_ma = carga_usb_ma, .fg_ok = carga_fg_ok, .fg = carga_fg };
}
int hw_carga_fuera(void) { carga_soltar(); return pmu_carga_fuera() ? HW_ERROR : HW_OK; }   /* (R2) releído */
/* (Fase 3b) SEN_ENTRAR_BL: AL BOOTLOADER, con el USB ya soltado (shell/gestion_sis.c). Lo mismo que hw_reiniciar menos su última línea: la bandera y
 * reinicio_caliente, NUNCA sys_reset. [observado, 2026-10-24] sys_reset desarma la llave del perro y en silicio el reset cae al BOOTROM (0fce:adde),
 * no al BL; reinicio_caliente es un salto por software a BL_INI: la iRAM sigue ahí. Es la orden `pu.py bootloader` hecha desde dentro. El orden, el
 * de pu.py: el veto antes, para que nadie vuelva a poner la carga (el PMU no se reinicia con la CPU), y después la carga fuera. [emulador; sin probar en silicio] */
void reinicio_caliente(void);
void hw_entrar_bl(void)
{
    volatile u32 *b = (volatile u32 *)BL_BANDERA;
    veda = 0; nor_veda = 0; vp_vaciar();           /* lo aplazado por la veda de la música, antes de irse */
    hwo_tarjeta_apagar();                          /* la M2, cerrada y sin tensión: como hw_reiniciar */
    rst_apuntar(RST_PEDIDO, 0, 0);
    carga_veto = 1;
    hw_carga_fuera();
    irq_cerrar();
    b[0] = BL_MAGIA; b[1] = ~BL_MAGIA;
    reinicio_caliente();
    for (;;) ;
}
#ifdef ARRANQUE_VIGILADO
/* (Fase 3b) ARRANQUE VIGILADO, apagado por omisión: docs/mtp.md §7. Lo primero del main pone la bandera y arma el perro; al llegar al bucle, los quita. Si
 * cuelga antes, el perro reinicia y el BL ve la bandera: SENUS lo recupera. NO ACTIVAR EN NADA QUE VAYA A UN TELÉFONO hasta medirlo en silicio con el BL delante.
 * [medido 27-oct en el aparato, docs/senew_en_ram.md; sin BL en la cadena] armado con +30 distinto de 0 y +2C = 0 muerde a los 4,0 s (con +30 = 0, a los 11 ms: no vale para esto),
 * la llave lo desarma, «llave + armar» no lo alimenta, y la iRAM en 0xF3003000 sobrevive. El mismo 0x0F que el BL en RAM (plat/bl/bl_main.c). hal/mic_w580.c anota además que
 * cuenta de forma acumulada. [sin medir] arrancando desde la NOR: si el main llega al bucle en 4 s; que BLOA, en 0xF3002F00, sobreviva con el BL delante; que el reset llegue al
 * BL y no al modo flash del bootrom, que es donde cayó sys_reset el 24-oct, con +30 = 0 y el cable puesto. */
void hw_vigilado_armar(void)
{
    volatile u32 *b = (volatile u32 *)BL_BANDERA;
    b[0] = BL_MAGIA; b[1] = ~BL_MAGIA;
    *(volatile u32 *)0xF9010030u = 0x0F;
    *(volatile u32 *)0xF901002Cu = 0;
}
void hw_vigilado_quitar(void)
{
    volatile u32 *b = (volatile u32 *)BL_BANDERA;
    *(volatile u32 *)0xF9010010u = 0xAA;
    *(volatile u32 *)0xF9010024u = 0x5A;
    *(volatile u32 *)0xF901002Cu = 0xA5;
    b[0] = 0; b[1] = 0;
}
#endif
/* (R2; auditoría de S7, A4) LA CARGA, VIGILADA: se RELEEN 0x61 y 0x63 (dos lecturas; el shell lo llama cada 10 s junto con la
 * batería). 0x61 tiene que ser 10 (el cargador de pared no se usa NUNCA) y 0x63 sin corriente (bits 4..5 a 0: el original deja 03 al
 * parar), SALVO que la carga USB esté cargando (CARGA: esa la lleva hw_carga_paso). Si no, se reescriben (pmu_carga_fuera: 3
 * intentos). HW_OK = confirmado; HW_ERROR = NO se ha podido. */
int hw_carga_vigilar(void)
{
    u8 a = 0, b = 0;
    int cargando = carga_m.estado == CARGA_CARGANDO && carga_m.icod;
    if (!pmu_leer(0x61, &a) && !pmu_leer(0x63, &b) && a == 0x10 && (cargando || !(b & 0x30))) { pmu_carga.confirmada = !(b & 0x30); pmu_carga.r61 = a; pmu_carga.r63 = b; return HW_OK; }
    hw_log("carga: 61/63 no son lo que deben (61 = 10; 63 sin corriente fuera de la carga USB): se reescriben\n");
    carga_soltar();
    return pmu_carga_fuera() ? HW_ERROR : HW_OK;
}
void hw_pmu_salud(HwPmuSalud *p)
{
    *p = (HwPmuSalud){ .carga_confirmada = pmu_carga.confirmada, .r61 = pmu_carga.r61, .r63 = pmu_carga.r63, .lista_mal = pmu_carga.lista_mal,
                       .i2c_topes = i2c_cta.topes, .i2c_nack = i2c_cta.nack, .i2c_reintentos = i2c_cta.reintentos, .i2c_recuperados = i2c_cta.recuperados,
                       .enc_foto_armada = tel_enc.foto_suelta, .enc_nivel_armado = tel_enc.nivel_suelta, .enc_pmu_armado = tel_enc.pmu_suelta,
                       .enc_pmu_probado = (unsigned char)enc_pmu_probado(&tel_enc), .enc_descartes = tel_enc.descartes };
}
/* (R2; A3) La tecla de encendido MANTENIDA, a efectos de apagar: con las reglas de api/enc.h (vías vistas sueltas; si el PMU
 * es vía probada y dice «suelta», al teclado no se le cree). El shell cuenta con esto los 8 s. */
int hw_encendido_mantenida(void) { int e = estado23(); return enc_mantenida(&tel_enc, e < 0 ? -1 : !(e & 1)); }   /* (2026-10-20) ídem */
/* La pantalla «Energía (PMU)»: todo releído del chip, nada de sombras. Lecturas de registro (0x23, 0x60..0x63, 0x75..0x77, 0x7A),
 * la lectura en frío (0x70 <- 0x6A) y tres conversiones con pulso (canales 8, 9 y 1). No escribe nada más que el selector del ADC. */
int hw_pmu_diag(HwPmuDiag *d)
{
    /* (R3; calidad) cada registro con SU campo (antes se escribían nueve por desborde de &d->r23: valía mientras nadie reordenase la estructura) */
    static const struct { u8 reg; u8 off; } t[9] = {
        { 0x23, __builtin_offsetof(HwPmuDiag, r23) }, { 0x60, __builtin_offsetof(HwPmuDiag, r60) }, { 0x61, __builtin_offsetof(HwPmuDiag, r61) }, { 0x62, __builtin_offsetof(HwPmuDiag, r62) }, { 0x63, __builtin_offsetof(HwPmuDiag, r63) },
        { 0x75, __builtin_offsetof(HwPmuDiag, r75) }, { 0x76, __builtin_offsetof(HwPmuDiag, r76) }, { 0x77, __builtin_offsetof(HwPmuDiag, r77) }, { 0x7A, __builtin_offsetof(HwPmuDiag, r7a) } };
    int i, bien = 0; u8 v;
    for (i = 0; i < 9; i++) { short x = -1; if (!pmu_leer(t[i].reg, &v)) { x = v; bien++; } memcpy((char *)d + t[i].off, &x, sizeof x); }
    d->l6a = (short)pmu_bateria();
    d->adc8 = (short)pmu_adc(8); d->adc9 = (short)pmu_adc(9); d->adc1 = (short)pmu_adc(1);
    if (d->r23 >= 0) { e23 = d->r23; e23_ms = hw_ms(); e23_hay = 1; }
    return bien ? HW_OK : HW_NO_DISPONIBLE;
}
int hw_tecla_encendido(void)
{
    u8 col[7];
    hw_teclas_mapa(col);
    return (col[6] & 0x01) != 0;                   /* celda 30 (ON_OFF, a masa) o el PMU (0x23 bit 0): las dos caen ahí (barrer) */
}
int hw_pantalla_dormir(int si)
{
    if (si) lcd_dormir();
    else if (!giro_pant) lcd_despertar(lcd_cuadro);   /* al despertar, el último cuadro antes de encender (normal_from_standby_seq) */
    else { lcd_despertar(0); lcd_enviar_girado(lcd_cuadro, (u32)GIRO_AN(giro_pant), giro_codigo[giro_pant], giro_esquina(giro_pant)); }   /* (giro) */
    return HW_OK;
}
int hw_cargador(void) { int e = estado23(); return e < 0 ? HW_NO_DISPONIBLE : ((e >> 3) & 1) | ((e >> 6) & 2); }   /* bit 0 = cargador, bit 1 = USB */
int hw_vibrar(int ms)
{
    if (ms <= 0) { vibra_hasta = 0; return pmu_vibrador(0) ? HW_ERROR : HW_OK; }
    if (ms > 2000) ms = 2000;
    if (pmu_vibrador(1)) return HW_ERROR;
    vibra_hasta = hw_ms() + (u32)ms; if (!vibra_hasta) vibra_hasta = 1;
    return HW_OK;
}
int hw_vibrar_patron(int hz, int pct, int ms)       /* (API 1.5) el patrón lo lleva el PMU solo; aquí, como hw_vibrar, lo apaga el bucle a su hora */
{
    if (ms <= 0 || hz <= 0 || pct <= 0) { vibra_hasta = 0; return pmu_vibrador(0) ? HW_ERROR : HW_OK; }
    if (ms > 10000) ms = 10000;
    if (pmu_vibrador_patron(hz, pct)) return HW_ERROR;
    vibra_hasta = hw_ms() + (u32)ms; if (!vibra_hasta) vibra_hasta = 1;
    return HW_OK;
}
#define PIN_MAL(p, n) ((p) < 0 || (p) > 4 || (n) < 0 || (n) > 7)
/* (R2; auditoría del sistema, §5) LISTA BLANCA de pines que una app puede poner como salida o cambiar de nivel. Antes valía
 * cualquiera de los 40: la alimentación del acelerómetro (P1.1), los PWM, y las salidas que el arranque deja a 1 sin que se
 * sepa qué son (P0.7, P1.2, P1.3, P2.1, P3.3). Hoy la lista está VACÍA: ningún pin del W580i está identificado como «de
 * usuario». Leer sigue valiendo para todos. Es DESCRIPCIÓN DE LA PLACA: otro modelo cambia solo esta tabla. */
static const u8 gpio_de_usuario[5] = { 0, 0, 0, 0, 0 };   /* por puerto, un bit por pin */
#define PIN_VEDADO(p, n) (PIN_MAL(p, n) || !(gpio_de_usuario[p] & (1u << (n))))
int hw_gpio_modo(int p, int pin, int salida) { if (PIN_VEDADO(p, pin)) return HW_ERROR; gpio_dir((unsigned)p, (u8)(1u << pin), salida); return HW_OK; }
int hw_gpio_poner(int p, int pin, int v) { if (PIN_VEDADO(p, pin)) return HW_ERROR; gpio_set((unsigned)p, (u8)(1u << pin), v); return HW_OK; }
int hw_gpio_leer(int p, int pin) { if (PIN_MAL(p, pin)) return HW_ERROR; return gpio_get((unsigned)p, (u8)(1u << pin)); }
/* La «tapa» de la API es aquí el DESLIZADOR (P2.6). Qué nivel es «cerrado» lo dice DESLIZADOR_CERRADO_ES (hal.h):
 * [inferido] 1, por cómo lo trata el firmware original en el emulador; en un aparato, [sin saber]. Se sondea (el
 * sistema lo mira cada 200 ms, que hace de antirrebote como los 200 ms del original) y además interrumpe (IRQ 67,
 * hal/gpio.c), que es lo que saca al sistema del reposo profundo al abrir o cerrar. */
static u8 desliz_antes = 0xFF;
int hw_tapa_cerrada(void)
{
    u8 c = (u8)(gpio_deslizador() == DESLIZADOR_CERRADO_ES);
    if (desliz_antes == 0xFF) desliz_antes = c;
    if (c != desliz_antes) { desliz_antes = c; desliz_cambios++; }
    return c;
}
int hw_tapa(void) { return !hw_tapa_cerrada(); }
u32 hw_tapa_cambios(void) { hw_tapa_cerrada(); return desliz_cambios; }
/* Hallazgo M6 de la pista R: por aquí cualquier app (también las externas) podía escribir en el PMU (0x4A): un
 * 0x41 <- 0 apaga el teléfono y 0x60..0x63 son el cargador. La API ya no deja ESCRIBIR al PMU (leer, sí). */
int hw_i2c_escribir(int dir7, const u8 *d, int n) { if (dir7 < 0 || dir7 > 127 || n < 0 || dir7 == 0x4A) return HW_ERROR; return i2c_escribir(0, (u8)dir7, d, (u32)n) ? HW_ERROR : n; }
int hw_i2c_leer(int dir7, int reg, u8 *d, int n)
{
    u8 r = (u8)reg;
    if (dir7 < 0 || dir7 > 127 || n < 1) return HW_ERROR;
    return i2c_escribir_leer(0, (u8)dir7, &r, reg >= 0 ? 1 : 0, d, (u32)n) ? HW_ERROR : n;
}
/* El canal 10 (la batería) NO se convierte con 0xA0 | canal: el original no lo hace nunca (aviso [adc] del modo estricto
 * de la pista E4); se da la lectura de la batería como la hace el original (0x6A). Los demás, con su pulso. */
int hw_adc(int canal) { int r = canal == 10 ? bat_cruda() : pmu_adc((u32)canal); return r < 0 ? HW_NO_DISPONIBLE : r; }

/* ---- serie y tono --------------------------------------------------------------------------------------- */
int hw_serie_conectado(void) { return hay_serie; }
int hw_serie_escribir(const u8 *d, int n)
{
    int i;
    if (!hay_serie) return 0;
    for (i = 0; i < n; i++) uart_putc(UART0, (char)d[i]);
    return n;
}
int hw_serie_leer(u8 *d, int max)
{
#ifdef MEDIR
    int n = 0, c;
    while (n < max && (c = uart_getc(UART0)) >= 0) d[n++] = (u8)c;
    return n;
#else
    return uart_rx(d, max);                        /* anillo que llena la IRQ 46 + lo que quede en la FIFO */
#endif
}
/* ---- SONIDO ---------------------------------------------------------------------------------------------
 * Solo el generador de tonos 0xF9020000 (hal/gpio.c), el mismo bloque que el zumbador del W600. [emulador:
 * cerrojo] las escrituras llegan y no suena nada; [sin saber] si en el W580i ese generador llega a un
 * transductor y por qué pin (hal.md §17): no se ruta ningún pin. El PCM por el DSP no está abierto en el
 * DB2020: hw_pcm_* contestan HW_NO_DISPONIBLE y la app de Música degrada sola al «zumbador». */
/* (tono_listo: declarado arriba, con el perro) */
static void tono_asegurar(void)
{
    if (!tono_listo) { tono_init(); tono_listo = 1; }
}
/* La SOMBRA de lo que el generador está dando (pista S7, para hw_tono_mantener): hz y ciclo de la última orden,
 * 0 = callado, ZUM_NO_SE = «alguien puso un plazo y se apagará solo: no se sabe». */
#define ZUM_NO_SE 0xFFFFFFFFu
static volatile u32 zum_hz;
static volatile u8 zum_ciclo;
int hw_tono(u32 hz, int ciclo)
{
    if (hz < 397 || hz > 20000 || ciclo < 1 || ciclo > 100) return HW_ERROR;
    tono_asegurar();
    tic_tono_hasta = 0;                            /* sin plazo: suena hasta hw_tono_fin */
    tono(hz, (u32)ciclo);
    zum_hz = hz; zum_ciclo = (u8)ciclo;
    return HW_OK;
}
/* Con plazo: lo apaga el TIC (hal/timer.c), no el bucle. El grano es el del tic: 4 ms, redondeado hacia arriba. */
int hw_tono_ms(u32 hz, int ciclo, u32 ms)
{
    u32 n = (ms + 3) / 4, e, hasta;
    if (hw_tono(hz, ciclo) != HW_OK) return HW_ERROR;
    e = irq_cerrar();
    hasta = tics + (n ? n : 1);
    tic_tono_hasta = hasta ? hasta : 1;
    zum_hz = ZUM_NO_SE;
    if (!e) irq_abrir();
    return HW_OK;
}
int hw_tono_fin(void) { tic_tono_hasta = 0; if (tono_listo) tono_fin(); zum_hz = 0; return HW_OK; }
/* Se llama desde el gancho del tic (interrupción) y desde el bucle. LA PRIMERA VEZ hay que llamarla desde el
 * bucle (vale con hz = 0): abre los relojes y la ruta del pin, que son lecturas-y-escrituras de SYSCON y no
 * deben hacerse desde una interrupción (shell/musica.c lo hace al arrancar una pista). */
int hw_tono_mantener(u32 hz, int ciclo)
{
    tono_asegurar();
    if (!hz) {
        if (zum_hz == 0) return 0;
        tic_tono_hasta = 0; if (tono_listo) tono_fin(); zum_hz = 0;
        return 1;
    }
    if (hz < 397 || hz > 20000 || ciclo < 1 || ciclo > 100) return HW_ERROR;
    if (zum_hz == hz && zum_ciclo == (u8)ciclo) return 0;
    tic_tono_hasta = 0;
    tono(hz, (u32)ciclo);
    zum_hz = hz; zum_ciclo = (u8)ciclo;
    return 1;
}
/* (R2; auditoría del sistema, A2) EL GANCHO DEL TIC CON DUEÑO. El tic (hal/timer.c) tiene UNA ranura; antes hw_tic_gancho la
 * pisaba sin más: una app que lo llamara dejaba muda a la música del sistema sin que se enterase, y al salir su función
 * seguía ejecutándose 250 veces por segundo desde la interrupción, sobre memoria ya devuelta. Ahora la ranura del tic es de
 * un repartidor con DOS dueños: el SISTEMA (hwo_tic_sistema: la música y la melodía que el sistema toca por encargo de una
 * app) y LA APP (hw_tic_gancho, la función pública de siempre). Poner el de la app no quita el del sistema, y el de la app
 * se quita solo al devolver la pantalla o al volver de `entrar` (hwo_tic_app_fuera, desde shell/app_externa.c). */
extern void (*volatile tic_gancho)(void);
static void (*volatile gancho_sis)(void), (*volatile gancho_app)(void);
static void ganchos(void) { void (*f)(void) = gancho_sis; if (f) f(); f = gancho_app; if (f) f(); }
static void ganchos_poner(void) { tic_gancho = (gancho_sis || gancho_app) ? ganchos : 0; }
int hw_tic_gancho(void (*f)(void)) { if (en_panico) return HW_ERROR; gancho_app = f; ganchos_poner(); return HW_OK; }
int hwo_tic_sistema(void (*f)(void)) { if (en_panico) return HW_ERROR; gancho_sis = f; ganchos_poner(); return HW_OK; }
int hwo_tic_app_fuera(void) { int habia = gancho_app != 0; gancho_app = 0; ganchos_poner(); return habia; }
/* (R2; §5) LAS CLAVES DEL ALMACÉN CON DUEÑO, lo que se puede sin cargador de apps: mientras una app EXTERNA tiene la pantalla
 * solo puede guardar y olvidar claves del rango de las externas (0x6000..0x6FFF: shell/shell.h); las del sistema y las de
 * las apps de casa (ajustes, notas…) le quedan de solo lectura. Lo pone y lo quita shell/app_externa.c. */
static u8 claves_de_app;
void hwo_claves_app(int si) { claves_de_app = (u8)(si != 0); }
static int clave_vedada(u16 clave) { return claves_de_app && (clave < HW_NV_EXT_INI || clave > HW_NV_EXT_FIN); }

static signed char pcm_motivo;
int  hw_pcm_init(u32 rate, int canales) { if (!rate || canales < 1 || canales > 2) return HW_ERROR; pcm_motivo = -10; return HW_NO_DISPONIBLE; }
int  hw_pcm_motivo(void) { return pcm_motivo; }
int  hw_pcm_escribir(const short *pcm, u32 marcos) { (void)pcm; (void)marcos; return 0; }
void hw_pcm_cerrar(void) { }
u32  hw_pcm_marcos(void) { return 0; }
int  hw_sonido_hay(void) { return HW_SND_ZUMBADOR; }

/* ---- acelerómetro (hal.md §13) -------------------------------------------------------------------------- */
int hw_acel_alimentar(int si) { acel_alimentar(si); return HW_OK; }
int hw_acel_leer(int *x, int *y) { return acel_leer(x, y) ? HW_NO_DISPONIBLE : HW_OK; }
/* (2026-10-20, tmp/acel_ag) el diagnóstico del ADC del acelerómetro (api/hw.h HwAcelDiag; el autodetector está en hal/i2c.c) */
int hw_acel_diag(int accion, HwAcelDiag *d)
{
    int m, k, x, y, encendido; u8 v;
    if (accion == HW_ACEL_DIAG_PROBAR) adc_autodetectar();
    else if (accion == HW_ACEL_DIAG_SIGUIENTE) adc_forzar(adc_dg.forzado + 1);
    else if (accion == HW_ACEL_DIAG_RANGO) adc_dg.rango_hay = 0;
    if (!d) return HW_OK;
    encendido = gpio_get(1, 0x02);
    if (!encendido) { acel_alimentar(1); espera_ms(20); }
    k = acel_leer(&x, &y);
    if (!encendido) acel_alimentar(0);
    d->x = k ? -1 : x; d->y = k ? -1 : y;
    d->modo = adc_dg.modo; d->forzado = adc_dg.forzado; d->auto_modo = adc_dg.auto_modo; d->hallado = adc_dg.hallado; d->intentos = adc_dg.intentos;
    for (m = 0; m < HW_ADC_MODOS && m < ADC_MODOS; m++) {
        for (k = 0; k < 4; k++) d->v[m][k] = adc_dg.probado ? adc_dg.v[m][k] : -1;
        d->veneno[m] = adc_dg.veneno[m]; d->viejos[m] = adc_dg.viejos[m]; d->distintos[m] = adc_dg.distintos[m]; d->vivo[m] = adc_dg.vivo[m];
    }
    d->xmin = adc_dg.xmin; d->xmax = adc_dg.xmax; d->ymin = adc_dg.ymin; d->ymax = adc_dg.ymax;
    i2c_revivir(0); d->r70 = pmu_leer(0x70, &v) ? -1 : v;
    i2c_revivir(0); d->r72 = pmu_leer(0x72, &v) ? -1 : v;
    i2c_revivir(0); d->r73 = pmu_leer(0x73, &v) ? -1 : v;
    d->p11 = encendido;
    d->lecturas = adc_dg.lecturas; d->fallos = adc_dg.fallos;
    return HW_OK;
}

/* ---- (2026-10-20) PRUEBA DEL ALTAVOZ (tmp/sonido_re/informe.md §6.1): ¿qué camino SUENA en el aparato, sin DSP? -------------
 * El original no usa nunca el generador F9020000: suena por DSP -> PCM (FA020000) -> códec del PMU (C0..D0) -> ELEVADOR del
 * altavoz (P3.7 = 1, «Audio Stepup», ac_AudioExtHWDriver). Esto abre el camino analógico EXACTAMENTE como «Service tests ->
 * Speaker» del original ([ejecutado] en el emulador; valores y orden del informe §2) y prueba por tramos de ~3 s, diciendo el
 * tramo con paso(): 1 abrir y silencio (¿pop?), 2 reloj y configuración del PCM, 3 F9020000 en modo CONTINUO (0x80) a 1 kHz,
 * 4 lo mismo con P0.7 rutado (F900003C bits 14..15 = 1, 2, 3), 5 «tren de pops» por I2C (C0 47/57 cada 2 ms), 6 cerrar.
 * El perro late durante las esperas. Riesgo bajo: las mismas escrituras que el original; la ruta de P0.7 se repone. */
static void alt_espera(u32 ms) { while (ms) { u32 t = ms > 100 ? 100 : ms; espera_ms(t); ms -= t; hw_perro_latido(); } }
static void alt_pmu(const u8 (*l)[2], u32 n) { u32 i; for (i = 0; i < n; i++) { i2c_revivir(0); pmu_escribir(l[i][0], l[i][1]); } }
int hw_prueba_altavoz(void (*paso)(int tramo, const char *que))
{
    static const u8 rutas[][2] = { {0xC1,0x00},{0xC2,0x44},{0xC3,0x66},{0xC4,0x66},{0xC5,0x04},{0xC6,0x80},{0xC8,0x0C},
        {0xC9,0x00},{0xCA,0x10},{0xCB,0x50},{0xCC,0x50},{0xCD,0x50},{0xCE,0x32},{0xCF,0x22},{0xD0,0x35} };
    static const u8 subir[][2] = { {0xC0,0x00},{0xCD,0x50},{0xCC,0x50},{0xCB,0x50},{0xC7,0x40},
        {0xC0,0x01},{0xC0,0x03},{0xC8,0x0C},{0xC0,0x43},{0xC7,0x40},{0xC0,0x47},{0xC0,0x57},{0xCD,0x57},{0xCC,0x50},{0xCB,0x50} };
    static const u8 bajar[][2] = { {0xC0,0x17},{0xCD,0x50},{0xCC,0x50},{0xCB,0x50},{0xC7,0x40},{0xC0,0x13},{0xC0,0x03},{0xC0,0x02},{0xC0,0x00} };
    u16 ruta0; int k, i;
    hw_log("altavoz: PRUEBA por tramos\n");
    paso(0, "empieza (vibra)"); hw_vibrar(150); alt_espera(1000);
    paso(1, "codec + P3.7: ¿pop?");
    alt_pmu(rutas, sizeof rutas / sizeof rutas[0]); alt_pmu(subir, sizeof subir / sizeof subir[0]);
    gpio_dir(1, 0x20, 1); gpio_set(1, 0x20, 0);                    /* P1.5 = 0 (ampCtrl), como el original */
    gpio_dir(3, 0x80, 1); gpio_set(3, 0x80, 1);                    /* P3.7 = 1: elevador del altavoz */
    alt_espera(3000);
    paso(2, "reloj del PCM");
    R16(SYSCON + 0x18) &= (u16)~0x0002u; R16(SYSCON + 0x12) |= 0x0002;            /* puerta 26, bloque 20 (FUN_4459f428) */
    R16(0xFA02003E) = 1; R16(0xFA02000E) = 0x3000; R16(0xFA020010) = 0x1010;   /* FUN_441a7fe8 */
    R16(0xFA02001E) = 0x0CB2; R16(0xFA020020) = 0x0CB2; R16(0xFA020022) = 0; R16(0xFA020024) = 0;
    R16(0xFA020026) = 1; R16(0xFA020028) = 0x3C; R16(0xFA02002A) = 0x28;
    R16(0xFA020000) |= 0x0200; R16(0xFA020000) |= 0x0001; R16(0xFA02003E) = 0;
    alt_espera(3000);
    paso(3, "tono 1 kHz (F902)");
    tono_init(); tono_listo = 1;
    R16(0xF9020004) = 13000; R16(0xF9020006) = 6500; R16(0xF9020000) = 0x80;
    alt_espera(3000);
    R16(0xF9020000) = 0;
    paso(4, "tono por P0.7 (3x1 s)");
    ruta0 = R16(0xF900003C);
    for (k = 1; k <= 3; k++) {
        R16(0xF900003C) = (u16)((ruta0 & 0x3FFFu) | (u16)(k << 14));
        R16(0xF9020000) = 0x80; alt_espera(1000); R16(0xF9020000) = 0;
    }
    R16(0xF900003C) = ruta0;
    paso(5, "pops por I2C (250 Hz)");
    for (i = 0; i < 250; i++) { pmu_escribir(0xC0, (i & 1) ? 0x57 : 0x47); espera_ms(2); if (!(i % 50)) hw_perro_latido(); }
    pmu_escribir(0xC0, 0x57);
    alt_espera(500);
    paso(6, "cerrar: ¿pop?");
    gpio_set(3, 0x80, 0);
    alt_pmu(bajar, sizeof bajar / sizeof bajar[0]);
    alt_espera(1500);
    hw_log("altavoz: fin de la prueba\n");
    return HW_OK;
}

/* (2026-10-21) EL BANCO (api/hw.h HwBanco): acción + releer todo. El acelerómetro se lee en TRES formas de conversión para separar
 * «el ADC con pulso no convierte» (tmp/acel_re §2.3: F5000800 bit 14) de «el sensor no está alimentado» (P1.1); el códec se
 * abre como «Service tests -> Speaker» y SE QUEDA ABIERTO (con P3.7) hasta cerrarlo, releyendo C0..CD: si no se releen los
 * valores escritos, el PMU no toma las escrituras del códec y el problema del silencio está antes del altavoz. */
#define BR(a) R16(a)
#define BW(a, v) (R16(a) = (u16)(v))
static u8 banco_bit14, banco_acel, banco_codec; static int banco_mal; static u32 banco_vueltas;
static int banco_lee(u8 r) { u8 v; i2c_revivir(0); return pmu_leer(r, &v) ? -1 : v; }
static int banco_conv(u8 orden, int pulso, int esperar)
{
    u8 v;
    i2c_revivir(0);
    if (pmu_escribir(0x70, orden)) return -1;
    if (pulso) { BW(SYSCON + 0x16, BR(SYSCON + 0x16) & (u16)~0x0800u); BW(0xF5000874, 0x11); espera_ms(1); BW(0xF5000874, 0x12); BW(SYSCON + 0x16, BR(SYSCON + 0x16) | 0x0800u); }
    if (esperar) espera_ms(2);
    return pmu_leer(0x71, &v) ? -1 : v;
}
static int banco_pmu(const u8 (*l)[2], u32 n) { u32 i; int m = 0; for (i = 0; i < n; i++) { i2c_revivir(0); if (pmu_escribir(l[i][0], l[i][1])) m++; } return m; }
int hw_banco(int accion, HwBanco *b)
{
    static const u8 rutas[][2] = { {0xC1,0x00},{0xC2,0x44},{0xC3,0x66},{0xC4,0x66},{0xC5,0x04},{0xC6,0x80},{0xC8,0x0C},
        {0xC9,0x00},{0xCA,0x10},{0xCB,0x50},{0xCC,0x50},{0xCD,0x50},{0xCE,0x32},{0xCF,0x22},{0xD0,0x35} };
    static const u8 rutas_aur[][2] = { {0xC1,0x00},{0xC2,0x05},{0xC3,0xE4},{0xC4,0xF5},{0xC5,0xA0},{0xC6,0x00},{0xC8,0x0F},
        {0xC9,0x10},{0xCA,0x10},{0xCB,0x50},{0xCC,0x50},{0xCD,0x50},{0xCE,0x30},{0xCF,0x20},{0xD0,0xC5} };
    static const u8 subir[][2] = { {0xC0,0x00},{0xCD,0x50},{0xCC,0x50},{0xCB,0x50},{0xC7,0x40},
        {0xC0,0x01},{0xC0,0x03},{0xC8,0x0C},{0xC0,0x43},{0xC7,0x40},{0xC0,0x47},{0xC0,0x57},{0xCD,0x57},{0xCC,0x50},{0xCB,0x50} };
    static const u8 subir_aur[][2] = { {0xC0,0x00},{0xCD,0x50},{0xCC,0x50},{0xCB,0x50},{0xC7,0x40},
        {0xC0,0x01},{0xC0,0x03},{0xC8,0x0F},{0xC0,0x43},{0xC7,0x40},{0xC0,0x47},{0xC0,0x57},{0xCC,0x56},{0xCD,0x50},{0xCB,0x50} };
    static const u8 bajar[][2] = { {0xC0,0x17},{0xCD,0x50},{0xCC,0x50},{0xCB,0x50},{0xC7,0x40},{0xC0,0x13},{0xC0,0x03},{0xC0,0x02},{0xC0,0x00} };
    int i;
    switch (accion) {
    case HW_BANCO_BIT14:
        BW(SYSCON + 0x16, BR(SYSCON + 0x16) & (u16)~0x0800u);
        banco_bit14 = !(BR(0xF5000800) & 0x4000u);                 /* (acel_ag) conmuta lo que HAY: el arranque ya lo deja puesto (pmu_encendido) */
        BW(0xF5000800, banco_bit14 ?(u16)(BR(0xF5000800) | 0x4000u) : (u16)(BR(0xF5000800) & (u16)~0x4000u));
        BW(SYSCON + 0x16, BR(SYSCON + 0x16) | 0x0800u);
        break;
    case HW_BANCO_ACEL: banco_acel = !banco_acel; break;
    case HW_BANCO_CODEC_ABRIR: case HW_BANCO_AURICULAR:
        if (accion == HW_BANCO_AURICULAR) { banco_mal = banco_pmu(rutas_aur, sizeof rutas_aur / 2) + banco_pmu(subir_aur, sizeof subir_aur / 2); gpio_dir(3, 0x80, 1); gpio_set(3, 0x80, 0); }
        else { banco_mal = banco_pmu(rutas, sizeof rutas / 2) + banco_pmu(subir, sizeof subir / 2); gpio_dir(1, 0x20, 1); gpio_set(1, 0x20, 0); gpio_dir(3, 0x80, 1); gpio_set(3, 0x80, 1); }
        banco_codec = accion == HW_BANCO_AURICULAR ? 2 : 1;
        hw_log("banco: codec abierto\n");
        break;
    case HW_BANCO_CODEC_CERRAR:
        gpio_set(3, 0x80, 0); banco_mal = banco_pmu(bajar, sizeof bajar / 2); banco_codec = 0;
        break;
    case HW_BANCO_POPS:                                           /* 1 s de «pops» por I²C a 250 Hz (con el códec abierto, si lo está) */
        for (i = 0; i < 250; i++) { pmu_escribir(0xC0, (i & 1) ? 0x57 : 0x47); espera_ms(2); if (!(i % 50)) hw_perro_latido(); }
        pmu_escribir(0xC0, banco_codec ? 0x57 : 0x00);
        break;
    case HW_BANCO_TONO:                                           /* 1 s del generador F902 en continuo a ~1 kHz */
        if (!tono_listo) { tono_init(); tono_listo = 1; }
        R16(0xF9020004) = 13000; R16(0xF9020006) = 6500; R16(0xF9020000) = 0x80;
        for (i = 0; i < 10; i++) { espera_ms(100); hw_perro_latido(); }
        R16(0xF9020000) = 0;
        break;
    default: break;
    }
    if (!b) return HW_OK;
    banco_vueltas++;
    BW(SYSCON + 0x16, BR(SYSCON + 0x16) & (u16)~0x0800u);
    b->f5000800 = BR(0xF5000800);
    BW(SYSCON + 0x16, BR(SYSCON + 0x16) | 0x0800u);
    b->syscon16 = BR(SYSCON + 0x16);
    b->p1 = R16(0xF900000A) & 0xFF; b->p1dir = R16(0xF900000C) & 0xFF; b->p3 = R16(0xF900001E) & 0xFF; b->p3dir = R16(0xF9000020) & 0xFF;
    if (banco_acel) { acel_alimentar(1); espera_ms(20); }          /* P1.1 a 1 justo antes de leer (el podómetro lo apaga entre medias) */
    b->l6a = banco_conv(0x6A, 0, 0);
    b->a_y = banco_conv(0xA4, 1, 0); b->a_x = banco_conv(0xAC, 1, 0); b->a_1 = banco_conv(0xA1, 1, 0); b->a_8 = banco_conv(0xA8, 1, 0);
    b->i_y = banco_conv(0x64, 0, 0); b->i_x = banco_conv(0x6C, 0, 0); b->i_1 = banco_conv(0x61, 0, 0); b->i_8 = banco_conv(0x68, 0, 0);
    b->s_y = banco_conv(0xA4, 0, 1); b->s_x = banco_conv(0xAC, 0, 1);
    if (banco_acel) acel_alimentar(0);
    b->bit14 = (b->f5000800 >> 14) & 1; b->acel = banco_acel; b->codec = banco_codec; b->esc_mal = banco_mal; b->vueltas = banco_vueltas;
    b->c0 = banco_lee(0xC0); b->c7 = banco_lee(0xC7); b->c8 = banco_lee(0xC8); b->cb = banco_lee(0xCB); b->cc = banco_lee(0xCC); b->cd = banco_lee(0xCD);
    return HW_OK;
}
/* al salir de la pantalla: nada se queda encendido */
void hw_banco_salir(void)
{
    if (banco_codec) hw_banco(HW_BANCO_CODEC_CERRAR, 0);
    banco_acel = 0;
}
