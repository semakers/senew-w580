/* cg12_a.c — (1.2, pista APX) las envolturas del grupo A de la tabla (api/cargable12_a.h). Mismas reglas que cg_tabla.c:
 * todo puntero de la app se comprueba (cg_en_app / cg_legible) o se copia; nada del sistema sale por puntero.
 *
 * Lo que una app puede dejar retenido de este grupo, y que cg12_a_soltar() suelta al descargarla:
 *   - las retenciones W5_RET_* (pantalla encendida, teclas de música);
 *   - ajustes cambiados con guardar = 0 y aún no guardados: se GUARDAN (lo que el usuario puso no se pierde);
 *   - la serie: lo que haya quedado en el anillo de recepción sin leer se tira (el siguiente que escuche, p. ej.
 *     «Recibir por serie», no debe ver la cola de la consola; esa pantalla ya descarta lo viejo al abrirse, pero así
 *     no depende de ello).
 * NO se suelta (es a propósito): el temporizador y la alarma son servicios del sistema y siguen en marcha con la app
 * cerrada; la hora puesta, puesta queda; el contador de pasos a cero, a cero queda. El acelerómetro en vivo
 * (ap_acel, 1.0) lo suelta el cargador. */
#include "cargador.h"

static struct { u8 ret, ajustes_pend, serie; } a12;

void cg12_a_soltar(void)                  /* al descargar la app: suelta lo que tuviera retenido de este grupo */
{
    if (a12.ret & W5_RET_ENERGIA) sh_energia_retener(0);
    if (a12.ret & W5_RET_TECLAS_MUSICA) sh_musica_sin_teclas(0);
    if (a12.ajustes_pend) sh_guardar_ajustes();
    if (a12.serie) { u8 b[64]; int k = 0; while (k++ < 32 && hw_serie_leer(b, sizeof b) > 0) ; }
    a12.ret = a12.ajustes_pend = a12.serie = 0;
}

/* ---- ajustes: lista blanca ------------------------------------------------------------------------------------------- */
int t_ap_ajuste(int clave)
{
    switch (clave) {
    case W5_AJ_HORA12: return aj.hora12 ? 1 : 0;
    case W5_AJ_ALARMA_ON: return aj.alarma_on ? 1 : 0;
    case W5_AJ_ALARMA_H: return aj.alarma_h;
    case W5_AJ_ALARMA_M: return aj.alarma_m;
    case W5_AJ_VOLUMEN: return aj.volumen;                                    /* (1.5) */
    case W5_AJ_VOLUMEN_MUSICA: { MuInfo m; sh_musica_abrir(); sh_musica_info(&m); return m.volumen; }   /* (1.5) lo recuperado, no el de antes de abrir */
    }
    return W5_ERROR;
}
/* (1.5, cargables/sdk/w5_sonido.h) LOS DOS VOLÚMENES, 0..W5_VOL_MAX. Sin W5_CAP_RELOJ (eso es de la alarma). El de tonos y
 * avisos es un ajuste más (aj.volumen: con guardar = 0 queda pendiente y se guarda al cerrar la app); el de la música es del
 * servicio, que lo apunta él en su silencio (sh_musica_paso), o ya con guardar = 1. */
static int volumen_poner(int clave, int valor, int guardar)
{
    if (valor < 0 || valor > W5_VOL_MAX) return W5_ERROR;
    if (clave == W5_AJ_VOLUMEN_MUSICA) {
        sh_musica_volumen(valor);
        if (guardar) sh_musica_guardar_ya();
        return W5_OK;
    }
    aj.volumen = (u8)valor;
    if (!valor) sh_callar();                                                  /* como sh_sonido_volumen: en silencio, ni lo que sonaba */
    if (!guardar) { a12.ajustes_pend = 1; return W5_OK; }
    a12.ajustes_pend = 0;
    return sh_guardar_ajustes() == HW_OK ? W5_OK : W5_ERROR;
}
int t_ap_ajuste_poner(int clave, int valor, int guardar)
{
    if (clave == W5_AJ_VOLUMEN || clave == W5_AJ_VOLUMEN_MUSICA) return volumen_poner(clave, valor, guardar);
    CG_PIDE(W5_CAP_RELOJ, W5_ERROR);
    switch (clave) {
    case W5_AJ_ALARMA_ON: if (valor != 0 && valor != 1) return W5_ERROR; aj.alarma_on = (u8)valor; break;
    case W5_AJ_ALARMA_H: if (valor < 0 || valor > 23) return W5_ERROR; aj.alarma_h = (u8)valor; break;
    case W5_AJ_ALARMA_M: if (valor < 0 || valor > 59) return W5_ERROR; aj.alarma_m = (u8)valor; break;
    default: return W5_ERROR;               /* W5_AJ_HORA12 y todo lo demás: solo desde Ajustes */
    }
    if (!guardar) { a12.ajustes_pend = 1; return W5_OK; }
    a12.ajustes_pend = 0;
    return sh_guardar_ajustes() == HW_OK ? W5_OK : W5_ERROR;
}

/* ---- la hora ---------------------------------------------------------------------------------------------------------- */
int t_ap_hora_poner(const W5Fecha *f)
{ CG_PIDE(W5_CAP_RELOJ, W5_ERROR);
    HwFecha h;
    W5Fecha c;
    if (!cg_en_app(f, sizeof *f)) return W5_ERROR;
    memcpy(&c, f, sizeof c); f = &c;                 /* (INT2) copiada: se valida y se usa la MISMA */
    if (f->anio < 2000 || f->anio > 2099 || f->mes < 1 || f->mes > 12 || f->dia < 1 || f->dia > 31 || f->hora > 23 || f->min > 59 || f->seg > 59)
        return W5_ERROR;
    memset(&h, 0, sizeof h);
    h.anio = f->anio; h.mes = f->mes; h.dia = f->dia; h.hora = f->hora; h.min = f->min; h.seg = f->seg;
    return sh_poner_hora_local(&h) == HW_OK ? W5_OK : W5_ERROR;
}
char *t_ap_zona_texto(void)
{
    const char *z;
    if (ui_arena_libre() < 512u + 3072u) { cg_matar("La app agotó la memoria de un cuadro (demasiados widgets o texto)."); return NULL; }
    z = sh_zona();
    return ui_fmt("%s", z);                 /* sh_zona ya da una cadena de la arena; se copia por si un día no */
}

/* ---- el temporizador del sistema (lo vigila el standby: shell/shell.c) ------------------------------------------------- */
int t_ap_temporizador_poner(unsigned ms)
{ CG_PIDE(W5_CAP_RELOJ, W5_ERROR);
    if (!ms) { sh_temporizador_fin = 0; return W5_OK; }
    if (ms > 100u * 3600u * 1000u) return W5_ERROR;       /* 100 h: más que cualquier cuenta atrás de una persona */
    sh_temporizador_fin = hw_ms() + ms;
    if (!sh_temporizador_fin) sh_temporizador_fin = 1;
    return W5_OK;
}
int t_ap_temporizador_queda(void)
{
    u32 q;
    if (!sh_temporizador_fin) return W5_NO_DISPONIBLE;
    q = sh_temporizador_fin - hw_ms();
    return (i32)q < 0 ? 0 : (int)q;
}

/* ---- retenciones -------------------------------------------------------------------------------------------------------- */
int t_ap_retener(int que, int si)
{
    if (que != W5_RET_ENERGIA && que != W5_RET_TECLAS_MUSICA) return W5_ERROR;
    if (cg_estado() != CG_VIVA) return W5_ERROR;
    si = !!si;
    if (!!(a12.ret & que) == si) return W5_OK;
    a12.ret = (u8)(si ? a12.ret | que : a12.ret & ~que);
    if (que == W5_RET_ENERGIA) sh_energia_retener(si);
    else sh_musica_sin_teclas(si);
    return W5_OK;
}

/* ---- el podómetro (shell/pasos.c): una FOTO, campo a campo ----------------------------------------------------------- */
static short corto(int v) { return (short)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
int t_ap_pasos(W5Pasos *p)
{
    const Podometro *d;
    PasosInfo i;
    CG_PIDE(W5_CAP_PASOS, W5_ERROR);                 /* (INT2; AUD10 MEDIO) el sensor solo para quien lo declaró, como ap_pasos_a_cero */
    if (!cg_en_app(p, sizeof *p)) return W5_ERROR;
    if (!(CG12_CAPS_A & W5_CAP_PASOS)) return W5_NO_DISPONIBLE;
    d = sh_pasos();
    sh_pasos_info(&i);
    memset(p, 0, sizeof *p);
    p->pasos = d->pasos; p->muestras = i.muestras; p->sacudidas = i.sacudidas; p->fallos = i.fallos;
    p->dist_dm = d->dist > 0 ? (unsigned)(d->dist * 10.0f) : 0; p->kmh10 = d->kmh > 0 ? (unsigned)(d->kmh * 10.0f) : 0;
    p->idle = d->idle; p->version = i.version;
    p->x = (short)(i.x - HW_ACEL_CERO); p->y = (short)(i.y - HW_ACEL_CERO);
    p->gx = corto((int)d->g[0]); p->gy = corto((int)d->g[1]);
    p->crudo_x = i.x; p->crudo_y = i.y; p->eje = d->eje; p->hz = d->hz;
    p->andando = d->andando; p->inactivo = d->inactivo; p->hay_sensor = i.hay_sensor; p->gesto = (unsigned char)(i.gesto & 7);
    p->candidatos = (unsigned char)(d->c > 255 ? 255 : d->c); p->activo = i.activo; p->dormido = i.dormido;
    return W5_OK;
}
int t_ap_pasos_a_cero(void)
{ CG_PIDE(W5_CAP_PASOS, W5_ERROR);
    if (!(CG12_CAPS_A & W5_CAP_PASOS)) return W5_NO_DISPONIBLE;
    sh_pasos_a_cero();
    return W5_OK;
}

/* ---- la serie (UART 0, 115200 8N1; api/hw.h). Por sondeo: el anillo lo llena la IRQ. ------------------------------------ */
#define SERIE_TOPE 4096u
int t_ap_serie_leer(void *buf, unsigned max)
{ CG_PIDE(W5_CAP_SERIE, W5_ERROR);
    if (!(CG12_CAPS_A & W5_CAP_SERIE)) return W5_NO_DISPONIBLE;
    if (!max) return 0;
    if (max > SERIE_TOPE) max = SERIE_TOPE;
    if (!cg_en_app(buf, max)) return W5_ERROR;
    a12.serie = 1;
    return hw_serie_leer((u8 *)buf, (int)max);
}
int t_ap_serie_escribir(const void *d, unsigned n)
{ CG_PIDE(W5_CAP_SERIE, W5_ERROR);
    if (!(CG12_CAPS_A & W5_CAP_SERIE)) return W5_NO_DISPONIBLE;
    if (!n) return 0;
    if (n > SERIE_TOPE || !cg_en_app(d, n)) return W5_ERROR;
    a12.serie = 1;
    return hw_serie_escribir((const u8 *)d, (int)n);
}
int t_ap_serie_conectado(void) { return (CG12_CAPS_A & W5_CAP_SERIE) && hw_serie_conectado() ? 1 : 0; }

/* ---- las teclas crudas ----------------------------------------------------------------------------------------------------- */
int t_ap_teclas_mapa(unsigned char *col)
{ CG_PIDE(W5_CAP_TECLAS_CRUDAS, W5_ERROR);
    u8 c[7];
    int n;
    if (!cg_en_app(col, 7)) return W5_ERROR;
    if (!(CG12_CAPS_A & W5_CAP_TECLAS_CRUDAS)) return W5_NO_DISPONIBLE;
    memset(c, 0, sizeof c);
    n = hw_teclas_mapa(c);
    memcpy(col, c, 7);
    return n;
}
int t_ap_celda_nombre(int celda, char *buf, unsigned max)
{
    const char *n;
    unsigned l;
    if (!max || max > 256 || !cg_en_app(buf, max)) return W5_ERROR;
    if (!(CG12_CAPS_A & W5_CAP_TECLAS_CRUDAS)) return W5_NO_DISPONIBLE;
    if (celda < 0 || celda > 34) return W5_ERROR;
    n = hw_celda_nombre(celda);
    if (!n) n = "";
    for (l = 0; n[l] && l + 1 < max; l++) buf[l] = n[l];
    buf[l] = 0;
    return (int)l;
}
