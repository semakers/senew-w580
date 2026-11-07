/* Sistema: información en vivo del teléfono y del propio sistema (se refresca cada segundo). */
#include "shell.h"
#include "placa.h"
#include "usb.h"
#include "../../hal/usb_dc.h"          /* (2026-10-20) los contadores del driver USB: el MTP no contestaba en el aparato */
#include "../../hal/ms.h"              /* (M2, 2026-10-20) la Memory Stick: si cayó a 1 bit y sus cuentas. Débil: las placas sin hal/ms.c dicen «-» */
#pragma weak ms_info
#pragma weak ms_motivo
#pragma weak ms_par_fase

typedef struct { Widget base; } Sistema;
typedef struct { i32 sel; u32 cuadros_antes, cps; u32 instr; } SistemaE;
extern const Clase ClsSistema;
#define N_FILAS 60                              /* (I1) +4: la Memoria (W6F), al final: no se mueve ninguna fila de antes; (M2) +3: la tarjeta */
Widget *app_energia_pmu(void);                  /* apps/sistema/energia_pmu.c: la pantalla «Energía (PMU)» (S7) */
Widget *app_banco_hw(void);                     /* apps/sistema/banco_hw.c: «Banco acel/audio» (2026-10-21) */
Widget *app_mp3_diag(void);                     /* apps/sistema/mp3_diag.c: «Diagnóstico MP3» (2026-10-21; el MP3 por el DSP, solo a petición) */
Widget *app_dsp_diag(void);                     /* apps/sistema/dsp_diag.c: «Diagnóstico DSP» (2026-10-21; el DSP no se arranca solo, solo desde aquí) */

static void tic(Ctx *c)
{
    SistemaE *s = ui_estado(c);
    s->cps = ui_med.cuadros - s->cuadros_antes; s->cuadros_antes = ui_med.cuadros;
    ui_sucio(c);
}
static void iniciar(Ctx *c) { ((SistemaE *)ui_estado(c))->cuadros_antes = ui_med.cuadros; ui_cada(c, 1000, tic); }

/* (P600) LAS FILAS QUE APLICAN A ESTA PLACA: las de un hardware que no tiene (panel detectado, sensor de tapa, acelerómetro) no se
 * enseñan. La lista visible apunta a las filas lógicas de siempre (los números de case no se mueven). */
static u8 vis[N_FILAS]; static int n_vis;
static int fila_aplica(int i)
{
    if (i == 17) return placa_tiene(CAP_PANEL_DETECTADO);
    if (i == 34 || i == 36) return placa_tiene(CAP_TAPA);
    if (i == 41 || i == 42) return placa_tiene(CAP_ACELEROMETRO);
    return 1;
}
static void vis_hacer(void) { int i; n_vis = 0; for (i = 0; i < N_FILAS; i++) if (fila_aplica(i)) vis[n_vis++] = (u8)i; }
static const char *tapa_nombre(void) { u8 t = placa()->tapa_tipo; return t == TAPA_GIRO ? "Tapa (giro)" : t == TAPA_CONCHA ? "Tapa" : "Deslizador"; }
static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    SistemaE *s = ui_estado(ui_ancestro(l, &ClsSistema));
    HwInfo h; HwFsEstado f; HwFecha r; HwNvEstado nv; HwSalud sa; HwUsrEstado us; PasosInfo pa; HwArranque ar; HwPmuSalud ps;
    u32 seg = hw_ms() / 1000, cuentas = ui_med.t_construir + ui_med.t_disponer + ui_med.t_pintar + ui_med.t_volcar;
    const char *n = "", *v = "";
    (void)dato;
    hw_info(&h); hw_fs_estado(&f); hw_nv_estado(&nv); hw_salud(&sa); hw_usr_estado(&us); sh_pasos_info(&pa);
    if (i >= 0 && i < n_vis) i = vis[i];
    switch (i) {
    case 0:  n = "Sistema"; v = ui_fmt("%s", SISTEMA_VERSION); break;
    case 1:  n = "Plataforma"; v = h.plataforma; break;
    case 2:  n = "CPU"; v = h.cpu_mhz ? ui_fmt("%u MHz%s", h.cpu_mhz, h.pll ? " (PLL)" : "") : "sin dato"; break;
    case 3:  n = "MMU y cachés"; v = h.mmu ? "encendidas" : "no"; break;
    case 4:  n = "Encendido hace"; v = ui_fmt("%u:%02u:%02u", seg / 3600, seg / 60 % 60, seg % 60); break;
    case 5:  n = "RTC (UTC)"; v = hw_rtc_leer(&r) == HW_OK ? ui_fmt("%02d:%02d:%02d válido", r.hora, r.min, r.seg) : "NO válido"; break;
    case 6:  n = "RAM"; v = ui_fmt("%u KB", h.ram_total / 1024); break;
    case 7:  n = "  estática"; v = h.ram_estatica ? ui_fmt("%u KB", h.ram_estatica / 1024) : "sin dato"; break;
    case 8:  n = "Código (NOR)"; v = h.tam_codigo ? ui_fmt("%u bytes", h.tam_codigo) : "sin dato"; break;
    case 9:  n = "Montículo"; v = ui_fmt("%u de %u KB", (ui_med.monticulo_usado + 512) / 1024, ui_med.monticulo_tam / 1024); break;
    case 10: n = "  pico"; v = ui_fmt("%u bytes", ui_med.monticulo_pico); break;
    case 11: n = "Arena, pico"; v = ui_fmt("%u de %u", ui_med.arena_pico, ui_med.arena_tam); break;
    case 12: n = "Elementos"; v = ui_fmt("%u en %d rutas", ui_med.elementos, ui_rutas()); break;
    case 13: n = "Cuadros"; v = ui_fmt("%u (%u/s)", ui_med.cuadros, s->cps); break;
    case 14: n = "Último cuadro"; v = ui_fmt("%u px, %u rect.", ui_med.dano_px, ui_med.dano_rects); break;
    case 15: n = "  instrucciones"; v = cuentas ? ui_fmt("~%u", cuentas * 30) : "sin contador"; break;
    case 16: n = "NOR, id"; v = h.nor_id ? ui_fmt("%04X %04X", h.nor_id >> 16, h.nor_id & 0xFFFF) : "sin leer"; break;
    case 17: n = "Panel"; v = !h.panel ? "sin dato" : h.panel_r28 < 0 ? ui_fmt("%s (R28 sin leer)", h.panel) : ui_fmt("%s", h.panel); break;   /* el descriptor que eligió la detección (R28) */
    case 18: n = "FS de fábrica"; v = f.original ? (f.propio ? "en /original" : "montado (lectura)") : "no hay"; break;
    case 19: n = "  bloques"; v = ui_fmt("%u en %u cab.", f.bloques, f.cabeceras); break;
    case 20: n = "  lecturas"; v = ui_fmt("%u bloques", f.lecturas_nor); break;
    case 21: n = "  errores"; v = ui_fmt("%u", f.errores); break;
    case 22: n = "Serie (UART 0)"; v = hw_serie_conectado() ? "hay cable" : "sin cable"; break;
    case 29: n = "FS propio (W6FS)"; v = f.propio ? ui_fmt("%u ent., %s", f.propio_entradas, sh_tam(f.propio_bytes)) : "no hay"; break;
    case 30: n = "IRQ serie/alarma"; v = ui_fmt("%u / %u", h.irq_serie, h.irq_alarma); break;
    case 24: n = "Almacén"; v = !nv.montado ? "no hay" : nv.escribible ? "NOR, se escribe" : "solo lectura"; break;
    case 25: n = "  bloques"; v = nv.montado ? ui_fmt("%u x %u KB", nv.bloques, nv.tam_bloque / 1024) : "-"; break;
    case 26: n = "  usado"; v = nv.montado ? ui_fmt("%u B, %u claves", nv.usado, nv.claves) : "-"; break;
    case 27: n = "  generación"; v = ui_fmt("%u", nv.generacion); break;
    case 28: n = "  borr./prog./err."; v = ui_fmt("%u / %u / %u", nv.borrados, nv.programas, nv.errores); break;
    case 23: n = "Batería (PMU)"; v = hw_adc(10) < 0 ? "sin PMU" : ui_fmt("cuenta %d por %s%s", hw_adc(10), hw_bateria_fuente() ? "6A (77 no vale)" : "77", hw_bateria_presente() == 0 ? ", NO CONECTADA" : ""); break;
    /* energía y robustez (pista S3; S7). Los mV salen de la recta DE FÁBRICA del original, no de la calibración de este aparato. */
    case 31: n = "Batería"; v = hw_bateria_mv() < 0 ? "sin PMU" : sh_bateria_sin_calibrar() ? ui_fmt("SIN CALIBRAR (prov. %d mV)", hw_bateria_mv()) : hw_bateria_mv() < HW_BAT_MV_NO_CREIBLE ? ui_fmt("%d mV: NO CREIBLE, sin vigilar", hw_bateria_mv())
                                : ui_fmt("%d mV, %d %% (recta fábr.)", hw_bateria_mv(), hw_bateria_pct()); break;
    case 43: n = "Cargador"; v = hw_cargador() < 0 ? "sin PMU" : hw_cargador() == 0 ? "no (este sistema no carga)" : ui_fmt("%s PUESTO: NO SE CARGA", hw_cargador() & 1 ? "CARGADOR" : "USB"); break;
    case 44: hw_arranque(&ar); n = "Batería al arrancar"; v = ar.como == HW_ARR_BAT_NO_CREIBLE ? (ar.lectura < 0 ? "SIN LECTURA (se siguió)" : ui_fmt("NO CREIBLE: %d mV", ar.mv))
                                                           : ar.como == HW_ARR_CON_CARGADOR ? "con cargador o USB" : ui_fmt("bien, %d mV", ar.mv); break;
    case 45: n = "Energía (PMU)"; v = "pulsa OK"; break;
    case 46: { int p = hw_nor_proteccion(); n = "Flash"; v = !p ? (hw_formato_pendiente() ? "SIN FORMATO (pregunta al arrancar)" : "chip reconocido, se escribe")
                   : p & HW_PROT_SEGURO ? "SIN ESCRITURA: modo seguro" : p & HW_PROT_CHIP ? "SIN ESCRITURA: chip no esperado"
                   : p & HW_PROT_CFI ? "SIN ESCRITURA: CFI no cuadra" : "SIN ESCRITURA: binario sin ella"; break; }
    /* (R2; auditoría de S7) lo que antes solo se sabía abriendo «Energía (PMU)» o no se sabía */
    case 48: hw_pmu_salud(&ps); n = "Carga a 0 (61, 63)"; v = ps.carga_confirmada ? "CONFIRMADA (10, 00)"
                 : ui_fmt("SIN CONFIRMAR (%d, %d)%s", ps.r61, ps.r63, ps.lista_mal ? ui_fmt(", lista: %u mal", ps.lista_mal) : ""); break;
    case 49: hw_pmu_salud(&ps); n = "I2C topes/reint./recup."; v = ui_fmt("%u / %u / %u, %u nack%s", ps.i2c_topes, ps.i2c_reintentos, ps.i2c_recuperados, ps.i2c_nack, sh_pmu_mudo() ? " PMU MUDO" : ""); break;
    case 50: hw_pmu_salud(&ps); n = "Tecla enc.: vías"; v = ui_fmt("foto %s, nivel %s, PMU %s%s", ps.enc_foto_armada ? "sí" : "NO", ps.enc_nivel_armado ? "sí" : "NO",
                 ps.enc_pmu_probado ? "probado" : ps.enc_pmu_armado ? "sí" : "NO", ps.enc_descartes ? ui_fmt(", %u desc.", ps.enc_descartes) : ""); break;
    case 47: n = "Modo seguro"; v = hw_modo_seguro() ? "SI (tecla C al encender)" : "no"; break;
    case 32: n = "Pantalla"; v = sh_pantalla_estado() == EN_VIVA ? "encendida" : sh_pantalla_estado() == EN_ATENUADA ? "atenuada" : "apagada"; break;
    case 33: n = "Teclado"; v = sh_bloqueado() ? "BLOQUEADO" : "libre"; break;
    case 34: n = tapa_nombre(); v = ui_fmt("%s, %u cambios", sh_tapa_cerrada() ? "cerrado" : "abierto", hw_tapa_cambios()); break;
    case 35: n = "Reposo profundo"; v = sa.dormidas ? ui_fmt("%u veces, %u s", sa.dormidas, sa.ms_dormido / 1000) : "aún ninguno"; break;
    case 36: n = "  despertares"; v = ui_fmt(placa()->tapa_tipo == TAPA_DESLIZADOR ? "tecla %u, desliz. %u" : "tecla %u, tapa %u", sa.despertares_tecla, sa.despertares_tapa); break;
    case 37: n = "Pila libre SVC/IRQ"; v = sa.pila_svc_libre || sa.pila_irq_libre ? ui_fmt("%u / %u B, %u exc.%s", sa.pila_svc_libre, sa.pila_irq_libre, sa.excepciones, sa.esperas_cortadas ? ui_fmt(" ESPERAS CORTADAS %u", sa.esperas_cortadas) : "") : "sin vigilar"; break;
    /* la zona de ficheros de usuario (W6U, pista S6) */
    case 38: n = "Ficheros de usuario"; v = !us.montado ? "no hay zona" : us.escribible ? ui_fmt("%u x %u KB", us.bloques, us.tam_bloque / 1024) : "solo lectura"; break;
    case 39: n = "  ficheros"; v = us.montado ? ui_fmt("%u, %s", us.ficheros, sh_tam(us.ocupado)) : "-"; break;
    /* el podómetro como servicio (pista S2, shell/pasos.c) */
    case 41: n = "Podómetro"; v = !pa.activo ? "apagado" : !pa.hay_sensor ? "el PMU no contesta" : ui_fmt("%u pasos, %s", sh_pasos()->pasos, pa.dormido ? "sensor dormido" : "midiendo"); break;
    case 42: n = "  muestras"; v = ui_fmt("%u + %u sin sensor, %u tarde", pa.muestras, pa.sin_muestra, pa.tarde); break;
    case 40: n = "  libre"; v = us.montado ? ui_fmt("%s (%u limp., %u err.)", sh_tam(us.libre), us.limpiezas, us.errores) : "-"; break;
    /* (I1) la Memoria (W6F): la situación, el espacio, el desgaste y el porqué de un «solo lectura» (condición 4 de la auditoría) */
    case 51: case 52: case 53: case 54: {
        HwW6fEstado w; hw_w6f_estado(&w);
        if (i == 51) { n = "Memoria (W6F)"; v = sh_w6f_situacion(&w); }
        else if (w.situacion != HW_W6F_MONTADO) { n = i == 52 ? "  objetos" : i == 53 ? "  borrados" : "  estado"; v = "-"; }
        else if (i == 52) { n = "  objetos"; v = ui_fmt("%u de %u, libre %s", w.objetos, w.max_obj, sh_tam(w.libre)); }
        else if (i == 53) { n = "  borrados"; v = ui_fmt("%u..%u, %u malos, %u/%u", w.borrados_min, w.borrados_max, w.bloques_malos, w.borrados, w.programas); }
        else { n = "  estado"; v = w.solo_lectura ? ui_fmt("SOLO LECTURA: %s", sh_w6f_sl(w.solo_lectura)) : w.deficit ? "FALTA UN BLOQUE DE RESERVA (borrar sí)" : ui_fmt("bien, %u pasos de fondo", w.pasos_fondo); }
        break; }
    /* (2026-10-20) EL USB EN EL APARATO: enumera, pero el MTP no contestaba. Lo del driver (IRQ, eventos, perdidos, errores) y lo de la
     * pila (resets, SETUP, bytes de los endpoints de datos, contenedores) dicen en qué tramo se pierde. */
    case 55: { UsbInfo u; usb_info(&u); n = "USB"; v = ui_fmt("est %u cfg %u, irq %u ev %u perd %u err %u", u.estado, u.configuracion, udc_cta.irqs, udc_cta.eventos, udc_cta.perdidos, udc_cta.errores); break; }
    case 56: { UsbInfo u; usb_info(&u); n = "  datos"; v = ui_fmt("rst %u set %u rx %u tx %u cont %u st %u", u.resets, u.setups, u.rx_bytes, u.tx_bytes, u.contenedores_in, u.stalls); break; }
    /* (M2, 2026-10-20) LA MEMORY STICK EN EL APARATO: «no pasa a 4 bits» era el aviso; ahora cae sola a 1 bit. Aquí se ve en qué bus quedó,
     * cuántas veces falló el paso a 4 bits y, en serie, si el DB2020 da la INT de la tarjeta (sí / no / ? = aún no se sabe; sinINT = respuestas
     * halladas preguntando, sin su INT). */
    case 57: case 58: case 59: {
        const MsInfo *m = ms_info ? ms_info() : 0;
        static const char *const est[] = { "fuera", "lista", "dormida", "MALA", "preparando" };
        n = i == 57 ? "Tarjeta (M2)" : i == 58 ? "  bus" : "  cuentas";
        if (!m) v = "-";
        else if (i == 57) v = m->estado == MS_FUERA ? "no hay" : ui_fmt("%s, %u MB%s%s", est[m->estado < 5 ? m->estado : 3], m->sectores / 2048, m->motivo ? ": " : "", m->motivo ? ms_motivo(m->motivo) : "");
        else if (i == 58) v = !m->serie ? (m->paralelo_bien ? ui_fmt("4 bits (bien %u, espera %s %u ms)", m->paralelo_bien, m->par_fin == MS_PAR_FIN_INT ? "INT" : m->par_fin == MS_PAR_FIN_CED ? "CED" : "tope", m->par_ms) : "4 bits (sin probar)")
                                        : ui_fmt("1 bit: 4b fallo en %s t=%u, INT %s", ms_par_fase ? ms_par_fase(m->par_fase) : "?", m->par_t,      /* (M2 ag) el tramo y su código */
                                                 m->int_serie == MS_INT_SERIE_SI ? "si" : m->int_serie == MS_INT_SERIE_NO ? "NO" : "?");
        else v = ui_fmt("enc %u tpc %u t/o %u crc %u sinINT %u rec %u", m->encendidos, m->tpc, m->e_tiempo, m->e_crc, m->e_int, m->recuperaciones);
        break; }
    }
    return sh_fila(n, v, sel);
}
static void opcion_si(void *d) { (void)d; if (sh_reiniciar() != HW_OK) ui_aviso("Reiniciar", "No disponible en esta plataforma."); }
static void apagar_si(void *d) { (void)d; sh_apagar(); }        /* pantalla de despedida y hw_apagar() */
static void borrar_si(void *d)
{
    int r;
    (void)d;
    sh_pantalla_arranque("borrando los ajustes guardados…");
    r = hw_borrar_ajustes();
    ui_forzar_todo();
    ui_aviso("Ajustes", r == HW_OK ? "Ajustes guardados borrados. Apaga y enciende sin la tecla C: arrancará de fábrica." : r == HW_NO_DISPONIBLE ? "No se puede: la memoria está protegida." : "El borrado ha fallado.");
}
/* (S7) LA PRUEBA DEL PERRO: un bucle infinito de verdad, con las interrupciones abiertas. A los 10 s tiene que salir la pantalla
 * de fallo «EL SISTEMA NO RESPONDE» y, 15 s después, reiniciarse. En el PC no hay perro: no se cuelga. */
static void colgar_si(void *d)
{
    (void)d;
#if TELEFONO
    hw_log("PRUEBA DEL PERRO: bucle infinito a proposito\n");
    for (;;) { }
#else
    ui_aviso("Perro", "En el PC no hay perro que probar.");
#endif
}
static void borrar_usr_si(void *d)
{
    int r;
    (void)d;
    sh_pantalla_arranque("borrando Mis archivos…");
    r = hw_borrar_usuario();
    ui_forzar_todo();
    ui_aviso("Mis archivos", r == HW_OK ? (hw_modo_seguro() ? "Mis archivos borrado entero. Apaga y enciende sin la tecla C: se le dará formato de nuevo." : "Mis archivos vaciado y con formato nuevo: ya se puede guardar otra vez.") : r == HW_NO_DISPONIBLE ? "No se puede: la memoria está protegida." : "El borrado ha fallado.");
}
/* (R2; auditoría de datos, A1) DOBLE confirmación, las dos con el foco en «No» */
static void borrar_usr_seguro(void *d) { ui_confirmar_no("Vaciar Mis archivos", "SEGUNDA y última pregunta: se van a perder TODOS los archivos de Mis archivos. ¿Seguro?", borrar_usr_si, d); }
/* (2026-10-20) LA PRUEBA DEL ALTAVOZ (plat/hw_w580.c, tmp/sonido_re/informe.md §6.1): cada tramo se pinta en pantalla; se escucha
 * con el teléfono pegado a la oreja. Débil: las placas que no la tienen (el PC) dicen que no. */
int hw_prueba_altavoz(void (*paso)(int tramo, const char *que)) __attribute__((weak));
static void alt_paso(int t, const char *que) { sh_pantalla_arranque(ui_fmt("Altavoz %d: %s", t, que)); }
static void altavoz_si(void *d)
{
    (void)d;
    if (!hw_prueba_altavoz) { ui_aviso("Altavoz", "Esta plataforma no tiene la prueba del altavoz."); return; }
    hw_prueba_altavoz(alt_paso);
    ui_forzar_todo();
    ui_aviso("Altavoz", "Prueba terminada. ¿En qué tramo (0 a 6) se oyó algo: pop, soplido, pitido o zumbido?");
}
static void fallo_si(void *d) { (void)d; hw_panico("PRUEBA DE LA PANTALLA DE FALLO", "La has pedido tú desde Sistema -> Opciones. Nada se ha roto."); }
static void opcion(int i, void *d)
{
    (void)d;
    if (i == 0) ui_confirmar("Reiniciar", "¿Reiniciar el sistema? Lo guardado (ajustes, notas, alarma) sigue ahí.", opcion_si, NULL);
    else if (i == 1) ui_confirmar("Apagar", "¿Apagar el teléfono? Lo guardado sigue ahí.", apagar_si, NULL);
    else if (i == 3) ui_empujar(app_energia_pmu());
    else if (i == 4) ui_confirmar_no("Prueba del perro", "¿Colgar el sistema a propósito (bucle infinito)? A los 10 s debe salir la pantalla de fallo y reiniciarse solo.", colgar_si, NULL);
    else if (i == 6) ui_confirmar("Prueba del altavoz", "Unos 20 s por tramos, diciendo cada uno en pantalla. Pega el teléfono a la oreja y fíjate en qué tramo se oye algo.", altavoz_si, NULL);
    else if (i == 7) ui_empujar(app_banco_hw());
    else if (i == 8) ui_empujar(app_dsp_diag());
    else if (i == 9) ui_empujar(app_mp3_diag());
    else if (i == 10) ui_confirmar_no("Borrar ajustes", "¿Borrar TODOS los ajustes guardados (tema, alarma, fondo…)? Los archivos no se tocan.", borrar_si, NULL);
    else if (i == 5) ui_confirmar_no("Vaciar Mis archivos", "¿Borrar ENTERA la zona de Mis archivos (todos tus ficheros: notas, capturas, música copiada)? Es la salida si Mis archivos se ha quedado sin poder guardar ni borrar, o si un fichero dañado cuelga el arranque. Los ajustes no se tocan. No se puede deshacer.", borrar_usr_seguro, NULL);
    else ui_confirmar_no("Pantalla de fallo", "¿Provocar un fallo a propósito para ver la pantalla de fallo? El teléfono se reiniciará.", fallo_si, NULL);
}
Widget *app_usb_diag(void);                     /* apps/sistema/usb_diag.c: «Diagnóstico USB» (2026-10-20), OK en las filas 55-56 */
Widget *app_tarjeta_m2(void);                   /* apps/sistema/tarjeta_m2.c: «Tarjeta (M2)» a fondo (M2 ag, 2026-10-21), OK en las filas 57-59 */
static void activar(Ctx *c, int i, void *dato)  /* (S7) OK en cualquier fila: «Energía (PMU)»; en las del USB, «Diagnóstico USB» */
{
    int f = i >= 0 && i < n_vis ? vis[i] : i;
    (void)c; (void)dato;
    ui_empujar(f == 55 || f == 56 ? app_usb_diag() : f >= 57 && f <= 59 ? app_tarjeta_m2() : app_energia_pmu());   /* (M2 ag) 57-59: la tarjeta a fondo */
}
static int tecla(Ctx *t, int k, void *dato)
{
    static const char *const ops[] = { "Reiniciar", "Apagar", "Ver la pantalla de fallo", "Energía (PMU)", "Probar el perro (colgar)", "Vaciar Mis archivos", "Prueba del altavoz", "Banco acel/audio", "Diagnóstico DSP", "Diagnóstico MP3",
        "Borrar ajustes guardados" };
    if (k == K_SEL_IZQ) {
        int n = hw_modo_seguro() ? 11 : 10;
        ui_opciones("Sistema", ops, n, opcion, NULL);        /* el borrado de AJUSTES, solo en modo seguro; vaciar Mis archivos, siempre */
        return 1;
    }
    if (k == K_OK) return 1;
    return sh_teclas(t, k, dato);
}
static Widget *construir(Ctx *c)
{
    SistemaE *s = ui_estado(c);
    return Teclas(tecla, Pantalla("Sistema", ListaPerezosa(.n = (vis_hacer(), n_vis), .fila = fila, .al_activar = activar, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila), "Opciones", "Atrás"));
}
COMPONENTE_CON_ESTADO(Sistema, SistemaE, construir, iniciar);
Widget *app_sistema(void) { return UI_W(Sistema); }
