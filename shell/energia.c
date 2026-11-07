/* Energía, bloqueo del teclado, tapa giratoria, tecla de encendido y las pantallas de arranque y de
 * apagado. Pista S3.
 *
 * Todo lo de aquí es PORTABLE (va por la API hw_*), así que la máquina de estados se prueba en el PC con
 * guiones y capturas; lo único que solo existe en el teléfono es el reposo profundo de verdad (hw_dormir),
 * que se prueba en el emulador.
 *
 * Por qué la máquina de estados vive en el shell y no en la plataforma: quién decide cuándo atenuar, cuándo
 * apagar y cuándo bloquear es una decisión de PRODUCTO (y es ajustable), no de hardware; la plataforma solo
 * sabe encender la luz, dormir el panel y dormir la CPU.
 */
#include "shell.h"
#include "placa.h"
#include "usb.h"                                  /* (CARGA) usb_carga_ma: lo que el PC concede para cargar */
#include "carga.h"
#include "camara_sis.h"                           /* (1.9) sh_cam_activa: con la cámara abierta no se duerme a fondo */
#include "micro_sis.h"                            /* (1.10) sh_mic_activo: el tic recoge las muestras */

static u8 estado = EN_VIVA, bloqueado, tapa_cerrada, arrancado;
static u32 ultima;                       /* hw_ms() de la última actividad */
static u32 ast_ms;                       /* hw_ms() en que se pulsó «*» (combinación de bloqueo) */
static u32 tapa_vistas;                  /* hw_tapa_cambios() que ya hemos tenido en cuenta */
static u32 cta_apagadas, cta_bloqueos;
static int rutas_al_bloquear;            /* cuántas rutas había con el candado puesto (ver el filtro) */
static u32 insistir_hasta;               /* hasta cuándo NO se puede apagar la pantalla (una alarma sonando) */
/* (R2; auditoría del sistema, A4) Antes era UN booleano con dos dueños (las apps externas y la app Movimiento): el que soltaba
 * primero se lo quitaba al otro, y Movimiento abierta dejaba la pantalla encendida sin límite. Ahora son dos bits, y el de las
 * pantallas DE CASA (SUSP_CASA) caduca: a los SUSP_TOPE_MS sin teclas la energía vuelve a mandar aunque la pantalla siga abierta. */
enum { SUSP_EXTERNA = 1, SUSP_CASA = 2 };
#define SUSP_TOPE_MS 600000u
static u8 suspendida;                    /* bits SUSP_*: una app a pantalla completa manda: la energía no toca nada */
static u8 falso;                         /* (S7) APAGADO EN FALSO: se pidió apagar y el teléfono sigue con corriente (el cargador puesto) */

int sh_pantalla_estado(void) { return estado; }
int sh_bloqueado(void) { return bloqueado; }
int sh_tapa_cerrada(void) { return tapa_cerrada; }

/* ---- la luz ---------------------------------------------------------------------------------------------
 * (Pista S4.) El shell dice la FASE (viva, atenuada, panel dormido) y el brillo que el usuario eligió en Ajustes;
 * qué se enciende en cada una, a qué nivel y con qué rampa lo sabe la plataforma (en el W580i, lo que hace el
 * firmware original: plat/hw_w580.c). Se llama cada vez que hay actividad con la pantalla atenuada o dormida (y con
 * la tecla de encendido): la plataforma no hace nada si la fase no cambia. */
static void luz_segun_estado(void)
{
    hw_luz(0, aj.brillo < 10 ? 10 : aj.brillo);
    hw_luz_fase(estado == EN_VIVA ? HW_LUZ_VIVA : estado == EN_ATENUADA ? HW_LUZ_ATENUADA : falso ? HW_LUZ_APAGADA : HW_LUZ_DORMIDA);
}

int sh_pantalla_apagada(void) { return estado == EN_APAGADA; }   /* (R3) para quien pinta fuera del árbol (la franja «Guardando…») */
static void apagar_pantalla(void)
{
    if (estado == EN_APAGADA) return;
    estado = EN_APAGADA;
    cta_apagadas++;
    luz_segun_estado();
    hw_pantalla_dormir(1);               /* el guion power_off del panel: deja de refrescar */
    ui_pantalla_activa(0);               /* la UI sigue al día (reloj, alarma) pero ni pinta ni vuelca */
    if (aj.bloqueo_auto) sh_bloquear(1);
}

/* «Insiste»: una alarma o una cuenta atrás que nadie ha atendido tiene que quedarse encendida hasta que
 * alguien la vea; si no, suena con la pantalla a oscuras y no sirve de nada. Con un tope: una alerta
 * olvidada no puede dejar la retroiluminación encendida para siempre (es una batería de teléfono). */
void sh_insistir(u32 ms)
{
    sh_actividad();
    insistir_hasta = hw_ms() + (ms > 120000 ? 120000 : ms);
}
u32 sh_inactivo_ms(void) { return hw_ms() - ultima; }   /* (I1) para la recolección de la Memoria: solo con el usuario quieto */
void sh_actividad(void)
{
    ultima = hw_ms();
    if (estado == EN_APAGADA) { hw_pantalla_dormir(0); ui_pantalla_activa(1); ui_reconstruir_todo(); }   /* ~0,4 s de guion power_on; reconstruir: quien
                                                                                   * dejó de animar a oscuras (el menú 3D) se entera de que se le ve */
    estado = EN_VIVA;
    luz_segun_estado();
}

/* ---- la ruta del candado --------------------------------------------------------------------------------
 * Es una ruta normal empujada encima de lo que hubiera: así lo de debajo sigue vivo (y la alarma sigue
 * avisando) y al desbloquear se vuelve exactamente a donde se estaba. Las teclas no llegan aquí: se las
 * queda el filtro; esta pantalla solo ENSEÑA cómo salir. */
typedef struct { Widget base; } Candado;
typedef struct { HwFecha f; u8 insiste; } CandadoE;
extern const Clase ClsCandado;

static Ctx *candado_ctx;                 /* para poder insistir cuando se pulsa una tecla cualquiera */
static void cnd_tic(Ctx *c) { CandadoE *s = ui_estado(c); sh_ahora(&s->f); ui_sucio(c); }
static void cnd_iniciar(Ctx *c)
{
    CandadoE *s = ui_estado(c);
    candado_ctx = c;
    sh_ahora(&s->f);
    ui_cada(c, 1000, cnd_tic);
}
static void cnd_calla(Ctx *c) { ((CandadoE *)ui_estado(c))->insiste = 0; ui_sucio(c); }
static Widget *cnd_construir(Ctx *c)
{
    CandadoE *s = ui_estado(c);
    const TemaDatos *t = sh_tema();
    return Pantalla("Bloqueado", Columna(.principal = P_CENTRO, .cruzado = P_CENTRO, .espacio = 10, .hijos = HIJOS(
            Icon(&sh_iconos[IC_CANDADO], .escala = 4, .color = t->acento),
            Texto(sh_hora(&s->f, 0), .escala = 2, .color = t->texto),
            Texto(ui_fmt("%s %d %s", sh_dia[hw_dia_semana(s->f.anio, s->f.mes, s->f.dia)], s->f.dia, sh_mes[(s->f.mes + 11) % 12]),
                  .color = t->tenue),
            Relleno(6, Parrafo(s->insiste ? "El teclado está bloqueado.\nPulsa * y después Desbloq." : "Pulsa * y Desbloq.",
                               .color = s->insiste ? t->acento : t->tenue, .centrado = 1)))),
        "Desbloq.", "");
}
COMPONENTE_CON_ESTADO(Candado, CandadoE, cnd_construir, cnd_iniciar);

void sh_bloquear(int si)
{
    if (!!si == bloqueado) return;
    bloqueado = (u8)!!si;
    sh_sonar(si ? SN_BLOQUEO : SN_DESBLOQUEO);
    if (si) { cta_bloqueos++; ui_empujar(UI_W(Candado)); rutas_al_bloquear = ui_rutas() + 1; }
    else { ui_sacar(); candado_ctx = NULL; }
}

/* ---- el menú de la tecla de encendido ------------------------------------------------------------------- */
static void enc_elegir(int i, void *dato)
{
    (void)dato;
    if (i == 0) sh_apagar();
    else if (i == 1) sh_bloquear(1);
    else if (i == 2) sh_reiniciar();
    else if (i == 3) apagar_pantalla();
}
static void menu_encendido(void)
{
    static const char *const ops[4] = { "Apagar el teléfono", "Bloquear el teclado", "Reiniciar", "Apagar la pantalla" };
    ui_opciones("Encendido", ops, 4, enc_elegir, NULL);
}

/* ---- el filtro de teclas: lo ve TODO antes que el foco --------------------------------------------------- */
static int filtro(int k)
{
    u32 t = hw_ms();
    int estaba_apagada = estado == EN_APAGADA;
    /* (S7; auditoría C1) Con la energía SUSPENDIDA (una app a pantalla completa) el filtro salía aquí ANTES de mirar la tecla de
     * encendido: ni menú de encendido ni apagado. Ahora la tecla de encendido manda también entonces (el menú se abre encima);
     * las demás teclas siguen siendo de la app. (Una app EXTERNA lee las teclas en crudo y no pasa por aquí: para ella están
     * la pulsación larga —sh_encendido_largo, desde app_latido— y, si ni late, la plataforma: perro y apagado forzado.) */
    if (suspendida && k != K_ENCENDIDO) {                 /* la app ve todas las demás teclas, en crudo */
        if (estado != EN_VIVA) { sh_actividad(); return estaba_apagada; }   /* (R2) retenida y caducada: la primera tecla solo enciende */
        ultima = t;                                       /* (R2) y cuentan como actividad: el tope de SUSP_CASA es «sin teclas» */
        return 0;
    }

    /* Lo primero: una alerta que está pidiendo atención se CALLA con cualquier tecla (es lo que uno hace
     * con un despertador). La tecla sigue su camino: aquí solo se apaga el sonido. */
    if (sh_sonando() >= SN_BATERIA) sh_callar();
    else if (aj.tono_teclas && !(tapa_cerrada && aj.tapa_apaga)) sh_sonar(SN_TECLA);   /* el clic */
    /* …y deja de «insistir»: quien pulsa una tecla ya ha visto el aviso. Sin esto la pantalla se quedaba
     * encendida el minuto entero que pidió la alerta aunque se hubiera aceptado al segundo [observado en el
     * emulador: 40 s después de aceptar una cuenta atrás el standby seguía encendido]. */
    insistir_hasta = 0;

    if (falso) {                                          /* (S7) «apagado» con el cargador puesto: cualquier tecla enciende la pantalla
                                                           * unos segundos y repite el aviso; nada más responde (el teléfono está apagado) */
        sh_actividad();                                   /* el aviso «cargador conectado… no puede apagarse» ya está encima (apagar_con) */
        if (estaba_apagada) sh_sonar(SN_BATERIA);
        return 1;
    }
    if (k == K_ENCENDIDO) {                               /* la tecla de encendido manda siempre */
        sh_actividad();
        menu_encendido();
        return 1;
    }
    if (tapa_cerrada && aj.tapa_apaga) { ultima = t; return 1; }   /* tapa cerrada: nada responde (como el original) */

    ultima = t;
    if (estado != EN_VIVA) {
        sh_actividad();
        if (estaba_apagada) return 1;                     /* la primera tecla solo enciende la pantalla */
    }
    if (bloqueado) {
        /* Si hay algo ENCIMA del candado (un diálogo: la alarma, el menú de la tecla de encendido), las
         * teclas tienen que llegarle: si no, el aviso no se puede cerrar y el teléfono no se puede apagar. */
        if (ui_rutas() > rutas_al_bloquear) return 0;
        if (k == K_BLOQUEO) { sh_bloquear(0); return 1; }  /* la tecla dedicada abre y cierra, sin combinación */
        if (k == K_AST) { ast_ms = t; return 1; }
        if ((k == K_SEL_IZQ || k == K_OK) && ast_ms && t - ast_ms < 3000) { ast_ms = 0; sh_bloquear(0); return 1; }
        if (candado_ctx) { ((CandadoE *)ui_estado(candado_ctx))->insiste = 1; ui_sucio(candado_ctx); ui_tras(candado_ctx, 2500, cnd_calla); }
        return 1;                                         /* cualquier otra tecla: nada llega a las apps */
    }
    if (k == K_BLOQUEO) { sh_bloquear(1); return 1; }     /* la tecla dedicada, sin combinación */
    if (k == K_ALM && hw_tecla_abajo(K_WEB)) { sh_captura(); return 1; }   /* (S6) «Actividad» mantenida + «#»: la pantalla, a un PNG en Mis archivos */
    if (sh_musica_tecla(k)) return 1;                     /* Walkman, Play y el volumen de la música (pista S7) */
    if (k == K_AST && ui_rutas() == 1) { ast_ms = t; return 0; }   /* desde el standby «*» arma la combinación y sigue su camino */
    if (k == K_SEL_IZQ && ast_ms && t - ast_ms < 3000) { ast_ms = 0; sh_bloquear(1); return 1; }
    return 0;
}

/* Mientras una app externa tiene la pantalla, el shell no atenúa, no apaga, no bloquea y no hace caso
 * a la tapa: un juego no puede quedarse a oscuras a mitad de partida, y sus teclas son suyas. */
void sh_energia_suspender(int si)
{
    suspendida = (u8)(si ? suspendida | SUSP_EXTERNA : suspendida & ~SUSP_EXTERNA);
    if (!si) { ultima = hw_ms(); insistir_hasta = 0; }
}
void sh_energia_retener(int si)
{
    suspendida = (u8)(si ? suspendida | SUSP_CASA : suspendida & ~SUSP_CASA);
    ultima = hw_ms(); if (!si) insistir_hasta = 0;
}
static int susp_viva(void) { return (suspendida & SUSP_EXTERNA) || ((suspendida & SUSP_CASA) && hw_ms() - ultima < SUSP_TOPE_MS); }

void sh_energia_init(void)
{
    ultima = hw_ms();
    tapa_vistas = hw_tapa_cambios();
    tapa_cerrada = (u8)(hw_tapa_cerrada() == 1);
    ui_filtro(filtro);
    arrancado = 1;
    luz_segun_estado();
    sh_sonar(SN_ARRANQUE);               /* el do-mi-sol-do del arranque (es un teléfono) */
}

/* ---- LA BATERÍA Y EL CARGADOR (pista S7; docs/bateria.md §8.A, docs/sistema.md §15) ------------------------
 * ESTE SISTEMA NO CARGA LA BATERÍA: la corriente de carga del PMU está a 0 desde el arranque. Así que:
 *  - se vigila en MILIVOLTIOS, como el original: cada 10 s, la media de las dos últimas lecturas (hw_bateria_mv:
 *    el registro 0x77 del PMU por la recta de fábrica);
 *  - por debajo de 3500 mV, UN aviso («Batería baja»); se rearma si vuelve a pasar de 3600;
 *  - DOS sondeos seguidos por debajo de 3400 mV: se guarda lo pendiente y el teléfono SE APAGA (el original aguanta
 *    hasta 3250; nosotros no, porque la recta no está calibrada para ESE aparato y 3400 es el umbral por debajo del
 *    cual el propio original se niega a arrancar);
 *  - con el cargador o el USB puestos SE SIGUE VIGILANDO IGUAL (no cargamos: la batería baja de todos modos);
 *  - si el PMU no contesta (hw_bateria_mv < 0) no se avisa de nada ni se apaga: un dato inventado es peor.
 *  - una lectura por debajo de 2850 mV NO ES CREÍBLE (el original la descarta en su arranque): se avisa una vez de que
 *    no hay medida y NO se apaga por ella.
 * en_app: una app externa tiene la pantalla. El aviso SUENA y queda pendiente (sh_avisos_mostrar); el apagado
 * por batería agotada NO espera a nadie: es la batería. */
/* (R2; auditoría de S7: A1, A2, A4, M1, M3) LO QUE CAMBIA, Y EL CRITERIO:
 *  - SIN LECTURA (el PMU no contesta): una lectura fallida se IGNORA (antes borraba la cuenta de sondeos bajos, y con la
 *    lectura fallando a ratos el teléfono no se apagaba nunca); TRES seguidas se DICEN en pantalla, una vez: «el chip de
 *    energía no contesta». Antes: silencio.
 *  - CONTRASTE antes de apagar. Dos sondeos por debajo del corte solo apagan si la medida es FIABLE, y lo es si
 *      a) en marcha se la ha visto alguna vez POR ENCIMA del corte desde el arranque (ha bajado: es una descarga), o
 *      b) la lectura del arranque (0x6A, la que el original usa para decidir si arranca) era creíble y también andaba baja
 *         (< corte + 150 mV): las dos vías casan.
 *    Si no (baja y clavada desde el primer sondeo, mientras el arranque acababa de decir «bien» o no dijo nada): MEDIDA
 *    DUDOSA. Se avisa con las dos cifras, el rótulo queda en el standby, y el teléfono NO se apaga a los 20 s de cada
 *    encendido: da BAT_DUDOSA_MS (10 min) para abrir Sistema → «Energía (PMU)» y, si entonces sigue baja, apaga (conservador
 *    para la batería: una dudosa puede ser verdad). Volver por encima del corte lo cancela todo.
 *  - MODO SEGURO: no se apaga solo NUNCA (es la salida de emergencia: tiene que dejar diagnosticar). Avisa y lo dice.
 *  - Si el apagado FALLA (0x41 <- 0 no corta): antes se repetía cada 10 s, con pantalla, sonido y un aviso apilado cada vez.
 *    Ahora: un aviso, y reintentos CALLADOS al minuto y luego cada 5 min.
 *  - LA CARGA: cada sondeo relee 0x61/0x63 (hw_carga_vigilar); si no se puede confirmar que está a 0, aviso fijo. */
#define BAT_DUDOSA_MS 600000u
static u32 bat_mirada;
static u8 bat_avisado;                   /* 1 = ya se avisó de «baja» en esta bajada */
static u8 bat_bajos;                     /* sondeos seguidos con la media por debajo del corte */
static u8 bat_no_creible;                /* la lectura está por debajo de 2850 mV: no se obra por ella (ya se dijo) */
static int bat_anterior = -1;            /* mV del sondeo anterior (-1 = no hay) */
static u8 bat_sin_lectura, bat_mudo;     /* sondeos seguidos sin lectura; ya se dijo que el PMU no contesta */
static u8 bat_vista_bien;                /* algún sondeo en marcha dio una lectura creíble por encima del corte */
static u8 bat_dudosa; static u32 bat_dudosa_desde;
static u8 bat_seguro_dicho;              /* modo seguro: ya se dijo que está agotada y que no se apaga solo */
static u8 bat_fallos; static u32 bat_reintento;   /* apagados por batería que no apagaron; cuándo toca el siguiente */
static u8 carga_mal, carga_dicha;        /* la carga a 0 no se ha podido confirmar; ya se dijo */
static u8 carg_visto = 0xFF;             /* lo último que dijo hw_cargador() (0xFF = aún no se ha mirado) */
static void apagar_con(const char *titulo, const char *linea, int sonido, int callado);
int sh_bateria_dudosa(void) { return bat_dudosa; }
/* (P600; docs/portabilidad.md §2 n.º 6 y §4, riesgo 1) SIN RECTA DE BATERÍA EN LA PLACA (Placa.bat.marcha.pend_umv = 0: un port
 * nuevo, hoy el W600i) NO HAY REGLAS de batería: ni aviso, ni corte, ni «no creíble». Un umbral aplicado a una lectura sin
 * calibrar es un dato inventado. Se DICE: rótulo fijo en el standby y en Sistema. El W580i tiene sus rectas: no le cambia nada. */
int sh_bateria_sin_calibrar(void) { return placa()->bat.marcha.pend_umv == 0; }
int sh_pmu_mudo(void) { return bat_mudo; }
int sh_carga_sin_confirmar(void) { return carga_mal; }

void sh_bateria_vigilar(int en_app)
{
    int mv, m;
    u32 t = hw_ms();
    if (bat_mirada && t - bat_mirada < 10000) return;
    bat_mirada = t ? t : 1;
    carga_mal = hw_carga_vigilar() == HW_ERROR;           /* (A4) HW_NO_DISPONIBLE (PC) no es un fallo */
    if (!carga_mal) carga_dicha = 0;
    else if (!carga_dicha && !en_app) {
        HwPmuSalud p; hw_pmu_salud(&p);
        carga_dicha = 1;
        hw_log("carga: NO se ha podido confirmar la corriente de carga a 0\n");
        sh_sonar(SN_BATERIA); sh_insistir(20000);
        ui_aviso("Carga sin confirmar", ui_fmt("No he podido confirmar que la carga de la batería está como debe (registro 61 = %s, 63 = %s; 61 debe ser 10 y 63 sin corriente "
                                               "fuera de la carga USB). Desconecta el cargador y el USB. Lo sigo intentando cada 10 s.",
                                               p.r61 < 0 ? "sin leer" : ui_fmt("%02X", p.r61), p.r63 < 0 ? "sin leer" : ui_fmt("%02X", p.r63)));
        ui_reconstruir_todo();
    }
    if (sh_bateria_sin_calibrar()) {                      /* (P600) sin recta: no se obra por la lectura (ver arriba) */
        static u8 dicho;
        if (!dicho) { dicho = 1; hw_log("bateria: SIN CALIBRAR (la placa no da recta): sin aviso ni corte por tension\n"); }
        return;
    }
    mv = hw_bateria_mv();
    if (mv < 0) {                                         /* sin lectura: no se inventa nada, pero tampoco se olvida lo contado (M1) */
        if (bat_sin_lectura < 200) bat_sin_lectura++;
        if (bat_sin_lectura >= 3 && !bat_mudo && !en_app) {
            bat_mudo = 1;
            hw_log("bateria: el PMU no contesta (3 sondeos sin lectura): SIN VIGILANCIA\n");
            sh_sonar(SN_BATERIA); sh_insistir(20000);
            ui_aviso("Batería sin medida", "El chip de energía (PMU) no contesta. El sistema NO puede vigilar la batería ni ver el cargador, y puede que no consiga apagar el teléfono. "
                                           "Vigílala tú y no lo dejes encendido. Lo sigo intentando: si vuelve a contestar, desaparece el rótulo.");
            ui_reconstruir_todo();
        }
        return;
    }
    if (bat_mudo) { bat_mudo = 0; hw_log("bateria: el PMU vuelve a contestar\n"); if (!en_app) ui_reconstruir_todo(); }
    bat_sin_lectura = 0;
    if (mv < HW_BAT_MV_NO_CREIBLE) {                      /* LECTURA NO CREÍBLE (< 2850 mV: con eso el teléfono no estaría encendido). Como el original
                                                           * en su arranque: no se obra por ella —apagarse a los 20 s de cada arranque por una medida que
                                                           * no funciona dejaría el aparato sin diagnóstico—; se dice UNA vez y se ve en Sistema */
        bat_anterior = -1; bat_bajos = 0;
        if (!bat_no_creible) {
            bat_no_creible = 1;
            hw_log("bateria: lectura NO CREIBLE; no se vigila\n");
            if (!en_app) ui_aviso("Batería sin medida", ui_fmt("La lectura de la batería no es creíble (%d mV). El sistema NO puede vigilarla ni apagarse solo: vigílala tú y no lo dejes encendido.", mv));
        }
        return;
    }
    bat_no_creible = 0;
    if (mv >= HW_BAT_MV_CORTE) bat_vista_bien = 1;
    m = bat_anterior < 0 ? mv : (mv + bat_anterior) / 2;
    bat_anterior = mv;
    if (m < HW_BAT_MV_CORTE) {
        if (bat_bajos < 200) bat_bajos++;
        if (bat_bajos >= 2 && !falso) {
            HwArranque a; int fiable;
            hw_arranque(&a);
            fiable = bat_vista_bien || (a.mv >= HW_BAT_MV_NO_CREIBLE && a.mv < HW_BAT_MV_CORTE + 150);
            if (hw_modo_seguro()) {                       /* la salida de emergencia no se apaga sola */
                if (!bat_seguro_dicho && !en_app) {       /* se DICE (UART y sonido) en cuanto se sabe… */
                    bat_seguro_dicho = 1;
                    hw_log("bateria: por debajo del corte en MODO SEGURO: se avisa, NO se apaga\n");
                    sh_sonar(SN_BATERIA); sh_insistir(20000);
                }
                /* (R3) …y el DIÁLOGO, de uno en uno: si hay otro arriba (el del modo seguro, que sale al arrancar), espera al sondeo en
                 * que ya lo hayan cerrado. Antes salían los dos apilados, con dos «Aceptar» a la vista. */
                if (bat_seguro_dicho == 1 && !en_app && !ui_cima_emergente()) {
                    bat_seguro_dicho = 2;
                    ui_aviso("Batería agotada", ui_fmt("La batería marca %d mV, por debajo del corte (%d). En MODO SEGURO el teléfono no se apaga solo: apágalo tú (tecla de encendido, «Apagar») en cuanto acabes. "
                                                       "Sistema, OK: «Energía (PMU)» enseña las lecturas.", m, HW_BAT_MV_CORTE));
                }
                return;
            }
            if (!fiable) {
                if (!bat_dudosa) {
                    bat_dudosa = 1; bat_dudosa_desde = t;
                    hw_log("bateria: MEDIDA DUDOSA (baja desde el primer sondeo y no casa con la del arranque): se avisa; no se apaga hasta dentro de 10 min\n");
                    sh_sonar(SN_BATERIA); sh_insistir(20000);
                    if (!en_app) {
                        ui_aviso("Medida de batería dudosa", ui_fmt("En marcha marca %d mV (bajo el corte, %d) desde el primer momento, pero al arrancar %s. No casan: puede que una medida no funcione en este aparato. "
                                                                    "NO me apago ahora. Mira Sistema, OK: «Energía (PMU)». Si en 10 min sigue igual, me apago por prudencia.",
                                                                    m, HW_BAT_MV_CORTE, a.mv >= HW_BAT_MV_NO_CREIBLE ? ui_fmt("marcaba %d", a.mv) : "no hubo lectura creíble"));
                        ui_reconstruir_todo();
                    }
                }
                if (t - bat_dudosa_desde < BAT_DUDOSA_MS) return;
            }
            if (bat_fallos && (i32)(t - bat_reintento) < 0) return;   /* (M3) ya se intentó y no apagó: se espacia, callado */
            hw_log(bat_fallos ? "bateria: reintento CALLADO de apagar\n" : "bateria: dos sondeos por debajo del corte: guardar y APAGAR\n");
            apagar_con("Batería agotada", "Apagando. Cárgala por USB a un ordenador.", SN_BATERIA, bat_fallos != 0);
            if (bat_fallos < 200) bat_fallos++;           /* si se llega aquí, el teléfono sigue vivo */
            bat_reintento = hw_ms() + (bat_fallos == 1 ? 60000u : 300000u);
            return;
        }
    } else {
        bat_bajos = 0; bat_fallos = 0; bat_seguro_dicho = 0;
        if (bat_dudosa) { bat_dudosa = 0; hw_log("bateria: la medida vuelve a estar por encima del corte\n"); if (!en_app) ui_reconstruir_todo(); }
    }
    if (m >= HW_BAT_MV_AVISO + 100) bat_avisado = 0;      /* recuperada (la cargaron con el original y han vuelto): se rearma */
    else if (m < HW_BAT_MV_AVISO && !bat_avisado) {
        bat_avisado = 1;
        sh_sonar(SN_BATERIA);
        if (en_app) { sh_aviso_pendiente(SH_AV_BATERIA, m); return; }
        sh_insistir(20000);
        sh_luces_evento(SH_LUZ_EV_BATERIA);
        ui_aviso("Batería baja", ui_fmt("Quedan %d mV (%d %%). Para cargarla, conéctalo por USB a un ordenador. Por debajo de %d mV el teléfono se apaga solo.",
                                        m, hw_bateria_pct(), HW_BAT_MV_CORTE));
    }
}
/* EL CARGADOR (docs/bateria.md §8.A5) y (CARGA, 25-oct-2026) LA CARGA POR USB (api/carga.h, docs/bateria.md §12). Al aparecer el
 * cargador o el USB (y al arrancar con él): aviso, sonido e icono propio en la barra. El USB al ordenador CARGA la batería (la lleva
 * hw_carga_paso, una vez por segundo, también con una app en pantalla); el cargador de pared (0x23 bit 3) NO se usa para cargar. */
static void aviso_cargador(void)
{
    sh_sonar(SN_BATERIA);
    sh_insistir(20000);
    if (hw_cargador() == 2) ui_aviso("USB conectado", placa()->bat.carga_permitida && !hw_modo_seguro()
                                     ? "La batería se carga del ordenador mientras el teléfono esté conectado (el rayo de la barra). Cómo va: Sistema, «Energía (PMU)»."
                                     : "Este sistema NO carga la batería en este modo: el USB es solo para pasar ficheros (MTP).");   /* (USBD) solo USB */
    else ui_aviso("Cargador conectado", "Este sistema NO carga con el cargador de pared (no está medido en el aparato): desconéctalo. Para cargar, el USB a un ordenador.");
}
/* (CARGA) Los estados de la carga que se DICEN: un tope de seguridad (DETENIDA) y la pausa por temperatura, una vez cada uno. */
static void carga_avisar(int en_app)
{
    static int visto = -1;
    HwCarga c; int e = hw_carga_paso(usb_carga_ma());
    if (e == visto || e < 0) return;
    visto = e;
    if (e != CARGA_DETENIDA && e != CARGA_PAUSA_TEMP) return;
    hw_carga_estado(&c);
    sh_sonar(SN_BATERIA);
    if (en_app) return;
    sh_insistir(20000);
    if (e == CARGA_PAUSA_TEMP) ui_aviso("Carga en pausa", ui_fmt("La batería está a %d °C, fuera de 5..45 °C: no se carga hasta que vuelva.", c.temp_dc / 10));
    else ui_aviso("Carga detenida", ui_fmt("%s. No se vuelve a cargar hasta desconectar y conectar el USB.",
                  c.motivo == CARGA_M_TIEMPO ? "Seis horas cargando sin acabar" : c.motivo == CARGA_M_TENSION ? "La batería ha pasado de 4200 mV cargando" :
                  c.motivo == CARGA_M_QUITADA ? "El chip dice que no hay batería" : c.motivo == CARGA_M_DESCONOCIDA ? "La batería no se reconoce (su resistencia de identificación)" :
                  "Un tope de seguridad"));
}
void sh_cargador_vigilar(int en_app)
{
    int c;
    carga_avisar(en_app);                                 /* (CARGA) y, con él, el paso de la carga (una vez por segundo) */
    c = hw_cargador();
    if (c < 0) return;
    c = c != 0;
    if (c == carg_visto) return;
    if (c) {
        hw_log("cargador: CONECTADO\n");
        if (en_app) { sh_sonar(SN_BATERIA); sh_aviso_pendiente(SH_AV_CARGADOR, 0); }
        else { sh_actividad(); aviso_cargador(); }
    } else if (carg_visto != 0xFF) hw_log("cargador: desconectado\n");
    carg_visto = (u8)c;
    if (!en_app) ui_reconstruir_todo();                   /* el icono de la barra */
}

/* ---- (S7) LA PULSACIÓN LARGA DE LA TECLA DE ENCENDIDO: 8 s -> apagar, sin menú y sin preguntar ------------------------------
 * Tres niveles, de más a menos amable (docs/sistema.md §15.6):
 *  1. aquí, con la interfaz viva: la tecla mantenida 8 s (por el teclado o por el PMU: hw_tecla_encendido) apaga CON ORDEN
 *     (sh_apagar: cancela copias, guarda, se despide). Se mira en cada vuelta del bucle y desde app_latido (apps externas);
 *  2. la plataforma, desde la interrupción del tic: si a los 11 s sigue pulsada es que nadie ha apagado -> apagado forzado;
 *  3. si ni eso (la tecla solo llega por el PMU y la interfaz está colgada): el perro salta a los 10 s y la pantalla de fallo
 *     apaga con la tecla mantenida 3 s.
 * La pulsación corta sigue abriendo el menú de encendido. */
static u32 enc_desde; static u8 enc_hecho;
void sh_encendido_largo(void)
{
    u32 t = hw_ms();
    if (!arrancado) return;
    /* (R2; auditoría de S7, A3) ya no vale «la lectura cruda O la bandera de la interrupción»: hw_encendido_mantenida() aplica
     * las reglas de api/enc.h (cada vía, vista suelta una vez; si el PMU es vía probada y dice «suelta», no se cree al teclado).
     * En MODO SEGURO la pulsación larga no apaga: avisa (el menú de la pulsación corta sí apaga, porque lo pide alguien). */
    if (hw_encendido_mantenida()) {
        if (!enc_desde) enc_desde = t ? t : 1;
        else if (!enc_hecho && t - enc_desde >= 8000u) {
            enc_hecho = 1;
            if (hw_modo_seguro()) {
                hw_log("encendido: pulsacion larga (8 s) en MODO SEGURO: se avisa, NO se apaga\n");
                if (!(suspendida & SUSP_EXTERNA)) ui_aviso("MODO SEGURO", "Tecla de encendido mantenida 8 s. En modo seguro la pulsación larga NO apaga (por si la señal de la tecla no fuera de fiar): pulsa la tecla un momento y elige «Apagar el teléfono».");
                return;
            }
            hw_log("encendido: pulsacion larga (8 s): APAGAR\n");
            apagar_con("Hasta luego", NULL, 0, 0);
        }
    } else { enc_desde = 0; enc_hecho = 0; }
}

/* ---- la máquina de estados: una vez por vuelta del bucle ------------------------------------------------- */
void sh_energia_paso(void)
{
    u32 t, inact, cambios;
    int cerrada;
    if (!arrancado) return;
    sh_encendido_largo();                                 /* (S7) también con la energía suspendida */
    if (suspendida & SUSP_EXTERNA) return;
    if (suspendida && susp_viva()) {                      /* (R2) una pantalla de casa retiene: se vigila lo vital y nada más */
        sh_cargador_vigilar(0); sh_bateria_vigilar(0);
        return;
    }
    t = hw_ms();
    sh_cargador_vigilar(0);
    sh_bateria_vigilar(0);
    if (falso && hw_cargador() == 0) {                    /* (S7) «apagado» con el cargador puesto y lo han quitado: ahora sí */
        falso = 0;
        hw_log("apagado en falso: sin cargador; se apaga ahora\n");
        apagar_con("Hasta luego", NULL, 0, 0);
        return;
    }

    /* La tapa, por las dos vías: el contador de la interrupción del pin (que puede no llegar nunca) y el
     * sondeo del nivel, que funciona siempre. hal.md §9 / docs/emulador_radio.md §7. */
    cambios = hw_tapa_cambios();
    cerrada = hw_tapa_cerrada() == 1;
    if (cerrada != tapa_cerrada || cambios != tapa_vistas) {
        tapa_vistas = cambios;
        if (cerrada != tapa_cerrada) {
            tapa_cerrada = (u8)cerrada;
            if (!aj.tapa_apaga) { }
            else if (cerrada) { if (aj.bloqueo_auto) sh_bloquear(1); apagar_pantalla(); }
            else sh_actividad();
            ui_reconstruir_todo();                        /* la barra de estado enseña la tapa */
        }
    }
    if (tapa_cerrada && aj.tapa_apaga) return;            /* cerrada: se queda apagada, sin cuentas que hacer */

    if (insistir_hasta && (i32)(t - insistir_hasta) < 0) { ultima = t; return; }   /* una alerta pidiendo atención */
    inact = t - ultima;
    if (ui_ocupada()) { ultima = t; return; }             /* algo en marcha (una imagen descodificándose, una transición) */
    if (estado == EN_VIVA && aj.atenuar_s && inact >= aj.atenuar_s * 1000u) { estado = EN_ATENUADA; luz_segun_estado(); }
    if (estado != EN_APAGADA && aj.apagar_s && inact >= aj.apagar_s * 1000u) apagar_pantalla();
}

/* Cuánto se puede dormir a fondo: solo con la pantalla apagada, y nunca más allá del siguiente
 * temporizador de la UI (el reloj del standby late cada segundo, así que en la práctica es ~1 s cada vez:
 * 1 despertar por segundo en vez de los 250 del tic). */
u32 sh_dormir_ms(void)
{
    u32 p, son;
    if (!arrancado || susp_viva() || estado != EN_APAGADA || ui_ocupada() || hw_modo_seguro()) return 0;   /* (S7) en modo seguro, sin reposo profundo */
    if (sh_usb_activo()) return 0;       /* (USBD) con el USB en marcha no se duerme a fondo: el reposo profundo cierra la puerta del intcon
                                          * y el host espera respuesta en milisegundos */
    if (sh_bt_activo()) return 0;        /* (BT) con la radio encendida no se duerme a fondo: el servicio va por SONDEO de la UART 3 en cada
                                          * vuelta del bucle (sin IRQ) y el reposo profundo para el tic; el WFI normal sí (despierta cada 4 ms) */
    if (sh_musica_estado() == MU_SONANDO) return 0;   /* la música cuelga del tic (zumbador) y del bucle (PCM): no se duerme a fondo */
    if (sh_musica_dsp_activo()) return 0;             /* (MP3) el DSP y su reloj siguen vivos (también en pausa y en el cierre diferido): el reposo profundo los pararía */
    if (sh_cam_activa()) return 0;                    /* (1.9) la cámara: el tic vigila su cuadro cada 4 ms (el reposo profundo para el tic) */
    if (sh_mic_activo()) return 0;                    /* (1.10) el micrófono: el tic recoge sus muestras cada 4 ms */
    if (sh_sonando()) return 0;          /* una melodía en marcha: el reposo profundo PARA EL TIC y la
                                          * dejaría clavada en una nota (hw_ms() se pone al día al volver,
                                          * así que el siguiente paso saltaría varias notas de golpe) */
    p = ui_proximo_ms();
    if (p == 0xFFFFFFFFu) p = 1900;
    if (p > 1900) p = 1900;
    son = sh_sonido_proximo_ms();
    if (son && son < p) p = son;
    son = sh_pasos_proximo_ms();         /* el podómetro: despierto muestrea cada 20 ms (no se duerme a fondo); con el sensor
                                          * dormido por «vigilar», hasta que toque encenderlo */
    if (son < p) p = son;
    return p < 40 ? 0 : p;
}

/* ---- la batería de la barra de estado -------------------------------------------------------------------
 * Un dibujo (Pintura) en vez de 5 iconos: así el nivel es continuo y se repinta solo cuando cambia
 * (.version = el porcentaje). Si el PMU no contesta, no se dibuja nada: mejor nada que un dato inventado. */
static void pintar_bateria(Lienzo *l, Rect r, void *dato)
{
    int pct = (int)(size_t)dato & 0x7F, enchufado = ((int)(size_t)dato & 0x80) != 0, como = (int)(((size_t)dato >> 24) & 3);
    Color c = pct <= 15 ? RGB(255, 64, 48) : (Color)(((size_t)dato >> 8) & 0xFFFF);
    int x = r.x, y = r.y + (r.h - 7) / 2, an = 11, relleno = (an - 2) * pct / 100;
    if (enchufado && como == 1) {                         /* (CARGA) CARGANDO: el rayo */
        const Color e = RGB(255, 208, 64);
        li_rect(l, x + 3, y - 1, 3, 1, e); li_rect(l, x + 2, y, 3, 1, e); li_rect(l, x + 1, y + 1, 3, 1, e);
        li_rect(l, x, y + 2, 6, 1, e); li_rect(l, x + 3, y + 3, 2, 1, e); li_rect(l, x + 2, y + 4, 2, 1, e);
        li_rect(l, x + 1, y + 5, 2, 1, e); li_rect(l, x, y + 6, 2, 1, e);
        x += 9;
    } else if (enchufado) {                               /* (S7) ENCHUFE: TACHADO si hay cargador o USB y NO se carga; (CARGA) sin tachar = carga completa */
        const Color e = (Color)(((size_t)dato >> 8) & 0xFFFF), no = RGB(255, 64, 48);
        int i;
        li_rect(l, x + 1, y - 1, 1, 2, e); li_rect(l, x + 4, y - 1, 1, 2, e);   /* las dos patillas */
        li_rect(l, x, y + 1, 6, 3, e);                                          /* el cuerpo */
        li_rect(l, x + 1, y + 4, 4, 1, e); li_rect(l, x + 2, y + 5, 2, 3, e);   /* y el cable */
        if (como != 2) for (i = 0; i < 9; i++) li_rect(l, x - 1 + i * 8 / 9, y + 7 - i, 2, 1, no);   /* la raya que lo tacha */
        x += 9;
    }
    li_marco(l, x, y, an, 7, c);                          /* el cuerpo */
    li_rect(l, x + an, y + 2, 1, 3, c);                   /* el borne */
    if (relleno > 0) li_rect(l, x + 1, y + 1, relleno, 5, c);
}
Widget *sh_bateria_de(Color c)                           /* el color va en `dato` (la pintura solo depende de lo que lleva) */
{
    int pct = hw_bateria_pct(), carga = hw_cargador();
    u32 v;
    if (pct < 0) return NULL;                             /* sin PMU: la barra no miente */
    if (carga < 0) carga = 0;
    {   int e = hw_carga_paso(usb_carga_ma());            /* (CARGA) cómo está la carga: 1 = cargando (rayo), 2 = completa (enchufe sin tachar) */
        u32 como = e == CARGA_CARGANDO ? 1u : e == CARGA_COMPLETA ? 2u : 0u;
        v = (u32)(pct | (carga ? 0x80 : 0)) | ((u32)c << 8) | (carga ? como << 24 : 0); }
    return Pintura(pintar_bateria, .dato = (void *)(size_t)v, .version = v, .an = carga ? 22 : 13, .al = 9);
}
Widget *sh_bateria(void) { return sh_bateria_de(sh_tema()->texto); }

/* ---- pantallas de arranque y de apagado -----------------------------------------------------------------
 * Las dos se pintan A MANO sobre el cuadro estático: la de arranque porque todavía no hay árbol de widgets
 * (montar el FS y descodificar el fondo cuestan 4,2 M de instrucciones: 0,06–0,16 s a 104 MHz y 1,3 s a 13),
 * y la de apagado porque después no va a haber nada. */
void sh_pantalla_arranque(const char *linea)
{
    Lienzo l = { ui_pantalla, { 0, 0, UI_ANCHO, UI_ALTO } };
    const Color fondo = RGB(12, 14, 24), blanco = RGB(255, 255, 255), naranja = RGB(255, 120, 0);
    int an;
    int y = UI_ALTO * 70 / 220;                  /* (estética) de arriba abajo con los altos de la letra en uso (antes, posiciones fijas) */
    li_rect(&l, 0, 0, UI_ANCHO, UI_ALTO, fondo);
    an = li_ancho_texto(placa()->rotulo, 4);                                     /* (P600) el nombre de la carcasa, de la placa */
    li_texto(&l, (UI_ANCHO - an) / 2, y, placa()->rotulo, naranja, 4);
    y += dis_alto_texto(4, 0, placa()->rotulo) + 2;
    an = li_ancho_texto(SISTEMA_VERSION, 1);
    li_texto(&l, (UI_ANCHO - an) / 2, y, SISTEMA_VERSION, blanco, 1);
    y += dis_alto_texto(1, 0, SISTEMA_VERSION) + 6;
    li_linea_h(&l, 40, y, UI_ANCHO - 80, RGB(60, 64, 90));
    y += 8;
    if (linea) {                                 /* centrado, pero nunca fuera de la pantalla por la izquierda; en la talla pequeña */
        const int pq = F_ESTILO(DIS_T_PEQUENA);
        an = li_ancho_texto_f(linea, 1, pq);
        li_texto_f(&l, an < UI_ANCHO - 4 ? (UI_ANCHO - an) / 2 : 2, y, linea, RGB(150, 154, 170), 1, pq);
    }
    hw_pantalla_volcar(ui_pantalla, 0, UI_ALTO);
}
#ifdef MIGAS
/* (DIAG 2026-10-20) Una línea por paso, debajo del splash, con los ms desde el arranque (salen del tic: si no avanzan, la IRQ no late) y el bit I del
 * CPSR. Se pinta y se vuelca al momento: si el arranque se cuelga, la ÚLTIMA línea visible es el paso que no acabó. */
__attribute__((target("arm"), noinline)) static u32 cpsr_i(void) { u32 v; __asm__ volatile("mrs %0, cpsr" : "=r"(v)); return (v >> 7) & 1; }
static void dec(char *t, int *n, u32 v, int ancho) { char b[11]; int k = 0; do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k < ancho--) t[(*n)++] = ' '; while (k) t[(*n)++] = b[--k]; }
void sh_miga(const char *paso)
{
    static int fila;
    Lienzo l = { ui_pantalla, { 0, 0, UI_ANCHO, UI_ALTO } };
    char t[64]; int n = 0, i, y;
    hw_log("MIGA "); hw_log(paso); hw_log("\n");
    dec(t, &n, (u32)fila, 2); t[n++] = ' ';
    dec(t, &n, hw_ms(), 6); t[n++] = 'm'; t[n++] = 's'; t[n++] = ' ';
    t[n++] = 'I'; t[n++] = (char)('0' + cpsr_i()); t[n++] = ' ';
    for (i = 0; paso[i] && n < (int)sizeof t - 1; i++) t[n++] = paso[i];
    t[n] = 0;
    {   /* en anillo, solo las filas que caben bajo la línea del splash: la que lleva el número MAYOR es la última */
        int alto = dis_alto_texto(1, F_ESTILO(DIS_T_PEQUENA), NULL) + 1, y0 = UI_ALTO * 160 / 220, filas = (UI_ALTO - y0) / alto;   /* (estética) en la talla pequeña */
        y = y0 + (fila % filas) * alto;
        li_rect(&l, 0, y, UI_ANCHO, alto, RGB(12, 14, 24));
        li_texto_f(&l, 2, y + 1, t, RGB(255, 255, 0), 1, F_ESTILO(DIS_T_PEQUENA));
        hw_pantalla_volcar(ui_pantalla, y, alto);
    }
    fila++;
}
#endif
/* APAGAR, de punta a punta (S7): cancelar la copia a medias (el destino queda como estaba), callar y GUARDAR lo de la música
 * y los ajustes, despedida en pantalla (que se lea), luces y panel fuera, y hw_apagar (0x41 <- 0, fuera la retención). Si
 * vuelve, el teléfono sigue con corriente:
 *  - con el cargador o el USB puestos ([sin saber] si el PMU apaga así; el original ni lo intenta): APAGADO EN FALSO. La
 *    pantalla y las luces se quedan apagadas y el bucle sigue, durmiendo a fondo; una tecla enciende la pantalla unos
 *    segundos y repite el aviso del cargador; al quitar el cargador se vuelve a apagar (si es que no cae solo);
 *  - sin cargador: se dice en pantalla y se sigue (no se reescribe nada en el PMU);
 *  - en el PC (HW_NO_DISPONIBLE): se dice y se sigue. */
static void apagar_con(const char *titulo, const char *linea, int sonido, int callado)   /* callado: un REINTENTO (M3): sin despedida, sin sonido y sin otro aviso */
{
    Lienzo l = { ui_pantalla, { 0, 0, UI_ANCHO, UI_ALTO } };
    const Color fondo = RGB(8, 8, 12), gris = RGB(190, 190, 200);
    u32 t0;
    int an = li_ancho_texto(titulo, 2), r, en_app = suspendida & SUSP_EXTERNA;
    hw_usr_cancelar();                                     /* (S6) una copia de Mis archivos a medias se abandona: el destino queda como estaba */
    sh_musica_pausa();                                     /* la música calla y lo suyo (pista, posición, volumen) se guarda antes de irse */
    sh_musica_guardar_ya();
    if (callado) goto apagar;
    hw_pantalla_dormir(0);                                 /* por si el aviso llega con el panel dormido (batería agotada en reposo) */
    hw_luz_fase(HW_LUZ_VIVA);
    li_rect(&l, 0, 0, UI_ANCHO, UI_ALTO, fondo);
    li_texto(&l, an < UI_ANCHO - 4 ? (UI_ANCHO - an) / 2 : 2, 96, titulo, gris, 2);
    if (linea) {                                           /* (estética) debajo del título, con el paso de la letra en uso */
        const char *d = linea; int y = 96 + dis_alto_texto(2, 0, titulo) + 6, pl = dis_paso_linea(0);
        while (*d && y + pl <= UI_ALTO - 4) {
            int n = li_partir(d, UI_ANCHO - 16, 1, 0);
            li_texto_n(&l, 8, y, d, n, gris, 1, 0);
            d += n; while (*d == ' ' || *d == '\n') d++;
            y += pl;
        }
    }
    hw_pantalla_volcar(ui_pantalla, 0, UI_ALTO);
    sh_sonar(sonido ? sonido : SN_APAGADO);                /* do-sol-do bajando, al revés que el arranque */
    for (t0 = hw_ms(); hw_ms() - t0 < (linea ? 2500u : 900u); ) { sh_sonido_paso(); hw_reposo(); }   /* que se lea (y se oiga) antes de irse */
    sh_callar();
apagar:
    hw_luz_fase(HW_LUZ_APAGADA);
    hw_pantalla_dormir(1);
    r = hw_apagar();
    if (r == HW_OK) return;                                /* (el PC con «apagable 1»: se ha apagado) */
    if (en_app) {                                          /* una app externa manda: se le devuelve la pantalla y que siga */
        hw_pantalla_dormir(0); hw_luz_fase(HW_LUZ_VIVA);
        return;
    }
    if (r == HW_ERROR && hw_cargador() > 0) {              /* APAGADO EN FALSO: a oscuras y durmiendo hasta que quiten el cargador */
        hw_log("apagado en falso: el cargador esta puesto; pantalla y luces fuera, se duerme\n");
        falso = 1;
        estado = EN_APAGADA;
        ui_pantalla_activa(0);
        ui_forzar_todo();
        ui_aviso("Cargador conectado", "El teléfono no puede apagarse del todo con el cargador o el USB puestos: desconéctalo. Con el USB al ordenador, la batería se sigue cargando.");
        return;
    }
    estado = EN_APAGADA;                                   /* el panel quedó dormido: que sh_actividad lo despierte y repinte */
    sh_actividad();
    ui_forzar_todo();
    if (callado) return;                                   /* (M3) el aviso ya está dicho: no se apila otro */
    ui_aviso("Apagar", r == HW_ERROR ? "El teléfono NO se ha apagado (el PMU no ha cortado la corriente). Prueba otra vez o quita la batería."
                                     : "Esta plataforma no sabe apagarse (solo el teléfono).");
}
void sh_apagar(void) { apagar_con("Hasta luego", NULL, 0, 0); }
/* (auditoría del sistema, B7) REINICIAR, simétrico de apagar: la copia de Mis archivos a medias se abandona (el destino queda como
 * estaba) y lo pendiente de la música (pista, posición, volumen) se guarda ANTES del reset; hw_reiniciar a secas lo perdía. */
int sh_reiniciar(void)
{
    hw_usr_cancelar();
    sh_musica_pausa();
    sh_musica_guardar_ya();
    hw_log("reiniciar: lo pendiente, guardado\n");
    sh_pantalla_arranque("Reiniciando…");
    return hw_reiniciar();                                 /* en el teléfono no vuelve; en el PC: HW_NO_DISPONIBLE */
}
int sh_apagado_en_falso(void) { return falso; }
