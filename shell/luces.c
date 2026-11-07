/* luces.c — (pista S5) EL SERVICIO DE LUCES del shell: qué hacen las dos luces laterales RGB y cuándo suenan los
 * efectos. Debajo está el motor portable (api/luces.c: intérprete de «.ble» y fundidos) y, debajo, la plataforma.
 *
 * Qué hace, y de dónde sale cada cosa (docs/luces.md; docs/sistema.md §12):
 *  - EL COLOR de las laterales con la pantalla viva. El original lo saca del tema («RGB base») y lo cambia a naranja
 *    (100, 50, 0) al entrar al Walkman. Aquí: «automático» = a juego con el estilo del menú 3D (Walkman naranja, Cristal
 *    azul hielo, Neón turquesa; menú clásico = blanco, lo que hace el original sin tema), o uno fijo de la paleta
 *    (los colores de los efectos de contacto de fábrica). Con música sonando, naranja, como el Walkman del original.
 *  - EL LATIDO: al mover el foco del menú o inclinar el teléfono, las laterales bajan al 40 % durante 160 ms y vuelven
 *    (dos órdenes «RGB fijo» por terna: FUN_4529c8b4, modo 2). Como mucho uno cada 600 ms.
 *  - CON LA MÚSICA: mientras suena, «respiran»: el PWM del chip las baja (~1 s) y las sube (~1 s), cada 2,4 s. El
 *    original NO sincroniza las luces con el sonido (docs/luces.md §6); esto es nuestro, y no va a compás.
 *  - LOS PASOS: un destello verde de 200 ms cada 10 pasos del podómetro (solo con la pantalla viva).
 *  - LOS EFECTOS: los 30 «.ble» de fábrica, leídos del FS original, y tres de casa que van en memoria. Uno a la vez.
 * Todo se apaga en Ajustes -> «Luces». Y todo esto solo existe con la pantalla VIVA: atenuada o dormida, la plataforma
 * apaga las ternas (como el original) y aquí no se escribe NADA (cero tráfico por I²C, y el teléfono puede dormir).
 * [sin saber] hasta tener un aparato: que las ternas sean las laterales, sus colores, y cuánto dura cada rampa. */
#include "shell.h"
#include "luces.h"
#include "m3d.h"

const char *const sh_luz_color_nombre[SH_LUZ_COLORES] = { "automático", "blanco", "naranja", "rojo", "verde", "azul", "amarillo",
                                                          "turquesa", "rosa", "púrpura" };
/* en %, como en los ficheros de fábrica: Red, Green, Yellow, Turquoise, Pink y Purple son los suyos; el naranja es el del Walkman */
static const u8 paleta[SH_LUZ_COLORES][3] = { { 100, 100, 100 }, { 100, 100, 100 }, { 100, 50, 0 }, { 100, 0, 0 }, { 0, 100, 0 }, { 0, 0, 100 },
                                              { 100, 100, 0 }, { 0, 100, 100 }, { 100, 25, 25 }, { 80, 60, 100 } };
static const u8 por_estilo[M3_ESTILOS][3] = { [M3_CLASICO] = { 100, 100, 100 }, [M3_WALKMAN] = { 100, 50, 0 },
                                              [M3_CRISTAL] = { 30, 40, 90 }, [M3_NEON] = { 0, 100, 100 }, [M3_MEDIOS] = { 100, 12, 12 },
                                              [M3_ESCRITORIO] = { 10, 40, 100 } };   /* (6-nov) el azul del escritorio */
static u8 color[3], puesto;              /* el color en uso (%) y si ya se le ha dicho a la plataforma */
static u8 pulso;                         /* 1 = las laterales están en un latido o un destello: hay que devolverlas a su color */
static u32 pulso_fin, pulso_ultimo;
static u8 resp;                          /* la respiración de la música: 0 = no, 1 = bajando, 2 = subiendo */
static u32 resp_t;
static u8 inclinado;
static u32 inclina_ultimo;
static u32 pasos_vistos;
static char sonando[40];                 /* el nombre del efecto en marcha ("" = ninguno) */
static u32 efecto_tope;                  /* hw_ms() en que se para solo (los de contacto no acaban nunca) */
#define LATIDO_PCT 40
#define LATIDO_MS 160
#define LATIDO_CADA_MS 600
#define INCLINA_CADA_MS 1500
#define DESTELLO_MS 200
#define PASOS_POR_DESTELLO 10
#define RESP_MEDIA_MS 1200               /* media vuelta de la respiración: la rampa del chip (velocidad 3, ~950 ms [inferido]) y un respiro */
#define EFECTO_TOPE_MS 20000

static u32 a_reg(u32 pct) { return pct * 255 / 100; }
static int viva(void) { return sh_pantalla_estado() == EN_VIVA; }
static void color_que_toca(u8 c[3])
{
    const u8 *o;
    if (!aj.luces) { c[0] = c[1] = c[2] = 0; return; }
    if (aj.luces_latido && sh_musica_estado() == MU_SONANDO) o = paleta[2];           /* el naranja del Walkman */
    else if (aj.luces_color && aj.luces_color < SH_LUZ_COLORES) o = paleta[aj.luces_color];
    else o = por_estilo[aj.menu_estilo < M3_ESTILOS ? aj.menu_estilo : M3_CLASICO];
    c[0] = o[0]; c[1] = o[1]; c[2] = o[2];
}
void sh_luces_color(u8 c[3]) { color_que_toca(c); }
static void las_dos_fijas(const u8 c[3], u32 pct)
{
    u32 t;
    for (t = 0; t < 2; t++) lu_rgb_poner(t, c[0] * pct / 100 ? c[0] * pct / 100 : !!c[0], c[1] * pct / 100 ? c[1] * pct / 100 : !!c[1],
                                         c[2] * pct / 100 ? c[2] * pct / 100 : !!c[2], 2);
}
static int se_puede_pulsar(void)
{
    return aj.luces && aj.luces_latido && viva() && !lu_efecto_activo() && !pulso && (color[0] || color[1] || color[2]);
}
void sh_luces_latido(void)
{
    u32 t = hw_ms();
    if (!se_puede_pulsar() || t - pulso_ultimo < LATIDO_CADA_MS) return;
    las_dos_fijas(color, LATIDO_PCT);
    pulso = 1; pulso_fin = t + LATIDO_MS;
}
/* La inclinación que pinta el menú 3D (1/4096 de vuelta; tope SH_INCLINA_MAX): un latido al pasar de medio tope,
 * y no otro hasta que vuelva a estar casi plano (y como mucho uno cada 1,5 s). */
void sh_luces_inclina(int x, int y)
{
    int m = (x < 0 ? -x : x) + (y < 0 ? -y : y);
    if (!inclinado && m > SH_INCLINA_MAX / 2) {
        inclinado = 1;
        if (hw_ms() - inclina_ultimo >= INCLINA_CADA_MS) { inclina_ultimo = hw_ms(); sh_luces_latido(); }   /* andando con el menú abierto no late a cada paso */
    } else if (inclinado && m < SH_INCLINA_MAX / 5) inclinado = 0;
}
static void destello(const u8 c[3])
{
    if (!se_puede_pulsar()) return;
    las_dos_fijas(c, 100);
    pulso = 1; pulso_fin = hw_ms() + DESTELLO_MS;
}

/* ---- los efectos ---------------------------------------------------------------------------------------- */
#define F(rgb, r, g, b, ms, subir) BLE_RGB_FUNDIR, rgb, r, g, b, ms, subir
#define DOS(r, g, b, ms, subir) F(1, r, g, b, ms, subir), F(2, r, g, b, ms, subir)
/* «Respirar»: las laterales bajan y suben, despacio, sin fin (con el color en uso: se toca como efecto «de sistema»). */
static const u32 ef_respirar[] = { DOS(100, 100, 100, 800, 0), BLE_ESPERAR, 1000, DOS(100, 100, 100, 600, 1), BLE_ESPERAR, 1200, BLE_SALTAR, 0, 0 };
/* «Arcoíris»: seis colores, cada uno sube, aguanta y baja; y una ola por los teclados entre color y color. */
#define COLOR(r, g, b) DOS(r, g, b, 400, 1), BLE_ESPERAR, 700, DOS(r, g, b, 400, 0), BLE_ESPERAR, 500
static const u32 ef_arcoiris[] = {
    COLOR(100, 0, 0), COLOR(100, 55, 0), COLOR(100, 100, 0),
    BLE_FUNDIR, 3, 100, 280, BLE_ESPERAR, 120, BLE_FUNDIR, 2, 100, 280, BLE_ESPERAR, 120, BLE_SI_ABIERTO, 1, BLE_FUNDIR, 1, 100, 280, BLE_FIN_SI, 1,
    COLOR(0, 100, 0), COLOR(0, 100, 100), COLOR(0, 0, 100), COLOR(80, 60, 100),
    BLE_SI_ABIERTO, 1, BLE_FUNDIR, 1, 36, 360, BLE_FIN_SI, 1, BLE_ESPERAR, 120, BLE_FUNDIR, 2, 36, 360, BLE_ESPERAR, 120, BLE_FUNDIR, 3, 36, 400,
    BLE_ESPERAR, 400, BLE_FIN };
/* «Baliza»: dos destellos secos y una pausa, cinco veces; con «RGB fijo» y «RGB apagar», las dos órdenes que la fábrica no usa. */
#define DESTELLA(r, g, b) BLE_RGB_PONER, 1, r, g, b, 2, BLE_RGB_PONER, 2, r, g, b, 2, BLE_ESPERAR, 120, BLE_RGB_APAGAR, 1, BLE_RGB_APAGAR, 2
static const u32 ef_baliza[] = { DESTELLA(100, 50, 0), BLE_ESPERAR, 120, DESTELLA(100, 50, 0), BLE_ESPERAR, 640, BLE_SALTAR, 0, 4, BLE_FIN };
/* «Aviso»: lo que suena con la alarma, la cuenta atrás y la batería si el FS de fábrica no está: tres respiraciones. */
static const u32 ef_aviso[] = { DOS(100, 100, 100, 200, 1), BLE_ESPERAR, 240, DOS(100, 100, 100, 560, 0), BLE_ESPERAR, 600, BLE_SALTAR, 0, 2, BLE_FIN };
static const struct { const char *nombre; const u32 *p; u16 n; u8 base; } casa[SH_LUZ_CASA] = {
    { "Respirar", ef_respirar, sizeof ef_respirar / 4, 1 }, { "Arcoíris", ef_arcoiris, sizeof ef_arcoiris / 4, 0 },
    { "Baliza", ef_baliza, sizeof ef_baliza / 4, 0 }, { "Aviso", ef_aviso, sizeof ef_aviso / 4, 1 } };

static void apunta(const char *nombre)
{
    u32 i;
    for (i = 0; nombre[i] && i + 1 < sizeof sonando; i++) sonando[i] = nombre[i];
    sonando[i] = 0;
    if (i > 4 && !memcmp(sonando + i - 4, ".ble", 4)) sonando[i - 4] = 0;
    efecto_tope = hw_ms() + EFECTO_TOPE_MS;
    pulso = 0; resp = 0;                 /* el efecto se queda las luces: lo que hubiera a medias se olvida */
}
const char *sh_luces_casa_nombre(int i) { return i >= 0 && i < SH_LUZ_CASA ? casa[i].nombre : ""; }
int sh_luces_casa(int i)
{
    int r;
    if (i < 0 || i >= SH_LUZ_CASA) return HW_ERROR;
    if (!viva()) return HW_NO_DISPONIBLE;
    r = lu_efecto_memoria(casa[i].p, casa[i].n, 100, casa[i].base ? LU_EF_BASE : LU_EF_PROPIO);
    if (r == HW_OK) apunta(casa[i].nombre);
    return r;
}
/* Dónde están los efectos de fábrica: en «/original/tpa/…» si hay FS nuestro, en «/tpa/…» si el de fábrica es la raíz. */
const char *sh_luces_dir(void)
{
    static const char *const d[2] = { "/original/tpa/preset/system/lighteffect", "/tpa/preset/system/lighteffect" };
    static signed char cual = -1;
    if (cual < 0) {
        if (!hw_fs_listo()) return 0;
        cual = hw_fs_contar(d[0]) > 0 ? 0 : hw_fs_contar(d[1]) > 0 ? 1 : 2;
    }
    return cual < 2 ? d[cual] : 0;
}
/* «contacteffects/Disco.ble», «systemeffects/alarm.ble»… Como el original: con el deslizador CERRADO, de los de sistema
 * se toca la variante «_closed» si existe; los de contacto y mt_call llevan su propio color y los demás, el color en uso. */
int sh_luces_fabrica(const char *relativo)
{
    const char *d = sh_luces_dir(), *nombre = relativo;
    static char ruta[128];
    u32 n = 0, k = 0, i; int r, sistema;
    if (!d || !viva()) return HW_NO_DISPONIBLE;
    while (d[k] && n + 1 < sizeof ruta) ruta[n++] = d[k++];
    if (n + 1 < sizeof ruta) ruta[n++] = '/';
    for (i = 0; relativo[i]; i++) { if (n + 12 >= sizeof ruta) return HW_ERROR; ruta[n++] = relativo[i]; if (relativo[i] == '/') nombre = relativo + i + 1; }
    ruta[n] = 0;
    sistema = relativo[0] == 's';
    if (sistema && sh_tapa_cerrada() && n > 4 && !memcmp(ruta + n - 4, ".ble", 4) && !(n > 11 && !memcmp(ruta + n - 11, "_closed", 7))) {
        memcpy(ruta + n - 4, "_closed.ble", 12);                 /* la variante para el deslizador cerrado, si existe */
        if (!hw_fs_tam(ruta)) memcpy(ruta + n - 4, ".ble", 5);
    }
    for (i = 0; i < 7 && nombre[i] == "mt_call"[i]; i++) ;       /* mt_call (la llamada) lleva su color, como los de contacto */
    r = lu_efecto_fichero(ruta, 100, sistema && i < 7 ? LU_EF_BASE : LU_EF_PROPIO);
    if (r == HW_OK) apunta(nombre);
    return r;
}
void sh_luces_parar(void) { lu_efecto_parar(); sonando[0] = 0; }
/* Quien se va a quedar con el bucle (una app externa a pantalla completa) deja las luces QUIETAS y en su color: aquí ya
 * no va a pasar nadie a acabar un latido, una respiración o un efecto a medias. */
void sh_luces_asentar(void)
{
    u32 k;
    if (lu_efecto_activo()) sh_luces_parar();
    lu_paso();                                           /* (la plataforma vuelve a montar su fase) */
    if (viva() && (color[0] || color[1] || color[2])) {
        if (pulso) las_dos_fijas(color, 100);
        else if (resp == 1) for (k = 0; k < 2; k++) lu_rgb_fundir(k, color[0], color[1], color[2], 2, 0);
    }
    pulso = 0; resp = 0;
}
const char *sh_luces_sonando(void) { return lu_efecto_activo() ? sonando : ""; }
/* Un suceso del sistema con su efecto, como el original (docs/luces.md §6.3): el de fábrica si está; si no, el de casa. */
void sh_luces_evento(int cual)
{
    static const char *const f[SH_LUZ_EVENTOS] = { "systemeffects/alarm.ble", "systemeffects/reminder.ble", "systemeffects/system.ble" };
    if (!aj.luces || !aj.luces_latido || cual < 0 || cual >= SH_LUZ_EVENTOS || !viva()) return;
    if (sh_luces_fabrica(f[cual]) != HW_OK) sh_luces_casa(SH_LUZ_CASA - 1);
}

/* ---- una vez por vuelta del bucle ----------------------------------------------------------------------- */
void sh_luces_init(void)
{
    color_que_toca(color);
    hw_luz_color((int)a_reg(color[0]), (int)a_reg(color[1]), (int)a_reg(color[2]));   /* ANTES de que la fase encienda nada */
    lu_color_base(color[0], color[1], color[2]);
    lu_rgb_permitir(aj.luces);
    puesto = 1;
    pasos_vistos = sh_pasos()->pasos;
}
void sh_luces_paso(void)
{
    u8 c[3]; u32 t;
    lu_paso();                                           /* los fundidos y la orden siguiente del efecto */
    color_que_toca(c);
    if (!puesto || c[0] != color[0] || c[1] != color[1] || c[2] != color[2]) {
        color[0] = c[0]; color[1] = c[1]; color[2] = c[2]; puesto = 1;
        pulso = 0; resp = 0;
        hw_luz_color((int)a_reg(c[0]), (int)a_reg(c[1]), (int)a_reg(c[2]));       /* con la pantalla viva las funde ya; si no, al despertar */
        lu_color_base(c[0], c[1], c[2]);
        lu_rgb_permitir(aj.luces);
    }
    if (!viva()) { pulso = 0; resp = 0; pasos_vistos = sh_pasos()->pasos; return; }   /* atenuada o dormida: aquí no se escribe nada */
    t = hw_ms();
    if (lu_efecto_activo()) {
        if ((int)(t - efecto_tope) >= 0) sh_luces_parar();                        /* los que no acaban solos */
        return;
    }
    if (pulso && (int)(t - pulso_fin) >= 0) {                                     /* el latido o el destello se acaba: a su color */
        if (color[0] || color[1] || color[2]) las_dos_fijas(color, 100);
        pulso = 0; pulso_ultimo = t;
    }
    if (aj.luces && aj.luces_latido && (color[0] || color[1] || color[2])) {
        const Podometro *p = sh_pasos();
        if (p->pasos < pasos_vistos) pasos_vistos = p->pasos;                     /* lo pusieron a cero */
        if (p->pasos - pasos_vistos >= PASOS_POR_DESTELLO) {
            static const u8 verde[3] = { 0, 100, 0 };
            pasos_vistos = p->pasos - (p->pasos - pasos_vistos) % PASOS_POR_DESTELLO;
            destello(verde);
        }
        if (sh_musica_estado() == MU_SONANDO && !pulso) {                         /* la respiración: el PWM del chip baja y sube las dos */
            if (!resp || (int)(t - resp_t) >= 0) {
                u32 k;
                resp = (u8)(resp == 1 ? 2 : 1);
                for (k = 0; k < 2; k++) lu_rgb_fundir(k, color[0], color[1], color[2], 3, resp == 1);
                resp_t = t + RESP_MEDIA_MS;
            }
        } else if (resp && !pulso) {                                              /* la música paró: arriba y quietas */
            if (resp == 1) { u32 k; for (k = 0; k < 2; k++) lu_rgb_fundir(k, color[0], color[1], color[2], 2, 0); }
            resp = 0;
        }
    } else { resp = 0; pasos_vistos = sh_pasos()->pasos; }
}
void sh_luces_ajuste(int luces, int col, int latido)
{
    aj.luces = (u8)!!luces; aj.luces_color = (u8)(col < 0 ? SH_LUZ_COLORES - 1 : col % SH_LUZ_COLORES); aj.luces_latido = (u8)!!latido;
    if (!aj.luces && lu_efecto_activo()) sh_luces_parar();
    sh_guardar_ajustes();
}
