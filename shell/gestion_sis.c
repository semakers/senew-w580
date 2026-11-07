/* gestion_sis.c — EL LADO DEL SISTEMA del canal de gestión de SENUS (api/gestion.h; contrato el contrato del canal de gestión (docs/ del repositorio de SENUS)).
 * Lo que depende de shell/: las claves con sus rangos, los valores de `aj`, aplicar la cola, listar las apps y preguntar «¿Actualizar el sistema?».
 * gs_paso() corre en el bucle principal, tras mtp_paso: ahí, y solo ahí, se toca `aj` y la interfaz. En el banco de PC de api/mtp.c
 * este fichero no entra: lo sustituye un doble (herr/pruebas/mtp/prueba_mtp.c). Sí entra en «make pc-usb» (herr/guiones_mtp/gestion.txt). */
#include "shell.h"
#include "gestion.h"
#include "usb.h"
#include "mtp.h"
#include "tarjeta.h"
#include "carga.h"
#include "placa.h"
#include "m3d.h"
#include "cargador.h"
#include "red_sis.h"

/* ---- LAS CLAVES (contrato §3): ESTABLES, no se renumeran nunca. Los rangos los da esto: dependen de N_TEMAS y compañía ------------------ */
#define DESP(campo) ((u8)__builtin_offsetof(Ajustes, campo))
#define NO_CAMPO 0xFF
static const struct { GsClave c; u8 desp; } tabla[] = {
    { { 0x0001, GS_T_U8,   0, N_TEMAS - 1 },            DESP(tema) },
    { { 0x0002, GS_T_BOOL, 0, 1 },                      DESP(hora12) },
    { { 0x0003, GS_T_BOOL, 0, 1 },                      DESP(alarma_on) },
    { { 0x0004, GS_T_U8,   0, 23 },                     DESP(alarma_h) },
    { { 0x0005, GS_T_U8,   0, 59 },                     DESP(alarma_m) },
    { { 0x0006, GS_T_I16,  -720, 840 },                 NO_CAMPO },              /* aj.zona_min: lo que acepta cargar_ajustes */
    { { 0x0007, GS_T_U8,   10, 100 },                   DESP(brillo) },          /* nunca a 0: no se vería nada (apps/ajustes) */
    { { 0x0008, GS_T_U8,   0, 120 },                    DESP(atenuar_s) },       /* 120: el plazo más largo de Ajustes */
    { { 0x0009, GS_T_U8,   0, 120 },                    DESP(apagar_s) },
    { { 0x000A, GS_T_BOOL, 0, 1 },                      DESP(bloqueo_auto) },
    { { 0x000B, GS_T_BOOL, 0, 1 },                      DESP(tapa_apaga) },
    { { 0x000C, GS_T_U8,   0, SN_VOL_MAX },             DESP(volumen) },
    { { 0x000D, GS_T_BOOL, 0, 1 },                      DESP(tono_teclas) },
    { { 0x000E, GS_T_BOOL, 0, 1 },                      DESP(pasos) },
    { { 0x000F, GS_T_U8,   0, SH_PASOS_AHORROS - 1 },   DESP(pasos_ahorro) },
    { { 0x0010, GS_T_U8,   0, M3_ESTILOS - 1 },         DESP(menu_estilo) },
    { { 0x0011, GS_T_U8,   0, SH_EFECTOS - 1 },         DESP(menu_efecto) },
    { { 0x0012, GS_T_U8,   1, 5 },                      DESP(menu_intensidad) },
    { { 0x0013, GS_T_U8,   0, 7 },                      DESP(menu_ejes) },
    { { 0x0014, GS_T_BOOL, 0, 1 },                      DESP(luces) },
    { { 0x0015, GS_T_U8,   0, SH_LUZ_COLORES - 1 },     DESP(luces_color) },
    { { 0x0016, GS_T_BOOL, 0, 1 },                      DESP(luces_latido) },
    { { 0x0017, GS_T_BOOL, 0, 1 },                      DESP(menu_propio) },
    { { 0x0018, GS_T_BOOL, 0, 1 },                      DESP(giro) },
    { { 0x0019, GS_T_U8,   0, SH_MEDIOS_COLORES - 1 },  DESP(medios_color) },
    { { 0x001A, GS_T_TEXTO, 0, sizeof aj.fondo - 1 },   NO_CAMPO },              /* aj.fondo */
};
#define N_CLAVES (sizeof tabla / sizeof tabla[0])
enum { K_TEMA = 1, K_ZONA = 6, K_BRILLO = 7, K_VOLUMEN = 0x0C, K_PASOS = 0x0E, K_AHORRO = 0x0F, K_ESTILO = 0x10, K_EFECTO = 0x11, K_LUCES = 0x14, K_LUZ_COLOR = 0x15,
       K_LUZ_LATIDO = 0x16, K_MENU_PROPIO = 0x17, K_FONDO = 0x1A };
_Static_assert(N_CLAVES == 26 && N_CLAVES <= GS_COLA_MAX, "las 26 claves del contrato §3");
const GsClave *gs_sis_claves(u32 *n)
{
    static GsClave c[N_CLAVES];
    u32 i;
    for (i = 0; i < N_CLAVES; i++) c[i] = tabla[i].c;
    *n = N_CLAVES;
    return c;
}
static u8 *campo(u32 i) { return (u8 *)&aj + tabla[i].desp; }
int gs_sis_valor(u16 clave, i16 *v, const char **txt)
{
    u32 i;
    if (clave == K_ZONA) { *v = aj.zona_min; return 0; }
    if (clave == K_FONDO) { *txt = aj.fondo; return 0; }
    for (i = 0; i < N_CLAVES; i++) if (tabla[i].c.clave == clave) { *v = *campo(i); return 0; }
    return -1;
}
/* El fondo: «» (ninguno), «@medios» o una ruta absoluta. No se mira si el fichero está: la tarjeta puede no estar puesta, y el
 * standby ya sale sin fondo si no se puede abrir. */
int gs_sis_texto_vale(u16 clave, const char *s) { return clave == K_FONDO && (!s[0] || s[0] == '/' || !strcmp(s, SH_FONDO_MEDIOS)); }

/* ---- APLICAR (contrato §3): aquí, fuera del manejador. Una sola pasada por la cola y UNA escritura al final: SENUS puede mandar las 26 de golpe,
 * y cada sh_guardar_ajustes es un registro nuevo en el almacén. Los que traen efecto (tema, fondo…) van con su función, que es la de Ajustes. ---- */
static void aplicar(void)
{
    u32 n, i, utc = 0;
    const GsCambio *c = gs_cola(&n);
    const char *fondo = gs_cola_texto();
    i16 zona = 0;
    int tema = -1, estilo = 0, pasos = 0, luces = 0, menu = 0;
    u8 addr[6]; char pin[GS_PIN_MAX + 1];
    if (gs_cola_bt(addr, pin)) {                         /* SEN_BT_EMPAREJAR va solo en la cola: nadie encola con la cola llena. Ni ajustes ni repintado */
        gs_cola_vaciar();
        sh_red_emparejar(addr, pin);                     /* el resultado, por SEN_BT_ESTADO */
        memset(pin, 0, sizeof pin);
        sh_actividad();
        return;
    }
    for (i = 0; i < n; i++) {
        u16 k = c[i].clave; i16 v = c[i].v; u32 j;
        switch (k) {
        case K_TEMA: if (v != aj.tema) tema = v; continue;
        case K_ZONA: aj.zona_min = v; continue;
        case K_BRILLO: aj.brillo = (u8)v; hw_luz(0, aj.brillo); continue;
        case K_VOLUMEN: aj.volumen = (u8)v; if (!aj.volumen) sh_callar(); continue;
        case K_PASOS: case K_AHORRO: pasos = 1; break;
        case K_ESTILO: estilo = v != aj.menu_estilo; break;
        case K_EFECTO: if (v == SH_EFECTO_INCLINA && !placa_tiene(CAP_ACELEROMETRO)) v = SH_EFECTO_VAIVEN; break;   /* (como Ajustes) sin sensor no hay inclinación */
        case K_LUCES: case K_LUZ_COLOR: case K_LUZ_LATIDO: luces = 1; break;
        case K_MENU_PROPIO: menu = v != aj.menu_propio; break;
        default: break;
        }
        for (j = 0; j < N_CLAVES; j++) if (tabla[j].c.clave == k) { *campo(j) = (u8)v; break; }
    }
    if (gs_cola_reloj(&utc, &zona)) {                    /* SEN_RELOJ: el RTC va en UTC; la zona, en Ajustes */
        HwFecha f;
        gs_fecha_de_unix(utc, &f);
        if (hw_rtc_poner(&f) != HW_OK) hw_log("gestion: el RTC no ha aceptado la fecha\n");
        aj.zona_min = zona;
    }
    if (aj.apagar_s && aj.atenuar_s > aj.apagar_s) aj.atenuar_s = aj.apagar_s;   /* como al cargar: SENUS relee y lo ve */
    if (estilo && !fondo) {                              /* (28-oct, como Ajustes) con el menú de Medios, su fondo animado; al dejarlo, se quita si era ese */
        if (aj.menu_estilo == M3_MEDIOS) fondo = SH_FONDO_MEDIOS;
        else if (sh_fondo_medios()) fondo = "";
    }
    if (pasos) sh_pasos_ajuste(aj.pasos, aj.pasos_ahorro);
    if (luces) sh_luces_ajuste(aj.luces, aj.luces_color, aj.luces_latido);
    if (fondo && strcmp(fondo, aj.fondo)) sh_poner_fondo(fondo);
    if (tema >= 0) sh_poner_tema(tema);
    if (menu) sh_apps_pendiente();                       /* el menú se rehace al volver al standby, como con una app nueva */
    sh_actividad();                                      /* los plazos de energía, otra vez desde ahora */
    sh_guardar_ajustes();                                /* programa también la alarma del chip; si nada cambió no gasta flash */
    ui_reconstruir_todo();
    gs_cola_vaciar();
}

/* ---- SEN_INFO, SEN_BT_ESTADO, el id --------------------------------------------------------------------------------------------------- */
void gs_sis_info(GsInfo *i)
{
    HwTarjeta t; HwCarga g; const BtDriver *b = bt_driver(); int p = hw_bateria_pct();
    i->api_mayor = W5_API_MAYOR; i->api_menor = CG_API_MENOR;
    i->bl_version = 0;                                   /* la fuente real es BL_R_ID: senew no lo sabe */
    i->bateria_pct = p < 0 || p > 100 ? 0xFF : (u8)p;
    hw_tarjeta_estado(&t); i->tarjeta = t.estado == HW_TJ_LISTA;
    hw_carga_estado(&g); i->cargando = g.estado == CARGA_CARGANDO;
    if (b) b->propia(i->bd_addr);                        /* (0.29.1) con la radio apagada también: SENUS la pone en sus permitidos ANTES de la llamada. Sin radio, ceros */
    i->ajustes_version = (u32)sh_ajustes_version();
    i->soc = placa()->soc;
}
void gs_sis_bt(GsBt *b)
{
    const BtDriver *d = bt_driver();
    b->encendido = (u8)sh_bt_activo();
    b->puente_conectado = d && d->estado() == SH_BT_CONECTADO;
    b->red_lista = sh_red_estado() == SH_RED_LISTA;
    b->fallo = (u8)sh_red_fallo();
    if (!sh_red_puente(b->puente_addr)) memset(b->puente_addr, 0, sizeof b->puente_addr);
}
/* Sin radio no hay orden; con una app delante, ni se intenta: puede tener un robot, y la red y un robot son excluyentes */
int gs_sis_bt_puede(void) { return !sh_bt_hay() ? GS_R_OP_NO : cg_estado() != CG_LIBRE ? GS_R_OCUPADO : GS_R_OK; }
int gs_sis_id_leer(char id[GS_ID_TAM]) { return hw_cargar(NV_APARATO, id, GS_ID_TAM) == GS_ID_TAM; }
int gs_sis_id_guardar(const char id[GS_ID_TAM]) { return hw_guardar(NV_APARATO, id, GS_ID_TAM); }
void gs_sis_serie(const char *s) { usb_poner_serie(s); }

/* ---- SEN_ENTRAR_BL (contrato §5) --------------------------------------------------------------------------------------------------------
 * La primera llamada abre «¿Actualizar el sistema?» y contesta OCUPADO; SENUS reintenta cada 500 ms. Sí: R_OK, y al menos 150 ms después, para que la
 * respuesta salga, fuera el USB y al BL. No: R_DENEGADO una vez y como al principio. El diálogo no tiene retrollamada de «No»: se cierra
 * sin haber dicho «Sí» (botón, tecla o cualquier otra cosa) = No. Si SENUS deja de preguntar, se olvida: un «Sí» a un host que ya
 * se fue no puede dejar un reinicio pendiente para la próxima vez. hw_entrar_bl solo existe en el W580 (y un doble en el PC): débil. */
void hw_entrar_bl(void) __attribute__((weak));
extern const Clase ClsDialogo;
#define BL_ESPERA_MS   250u                              /* >= 150: lo que aguanta el USB tras dejar la respuesta en el tubo */
#define BL_TOPE_MS     2000u                             /* la respuesta no sale (el host no la lee): se va igual, el usuario ya dijo «Sí» */
#define BL_SIN_HOST_MS 5000u                             /* sin un reintento en este tiempo, el host se ha ido */
enum { EB_LIBRE, EB_ABRIR, EB_PREGUNTA, EB_SI, EB_NO, EB_SALIENDO };
static struct { u8 e, visto, dicho; u32 t_poll, t0, t1; } eb;   /* visto: el diálogo ya está arriba (ui_empujar_emergente se hace en el ui_paso siguiente) */
static void bl_si(void *d) { (void)d; if (eb.e == EB_PREGUNTA) eb.e = EB_SI; }
static void bl_olvidar(void) { if (eb.e == EB_PREGUNTA) ui_quitar_ruta(&ClsDialogo); eb.e = EB_LIBRE; }
int gs_sis_bl(void)
{
    if (!hw_entrar_bl) return GS_R_OP_NO;
    eb.t_poll = hw_ms();
    switch (eb.e) {
    case EB_LIBRE: eb.e = EB_ABRIR; return GS_R_OCUPADO;
    case EB_SI: eb.e = EB_SALIENDO; eb.t0 = eb.t_poll; eb.dicho = 0; return GS_R_OK;
    case EB_NO: eb.e = EB_LIBRE; return GS_R_DENEGADO;
    case EB_SALIENDO: return GS_R_OK;
    default: return GS_R_OCUPADO;
    }
}
static void bl_paso(void)
{
    u32 t = hw_ms();
    MtpInfo mi;
    switch (eb.e) {
    case EB_ABRIR:
        if (t - eb.t_poll > BL_SIN_HOST_MS) { eb.e = EB_LIBRE; return; }
        if (ui_cima_emergente()) return;                 /* lo que esté en pantalla (la batería, el permiso) va antes */
        eb.e = EB_PREGUNTA; eb.visto = 0;
        sh_actividad();
        ui_confirmar_no("Actualizar", "¿Actualizar el sistema? La computadora va a instalar otra versión de senew. No desconectes el cable hasta que acabe.", bl_si, NULL);
        return;
    case EB_PREGUNTA:
        if (t - eb.t_poll > BL_SIN_HOST_MS) { bl_olvidar(); return; }
        if (ui_cima_emergente()) eb.visto = 1;
        else if (eb.visto) eb.e = EB_NO;                 /* se cerró y bl_si no dijo nada */
        return;
    case EB_SI: case EB_NO:
        if (t - eb.t_poll > BL_SIN_HOST_MS) eb.e = EB_LIBRE;
        return;
    case EB_SALIENDO:
        mtp_info(&mi);
        if (!mi.ocupado && !eb.dicho) { eb.dicho = 1; eb.t1 = t; }   /* la respuesta ya está en el tubo */
        if ((eb.dicho && t - eb.t1 >= BL_ESPERA_MS) || t - eb.t0 >= BL_TOPE_MS) {
            hw_log("gestion: al bootloader\n");
            sh_red_desconectar(); sh_bt_apagar();        /* (0.29.1) el chip en reset: tras emparejar, la radio suele estar encendida, y el BL no la atiende */
            mtp_parar();                                 /* un temporal a medias se tira */
            usb_parar();                                 /* fuera el pull-up; el BL levanta el suyo */
            hw_entrar_bl();                              /* la carga fuera, la bandera BLOA y reinicio_caliente: no vuelve en el teléfono */
            eb.e = EB_LIBRE;
        }
        return;
    }
}

/* ---- SEN_APPS (contrato §6): TODAS las apps con su origen. `/apps` de fábrica no se ve por MTP: por eso esta orden.
 * El orden es el del menú (shell/cargador.c: descubrir_menu), porque de él depende cuál manda si dos se llaman igual. Una entrada es de quien
 * trae una cabecera buena, aunque el sistema no la pueda abrir (pide una API más nueva): SENUS tiene api_mayor y api_menor para decirlo, y es
 * justo la que hay que actualizar. Una cabecera rota no es una app. Las RETIRADAS del W6FS (shell/apps.c) no se listan: ya no son del
 * sistema, y por MTP no se pueden borrar. Las .sen de «Mis archivos» (/usuario) quedan fuera: el contrato no les da origen.
 * Se recorre a pasos en gs_paso, con OCUPADO mientras tanto: la tarjeta tarda y el USB no se puede quedar sin atender. */
#ifndef GS_APPS_MAX
#define GS_APPS_MAX 64u                                  /* lo que entra en SenApp[]: hoy, 21 del SDK + los juegos. Más: no se listan */
#endif
#define AP_POR_PASO 4u
static const struct { const char *dir; u8 origen; } ap_dirs[] = { { "/apps", 0 }, { "/tarjeta/apps", 2 }, { HW_FS_MEMORIA "/apps", 1 }, { "/tarjeta/juegos", 3 } };
#define AP_DIRS (sizeof ap_dirs / sizeof ap_dirs[0])
enum { AP_NADA, AP_RECORRE, AP_LISTA };
static u8 ap_t[GS_APPS_MAX][GS_APP_TAM];
static struct { u8 e, fresca, lleno; u32 dir, desde, n, cambios; } ap;
static void p16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void p32(u8 *p, u32 v) { p16(p, v); p16(p + 2, v >> 16); }
static int es_sen(const char *s)
{
    size_t n = strlen(s), i;
    static const char x[] = ".sen";
    if (n <= 4) return 0;
    for (i = 0; i < 4; i++) { char a = s[n - 4 + i]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32); if (a != x[i]) return 0; }
    return 1;
}
static int nombre_igual(const u8 *a, const char *b) { return !memcmp(a, b, 30); }
static void ap_uno(u32 d, const HwEntrada *e)
{
    static char r[96];
    W5xCab c;
    u8 *a; u8 b = 0;
    u32 i, l = (u32)strlen(ap_dirs[d].dir), k;
    if (e->es_dir || !es_sen(e->nombre) || l + strlen(e->nombre) + 2 > sizeof r) return;
    memcpy(r, ap_dirs[d].dir, l); r[l] = '/'; memcpy(r + l + 1, e->nombre, strlen(e->nombre) + 1);
    if (sh_ruta_retirada(r)) return;
    if (hw_fs_leer_desde(r, 0, (u8 *)&c, W5X_TAM_CAB) != W5X_TAM_CAB) return;
    if (memcmp(c.magia, W5X_MAGIA, 4) || c.tam_cab != W5X_TAM_CAB || cg_crc(0, &c, W5X_TAM_CAB - 4) != c.crc_cab) return;
    if (ap.n >= GS_APPS_MAX) { if (!ap.lleno) hw_log("gestion: más apps de las que se listan\n"); ap.lleno = 1; return; }
    c.nombre[sizeof c.nombre - 1] = 0;
    if (ap_dirs[d].origen == 0) b |= 1;                  /* protegida: el W6FS no se toca por USB */
    if (cg_ruta_nueva(r) != r || cg_copia_de_fabrica(ap_dirs[d].dir, e->nombre)) b |= 2;   /* su copia nueva en la Memoria, o ella es la copia vieja */
    for (i = 0; i < ap.n && !(b & 2); i++) if (!(ap_t[i][1] & 2) && nombre_igual(ap_t[i] + 44, c.nombre)) b |= 2;   /* (28-oct) sin repetidas: la primera sale */
    a = ap_t[ap.n++];
    memset(a, 0, GS_APP_TAM);
    a[0] = ap_dirs[d].origen; a[1] = b; a[2] = c.perfil; a[3] = c.icono_gen; a[4] = c.icono_an;
    p16(a + 6, c.api_mayor); p16(a + 8, c.api_menor); p32(a + 12, c.ver_app); p32(a + 16, e->tam);
    for (k = 0; k < 12; k++) p16(a + 20 + 2 * k, c.icono[k]);
    memcpy(a + 44, c.nombre, 30);
    for (k = 0; e->nombre[k] && k < 53; k++) a[74 + k] = (u8)e->nombre[k];
}
static void ap_paso(void)
{
    static HwEntrada e[AP_POR_PASO];
    int k, i;
    if (ap.dir >= AP_DIRS) { ap.e = AP_LISTA; ap.fresca = 1; return; }
    if (ap_dirs[ap.dir].origen == 0 && !hw_fs_listo()) { ap.dir++; ap.desde = 0; return; }
    k = hw_fs_listar(ap_dirs[ap.dir].dir, ap.desde, e, AP_POR_PASO);
    for (i = 0; i < k; i++) ap_uno(ap.dir, &e[i]);
    ap.desde += k > 0 ? (u32)k : 0;
    if (k < (int)AP_POR_PASO) { ap.dir++; ap.desde = 0; }
    hw_perro_latido();
}
int gs_sis_apps(u32 desde, u32 *total, const u8 **e)
{
    if (ap.e == AP_LISTA && (ap.cambios != mtp_apps_cambios() || (!desde && !ap.fresca))) ap.e = AP_NADA;   /* subió o se borró algo; o es otra vuelta desde el principio */
    if (ap.e == AP_NADA) { memset(&ap, 0, sizeof ap); ap.e = AP_RECORRE; ap.cambios = mtp_apps_cambios(); }
    if (ap.e != AP_LISTA) return 1;
    if (!desde) ap.fresca = 0;
    *total = ap.n; *e = ap_t[0];
    return 0;
}

/* ---- la vida del canal ------------------------------------------------------------------------------------------------------------------ */
void gs_sis_sesion_nueva(void) { eb.e = EB_LIBRE; ap.e = AP_NADA; }
void gs_sis_parar(void) { bl_olvidar(); ap.e = AP_NADA; }
void gs_paso(void)
{
    if (gs_cola_hay()) aplicar();
    if (ap.e == AP_RECORRE) ap_paso();
    if (eb.e != EB_LIBRE) bl_paso();
}
