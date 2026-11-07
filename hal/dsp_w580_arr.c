/* EL DSP DEL W580i, parte 2: EL ARRANQUE (DSP_RM_LoadAllMemories 0x4469F7A4 y lo que el original hace antes y después), la petición de versión y el
 * apagado. El transporte (SYSCON, DMA, buzón) está en hal/dsp_w580.c. Orden, valores y direcciones: tmp/audio_ag/informe.md e informe2.md;
 * la comparación con las escrituras del ORIGINAL, tmp/audio_ag/comparar.py. TODO con tope: nada se cuelga. */
#include "dsp_w580_i.h"

static u16 sv12, sv18, sv2c;                       /* SYSCON +12, +18, +2C como estaban ANTES del arranque (para dsp_apagar) */
static u8 guardado;

/* ---- la ventana de memoria del DSP: 0x441FE4DC [observado, desensamblado; ejecutado: o1.qemu.log 132150..173020] ---------------------------
 * Pares de palabras: +28 = (dir + índice tras la primera) & 0x7FFFFF, +2C = (primera << 16) | segunda, +24 = 1 y se espera +24 == 0. Un principio impar
 * o un final suelto leen antes el par (+24 = 2) para conservar la mitad vecina. El original espera sin tope; aquí 100 000 vueltas y error. */
static int vent_espera(void)
{
    u32 t;
    for (t = 0; t < 100000u; t++) if (!D32(0x24)) return 0;
    return -1;
}
static int ventana(u32 dir, u32 n, const u16 *w)
{
    const u32 M = 0x7FFFFFu;
    u32 i = 0, v, hi;
    if (!n) return 0;
    if (dir & 1u) {                                /* 0x441FE4F0 */
        dir--;
        D32(0x28) = dir & M; D32(0x24) = 2; if (vent_espera()) return -1;
        v = D32(0x2C) & 0xFFFF0000u;
        D32(0x28) = dir & M; D32(0x2C) = v | w[0]; D32(0x24) = 1; if (vent_espera()) return -1;
        i = 1; dsp_st.ventana_esc++;
    }
    while (i < n) {
        hi = (u32)w[i++] << 16;
        if (i >= n) {                              /* 0x441FE520: la última, suelta */
            D32(0x28) = (dir + i) & M; D32(0x24) = 2; if (vent_espera()) return -1;
            v = (D32(0x2C) & 0xFFFFu) | hi;
            D32(0x28) = (dir + i) & M; D32(0x2C) = v; D32(0x24) = 1; if (vent_espera()) return -1;
            dsp_st.ventana_esc++;
            break;
        }
        v = hi | w[i];
        D32(0x28) = (dir + i) & M; D32(0x2C) = v; D32(0x24) = 1; if (vent_espera()) return -1;
        dsp_st.ventana_esc++;
        i++;
    }
    return 0;
}

/* fase 1 de una imagen (0x4469FE04): FUN_44227CCC(0) y las secciones en orden hasta la primera de n = 0 o con dirección > 0x13FFF (break) */
typedef struct { const DspImagen *img; u32 k, pos, resto, dir_sig; } Cursor;
static int fase1(Cursor *c, const DspImagen *img)
{
    c->img = img; c->k = 0; c->pos = 0; c->resto = 0; c->dir_sig = 0;
    if (!img || !img->n_sec || !img->tab || !img->dat) return DSP_E_IMAGEN;
    dspi_ctl0();
    dsp_st.gate5 = 1;                              /* ctl0 abre las puertas 5, 4, 3, 2: a partir de aquí el DSPIF se puede leer */
    for (; c->k < img->n_sec; c->k++) {
        u32 n = img->tab[c->k][0], d = img->tab[c->k][1];
        if (!n || d > 0x13FFFu) break;
        if (c->pos + n > img->n_pal) return DSP_E_IMAGEN;
        if (ventana(d, n, img->dat + c->pos)) return DSP_E_VENTANA;
        c->pos += n; dsp_st.ventana_pal += n;
        dspi_latir();
    }
    return c->k < img->n_sec ? 0 : DSP_E_IMAGEN;
}

/* ---- la XRAM: el DSP pide con 0x1800 por el canal 0x24 (palabras 2..3 = tamaño, 4..5 = dirección, la ALTA primero) [observado, 0x4469F744/FB60] ------ */
#define XRAM_MAX 8188u                             /* 0x44227BC4 rechaza más de 0xFFE transferencias de 32 bits */
static u16 b_xram[XRAM_MAX + 32] __attribute__((aligned(32)));

static int esperar_1800(u32 *tam, u32 *dir, u32 ms)
{
    DspMsg m;
    if (dspi_esperar_msg(&m, 0x24, 0x1800, ms)) return -1;
    dsp_st.xram_pet++;
    if (tam) *tam = m.n >= 6 ? ((u32)m.d[2] << 16) | m.d[3] : 0;
    if (dir) *dir = m.n >= 6 ? ((u32)m.d[4] << 16) | m.d[5] : 0;
    return 0;
}
static int enviar_180x(u16 orden) { return dspi_tx_corto(0x24, &orden, 1); }

/* FUN_4469F8E4: llena el búfer intermedio de `size` palabras con registros {n bajo, n alto, dir baja, dir alta, datos}, partiendo una sección entre
 * búferes si hace falta. 0 = búfer lleno (quedan datos), 1 = acabó (cuatro ceros de terminador), < 0 = error */
static int xram_llenar(Cursor *c, u32 size)
{
    const DspImagen *im = c->img;
    u16 *p = b_xram;
    u32 usado = 0, room, n, dir, take, j;
    int cabe;
    for (;;) {
        if (usado + 4u >= size) return 0;
        room = size - usado - 4u;
        if (c->resto) { n = c->resto; dir = c->dir_sig; }
        else {
            if (c->k >= im->n_sec) return DSP_E_XRAM_RELLENO;
            n = im->tab[c->k][0]; dir = im->tab[c->k][1];
            if (n == 0) { p[0] = p[1] = p[2] = p[3] = 0; return 1; }
        }
        cabe = n <= room;
        take = cabe ? n : room;
        if (c->pos + take > im->n_pal) return DSP_E_XRAM_RELLENO;
        p[0] = (u16)take; p[1] = (u16)(take >> 16); p[2] = (u16)dir; p[3] = (u16)(dir >> 16);
        for (j = 0; j < take; j++) p[4 + j] = im->dat[c->pos + j];
        c->pos += take; p += 4 + take; usado += 4 + take; dsp_st.xram_pal += take;
        if (c->resto) { if (cabe) { c->resto = 0; c->k++; } else { c->resto -= take; c->dir_sig += take; } }
        else { if (cabe) c->k++; else { c->resto = n - take; c->dir_sig = dir + take; } }
    }
}
/* FUN_4421E124 / FUN_4421DCC4 caso 4: DMA (memoria -> FA040020, anchura 1, ráfaga 1) hacia la dirección del DSP: +14 &= ~0x18, +18 = dir, +14 |= 2 */
static int xram_mandar(u32 tam, u32 dir_dsp)
{
    dspi_ev_limpiar();
    if (dspi_dma_lanzar(1, b_xram, tam / 2u, 0)) return DSP_E_XRAM_DMA;
    D32(0x14) &= ~0x18u;
    D32(0x18) = dir_dsp & 0x7FFFFFu; D32(0x14) |= 2u;
    dsp_st.xram_trozos++;
    if (dspi_esperar_ev(DSP_EV_FIN_TX, 1000)) { dspi_dma_estado(); dspi_dma_parar(); return DSP_E_XRAM_FIN; }
    dspi_dma_estado();
    return 0;
}
/* (2026-10-25) VERIFICACIÓN de la XRAM: tras el DMA de cada trozo (y ANTES del 0x1801 que hace que el DSP lo copie a su XRAM) se relee por la ventana el
 * búfer del DSP donde lo dejó el DSPIF y se compara con lo enviado. Solo si dsp_xram_verif != 0 (por omisión 0: no cambia nada). Contadores por imagen
 * (0 = común, 1 = arranque, 2 = mp3): trozos comprobados, palabras distintas, y la primera distinta {imagen, trozo, desplazamiento, esperada, leída}. */
volatile u32 dsp_xram_verif, dsp_xram_n_imagen;
volatile u32 dsp_xram_trozos_ok[3], dsp_xram_pal_mal[3], dsp_xram_trozos_mal[3], dsp_xram_primero[5], dsp_xram_fallo_vent;
static void xram_verificar(u32 tam, u32 dir_dsp, u32 trozo)
{
    u32 i, img = dsp_xram_n_imagen > 2u ? 2u : dsp_xram_n_imagen, mal = 0;
    if (dir_dsp + tam > 0x14000u) return;
    for (i = 0; i < tam; i += 2) {
        u32 v, a, b;
        D32(0x28) = (dir_dsp + i) & 0x7FFFFFu; D32(0x24) = 2;
        if (vent_espera()) { dsp_xram_fallo_vent++; return; }
        v = D32(0x2C); a = v >> 16; b = v & 0xFFFFu;
        if (a != b_xram[i] || (i + 1 < tam && b != b_xram[i + 1])) {
            if (!mal && !dsp_xram_pal_mal[0] && !dsp_xram_pal_mal[1] && !dsp_xram_pal_mal[2]) {
                dsp_xram_primero[0] = img; dsp_xram_primero[1] = trozo; dsp_xram_primero[2] = i;
                dsp_xram_primero[3] = ((u32)b_xram[i] << 16) | (i + 1 < tam ? b_xram[i + 1] : 0); dsp_xram_primero[4] = v;
            }
            mal += (a != b_xram[i]) + (i + 1 < tam && b != b_xram[i + 1]);
        }
    }
    if (mal) { dsp_xram_trozos_mal[img]++; dsp_xram_pal_mal[img] += mal; } else dsp_xram_trozos_ok[img]++;
}
static int xram_imagen(Cursor *c, u32 dir_dsp, u32 tam, int ultima)
{
    int r;
    u32 trozo = 0;
    c->resto = 0;                                  /* DAT_20069128 = 0 al empezar cada imagen */
    for (;;) {
        r = xram_llenar(c, tam);
        if (r < 0) return r;
        { int e = xram_mandar(tam, dir_dsp); if (e) return e; }
        if (dsp_xram_verif) xram_verificar(tam, dir_dsp, trozo);
        trozo++;
        if (r) break;
        if (enviar_180x(0x1801)) return DSP_E_BUZON;
        if (esperar_1800(0, 0, 1000)) return DSP_E_NO_1800_MAS;
    }
    if (ultima) {
        if (enviar_180x(0x1802)) return DSP_E_BUZON;
        espera_ms(6);
        return 0;
    }
    if (enviar_180x(0x1801)) return DSP_E_BUZON;
    if (esperar_1800(0, 0, 1000)) return DSP_E_NO_1800_MAS;
    return 0;
}

DspExp dsp_exp = { 0, 0, 1, 0, 0 };     /* r2c = 0 (2026-10-24): NUNCA tocar +2C bits 8:9 (con r2c = 1 el aparato se cuelga en el primer paso de dsp_arrancar; ver memoria syscon-2c-bits-8-9-cuelgan). El experimento sigue en la tecla 9 de «Diagnóstico DSP» */
static void pausa_larga(u32 ms) { while (ms) { u32 t = ms > 10 ? 10 : ms; espera_ms(t); ms -= t; dspi_latir(); } }
/* un mensaje CORTO de los del original (0x1101 por el canal 0x16) y mirar si el DSP lo lee (FA040000 bit 0 = «el buzón ARM->DSP sigue ocupado»): separa «el DSP no atiende el buzón
 * para nada» de «solo falla el mensaje largo» */
static void sondeo_corto(void)
{
    static const u16 w[6] = { 0x1101, 0, 0, 0x1000, 0x007F, 0 };
    u32 t0 = dspi_ms();
    dsp_st.sondeo_hecho = 1; dsp_st.sondeo_ms = -1;
    if (dspi_tx_corto(0x16, w, 6)) return;
    while ((u32)(dspi_ms() - t0) < 500u) {
        if (!(D32(0x00) & 1u)) { dsp_st.sondeo_ms = (int)(dspi_ms() - t0); return; }
        dspi_latir(); espera_ms(1);
    }
}

/* ---- la versión: 0x0201 por el canal 2, mensaje largo (101 palabras de carga -> 112 con el canal) [ejecutado: o1.volcado.txt 3.9327] ---------- */
static int version_i(void)
{
    static u16 carga[101];
    DspMsg m;
    unsigned i, k = 0;
    int rt;
    dsp_st.version[0] = 0;
    for (i = 0; i < 101; i++) carga[i] = 0;
    carga[0] = 0x0201;
    rt = dspi_tx_largo(2, carga, 101);             /* si el flujo no se confirma se espera la respuesta igualmente: dice más */
    if (dspi_esperar_msg(&m, 2, 0, 1000)) return rt ? rt : DSP_E_SIN_VERSION;
    for (i = 1; i < m.n && m.d[i] && k < sizeof dsp_st.version - 1; i++) dsp_st.version[k++] = (char)(m.d[i] >= 32 && m.d[i] < 127 ? m.d[i] : '?');
    dsp_st.version[k] = 0;
    return 0;
}
static int pedir_version_e(void)
{
    int r;
    if (!dsp_st.gate5) return DSP_E_NO_ARRANCADO;
    if (dsp_st.ocupado) return DSP_E_OCUPADO;
    dsp_st.ocupado = 1;
    dspi_arranque_ini();
    dspi_paso_ini(DSP_P_VERSION);
    if (dsp_exp.sondeo) sondeo_corto();
    { int k, n = dsp_exp.reintentos ? dsp_exp.reintentos : 1; r = -1; for (k = 0; k < n && r; k++) { r = version_i(); dsp_st.version_intentos = (u8)(k + 1); } }
    dspi_paso_fin(DSP_P_VERSION, r, (u32)dsp_st.version[0]);
    if (!r) { dsp_st.vivo = 1; dsp_st.paso = DSP_P_VIVO; }
    dsp_st.ocupado = 0;
    dspi_progreso_ya();
    return r;
}

/* ---- parar y apagar ------------------------------------------------------------------------------------------------------------ */
static void parar_nucleo(void)
{
    dspi_camp_activar(0);
    dspi_bloque(2, 1);                             /* el núcleo del DSP a reset; el DSPIF y sus puertas siguen para poder leerlo */
    dsp_st.nucleo = 0;
}
static void apagar_i(void)
{
    dspi_camp_activar(0);
    if (dsp_st.gate5) { dspi_dma_parar(); D32(0x0C) = D32(0x0C) & 0xFFFFFF20u; }   /* canal 4 del DMA parado; sin máscaras de eventos (como el «Interrupts disabled» del original) */
    dspi_bloque(2, 1); dspi_bloque(3, 1);
    dspi_puerta(5, 0); dspi_puerta(4, 0); dspi_puerta(3, 0); dspi_puerta(2, 0);
    if (guardado) {
        dspi_sc(0x12, 0x10, (u16)(sv12 & 0x10u));  /* el bloque 4 (ventana) como estaba */
        if (dsp_st.pcm_abierto) {
            dspi_bloque(20, 1); dspi_puerta(26, 0);   /* FUN_4459F428 estado 2: bloque 20 a reset y puerta 26 cerrada */
            if (sv12 & 0x02u) dspi_bloque(20, 0);     /* y el bloque 20 como lo deja syscon_init (vivo, con la puerta cerrada) */
        }
        dspi_sc(0x2C, 0x0003, (u16)(sv2c & 0x0003u));
        guardado = 0;
    }
    dsp_st.pcm_abierto = 0; dsp_st.gate5 = 0; dsp_st.nucleo = 0; dsp_st.vivo = 0;
}
static void apagar_e(void)
{
    if (dsp_st.ocupado) return;
    dspi_arranque_ini();
    dspi_paso_ini(DSP_P_APAGADO);
    apagar_i();
    dspi_cola_vaciar();
    dsp_st.paso = DSP_P_APAGADO;
    dspi_paso_fin(DSP_P_APAGADO, 0, ((u32)R16(SYSCON + 0x12) << 16) | R16(SYSCON + 0x18));
    dspi_progreso_ya();
}

/* ---- LoadAllMemories: la común + un módulo (ventana, suelta, XRAM) ----------------------------------------------------------------- */
#define FALLA(p, e, x) do { r = dspi_paso_fin((p), (e), (x)); goto falla; } while (0)
/* PARCHES de desarrollo (2026-10-24): ganchos en el código/datos bajos del DSP (<= 0x13FFF) que el anfitrión deja en dsp_parches y que se aplican entre la carga baja y la suelta, SOLO
 * a la imagen de arranque. Formato: [n_bloques, {dir_lo, dir_hi, n, palabras[n]} ...]. Vacío (dsp_parches[0] == 0) = el arranque de siempre. */
u16 dsp_parches[3072];
volatile u32 dsp_parches_modulo;                           /* 0 = los parches van a la imagen de arranque (como siempre); 1 = a la del MÓDULO (p. ej. el MP3) */
static int aplicar_parches(void)
{
    u32 nb = dsp_parches[0], i, p = 1;
    for (i = 0; i < nb; i++) {
        u32 d = (u32)dsp_parches[p] | ((u32)dsp_parches[p + 1] << 16), n = dsp_parches[p + 2];
        if (p + 3 + n > sizeof dsp_parches / sizeof dsp_parches[0] || d + n > 0x14000u) return DSP_E_IMAGEN;
        if (ventana(d, n, &dsp_parches[p + 3])) return DSP_E_VENTANA;
        p += 3 + n;
    }
    return 0;
}
/* EXPERIMENTO (2026-10-24): la init de la caché de instrucciones del DSP (0x10601, la llama el _c_int00 de cada imagen) se SALTA si la palabra 0x5F8 vale 1, y esa
 * palabra no la carga ninguna sección ni la borra nadie: tras una carga anterior (arranque -> MP3) llega a 1 y la caché del decodificador (que corre entero desde la
 * XRAM) se quedaría sin configurar. dsp_5f8_modo: 0 = nada, 1 = solo leerla, 2 = leerla y ponerla a 0 antes de soltar el núcleo. dsp_5f8_visto[k] = lo leído en
 * la carga k (0x10000 | valor; 0xFFFFFFFF = la ventana no contestó). */
volatile u32 dsp_5f8_modo, dsp_5f8_n, dsp_5f8_visto[8];
static void exp_5f8(void)
{
    u32 k = dsp_5f8_n++ % 8u;
    if (!dsp_5f8_modo) return;
    D32(0x28) = 0x5F8u; D32(0x24) = 2;
    dsp_5f8_visto[k] = vent_espera() ? 0xFFFFFFFFu : (0x10000u | (D32(0x2C) >> 16));
    if (dsp_5f8_modo == 2) { static const u16 cero = 0; ventana(0x5F8u, 1, &cero); }
}
static int cargar_i(const DspImagen *mod)
{
    Cursor cur[2];
    u32 tam = 0, dir = 0;
    int r;
    DspMsg m;
    dspi_paso_ini(DSP_P_VENT_COMUN);
    dspi_sc(0x12, 0, 0x10);                        /* FUN_4469F7A4: +12 |= 0x10 */
    r = fase1(&cur[0], &dsp_img_comun);
    if (r) FALLA(DSP_P_VENT_COMUN, r, dsp_st.ventana_pal);
    dspi_paso_fin(DSP_P_VENT_COMUN, 0, dsp_st.ventana_pal);
    dspi_paso_ini(DSP_P_VENT_MODULO);
    r = fase1(&cur[1], mod);
    if (r) FALLA(DSP_P_VENT_MODULO, r, dsp_st.ventana_pal);
    dspi_paso_fin(DSP_P_VENT_MODULO, 0, dsp_st.ventana_pal);
    if (dsp_parches[0] && (dsp_parches_modulo ? mod != &dsp_img_arranque : mod == &dsp_img_arranque)) { r = aplicar_parches(); if (r) FALLA(DSP_P_VENT_MODULO, r, 0); }
    exp_5f8();
    dspi_sc(0x12, 0x10, 0);                        /* +12 &= ~0x10 */
    dspi_paso_ini(DSP_P_SUELTA);
    dspi_cola_vaciar();
    dspi_ctl1(); dsp_st.nucleo = 1;
    espera_ms(14);
    dspi_paso_fin(DSP_P_SUELTA, 0, ((u32)R16(SYSCON + 0x12) << 16) | R16(SYSCON + 0x18));
    dspi_paso_ini(DSP_P_ESPERA_1800);
    if (esperar_1800(&tam, &dir, 1000)) FALLA(DSP_P_ESPERA_1800, DSP_E_NO_1800, 0);
    if (tam < 8u || tam > XRAM_MAX || !dir) FALLA(DSP_P_ESPERA_1800, DSP_E_1800_MAL, (tam << 16) ^ dir);
    dsp_st.xram_cap = tam; dsp_st.xram_dir = dir;
    dspi_paso_fin(DSP_P_ESPERA_1800, 0, tam);
    dspi_paso_ini(DSP_P_XRAM_COMUN);
    dsp_xram_n_imagen = 0;
    r = xram_imagen(&cur[0], dir, tam, 0);
    if (r) FALLA(DSP_P_XRAM_COMUN, r, dsp_st.xram_trozos);
    dspi_paso_fin(DSP_P_XRAM_COMUN, 0, dsp_st.xram_trozos);
    dspi_paso_ini(DSP_P_XRAM_MODULO);
    dsp_xram_n_imagen = mod == &dsp_img_arranque ? 1u : 2u;
    r = xram_imagen(&cur[1], dir, tam, 1);
    if (r) FALLA(DSP_P_XRAM_MODULO, r, dsp_st.xram_trozos);
    dspi_paso_fin(DSP_P_XRAM_MODULO, 0, dsp_st.xram_trozos);
    if (!dsp_exp.sin_camp) dspi_camp_activar(1);   /* el SO del DSP ya corre: a partir de aquí puede pedir dormir (ver dspi_campanilla) */
    dspi_paso_ini(DSP_P_FIN);
    (void)dspi_esperar_msg(&m, 0xFFFF, 0, 1000);   /* FUN_4469F744 otra vez: el original espera 1 s un 0x1800 y IGNORA el resultado */
    dspi_paso_fin(DSP_P_FIN, 0, dsp_st.xram_pal);
    return 0;
falla:
    dspi_sc(0x12, 0x10, 0);                        /* el original también deja +12 bit 4 a 0 al fallar */
    return r;
}
volatile u32 dsp_servir_tras_carga_ms = 700u;
static int cargar_e(const DspImagen *mod)
{
    int r;
    if (!mod) return DSP_E_PARAM;
    if (!dsp_st.gate5) return DSP_E_NO_ARRANCADO;
    if (dsp_st.ocupado) return DSP_E_OCUPADO;
    dsp_st.ocupado = 1;
    dspi_arranque_ini();
    r = cargar_i(mod);
    if (r) parar_nucleo();
    else dspi_servir(dsp_servir_tras_carga_ms);   /* (700 por omisión; interruptor 2026-10-25 para probar sin la espera, como el original) el DSP recién reiniciado pide dormir (campanilla) y hasta que se la atienda NO lee su buzón; el sondeo no empieza hasta 500 ms después de soltarlo (regla anticuelgue): sin esta espera un 0x1101 inmediato se queda sin leer y la versión da DSP_E_BUZON */
    dsp_st.ocupado = 0;
    dspi_progreso_ya();
    return r;
}

/* ---- EL ARRANQUE -------------------------------------------------------------------------------------------------------------- */
#ifdef MP3_MIGAS_FLASH
extern void mp3_miga(unsigned n);
#define AMIGA(n) mp3_miga(n)
#else
#define AMIGA(n) ((void)0)
#endif
static int arrancar_e(void)
{
    int r, intentos;
    AMIGA(900);
#ifdef MP3_MIGAS_FLASH
    *(volatile u32 *)0xF3001000u = 0;                   /* el rastro de mensajes de la iRAM empieza de cero */
    *(volatile u32 *)0xF3001800u = 0;
#endif
    if (dsp_st.ocupado) return DSP_E_OCUPADO;
    dspi_camp_activar(0);
    AMIGA(901);
    if (dsp_st.gate5 || dsp_st.vivo) apagar_i();   /* se empieza siempre de cero */
    AMIGA(902);
    intentos = dsp_st.intentos;
    memset(&dsp_st, 0, sizeof dsp_st);
    dsp_st.intentos = (u8)(intentos + 1); dsp_st.ocupado = 1;
    AMIGA(903);
    dspi_arranque_ini();
    dspi_cola_vaciar();
    AMIGA(904);
    dsp_st.s2c_ini = R16(SYSCON + 0x2C);
    AMIGA(920);
    { static const u8 of[10] = { 0x02, 0x04, 0x06, 0x0A, 0x0C, 0x1C, 0x38, 0x54, 0x80, 0x36 }; int k; for (k = 0; k < 10; k++) { AMIGA(930u + (unsigned)k); dsp_st.sc_ini[k] = R16(SYSCON + of[k]); } }
    AMIGA(921);
    /* (v15) SOLO los bits 8 y 9, que el original nunca tiene a 1 (+2C: 0CC0, 0CCC, 0CFC, 0CCF, 0CFF). La v14 escribió 0x0CCC entera y se COLGÓ en el paso 1: los bits 5:4 son el reloj del DMA
     * (el original los sube y baja alrededor de cada DMA) y el PL080 sin reloj cuelga el bus. */
    if (dsp_exp.r2c) { R16(SYSCON + 0x2C) = (u16)(R16(SYSCON + 0x2C) & ~0x0300u); dsp_st.s2c_puesto = R16(SYSCON + 0x2C); }

    AMIGA(905);
    dspi_paso_ini(DSP_P_PREPARA);
    if (!guardado) { sv12 = R16(SYSCON + 0x12); sv18 = R16(SYSCON + 0x18); sv2c = R16(SYSCON + 0x2C); guardado = 1; }
    if (!dsp_img_comun.n_sec || !dsp_img_arranque.n_sec) FALLA(DSP_P_PREPARA, DSP_E_IMAGEN, 0);
    dspi_dma_iniciar();
    dspi_paso_fin(DSP_P_PREPARA, 0, ((u32)sv12 << 16) | sv18);

    dspi_paso_ini(DSP_P_DSPIF);
    dspi_dspif_iniciar();
    dspi_paso_fin(DSP_P_DSPIF, 0, ((u32)R16(SYSCON + 0x12) << 16) | R16(SYSCON + 0x18));

    dspi_paso_ini(DSP_P_PCM);
    dspi_pcm_configurar(); dsp_st.pcm_abierto = 1;
    dspi_paso_fin(DSP_P_PCM, 0, R16(0xFA020000));

    dspi_paso_ini(DSP_P_MASCARAS);
    dspi_mascaras_iniciar();
    dspi_paso_fin(DSP_P_MASCARAS, 0, D32(0x0C));

    dspi_paso_ini(DSP_P_PREBOOT);
    dspi_puerta(26, 1); dspi_bloque(20, 0);
    dspi_paso_fin(DSP_P_PREBOOT, 0, ((u32)R16(SYSCON + 0x12) << 16) | R16(SYSCON + 0x18));

    r = cargar_i(&dsp_img_arranque);
    if (r) {
        if (dsp_st.xram_pet) {                     /* el DSP habló: vale la pena preguntar la versión (dice si el buzón funciona) */
            int e = DSP_E_OK;
            dspi_paso_ini(DSP_P_VERSION);
            e = version_i();
            dspi_paso_fin(DSP_P_VERSION, e, (u32)dsp_st.version[0]);
        }
        parar_nucleo();
        goto fin;
    }
    dspi_paso_ini(DSP_P_VERSION);
    if (dsp_exp.pausa_ms) pausa_larga(dsp_exp.pausa_ms);                     /* (v11) experimento: dejar tiempo al DSP tras la última carga */
    if (dsp_exp.re1802) { enviar_180x(0x1802); pausa_larga(20); }             /* (v12) experimento: reenviar el «fin de carga» por si el DSP no lo tomó */
    if (dsp_exp.sondeo) sondeo_corto();                                      /* (v11) experimento: ¿lee un mensaje corto? */
    { int k, n = dsp_exp.reintentos ? dsp_exp.reintentos : 1; r = -1; for (k = 0; k < n && r; k++) { r = version_i(); dsp_st.version_intentos = (u8)(k + 1); } }
    dspi_paso_fin(DSP_P_VERSION, r, (u32)dsp_st.version[0]);
    if (!r) { dsp_st.vivo = 1; dsp_st.paso = DSP_P_VIVO; dspi_paso_fin(DSP_P_VIVO, 0, 0); }
    goto fin;
falla:
    parar_nucleo();
fin:
    dsp_st.ms_total = dspi_desde_inicio();
    dsp_st.ocupado = 0;
    dspi_progreso_ya();
    return r;
}

/* (2026-10-26) las entradas públicas, con el cerrojo del bucle frente al tic (dsp_bucle_tomar, hal/dsp_w580.c): el arranque, la carga, la versión
 * y el apagado tocan la ventana, el buzón y el DMA, y el alimentador del tic no puede entrar a medias */
int dsp_pedir_version(void) { int r; dsp_bucle_tomar(); r = pedir_version_e(); dsp_bucle_soltar(); return r; }
void dsp_apagar(void) { dsp_bucle_tomar(); apagar_e(); dsp_bucle_soltar(); }
int dsp_cargar(const DspImagen *mod) { int r; dsp_bucle_tomar(); r = cargar_e(mod); dsp_bucle_soltar(); return r; }
int dsp_arrancar(void) { int r; dsp_bucle_tomar(); r = arrancar_e(); dsp_bucle_soltar(); return r; }
