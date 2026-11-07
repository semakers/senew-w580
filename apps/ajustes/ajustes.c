/* Ajustes: tema (el Tema heredado cambia para todas las pantallas), formato de hora, zona horaria,
 * brillo de la pantalla, SONIDO (volumen y tono de las teclas), ENERGÍA (atenuar, apagar, bloqueo
 * automático, qué hace cerrar el deslizador), fondo del standby, las luces del teclado, el sensor del deslizador,
 * las LUCES (S5: abre el apartado apps/luces: laterales RGB y efectos), qué apps van en el MENÚ PRINCIPAL (LANZ: apps/ajustes/menu_principal.c)
 * el BLUETOOTH (pista BT: la pantalla de diagnóstico del chip, apps/ajustes/bluetooth.c)
 * y el PODÓMETRO (si cuenta en segundo plano y qué ahorro hace en reposo: shell/pasos.c).
 * Se guardan en el almacén persistente (sh_guardar_ajustes). */
#include "shell.h"
#include "placa.h"
#include "m3d.h"

typedef struct { Widget base; } Ajustes_;
typedef struct { i32 sel; } AjustesE;
extern const Clase ClsAjustes_;
enum { A_TEMA, A_HORA, A_ZONA, A_BRILLO, A_VOLUMEN, A_TECLAS_TONO, A_ATENUAR, A_APAGAR, A_BLOQUEO, A_TAPA_APAGA,
       A_FONDO, A_SIN_FONDO, A_LUZ1, A_LUZ2, A_LUZ3, A_TAPA, A_TONO,
       /* Las cuatro del menú 3D (pista V) van AQUÍ y no en otro sitio: hay guiones que llegan a una fila contando
        * hacia abajo desde la primera (hasta «Luz 1») y otros que llegan a «ahorro en reposo» con UN «arriba» desde
        * la primera (da la vuelta a la última). Puestas al final, ese «arriba» caía en «ejes del sensor» y los
        * guiones del podómetro cambiaban los ejes creyendo cambiar el ahorro: pasaban en verde sin probar nada. */
       A_MENU_ESTILO, A_MENU_EFECTO, A_MENU_INTENSIDAD, A_MENU_EJES,
       A_MENU_COLOR,                 /* (28-oct) «  color de Medios»: AQUÍ, entre «ejes del sensor» y «Girar la pantalla», el mismo hueco que usó A_GIRO:
                                      * los guiones cuentan desde arriba hasta «Menú: estilo» (la 18.ª) y desde abajo hasta «Girar la pantalla» (la
                                      * 6.ª hacia arriba); esta no mueve ninguna de las dos cuentas */
       A_GIRO,                       /* (estética) «Girar la pantalla»: AQUÍ, entre las del menú 3D y «Bluetooth», la única posición que no mueve ni las
                                      * que se cuentan desde arriba (hasta «ejes del sensor») ni las que se cuentan desde abajo (desde «Bluetooth») */
       A_BT,                         /* (pista BT) «Bluetooth»: AQUÍ, entre las del menú 3D y «Menú principal», por lo mismo que esas: «Menú: estilo»
                                      * sigue siendo la fila 18 desde la primera, y «Menú principal», «Luces» y las dos del podómetro siguen a 4, 3, 2 y 1
                                      * «arriba» (el podómetro, las ÚLTIMAS). Es la 5.ª hacia arriba. */
       A_MENU_APPS,                  /* (LANZ) «Menú principal»: qué apps van y en qué orden. AQUÍ por lo mismo que las de arriba: «Menú: estilo»
                                      * sigue siendo la fila 18 contando desde la primera y «Luces», la tercera hacia arriba */
       A_LUCES,                      /* (S5) detrás de las del menú y delante de las dos últimas, por lo mismo: ni se mueven las de arriba ni las dos de abajo */
       A_PASOS, A_PASOS_AHORRO, A_N };

/* Los plazos de energía se eligen de una lista corta (y «nunca»): un selector libre de segundos sería
 * un campo que nadie quiere teclear. El de atenuar nunca puede pasar del de apagar. */
static const u8 plazos[] = { 0, 5, 10, 15, 30, 60, 120 };
#define N_PLAZOS ((int)(sizeof plazos / sizeof plazos[0]))
static const char *plazo_texto(u8 s) { return s ? ui_fmt(s < 60 ? "%d s" : "%d min", s < 60 ? s : s / 60) : "nunca"; }
static u8 plazo_siguiente(u8 v)
{
    int i;
    for (i = 0; i < N_PLAZOS; i++) if (plazos[i] == v) return plazos[(i + 1) % N_PLAZOS];
    return plazos[0];
}
static void poner_plazos(u8 atenuar, u8 apagar)
{
    aj.atenuar_s = atenuar; aj.apagar_s = apagar;
    if (aj.apagar_s && aj.atenuar_s > aj.apagar_s) aj.atenuar_s = aj.apagar_s;
    sh_actividad();
    sh_guardar_ajustes();
}

static void tic(Ctx *c) { ui_sucio(c); }
static void iniciar(Ctx *c) { ui_cada(c, 500, tic); }

/* ---- sonido (pista S5) ----------------------------------------------------------------------------------
 * El volumen son 6 niveles y el nivel 0 es SILENCIO de verdad (no suena nada, ni la alarma: el aviso en
 * pantalla lo dice). Lo que cambia el volumen es el ancho del pulso del zumbador, que es lo único medible
 * (docs/sistema.md §14), así que la fila lo ENSEÑA: «4 (pulso 25 %)». */
static const char *vol_texto(void)
{
    if (!aj.volumen) return "silencio";
    return ui_fmt("%d  (pulso %d %%)", aj.volumen, sh_sonido_ancho() / 2);
}
static void volumen(int v)
{
    sh_sonido_volumen(v);
    if (aj.volumen) sh_sonar(SN_PRUEBA);       /* se oye lo que se acaba de elegir */
}
/* El RTC va en UTC (en el emulador, la hora del anfitrión en UTC): la zona es el desfase de la hora local. */
static const char *const ops_zona[] = { "+1 hora", "-1 hora", "+15 minutos", "-15 minutos", "Sin desfase (UTC)" };
static void zona(int min)
{
    aj.zona_min = (i16)(min < -720 ? -720 : min > 840 ? 840 : min);
    ui_reconstruir_todo(); sh_guardar_ajustes();
}
static void zona_elegir(int i, void *d) { static const i16 paso[4] = { 60, -60, 15, -15 }; (void)d; zona(i < 4 ? aj.zona_min + paso[i] : 0); }
static void brillo(int pct)
{
    aj.brillo = (u8)(pct < 10 ? 10 : pct > 100 ? 100 : pct);      /* nunca a 0: no se vería nada */
    hw_luz(0, aj.brillo); sh_guardar_ajustes();
}
/* ---- el menú 3D (pista V; docs/menu3d.md §4) ---------------------------------------------------------------
 * Estilo, efecto, intensidad (la ganancia de cuentas del acelerómetro a grados: las cuentas por g del sensor
 * no se saben hasta tener el aparato) y qué eje del sensor es cuál (tampoco se sabe). */
static const char *const efecto_nombre[SH_EFECTOS] = { "inclinación", "vaivén", "quieto" };
static const char *const ejes_nombre[8] = { "X  Y", "-X  Y", "X  -Y", "-X  -Y", "Y  X", "-Y  X", "Y  -X", "-Y  -X" };
static void intensidad(int v) { aj.menu_intensidad = (u8)(v < 1 ? 1 : v > 5 ? 5 : v); sh_guardar_ajustes(); }
static int tecla(Ctx *t, int k, void *dato)                       /* izq./der. cambian el valor de la fila elegida */
{
    Ctx *c = ui_ancestro(t, &ClsAjustes_); AjustesE *s = ui_estado(c);
    if ((k == K_IZQ || k == K_DER) && s->sel == A_MENU_INTENSIDAD) { intensidad(aj.menu_intensidad + (k == K_DER ? 1 : -1)); ui_sucio(c); return 1; }
    if ((k == K_IZQ || k == K_DER) && s->sel == A_ZONA) { zona(aj.zona_min + (k == K_DER ? 60 : -60)); ui_sucio(c); return 1; }
    if ((k == K_IZQ || k == K_DER) && s->sel == A_BRILLO) { brillo(aj.brillo + (k == K_DER ? 10 : -10)); ui_sucio(c); return 1; }
    if ((k == K_IZQ || k == K_DER) && s->sel == A_VOLUMEN) { volumen(aj.volumen + (k == K_DER ? 1 : -1)); ui_sucio(c); return 1; }
    return sh_teclas(t, k, dato);
}
/* (28-oct) el FONDO: el de Medios, animado, o una imagen (desde Imágenes, como antes) */
static const char *const ops_fondo[] = { "Medios (animado)", "Una imagen…" };
static void fondo_elegir(int i, void *d) { (void)d; if (i == 0) sh_poner_fondo(SH_FONDO_MEDIOS); else sh_abrir_sen("/apps/imagenes.sen", NULL); }
static void color_elegir(int i, void *d)
{
    (void)d;
    if (i < 0 || i >= SH_MEDIOS_COLORES) return;
    aj.medios_color = (u8)i; sh_guardar_ajustes(); ui_reconstruir_todo();
}
static void activar(Ctx *l, int i, void *dato)
{
    Ctx *c = ui_ancestro(l, &ClsAjustes_);
    (void)dato;
    switch (i) {
    case A_TEMA: sh_poner_tema(aj.tema + 1); break;
    case A_HORA: aj.hora12 = !aj.hora12; ui_reconstruir_todo(); sh_guardar_ajustes(); break;
    case A_FONDO: ui_opciones("Fondo", ops_fondo, 2, fondo_elegir, NULL); break;   /* (28-oct) Medios animado, o una imagen (el .sen Imágenes) */
    case A_SIN_FONDO: sh_poner_fondo(""); break;
    case A_ZONA: ui_opciones("Zona horaria", ops_zona, 5, zona_elegir, c); break;
    case A_BRILLO: brillo(aj.brillo >= 100 ? 25 : aj.brillo + 25); break;
    case A_LUZ1: case A_LUZ2: case A_LUZ3: { int n = i - A_LUZ1 + 1; hw_luz(n, hw_luz_estado(n) > 0 ? 0 : 100); } break;
    case A_ATENUAR: poner_plazos(plazo_siguiente(aj.atenuar_s), aj.apagar_s); break;
    case A_APAGAR: poner_plazos(aj.atenuar_s, plazo_siguiente(aj.apagar_s)); break;
    case A_BLOQUEO: aj.bloqueo_auto = !aj.bloqueo_auto; sh_guardar_ajustes(); break;
    case A_GIRO: aj.giro = !aj.giro; sh_guardar_ajustes(); break;   /* (estética) apagado, sh_giro_paso la vuelve de pie en la vuelta siguiente */
    case A_TAPA_APAGA: aj.tapa_apaga = !aj.tapa_apaga; sh_guardar_ajustes(); break;
    case A_VOLUMEN: volumen(aj.volumen >= 5 ? 0 : aj.volumen + 1); break;
    case A_TECLAS_TONO: aj.tono_teclas = !aj.tono_teclas; sh_guardar_ajustes(); if (aj.tono_teclas) sh_sonar(SN_TECLA); break;
    case A_TAPA: ui_aviso("Deslizador", "Sensor en P2.6. Se toma 1 = cerrado: así lo trata el firmware original en el emulador (pista E2). En un teléfono, sin probar: si sale al revés, es una constante (DESLIZADOR_CERRADO_ES)."); break;
    case A_PASOS: if (!placa_tiene(CAP_ACELEROMETRO)) { ui_aviso("Podómetro", "Este teléfono no tiene acelerómetro: no puede contar pasos."); break; }
        sh_pasos_ajuste(!aj.pasos, aj.pasos_ahorro); ui_reconstruir_todo(); break;
    case A_LUCES: sh_abrir_sen("/apps/luces.sen", NULL); break;     /* (APX) .sen */
    case A_MENU_APPS: ui_empujar(app_menu_principal()); break;      /* (LANZ) apps/ajustes/menu_principal.c */
    case A_BT: ui_empujar(app_bluetooth()); break;                  /* (pista BT) apps/ajustes/bluetooth.c */
    case A_MENU_ESTILO:
        aj.menu_estilo = (u8)((aj.menu_estilo + 1) % M3_ESTILOS);
        /* (28-oct) con el menú de Medios, su fondo animado de fábrica (pedido del mantenedor); al dejarlo, se quita si era ese */
        if (aj.menu_estilo == M3_MEDIOS) sh_poner_fondo(SH_FONDO_MEDIOS);
        else if (sh_fondo_medios()) sh_poner_fondo("");
        sh_guardar_ajustes(); ui_reconstruir_todo(); break;
    case A_MENU_COLOR: ui_opciones("Color de Medios", sh_medios_color_nombre, SH_MEDIOS_COLORES, color_elegir, NULL); break;
    case A_MENU_EFECTO:
        aj.menu_efecto = (u8)((aj.menu_efecto + 1) % SH_EFECTOS);
        if (aj.menu_efecto == SH_EFECTO_INCLINA && !placa_tiene(CAP_ACELEROMETRO)) aj.menu_efecto = SH_EFECTO_VAIVEN;   /* (0.23) sin sensor no hay inclinación */
        sh_guardar_ajustes();
        ui_aviso("Efecto del menú", aj.menu_efecto == SH_EFECTO_INCLINA ?
            "INCLINACIÓN: los iconos siguen al acelerómetro mientras el menú está a la vista. «Plano» es la postura al abrir el menú; la tecla Actividad lo recalibra." :
            aj.menu_efecto == SH_EFECTO_VAIVEN ? "VAIVÉN: un balanceo propio, lento, sin usar el sensor." :
            "QUIETO: nada se mueve solo. Los iconos siguen siendo 3D y el del foco crece al llegar.");
        break;
    case A_MENU_INTENSIDAD: intensidad(aj.menu_intensidad >= 5 ? 1 : aj.menu_intensidad + 1); break;
    case A_MENU_EJES:
        if (!placa_tiene(CAP_ACELEROMETRO)) { ui_aviso("Ejes del sensor", "Este teléfono no tiene acelerómetro."); break; }
        aj.menu_ejes = (u8)((aj.menu_ejes + 1) & 7); sh_guardar_ajustes();
        if (!aj.menu_ejes) ui_aviso("Ejes del sensor", "Qué eje del acelerómetro inclina los iconos a los lados y cuál arriba y abajo, y con qué signo. No se sabe cómo va montado el sensor en la carcasa: si el menú se inclina al revés o cruzado, se corrige aquí.");
        break;
    case A_PASOS_AHORRO:
        sh_pasos_ajuste(aj.pasos, (aj.pasos_ahorro + 1) % SH_PASOS_AHORROS);
        ui_aviso("Ahorro en reposo", aj.pasos_ahorro == SH_PASOS_FIEL ?
            "FIEL: lo que hace el firmware original, con sus dos fallos. Tras 46 s quieto apaga el sensor 5 s y lo enciende 2." :
            aj.pasos_ahorro == SH_PASOS_SIN_AHORRO ? "NO: el sensor no duerme nunca. No pierde pasos al echar a andar; lo que gasta de más no está medido." :
            "CORREGIDO: tras 46 s quieto apaga el sensor 5 s y lo enciende 3 muestras. Ahorra más que el original y pierde más pasos al echar a andar.");
        break;
    case A_TONO:
        /* El la4 de referencia: 440 Hz un segundo. Es EXACTAMENTE el tono que la pista S4 midió en su WAV
         * (440,0 Hz por cruces, 439,8 por espectro), así que sirve para comparar. */
        if (!aj.volumen) ui_aviso("Tono", "El volumen está en silencio: no va a sonar nada. Súbelo en la fila «Volumen».");
        else {
            sh_sonar(SN_PRUEBA);
            ui_aviso("Tono de referencia", ui_fmt("La4 de 440 Hz durante un segundo, con el pulso al %d %%. "
                "Sale por el generador 0xF9020000 y el pin P0.7 (TGBUZZ). En el emulador se graba y se mide; "
                "en un teléfono de verdad no se sabe todavía a qué llega ese pin.", sh_sonido_ancho() / 2));
        }
        break;
    }
    ui_sucio(c);
}
/* (MD) las filas de sí/no son un INTERRUPTOR (el diseño decide cómo se ve: en el clásico sigue siendo «sí»/«no») */
static Widget *si_no(const char *t, int v, int sel)
{
    DisFila f = { .titulo = t, .accesorio = DIS_ACC_INTERRUPTOR, .activo = (u8)(v != 0) };
    return DisFilaW(&f, sel);
}
static Widget *fila(Ctx *c, int i, int sel, void *dato)
{
    const char *f = aj.fondo; size_t n = strlen(f);
    (void)c; (void)dato;
    while (n && f[n - 1] != '/') n--;
    switch (i) {
    case A_TEMA: return sh_fila("Tema", sh_tema_nombre[aj.tema], sel);
    case A_HORA: return sh_fila("Formato de hora", aj.hora12 ? "12 h" : "24 h", sel);
    case A_FONDO: return sh_fila("Fondo", sh_fondo_medios() ? "Medios (animado)" : aj.fondo[0] ? ui_fmt("%s", f + n) : "ninguno", sel);
    case A_SIN_FONDO: return sh_fila("Quitar el fondo", NULL, sel);
    case A_ZONA: return sh_fila("Zona horaria", sh_zona(), sel);
    case A_BRILLO: return sh_fila("Brillo de pantalla", hw_luz_estado(0) < 0 ? "no contesta" : ui_fmt("%d %%", aj.brillo), sel);
    case A_VOLUMEN: return sh_fila("Volumen", vol_texto(), sel);
    case A_TECLAS_TONO: return si_no("Tono de las teclas", aj.tono_teclas, sel);
    case A_LUZ1: return sh_fila("Luz 1 (chip LED, 0A)", hw_luz_estado(1) < 0 ? "no contesta" : hw_luz_estado(1) > 0 ? "encendida" : "apagada", sel);
    case A_LUZ2: return sh_fila("Luz 2 (chip LED, 2F)", hw_luz_estado(2) < 0 ? "no contesta" : hw_luz_estado(2) > 0 ? "encendida" : "apagada", sel);
    case A_LUZ3: return sh_fila("Luz 3 (chip LED, 09)", hw_luz_estado(3) < 0 ? "no contesta" : hw_luz_estado(3) > 0 ? "encendida" : "apagada", sel);
    case A_ATENUAR: return sh_fila("Atenuar la luz", plazo_texto(aj.atenuar_s), sel);
    case A_APAGAR: return sh_fila("Apagar la pantalla", plazo_texto(aj.apagar_s), sel);
    case A_BLOQUEO: return si_no("Bloqueo automático", aj.bloqueo_auto, sel);
    case A_GIRO: return si_no("Girar la pantalla", aj.giro, sel);
    case A_TAPA_APAGA: return si_no("Cerrar el teléfono apaga", aj.tapa_apaga, sel);
    case A_TAPA: return sh_fila("Deslizador (sensor)", sh_tapa_cerrada() ? "cerrado" : "abierto", sel);
    case A_PASOS: return sh_fila("Podómetro", !placa_tiene(CAP_ACELEROMETRO) ? "sin acelerómetro" : aj.pasos ? ui_fmt("sí, %u pasos", sh_pasos()->pasos) : "no", sel);
    case A_PASOS_AHORRO: return sh_fila("  ahorro en reposo", aj.pasos_ahorro == SH_PASOS_FIEL ? "fiel al original" : aj.pasos_ahorro == SH_PASOS_SIN_AHORRO ? "no" : "corregido", sel);
    case A_MENU_ESTILO: return sh_fila("Menú: estilo", m3_estilo_nombre[aj.menu_estilo % M3_ESTILOS], sel);
    case A_MENU_EFECTO: return sh_fila("  efecto", aj.menu_estilo == M3_CLASICO ? "(solo en 3D)" : efecto_nombre[aj.menu_efecto % SH_EFECTOS], sel);
    case A_MENU_INTENSIDAD: return sh_fila("  intensidad", ui_fmt("%d de 5", (int)aj.menu_intensidad), sel);
    case A_MENU_EJES: return sh_fila("  ejes del sensor", placa_tiene(CAP_ACELEROMETRO) ? ejes_nombre[aj.menu_ejes & 7] : "sin acelerómetro", sel);
    case A_MENU_COLOR: return sh_fila("  color de Medios", sh_medios_color_nombre[aj.medios_color % SH_MEDIOS_COLORES], sel);
    case A_BT: return sh_fila("Bluetooth", !sh_bt_hay() ? "sin Bluetooth" : sh_bt_activo() ? sh_bt_estado_nombre(bt_driver()->estado()) : "apagado (diagnóstico)", sel);   /* (pista BT) */
    case A_MENU_APPS: return sh_fila("Menú principal", aj.menu_propio ? "a tu gusto" : "de fábrica", sel);   /* (LANZ) */
    case A_LUCES: return sh_fila("Luces", !aj.luces ? "laterales apagadas" : ui_fmt("laterales: %s", sh_luz_color_nombre[aj.luces_color % SH_LUZ_COLORES]), sel);
    default: return sh_fila("Probar el tono", "la4, 440 Hz", sel);
    }
}
static Widget *construir(Ctx *c)
{
    AjustesE *s = ui_estado(c);
    return Teclas(tecla, Pantalla("Ajustes", Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Expandido(ListaPerezosa(.n = A_N, .fila = fila, .al_activar = activar, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila)),
        Caja(.an = INF, .color = sh_tema()->barra, .hijo = Relleno(4, Parrafo(
            "Izq./der. cambian el valor. Bloqueo: «*» y Sel. izq. Todo se guarda en el teléfono.", .color = sh_tema()->tenue, .marcas = F_ESTILO(DIS_T_PEQUENA)))))),   /* (estética) corto y en la pequeña */
        "Cambiar", "Atrás"));
}
COMPONENTE_CON_ESTADO(Ajustes_, AjustesE, construir, iniciar);
Widget *app_ajustes(void) { return UI_W(Ajustes_); }
