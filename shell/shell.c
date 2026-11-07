/* El shell: temas, andamio de pantalla, barra de estado, standby (con el fondo, el reloj del RTC y
 * el vigilante de alarma y cuenta atrás) y el menú principal. */
#include "shell.h"
#include "placa.h"
#include "tarjeta.h"                               /* (I1) la tarjeta de memoria: su aviso y el menú */
#include "m3d.h"
#include "mtp.h"                                  /* (MTP) una .sen que llega por USB a /memoria/apps: el menú se rehace */

/* ---- temas y ajustes ------------------------------------------------------------------------------------ */
/* (MD) El «tema» de Ajustes es ahora la pareja DISEÑO + PALETA (ui/diseno.h). Va en el MISMO byte de siempre (aj.tema), así el
 * registro de ajustes no cambia de versión: 0 y 1 son material3 oscuro y claro (el 0 de fábrica: lo que era «Oscuro» pasa a ser
 * el oscuro nuevo), 2..4 el clásico de siempre (oscuro, claro, fósforo). Un registro viejo con 2 («Fósforo») se lee como
 * «clásico oscuro»: lo único que cambia de sitio. docs/material.md §6. */
static const struct { const DisenoTabla *d; u8 paleta; } temas[N_TEMAS] = {
    { &dis_material3, 0 }, { &dis_material3, 1 }, { &dis_clasico, 0 }, { &dis_clasico, 1 }, { &dis_clasico, 2 } };
const char *const sh_tema_nombre[N_TEMAS] = { "Material oscuro", "Material claro", "Clásico oscuro", "Clásico claro", "Clásico fósforo" };
Ajustes aj = { .tema = TEMA_FABRICA, .alarma_h = 7, .alarma_m = 0, .brillo = 50,
               .atenuar_s = 15, .apagar_s = 30, .bloqueo_auto = 0,
#ifdef TELEFONO
               .tapa_apaga = 0,      /* hallazgo M9 de la pista R: la polaridad del deslizador es [inferido]; si estuviera al revés, con esto a 1
                                        el teléfono ABIERTO parecería muerto. De fábrica NO apaga; se enciende en Ajustes cuando se haya visto */
#else
               .tapa_apaga = 1,
#endif
               .volumen = 4, .tono_teclas = 1, .pasos = 1, .pasos_ahorro = SH_PASOS_SIN_AHORRO,
               /* el menú (pista V; el mantenedor, 2026-10-15: «Menú 3D» de fábrica): el estilo Walkman, que va con la semilla naranja de
                * Material; la rejilla (M3_CLASICO, que dibuja el diseño en uso) sigue en Ajustes -> «Menú: estilo» y es la del modo
                * seguro (docs/menu3d.md §5) */
               .menu_estilo = M3_WALKMAN, .menu_efecto = SH_EFECTO_INCLINA, .menu_intensidad = 3, .menu_ejes = 0,
               /* las luces (pista S5): laterales encendidas, a juego con el menú, con latido */
               .luces = 1, .luces_color = 0, .luces_latido = 1, .giro = 1 };

/* Los ajustes en el almacén: un registro con versión. Solo se escribe lo que hay (el fondo, hasta su fin).
 * La v2 (pista S3) añadió cuatro bytes de energía y la v3 (S5) dos de sonido, siempre ANTES del fondo; un
 * registro de una versión vieja se sigue leyendo y lo que no trae se queda en los valores de fábrica, así
 * que actualizar el sistema no borra lo que había. La v4 (S2) añade dos del podómetro y la v5 (V) cuatro del
 * menú 3D (estilo, efecto, intensidad, ejes). La v6 (S5) añade tres de las luces. La v7 (LANZ) uno: si el usuario eligió las
 * apps del menú (aj.menu_propio; la elección va aparte, en NV_MENU). Un registro v6 o anterior se lee con menu_propio = 0: el menú
 * de fábrica de siempre. */
#define AJ_VERSION 9                         /* (estética) v8: aj.giro (un registro v7 o anterior se lee con el giro automático); (28-oct) v9: aj.medios_color */
#define AJ_CAB_V1 9
#define AJ_CAB_V2 13
#define AJ_CAB_V3 15
#define AJ_CAB_V4 17
#define AJ_CAB_V5 21
#define AJ_CAB_V6 24
#define AJ_CAB_V7 25
#define AJ_CAB_V8 26
#define AJ_CAB    27
/* La alarma del CHIP (IRQ 43), además de la comparación por software del standby: el RTC va en UTC, así que
 * se programa la hora local menos la zona. */
static void programar_alarma(void)
{
    int m = aj.alarma_h * 60 + aj.alarma_m - aj.zona_min;
    m = ((m % 1440) + 1440) % 1440;
    hw_rtc_alarma(aj.alarma_on ? m / 60 : -1, m % 60);
}
int sh_ajustes_version(void) { return AJ_VERSION; }   /* (Fase 3b) para SEN_INFO */
static u8 aj_guardar_dicho;
int sh_guardar_ajustes(void)
{
    u8 b[AJ_CAB + sizeof aj.fondo]; u32 n = (u32)strlen(aj.fondo);
    b[0] = AJ_VERSION; b[1] = aj.tema; b[2] = aj.hora12; b[3] = aj.alarma_on; b[4] = aj.alarma_h; b[5] = aj.alarma_m;
    b[6] = (u8)aj.zona_min; b[7] = (u8)((u16)aj.zona_min >> 8); b[8] = aj.brillo;
    b[9] = aj.atenuar_s; b[10] = aj.apagar_s; b[11] = aj.bloqueo_auto; b[12] = aj.tapa_apaga;
    b[13] = aj.volumen; b[14] = aj.tono_teclas;
    b[15] = aj.pasos; b[16] = aj.pasos_ahorro;
    b[17] = aj.menu_estilo; b[18] = aj.menu_efecto; b[19] = aj.menu_intensidad; b[20] = aj.menu_ejes;
    b[21] = aj.luces; b[22] = aj.luces_color; b[23] = aj.luces_latido;
    b[24] = aj.menu_propio;                                  /* (LANZ) v7 */
    b[25] = aj.giro;                                         /* (estética) v8 */
    b[26] = aj.medios_color;                                 /* (28-oct) v9 */
    memcpy(b + AJ_CAB, aj.fondo, n);
    {   /* (R2; auditoría del sistema, M5) antes no se miraba: con el almacén lleno, protegido o con un fallo de flash cada cambio
         * parecía aceptado y se perdía al apagar. Se dice UNA vez (y otra si, tras volver a poder, vuelve a fallar). En modo
         * seguro o con la flash protegida ya lo dice el rótulo: ahí no se repite. */
        int r = hw_guardar(NV_AJUSTES, b, AJ_CAB + n);
        programar_alarma();
        if (r == HW_OK) aj_guardar_dicho = 0;
        else if (!aj_guardar_dicho && !hw_nor_proteccion() && !hw_formato_pendiente() && ui_rutas() > 0) {
            aj_guardar_dicho = 1;
            hw_log("ajustes: el cambio NO se ha podido guardar\n");
            ui_aviso("Ajustes", r == HW_ERROR ? "El cambio vale ahora, pero NO se ha podido guardar (memoria de ajustes llena o con fallo): se perderá al apagar."
                                              : "El cambio vale ahora, pero NO se guarda (no hay memoria de ajustes): se perderá al apagar.");
        }
        return r;
    }
}
static int cargar_ajustes(void)                   /* 1 = había ajustes guardados */
{
    u8 b[AJ_CAB + sizeof aj.fondo]; int n = hw_cargar(NV_AJUSTES, b, sizeof b - 1), cab;
    /* (S7; auditoría C1) hw_cargar devuelve el tamaño DEL REGISTRO aunque no quepa en el búfer (solo copia lo que cabe): un registro
     * más largo de la cuenta —dañado, o de otra versión— hacía que se leyera y se copiara de más. Se recorta a lo que hay en `b`. */
    if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
    if (n < AJ_CAB_V1 || b[0] < 1 || b[0] > AJ_VERSION) return 0;
    cab = b[0] == 1 ? AJ_CAB_V1 : b[0] == 2 ? AJ_CAB_V2 : b[0] == 3 ? AJ_CAB_V3 : b[0] == 4 ? AJ_CAB_V4 : b[0] == 5 ? AJ_CAB_V5 : b[0] == 6 ? AJ_CAB_V6 : b[0] == 7 ? AJ_CAB_V7 : b[0] == 8 ? AJ_CAB_V8 : AJ_CAB;
    if (n < cab) return 0;
    aj.tema = (u8)(b[1] % N_TEMAS); aj.hora12 = b[2] != 0; aj.alarma_on = b[3] != 0;
    aj.alarma_h = (u8)(b[4] % 24); aj.alarma_m = (u8)(b[5] % 60);
    aj.zona_min = (i16)(b[6] | (b[7] << 8)); if (aj.zona_min < -720 || aj.zona_min > 840) aj.zona_min = 0;
    aj.brillo = b[8] > 100 ? 100 : b[8];
    if (cab >= AJ_CAB_V2) {
        aj.atenuar_s = b[9]; aj.apagar_s = b[10]; aj.bloqueo_auto = b[11] != 0; aj.tapa_apaga = b[12] != 0;
        if (aj.apagar_s && aj.atenuar_s > aj.apagar_s) aj.atenuar_s = aj.apagar_s;
    }
    if (cab >= AJ_CAB_V3) { aj.volumen = b[13] > 5 ? 5 : b[13]; aj.tono_teclas = b[14] != 0; }
    if (cab >= AJ_CAB_V4) { aj.pasos = b[15] != 0; aj.pasos_ahorro = b[16] < SH_PASOS_AHORROS ? b[16] : 0; }
    if (cab >= AJ_CAB_V5) {
        aj.menu_estilo = b[17] < M3_ESTILOS ? b[17] : M3_WALKMAN; aj.menu_efecto = b[18] < SH_EFECTOS ? b[18] : 0;
        aj.menu_intensidad = b[19] >= 1 && b[19] <= 5 ? b[19] : 3; aj.menu_ejes = b[20] & 7;
    }
    if (cab >= AJ_CAB_V6) { aj.luces = b[21] != 0; aj.luces_color = b[22] < SH_LUZ_COLORES ? b[22] : 0; aj.luces_latido = b[23] != 0; }
    aj.menu_propio = cab >= AJ_CAB_V7 ? b[24] != 0 : 0;  /* (LANZ) v7; uno viejo (o en blanco): el menú de fábrica */
    aj.giro = cab >= AJ_CAB_V8 ? b[25] != 0 : 1;         /* (estética) v8; uno viejo: el giro automático (el de fábrica) */
    aj.medios_color = cab >= AJ_CAB && b[26] < SH_MEDIOS_COLORES ? b[26] : SH_MEDIOS_ROJO;   /* (28-oct) v9; uno viejo: el rojo */
    {   /* (S7; auditoría C1) EL TOPE: con una cabecera vieja (v1: 9 bytes) y un registro largo, n - cab pasaba de sizeof aj.fondo y el
         * memcpy escribía hasta 15 bytes FUERA de `aj`. El fondo se recorta a lo que cabe, con su cero. */
        size_t m = (size_t)n - (size_t)cab;
        if (m > sizeof aj.fondo - 1) m = sizeof aj.fondo - 1;
        memcpy(aj.fondo, b + cab, m); aj.fondo[m] = 0;
    }
    return 1;
}
u32 sh_temporizador_fin;
static UiGif *fondo_gif;

const TemaDatos *sh_tema(void) { return dis_tema(); }
static void tema_aplicar(void) { dis_poner(temas[aj.tema % N_TEMAS].d, temas[aj.tema % N_TEMAS].paleta); ui_tema_global(sh_tema()); }
void sh_poner_tema(int i)
{
    aj.tema = (u8)((i % N_TEMAS + N_TEMAS) % N_TEMAS);
    tema_aplicar();
    ui_redisponer_todo();                                 /* (MD) cambia la letra: las medidas de TODO el árbol, no solo los colores */
    sh_guardar_ajustes();
}
void sh_poner_fondo(const char *ruta)
{
    unsigned n = 0;
    if (strlen(ruta) > sizeof aj.fondo - 1) {             /* (R2; M7) antes se cortaba en silencio: el fondo se veía hasta el siguiente arranque y luego desaparecía */
        ui_aviso("Fondo", "La ruta de esa imagen es demasiado larga para guardarla como fondo. Cópiala a Mis archivos y ponla desde ahí.");
        return;
    }
    if (fondo_gif) { ui_gif_cerrar(fondo_gif); fondo_gif = NULL; }
    while (ruta[n] && n < sizeof aj.fondo - 1) { aj.fondo[n] = ruta[n]; n++; }
    aj.fondo[n] = 0;
    if (n && !sh_fondo_medios() && (fondo_gif = ui_gif_abrir_fondo(aj.fondo)) != NULL) ui_gif_soltar_fichero(fondo_gif);   /* (28-oct) el de Medios no es un fichero */
    ui_reconstruir_todo();
    sh_guardar_ajustes();
}
const Imagen *sh_fondo(void) { return fondo_gif ? &fondo_gif->im : NULL; }
int sh_fondo_medios(void) { return !strcmp(aj.fondo, SH_FONDO_MEDIOS); }
/* (28-oct) el color de Medios. «Según el mes»: los de la PS3 (enero plata, febrero oro, marzo verde, abril rosa, mayo verde, junio morado,
 * julio turquesa, agosto azul, septiembre morado, octubre oro, noviembre naranja, diciembre rojo); sin la hora puesta, el rojo. */
const char *const sh_medios_color_nombre[SH_MEDIOS_COLORES] = { "Rojo", "Naranja", "Oro", "Verde", "Turquesa", "Azul", "Morado", "Rosa",
                                                                 "Plata", "Negro", "Según el mes" };
int sh_medios_tono(void)
{
    static const u8 mes[12] = { SH_MEDIOS_PLATA, SH_MEDIOS_ORO, SH_MEDIOS_VERDE, SH_MEDIOS_ROSA, SH_MEDIOS_VERDE, SH_MEDIOS_MORADO,
                                SH_MEDIOS_TURQUESA, SH_MEDIOS_AZUL, SH_MEDIOS_MORADO, SH_MEDIOS_ORO, SH_MEDIOS_NARANJA, SH_MEDIOS_ROJO };
    HwFecha f;
    if (aj.medios_color < SH_MEDIOS_MES) return aj.medios_color;
    return sh_ahora(&f) == HW_OK && f.mes >= 1 && f.mes <= 12 ? mes[f.mes - 1] : SH_MEDIOS_ROJO;
}

/* ---- utilidades -------------------------------------------------------------------------------------------- */
const char *const sh_dia[7] = { "lun", "mar", "mié", "jue", "vie", "sáb", "dom" };
const char *const sh_mes[12] = { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic" };
int sh_ahora(HwFecha *f) { int r = hw_rtc_leer(f); hw_fecha_sumar(f, aj.zona_min); return r; }
int sh_poner_hora_local(const HwFecha *f) { HwFecha u = *f; hw_fecha_sumar(&u, -aj.zona_min); return hw_rtc_poner(&u); }
const char *sh_zona(void)                          /* (R3; calidad) const: uno de los dos caminos devuelve un literal */
{
    int z = aj.zona_min < 0 ? -aj.zona_min : aj.zona_min;
    return aj.zona_min ? ui_fmt("UTC%c%d:%02d", aj.zona_min < 0 ? '-' : '+', z / 60, z % 60) : "UTC";
}
char *sh_hora(const HwFecha *f, int seg)
{
    int h = f->hora;
    if (!aj.hora12) return seg ? ui_fmt("%02d:%02d:%02d", h, f->min, f->seg) : ui_fmt("%02d:%02d", h, f->min);
    h = h % 12 ? h % 12 : 12;
    return seg ? ui_fmt("%d:%02d:%02d %s", h, f->min, f->seg, f->hora < 12 ? "am" : "pm") : ui_fmt("%d:%02d %s", h, f->min, f->hora < 12 ? "am" : "pm");
}
char *sh_tam(u32 b)
{
    if (b < 1024) return ui_fmt("%u B", b);
    if (b < 1024 * 1024) return ui_fmt("%u KB", (b + 512) / 1024);
    return ui_fmt("%u,%u MB", b / (1024 * 1024), (b % (1024 * 1024)) * 10 / (1024 * 1024));
}

/* ---- andamio de pantalla ------------------------------------------------------------------------------------ */
typedef struct { Widget base; const TemaDatos *t; } RelojBarra;   /* la hora de la barra de estado: se refresca sola. t: sus colores (NULL = el tema) */
typedef struct { HwFecha f; } RelojBarraE;
static void rb_tic(Ctx *c) { sh_ahora(&((RelojBarraE *)ui_estado(c))->f); ui_sucio(c); }
static void rb_iniciar(Ctx *c) { sh_ahora(&((RelojBarraE *)ui_estado(c))->f); ui_cada(c, 1000, rb_tic); }
static Widget *rb_construir(Ctx *c)
{
    RelojBarraE *s = ui_estado(c);
    const RelojBarra *w = ui_widget(c);
    const TemaDatos *t = w->t ? w->t : sh_tema();
    return Fila(.espacio = 4, .hijos = HIJOS(
        sh_bateria_de(t->texto),                                      /* NULL si el PMU no contesta */
        sh_bloqueado() ? Icon(&sh_iconos[IC_CANDADO], .color = t->acento) : NULL,
        hw_serie_conectado() ? Icon(&sh_iconos[IC_ENCHUFE], .color = t->tenue) : NULL,
        sh_musica_estado() == MU_SONANDO ? Icon(&sh_iconos[IC_NOTA], .color = t->acento) : NULL,   /* suena música (se ve también fuera de su app) */
        aj.alarma_on ? Icon(&sh_iconos[IC_ALARMA], .color = t->acento) : NULL,
        Texto(sh_hora(&s->f, 0), .color = t->texto, .marcas = F_ESTILO(DIS_T_PEQUENA))));   /* (estética) la talla pequeña, como la hora del original */
}
COMPONENTE_CON_ESTADO(RelojBarra, RelojBarraE, rb_construir, rb_iniciar);
Widget *sh_reloj_barra(const TemaDatos *t) { return UI_W(RelojBarra, .t = t); }

/* (MD) El andamio y las piezas de siempre los dibuja el DISEÑO en uso (ui/diseno.h): aquí solo se dice QUÉ va en cada sitio. */
Widget *Pantalla(const char *titulo, Widget *cuerpo, const char *izq, const char *der)
{
    const DisenoTabla *d = dis_actual();
    return d->andamio(
        d->barra_superior(titulo, Fila(.espacio = 4, .hijos = HIJOS(
            hw_modo_seguro() ? Texto("MODO SEGURO", .color = dis_tokens()->c.error, .marcas = F_ESTILO(DIS_T_PEQUENA)) : NULL,      /* (S7) el rótulo, en TODAS las pantallas */
            UI_W(RelojBarra)))),
        cuerpo, d->barra_teclas(izq, der));
}
int sh_teclas(Ctx *c, int k, void *dato)
{
    (void)c; (void)dato;
    if (k == K_SEL_IZQ) { ui_tecla(K_OK); return 1; }
    if (k == K_SEL_DER) { ui_sacar(); return 1; }
    return 0;
}
Widget *sh_fila(const char *texto, const char *detalle, int sel)
{
    DisFila f = { .titulo = texto, .accesorio = DIS_ACC_VALOR, .valor = detalle };
    return dis_actual()->fila(&f, sel);
}
Widget *sh_dato(const char *nombre, const char *valor) { return dis_actual()->dato(nombre, valor); }
Widget *sh_boton(Ctx *c, const Foco *f, void *texto)
{
    (void)c;
    return dis_actual()->boton((const char *)texto, f->enfocado ? DIS_PRINCIPAL : DIS_SECUNDARIO, f->enfocado);
}

/* ---- el menú principal vive en shell/menu3d.c (pista V): el clásico de siempre y los estilos 3D ---------------- */
/* Abrir una entrada del menú. Una app de casa empuja su ruta y vuelve enseguida; una app EXTERNA se
 * queda con el teléfono aquí mismo y no devuelve el control hasta que el usuario sale de ella (el
 * sistema es cooperativo y de un solo hilo: esto es exactamente lo que quiere decir «manda ella»). */
void sh_abrir_app(int i)
{
    const App *a;
    if (i < 0 || i >= sh_n_apps) return;
    a = &sh_apps[i];
    if (a->entrar) { a->entrar(); sh_app_externa_recoger(); ui_forzar_todo(); }   /* (R2; A1) si volvió sin soltar la pantalla, antes el shell quedaba vivo e invisible */
    else if (a->abrir) ui_empujar(a->abrir());
    else if (a->ruta) sh_abrir_sen(a->ruta, NULL);           /* (APX) una app de fábrica de fuera: se carga del W6FS */
}

/* ---- standby ---------------------------------------------------------------------------------------------------
 * El reloj es el RTC del chip, leído una vez por segundo. Para que se lea sobre cualquier fondo, la
 * hora va sobre una banda de Velo (el fondo a la mitad de brillo) y con sombra. Aquí vive también el
 * vigilante de la alarma y de la cuenta atrás: el standby es la ruta 0 y nunca se desmonta. */
typedef struct { Widget base; } Standby;
typedef struct { HwFecha f; u8 valida; u8 md_activo; u16 md_pend; u32 md_t, md_ult; } StandbyE;   /* md_*: el fondo de Medios animado */

/* ---- EL VIGILANTE de la alarma y de la cuenta atrás -------------------------------------------------------
 * La alarma y la cuenta atrás SUENAN: shell/sonido.c las repite hasta que alguien pulse algo (el filtro de
 * teclas llama a sh_callar). Con el volumen a 0 no suena nada y el aviso en pantalla es lo único que queda:
 * el diálogo dice cuál de las dos cosas pasa.
 *
 * Vive FUERA del standby (pista S6) porque hay un caso en que el standby no late: una app EXTERNA con la
 * pantalla tomada (un juego grande). Decisión de producto para ese caso: **el aviso no le quita al juego ni la
 * pantalla ni una tecla** —la app es la dueña de las dos, y un diálogo encima de una partida a 30 Hz no hay
 * quien lo pinte—, pero **SUENA por encima** (la alarma 3 vueltas, ~7 s; la cuenta atrás 2) y queda
 * PENDIENTE: al devolver la pantalla, app_pantalla_soltar() enseña «mientras estabas en la app…».
 * Antes de esto, una alarma que saltaba jugando no hacía NADA hasta salir (y sonaba entonces, tarde, si el
 * cerrojo de la IRQ del RTC seguía puesto).
 *
 * El cerrojo de la IRQ del RTC (hw_rtc_alarma_salto) se consume SIEMPRE: antes solo se leía cuando la
 * comparación por software fallaba, así que se quedaba puesto todo el minuto de la alarma y la disparaba
 * OTRA VEZ en el minuto siguiente [leído en el código; en el PC no se veía porque allí vale siempre 0]. */
static u8 ultima_alarma_min;             /* minuto + 1 de la última alarma dada (0 = ninguna en este minuto) */
static struct { u8 cuales, h, m; u16 mv; } pend;   /* lo que saltó con una app externa en pantalla */

void sh_aviso_pendiente(int cual, int dato) { pend.cuales |= (u8)cual; if (cual == SH_AV_BATERIA) pend.mv = (u16)dato; }
int  sh_avisos_pendientes(void) { return pend.cuales; }
void sh_avisos_mostrar(void)
{
    const char *a = "", *b = "", *c = "", *d = "";
    HwFecha f = { .hora = pend.h, .min = pend.m };
    if (!pend.cuales) return;
    if (pend.cuales & SH_AV_ALARMA) a = ui_fmt("La alarma de las %s sonó.", sh_hora(&f, 0));
    if (pend.cuales & SH_AV_TEMPORIZADOR) b = "La cuenta atrás terminó.";
    if (pend.cuales & SH_AV_BATERIA) c = ui_fmt("Batería baja: %d mV. Conéctalo por USB a un ordenador para cargarla.", (int)pend.mv);
    if (pend.cuales & SH_AV_CARGADOR) d = hw_cargador() == 2 ? "USB conectado: la batería se carga del ordenador." : "Cargador de pared conectado: este sistema no carga con él. Desconéctalo.";
    pend.cuales = 0;
    sh_insistir(20000);
    ui_aviso("Mientras jugabas", ui_fmt("%s%s%s%s%s%s%s", a, *a && (*b || *c || *d) ? "\n" : "", b, *b && (*c || *d) ? "\n" : "", c, *c && *d ? "\n" : "", d));
}

void sh_vigilar_avisos(int en_app)
{
    HwFecha f;
    int valida = sh_ahora(&f) == HW_OK;
    int salto = hw_rtc_alarma_salto();                 /* SIEMPRE se consume (ver arriba) */
    if (aj.alarma_on && ((valida && f.hora == aj.alarma_h && f.min == aj.alarma_m) || salto)) {   /* por software o por la IRQ del chip */
        if (ultima_alarma_min != f.min + 1) {
            ultima_alarma_min = (u8)(f.min + 1);
            if (en_app) {
                sh_sonar_n(SN_ALARMA, 3);
                pend.cuales |= SH_AV_ALARMA; pend.h = f.hora; pend.m = f.min;
            } else {
                sh_sonar(SN_ALARMA);
                sh_insistir(60000);                    /* una alarma con la pantalla apagada no sirve de nada */
                sh_luces_evento(SH_LUZ_EV_ALARMA);     /* (S5) y su efecto de luz (alarm.ble, como el original) */
                ui_aviso("Alarma", ui_fmt("Son las %s.%s", sh_hora(&f, 0),
                                          sh_sonando() == SN_ALARMA ? " Pulsa cualquier tecla para callarla." : " (el sonido está en silencio)"));
            }
        }
    } else if (!valida || f.min + 1 != ultima_alarma_min) ultima_alarma_min = 0;
    if (sh_temporizador_fin && (i32)(hw_ms() - sh_temporizador_fin) >= 0) {
        sh_temporizador_fin = 0;
        if (en_app) { sh_sonar_n(SN_TEMPORIZADOR, 2); pend.cuales |= SH_AV_TEMPORIZADOR; }
        else {
            sh_sonar(SN_TEMPORIZADOR);
            sh_insistir(60000);
            sh_luces_evento(SH_LUZ_EV_TEMPORIZADOR);
            ui_aviso("Temporizador", "La cuenta atrás ha terminado.");
        }
    }
}
/* (I1) LA TARJETA DE MEMORIA, vista desde el shell (cada segundo, desde el standby, que está siempre debajo): si cambia lo que hay,
 *  - una tarjeta que NO SIRVE (no contesta, no es PRO, sin FAT que sepamos leer) se DICE con un aviso (antes solo se veía en
 *    Archivos, en el nombre de la carpeta);
 *  - el menú vuelve a mirar las apps cargables (también las de /tarjeta/apps), pero solo con el standby a la vista: nunca debajo
 *    de una app o del propio menú. Con una app externa a pantalla completa se espera a que la suelte. */
static u32 tj_visto; static u8 tj_menu_pend;
void sh_apps_pendiente(void) { tj_menu_pend = 1; }   /* (SER) apps/recibir: lo mismo que una tarjeta que entra */
static void tarjeta_vigilar(void)
{
    u32 c = hw_tarjeta_cambios();
    HwTarjeta t;
    { static u32 mtp_visto; u32 a = mtp_apps_cambios(); if (a != mtp_visto) { mtp_visto = a; sh_apps_pendiente(); } }   /* (MTP) como SER y la tarjeta */
    if (tj_menu_pend && ui_rutas() == 1) { tj_menu_pend = 0; sh_apps_init(); }
    if (c == tj_visto || app_pantalla_tomada() || ui_cima_emergente()) return;
    tj_visto = c;
    hw_tarjeta_estado(&t);
    tj_menu_pend = 1;
    if (t.estado == HW_TJ_MALA || t.estado == HW_TJ_SIN_FAT)
        ui_aviso("Tarjeta de memoria", ui_fmt("La tarjeta que has metido no se puede usar: %s. No se ha escrito nada en ella. Sácala y prueba otra, o vuelve a meterla.", t.texto));
}
/* (28-oct) EL FONDO DE MEDIOS ANIMADO (shell/menu_medios.c): ~15 cuadros/s mientras el standby está arriba y la pantalla encendida, como el
 * menú de Medios quieto; con el efecto «quieto», un cuadro fijo y ningún temporizador. */
static int standby_md_anima(Ctx *c) { return sh_fondo_medios() && aj.menu_efecto != SH_EFECTO_QUIETO && ui_raiz() == c && sh_pantalla_estado() != EN_APAGADA; }
static void standby_md_tic(Ctx *c)
{
    StandbyE *s = ui_estado(c);
    const u32 t = hw_ms();
    int dt = (int)(t - s->md_ult);
    s->md_ult = t;
    if (dt < 1) dt = 1;
    if (dt > 200) dt = 200;
    if (!standby_md_anima(c)) { ui_cancelar(c, standby_md_tic); s->md_activo = 0; return; }
    s->md_pend = (u16)(s->md_pend + dt);
    if (s->md_pend >= 62) { s->md_t += s->md_pend; s->md_pend = 0; ui_sucio(c); }
}
static void standby_tic(Ctx *c)
{
    StandbyE *s = ui_estado(c);
    tarjeta_vigilar();
    s->valida = sh_ahora(&s->f) == HW_OK;
    sh_vigilar_avisos(0);
    ui_sucio(c);
}
/* (S7) LO QUE HAY QUE DECIR AL ARRANCAR (docs/sistema.md §15.3, §15.4), en cuanto el standby está en pantalla:
 *  - modo seguro: qué es y cómo se sale;
 *  - flash protegida (el chip no es el esperado, o su tabla CFI no cuadra): el sistema funciona SIN ESCRIBIR;
 *  - una zona nuestra que no está en blanco ni lleva nuestra firma (en un aparato: restos del main original): se PREGUNTA
 *    antes de borrarla y darle formato. Con «No» (o sin contestar) no se toca: el sistema sigue, sin guardar. */
/* (AUD8 B1) En el aparato son hasta 76 borrados de bloque (la Memoria 64, ajustes 4, Mis archivos 8): de ~1,5 a ~5 min [inferido,
 * sin medir en un M18], no «15 s». La línea cabe en 176 px; la franja «Guardando…» con su barra cuenta los bloques.
 * (AUD8 B2) Si falla SOLO una zona, se dice cuál: las otras quedaron formateadas y montadas (y los ajustes se guardan). */
static const char *zonas_txt(int f);
static void formato_si(void *d)
{
    int f = hw_formato_pendiente(), r, bien = 0, malz = 0;
    HwNvEstado nv; HwUsrEstado us; HwW6fEstado w6;
    (void)d;
    sh_pantalla_arranque("formato: minutos. No apagues");
    r = hw_formatear(f);
    ui_forzar_todo();
    hw_nv_estado(&nv); hw_usr_estado(&us); hw_w6f_estado(&w6);
    if (f & HW_ZONA_AJUSTES) { if (nv.montado && nv.escribible) bien |= HW_ZONA_AJUSTES; else malz |= HW_ZONA_AJUSTES; }
    if (f & HW_ZONA_USUARIO) { if (us.montado && us.escribible) bien |= HW_ZONA_USUARIO; else malz |= HW_ZONA_USUARIO; }
    if (f & HW_ZONA_MEMORIA) { if (w6.escribible) bien |= HW_ZONA_MEMORIA; else malz |= HW_ZONA_MEMORIA; }
    if (bien & HW_ZONA_AJUSTES) sh_guardar_ajustes();
    hw_log(r == HW_OK && !malz ? "formato: hecho en todas las zonas\n" : bien ? "formato: hecho EN PARTE\n" : "formato: no se pudo\n");
    if (r == HW_OK && !malz) ui_aviso("Memoria", "Formato hecho. A partir de ahora los ajustes y los archivos se guardan.");
    else if (bien) ui_aviso("Memoria", ui_fmt("Formato hecho en %s. NO se pudo en %s: ahí el sistema sigue sin guardar.", zonas_txt(bien), zonas_txt(malz ? malz : f & ~bien)));
    else ui_aviso("Memoria", "No se pudo dar formato. El sistema sigue sin guardar nada.");
}
/* (I1) LA MEMORIA (W6F) AL ARRANCAR: en blanco (o con un formato a medias) se le da formato aquí, con su pantalla y la franja de
 * progreso (no destruye nada: la zona no tiene nada); hecha con OTRA configuración u OTRA versión, se DICE y no se toca (condición
 * 4 de la auditoría: NO dar formato); ajena (restos del main original), va con la pregunta de las demás zonas (hw_formato_pendiente). */
static int w6f_toca(void) { HwW6fEstado w; hw_w6f_estado(&w); return (w.situacion == HW_W6F_VIRGEN && !hw_nor_proteccion()) || w.situacion == HW_W6F_OTRA_CONF || w.situacion == HW_W6F_OTRA_VERSION; }
static void w6f_al_arrancar(void)
{
    HwW6fEstado w; int r;
    hw_w6f_estado(&w);
    if (w.situacion == HW_W6F_VIRGEN) {
        sh_pantalla_arranque("Memoria: minutos. No apagues");   /* (una sola vez; ~15 s en el emulador, 64 borrados en el aparato: AUD8 B1) */
        r = hw_w6f_formatear();
        ui_forzar_todo();
        hw_w6f_estado(&w);
        ui_aviso("Memoria", r == HW_OK ? ui_fmt("La Memoria está lista: %s para tus archivos, con carpetas (en Archivos, «Memoria»). Mis archivos sigue como siempre.", sh_tam(w.libre))
                                       : "No se pudo preparar la Memoria. El resto funciona igual; Mis archivos sigue como siempre.");
    } else
        ui_aviso("Memoria", ui_fmt("La Memoria se hizo con otra %s del sistema. NO se toca ni se le da formato: lo que tenga sigue ahí, sin usarse. Mis archivos funciona como siempre.",
                                   w.situacion == HW_W6F_OTRA_VERSION ? "versión" : "configuración (tamaño o número de archivos)"));
}
/* (P600; §2 n.º 9) las direcciones, de Placa.zonas */
static const char *zona_txt(int z, int hasta) { const PlacaZona *a = &placa()->zonas[z], *b = &placa()->zonas[hasta]; return ui_fmt("0x%08X a 0x%08X", a->base, b->base + b->tam - 1); }
static const char *zonas_txt(int f)                 /* (I1) las zonas sin formato, con palabras y direcciones */
{
    return ui_fmt("%s%s%s%s%s", f & HW_ZONA_MEMORIA ? ui_fmt("la Memoria (%s)", zona_txt(ZONA_MEMORIA, ZONA_MEMORIA)) : "", (f & HW_ZONA_MEMORIA) && (f & 3) ? ", " : "",
                  f & HW_ZONA_AJUSTES ? ui_fmt("la zona de ajustes (%s)", zona_txt(ZONA_AJUSTES, ZONA_AJUSTES)) : "", (f & 1) && (f & 2) ? ", " : "",
                  f & HW_ZONA_USUARIO ? ui_fmt("la de Mis archivos (%s)", zona_txt(ZONA_USUARIO, ZONA_USUARIO)) : "");
}
static void avisos_de_arranque(Ctx *c)
{
    static u8 w6f_dicho;
    int p = hw_nor_proteccion(), f = hw_formato_pendiente();
    HwInfo h; HwReinicio rr;
    static u8 reinicio_dicho;
    /* la tecla C que metió en el modo seguro sigue pulsada (y «C» cierra un diálogo, con repetición): se espera a que la suelten,
     * o el aviso se cerraría solo sin que nadie lo leyera [observado en el emulador con teclas-arranque=8] */
    if (hw_tecla_abajo(K_C)) { ui_tras(c, 300, avisos_de_arranque); return; }
    /* (R3) DE UNO EN UNO: si ya hay un diálogo arriba (este mismo, o el de la batería), se espera a que lo cierren. Antes el aviso
     * del modo seguro y el de «Batería agotada» salían apilados, con dos «Aceptar» a la vista. */
    if (ui_cima_emergente()) { ui_tras(c, 300, avisos_de_arranque); return; }
    hw_info(&h);
    hw_reinicio(&rr);
    if (!reinicio_dicho && rr.causa >= HW_RST_PERRO) {       /* (R3, P3) lo primero: por qué se reinició */
        reinicio_dicho = 1;
        ui_aviso("El sistema se reinició", ui_fmt("El sistema se reinició solo por %s%s. %s",
                 rr.causa == HW_RST_PERRO ? "un cuelgue (dejó de responder y lo sacó el perro guardián)" : rr.causa == HW_RST_EXCEPCION ? "un fallo del procesador (una excepción)" :
                 rr.causa == HW_RST_PILA ? "una pila casi llena" : rr.causa == HW_RST_FORZADO ? "un apagado forzado que no llegó a apagar (¿cargador puesto?)" : "un fallo interno (pánico)",
                 rr.causa == HW_RST_PERRO || rr.causa == HW_RST_EXCEPCION ? ui_fmt(", en pc = %x", rr.pc) : "",
                 rr.sin_bucle ? ui_fmt("Arranques seguidos sin terminar antes de este: %u.", rr.sin_bucle) : "Si se repite, apunta qué estabas haciendo."));
        if (hw_modo_seguro() || p || f || w6f_toca()) ui_tras(c, 600, avisos_de_arranque);   /* y después, los demás */
        return;
    }
    if (!w6f_dicho && w6f_toca()) {                          /* (I1) la Memoria: formato de una zona en blanco, u otra versión */
        w6f_dicho = 1;
        w6f_al_arrancar();
        if (hw_modo_seguro() || p || f) ui_tras(c, 600, avisos_de_arranque);
        return;
    }
    if (hw_modo_seguro() && rr.seguro_auto)
        ui_aviso("MODO SEGURO", "El sistema ha entrado SOLO en modo seguro: los 3 últimos arranques no llegaron a terminar. No se escribe en la memoria; no se han leído los ajustes guardados (todo está de fábrica); sin apps externas, podómetro, menú 3D ni luces. "
                                "Si un ajuste guardado colgaba el arranque: Sistema, Opciones, «Borrar ajustes guardados»; si era un archivo, «Vaciar Mis archivos». El próximo encendido vuelve a intentarlo normal.");
    else if (hw_modo_seguro())
        ui_aviso("MODO SEGURO", "Has encendido con la tecla C pulsada. No se escribe en la memoria; no se han leído los ajustes guardados (todo está de fábrica); sin apps externas, podómetro, menú 3D ni luces; la pantalla no se apaga sola. "
                                "Si un ajuste guardado colgaba el arranque: Sistema, Opciones, «Borrar ajustes guardados». Para salir, apaga y enciende sin la tecla.");
    else if (p & (HW_PROT_CHIP | HW_PROT_CFI))
        ui_aviso("Memoria protegida", ui_fmt("El chip de memoria NO es el que este sistema espera (id %04X %04X; se espera %s)%s. "
                                             "Por seguridad el sistema funciona SIN ESCRIBIR: lo que haya se lee, pero no se guarda nada y no se da formato a nada.",
                                             h.nor_id >> 16, h.nor_id & 0xFFFF, placa()->flash_esperada, p & HW_PROT_CFI ? ": su tabla CFI no cuadra" : ""));
    else if (f & hw_formato_propio()) {                       /* (AUD2 B4) NUESTRO y dañado: no son «restos del firmware original» */
        int pr = f & hw_formato_propio();
        const char *t = ui_fmt("%s lleva datos DE ESTE SISTEMA que no se pueden leer (dañados o a medio formatear)%s. "
                     "Darle formato BORRA lo que hubiera%s y no se puede deshacer. ¿Borrar y dar formato? Con «No» no se toca y el sistema sigue sin guardar ahí.",
                     pr == HW_ZONA_AJUSTES ? "La zona de ajustes (0x45B00000)" : pr == HW_ZONA_USUARIO ? "La zona de Mis archivos (0x45C00000)" : "Las zonas de ajustes y de Mis archivos",
                     f & ~pr ? ui_fmt("; y %s tiene datos que NO son de este sistema", zonas_txt(f & ~pr)) : "",
                     pr & HW_ZONA_USUARIO ? " (tus ficheros: notas, capturas, música copiada)" : " (tema, alarma, fondo…)");
        hw_log("formato: se pregunta por una zona NUESTRA que no monta\n");
        ui_confirmar_no("Memoria dañada", t, formato_si, NULL);
    }
    else if (f && (hw_log("formato: se pregunta por zonas que NO son de este sistema\n"), 1))
        ui_confirmar_no("Memoria sin formato", f == HW_ZONA_AJUSTES || f == HW_ZONA_USUARIO || f == 3
            ? ui_fmt("%s (%s) contiene datos que NO son de este sistema: en un teléfono, restos del firmware original. "
                     "¿Borrarla y darle formato? Con «No» no se toca y el sistema sigue sin guardar nada.",
                     f == 3 ? "La zona de ajustes y de Mis archivos" : f == 1 ? "La zona de ajustes" : "La zona de Mis archivos",
                     f == 3 ? zona_txt(ZONA_AJUSTES, ZONA_USUARIO) : f == 1 ? zona_txt(ZONA_AJUSTES, ZONA_AJUSTES) : zona_txt(ZONA_USUARIO, ZONA_USUARIO))
            : ui_fmt("Estas zonas contienen datos que NO son de este sistema (en un teléfono, restos del firmware original): %s. "   /* (I1) con la Memoria */
                     "¿Borrarlas y darles formato (la Memoria son %u bloques: tarda)? Con «No» no se tocan y el sistema sigue sin guardar ahí.", zonas_txt(f), placa()->zonas[ZONA_MEMORIA].tam / placa()->zonas[ZONA_MEMORIA].bloque_logico), formato_si, NULL);
}
static void standby_iniciar(Ctx *c)
{
    StandbyE *s = ui_estado(c);
    s->valida = sh_ahora(&s->f) == HW_OK;
    ui_cada(c, 1000, standby_tic);
    { HwReinicio rr; hw_reinicio(&rr);
      if (hw_modo_seguro() || hw_nor_proteccion() || hw_formato_pendiente() || rr.causa >= HW_RST_PERRO || w6f_toca()) ui_tras(c, 600, avisos_de_arranque); }
    /* (R2; A4) la carga sin confirmar la dice la vigilancia en su primer sondeo (shell/energia.c), con rótulo fijo arriba */
}
static int standby_tecla(Ctx *c, int k, void *dato)
{
    (void)c; (void)dato;
    if (k == K_SEL_IZQ || k == K_OK) { ui_empujar(sh_menu()); return 1; }
    if (k == K_SEL_DER) { sh_abrir_sen("/apps/reloj.sen", NULL); return 1; }   /* (APX) Reloj es un .sen */
    return 0;
}
static Widget *standby_construir(Ctx *c)
{
    StandbyE *s = ui_estado(c);
    const TemaDatos *t = sh_tema();
    const Imagen *fondo = sh_fondo();
    const Color blanco = RGB(255, 255, 255), negro = RGB(0, 0, 0), gris = RGB(210, 210, 210);
    int h = aj.hora12 ? (s->f.hora % 12 ? s->f.hora % 12 : 12) : s->f.hora;
    Widget *delante;
    /* (6-nov) con el ESCRITORIO (shell/menu_escritorio.c), él ES la pantalla de inicio: ni la hora grande ni la tecla «Menú» (el mantenedor:
     * «cuando está el menú tipo escritorio ya no hay pantalla de inicio con la hora ni el botón de menú: solo el escritorio»). La hora va en la
     * bandeja de su barra. El standby sigue siendo la ruta 0 con su vigilante (alarma, cuenta atrás, tarjeta): solo cambia lo que se ve. */
    if (aj.menu_estilo == M3_ESCRITORIO) return Tema(t, sh_menu());
    delante =
        dis_con_teclas(Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(   /* (giro) la tira de las teclas, en barra o en columna */
            Velo(RellenoXY(4, 3, Fila(.espacio = 4, .hijos = HIJOS(
                Texto(sh_tapa_cerrada() ? ui_fmt("%s ·", placa()->rotulo) : placa()->rotulo, .color = gris),   /* (P600) */
                hw_modo_seguro() ? Texto("MODO SEGURO", .color = RGB(255, 64, 48)) :           /* (S7) */
                hw_nor_proteccion() ? Texto("SIN ESCRITURA", .color = RGB(255, 176, 64)) : NULL,
                /* (R2) lo que NO puede pasar inadvertido de la energía: un rótulo fijo mientras dure */
                sh_carga_sin_confirmar() ? Texto("CARGA?", .color = RGB(255, 64, 48)) :
                sh_pmu_mudo() ? Texto("PMU MUDO", .color = RGB(255, 64, 48)) :
                sh_bateria_dudosa() ? Texto("BAT. DUDOSA", .color = RGB(255, 64, 48)) :
                sh_bateria_sin_calibrar() ? Texto("BAT. SIN CALIBRAR", .color = RGB(255, 176, 64)) : NULL,   /* (P600) */
                Hueco(),
                sh_bateria(),
                hw_serie_conectado() ? Icon(&sh_iconos[IC_ENCHUFE], .color = gris) : NULL,
                aj.alarma_on ? Icon(&sh_iconos[IC_ALARMA], .color = blanco) : NULL,
                aj.alarma_on ? Texto(ui_fmt("%02d:%02d", aj.alarma_h, aj.alarma_m), .color = gris) : NULL)))),
            Hueco(),
            /* (MD) el bloque del reloj lo dibuja el diseño en uso */
            dis_actual()->espera_reloj(ui_fmt(aj.hora12 ? "%d:%02d" : "%02d:%02d", h, s->f.min), ui_fmt("%02d", s->f.seg),
                s->valida ? ui_fmt("%s %d %s %d%s", sh_dia[hw_dia_semana(s->f.anio, s->f.mes, s->f.dia)], s->f.dia,
                                   sh_mes[(s->f.mes + 11) % 12], s->f.anio, aj.hora12 ? (s->f.hora < 12 ? "  am" : "  pm") : "")
                          : "reloj sin poner en hora"),
            Hueco(),
            /* el contador de pasos, abajo a la izquierda como en el original (se refresca con el segundero) */
            aj.pasos ? Alinear(-1, 0, Velo(RellenoXY(4, 2, Fila(.espacio = 4, .cruzado = P_CENTRO, .hijos = HIJOS(
                Icon(&sh_iconos[IC_PASOS], .color = RGB(255, 176, 64)),
                Texto(ui_fmt("%u", sh_pasos()->pasos), .color = blanco, .sombra = negro)))))) : NULL)),
            dis_actual()->espera_teclas("Menú", "Reloj"));
    if (sh_fondo_medios()) {                                  /* (28-oct) el fondo de Medios, animado, detrás de todo (shell/menu_medios.c) */
        if (!s->md_activo && standby_md_anima(c)) { s->md_activo = 1; s->md_ult = hw_ms(); ui_cada(c, 33, standby_md_tic); }
        return Teclas(standby_tecla, Tema(t, sh_medios_fondo(delante, s->md_t)));
    }
    return Teclas(standby_tecla, Tema(t, Caja(.an = INF, .al = INF, .color = fondo ? negro : dis_actual()->espera_fondo(), .hijo = Pila(
        fondo ? Centro(Image(fondo)) : NULL,
        delante))));
}
COMPONENTE_CON_ESTADO(Standby, StandbyE, standby_construir, standby_iniciar);

Widget *sistema_raiz(void)
{
    MIGA("raiz: iconos");
    sh_iconos_init();
    MIGA("raiz: apps (W6FS)");
    sh_apps_init();                               /* las de casa y las externas que haya enlazadas */
    { HwTarjeta t; hw_tarjeta_estado(&t);         /* (I1) lo de la tarjeta al arrancar ya lo ha visto sh_apps_init; pero una que NO SIRVE */
      tj_visto = hw_tarjeta_cambios() - (t.estado == HW_TJ_MALA || t.estado == HW_TJ_SIN_FAT ? 1u : 0u); }   /* se dice también si ya estaba al encender */
    MIGA("raiz: tarjeta ok, franja");
    sh_flash_progreso_init();                     /* (R3) la franja «Guardando…» de las operaciones largas de flash */
    sh_dictado_init();                            /* (E5) el dictado por voz en los campos de texto (pulsación larga de «*») */
    if (hw_modo_seguro()) {
        /* (S7) MODO SEGURO: NO se leen los ajustes ni se decodifica NINGÚN fondo (un ajuste o un fondo que cuelgue el arranque se
         * repetiría en cada encendido: auditoría C1). Todo de fábrica y lo más simple: menú clásico, sin podómetro, sin luces
         * laterales ni efectos, sin atenuar ni dormir el panel (ni reposo profundo), sin bloqueo ni deslizador. */
        aj.pasos = 0; aj.menu_estilo = M3_CLASICO; aj.menu_efecto = SH_EFECTO_QUIETO; aj.luces = 0; aj.luces_latido = 0; aj.giro = 0;   /* (estética) de pie */
        aj.atenuar_s = 0; aj.apagar_s = 0; aj.bloqueo_auto = 0; aj.tapa_apaga = 0; aj.alarma_on = 0; aj.fondo[0] = 0;
        tema_aplicar();
    } else
#ifdef PRUEBA_CUELGUE_ARRANQUE                    /* (S7) PRUEBA: «un ajuste guardado que cuelga el arranque» — el arranque NORMAL se cuelga aquí,
                                                   * donde se leen los ajustes y se decodifica el fondo. El perro tiene que sacarlo a la pantalla de
                                                   * fallo, y el modo seguro (que no pasa por aquí) tiene que arrancar limpio. */
    if (hw_log("PRUEBA_CUELGUE_ARRANQUE: el arranque normal se cuelga al cargar los ajustes\n"), 1) { for (;;) { } } else
#endif
    if (MIGA("raiz: cargar ajustes"), cargar_ajustes()) {                       /* lo guardado manda: tema, hora, alarma y fondo (puede ser «ninguno») */
        tema_aplicar();                /* el brillo lo pone sh_energia_init(), más abajo */
        if (aj.fondo[0] && hw_fs_listo()) { char f[sizeof aj.fondo]; memcpy(f, aj.fondo, sizeof f); sh_poner_fondo(f); }
    } else {
        MIGA("raiz: tema");
        tema_aplicar();
        MIGA("raiz: fondo (PNG)");
        if (hw_fs_listo()) sh_poner_fondo(hw_fs_tam("/fondos/Amanecer.png") ? "/fondos/Amanecer.png" : "/tpa/user/image/GoLive.gif");   /* el nuestro si hay FS propio */
    }
    MIGA("raiz: alarma (RTC)");
    programar_alarma();
    if (aj.menu_propio) sh_apps_init();           /* (LANZ) el menú que eligió el usuario: hacía falta leer los ajustes (sin elegir, el de arriba vale) */
    MIGA("raiz: menu 3D");
    sh_menu_preparar();                           /* el fondo del menú 3D, ahora que hay pantalla de arranque que mirar */
    MIGA("raiz: luces (I2C 1)");
    sh_luces_init();                              /* (S5) el color de las laterales, antes de que sh_energia_init() encienda la luz */
    MIGA("raiz: energia");
    sh_energia_init();                            /* el filtro de teclas, el deslizador y la cuenta de inactividad */
    MIGA("raiz: podometro");
    sh_pasos_init();                              /* el podómetro en segundo plano (shell/pasos.c) */
    MIGA("raiz: fin");
    return UI_W(Standby);
}
