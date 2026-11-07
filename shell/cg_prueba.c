/* cg_prueba.c — SOLO EN EL PC: las órdenes «cargable …» de un guion (plat/pc_main.c), para probar el cargador de
 * verdad con ASan/UBSan. En el teléfono este fichero no compila nada.
 *   cargable estado TEXTO          una línea con el estado y las medidas del cargador
 *   cargable temporizador MS       pone la cuenta atrás del sistema a vencer en MS (para ver la pausa por un aviso)
 *   cargable abrir RUTA            carga y abre ese .sen (ruta del FS: /apps/x.sen, /usuario/x.sen)
 *   cargable ciclos N RUTA         carga y descarga N veces (sin abrir) y otras N abriendo y cerrando; mira que no quede nada
 *   cargable fuzz N SEMILLA RUTA   N variantes dañadas de ese fichero por el cargador (la mitad con los CRC rehechos)
 *   cargable cambia RUTA           (AUD7 B4) un medio que CAMBIA entre las dos lecturas de la tabla de reubicación: la segunda vez una
 *                                  entrada apunta a otra palabra de la imagen cuyo valor NO es una dirección de la app: no se tiene que cargar */
#include "cargador.h"
#if !TELEFONO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc.h"
void ui_arena_vaciar(void);
typedef struct { const u8 *d; u32 n; u32 lecturas; } Mem;
static u32 leer_mem(void *q, u32 desde, void *buf, u32 max)
{
    Mem *m = q;
    m->lecturas++;
    if (desde >= m->n) return 0;
    if (max > m->n - desde) max = m->n - desde;
    memcpy(buf, m->d + desde, max);
    return max;
}
static u32 azar_s;
static u32 azar(void) { azar_s = azar_s * 1664525u + 1013904223u; return azar_s >> 8; }
static void crcs(u8 *f, u32 n)
{
    u32 c;
    if (n < W5X_TAM_CAB) return;
    c = cg_crc(0, f + W5X_TAM_CAB, n - W5X_TAM_CAB - ((W5xCab *)(void *)f)->tam_rec <= n ? n - W5X_TAM_CAB : 0);
    (void)c;
}
static void rehacer_crc(u8 *f, u32 n)
{
    W5xCab c;
    u32 cuerpo;
    if (n < W5X_TAM_CAB) return;
    memcpy(&c, f, sizeof c);
    cuerpo = n - W5X_TAM_CAB;
    if (c.tam_rec <= cuerpo) cuerpo -= c.tam_rec;
    c.crc_cuerpo = cg_crc(0, f + W5X_TAM_CAB, cuerpo);
    c.crc_cab = cg_crc(0, &c, W5X_TAM_CAB - 4);
    memcpy(f, &c, sizeof c);
}
static u8 *fichero(const char *ruta, u32 *n)
{
    u8 *d;
    *n = hw_fs_tam(ruta);
    if (!*n || !(d = malloc(*n + 64))) { printf("cargable: no puedo leer %s\n", ruta); exit(1); }
    if (hw_fs_leer_desde(ruta, 0, d, *n) != *n) { printf("cargable: lectura corta de %s\n", ruta); exit(1); }
    return d;
}
/* (AUD7 B4) el medio que cambia: la primera lectura de la tabla es la buena; las siguientes llevan la entrada «j» cambiada por «nueva» */
typedef struct { Mem m; u32 des_reub, j, nueva, vistas; } Cambia;
static u32 leer_cambia(void *q, u32 desde, void *buf, u32 max)
{
    Cambia *c = q; u32 n = leer_mem(&c->m, desde, buf, max), o = c->des_reub + c->j * 4;
    if (desde == c->des_reub || (desde < o && desde + n > o && desde >= c->des_reub)) { if (c->vistas++ && o >= desde && o + 4 <= desde + n) memcpy((u8 *)buf + (o - desde), &c->nueva, 4); }
    return n;
}
static void cambia(const char *ruta)
{
    char m[160]; u32 tam, imagen, total, j, d = 0, ant, sig, cand = 0, v = 0; W5xCab c;
    u8 *f = fichero(ruta, &tam);
    memcpy(&c, f, sizeof c);
    imagen = c.tam_cod + c.tam_dat; total = imagen + c.tam_bss;
    for (j = 0; j < c.n_reub; j++) {                       /* una entrada y otra palabra alineada entre sus vecinas con un valor FUERA de la app */
        memcpy(&d, f + W5X_TAM_CAB + imagen + j * 4, 4);
        ant = j ? 0 : 0; if (j) memcpy(&ant, f + W5X_TAM_CAB + imagen + (j - 1) * 4, 4);
        sig = imagen - 4; if (j + 1 < c.n_reub) memcpy(&sig, f + W5X_TAM_CAB + imagen + (j + 1) * 4, 4);
        for (cand = (j ? ant + 4 : 0); cand < sig; cand += 4) { memcpy(&v, f + W5X_TAM_CAB + cand, 4); if (v > total && cand != d) break; }
        if (cand < sig) break;
    }
    if (j == c.n_reub) { printf("cargable cambia: %s no tiene ninguna palabra que sirva para la prueba (SIN PROBAR)\n", ruta); free(f); return; }
    {
        Cambia q = { { f, tam, 0 }, W5X_TAM_CAB + imagen, j, cand, 0 };
        int r = cg_cargar_de(leer_cambia, &q, tam, "", m, sizeof m);
        printf("cargable cambia: entrada %u de %u: %08x -> %08x (valor %08x > total %u); lecturas de la tabla %u: %s%s\n", j, c.n_reub, d, cand, v, total, q.vistas,
               r ? "NO SE CARGA (bien): " : "SE HA CARGADO (MAL)", r ? m : "");
        if (!r) cg_descargar();
    }
    free(f);
}
void cg_prueba(const char *orden)
{
    char ruta[128], m[160];
    unsigned n, semilla;
    (void)crcs;
    if (sscanf(orden, "cambia %127s", ruta) == 1) { cambia(ruta); return; }
    if (!strncmp(orden, "estado", 6)) {
        printf("cargable%-22s estado=%d cargas=%u descargas=%u llamadas=%u muertes=%u | bloque=%u B, carga %u ms, pila usada %u B, retrollamadas %u | monticulo=%u elementos=%u rutas=%d\n",
               orden + 6, cg_estado(), cg_med.cargas, cg_med.descargas, cg_med.llamadas, cg_med.muertes, cg_med.tam_bloque, cg_med.t_carga_ms,
               cg_med.pila_usada, cg_med.rets, ui_med.monticulo_usado, ui_med.elementos, ui_rutas());
    } else if (sscanf(orden, "temporizador %u", &n) == 1) {
        sh_temporizador_fin = hw_ms() + n;                 /* la cuenta atrás del sistema vence dentro de n ms: saltará su aviso */
    } else if (sscanf(orden, "abrir %127s", ruta) == 1) {
        Widget *w = cg_abrir_ruta(ruta);
        if (w) ui_empujar(w);
    } else if (sscanf(orden, "ciclos %u %127s", &n, ruta) == 2) {
        u32 tam, i, mon0 = ui_med.monticulo_usado, el0 = ui_med.elementos, mal = 0;
        u8 *d = fichero(ruta, &tam);
        Mem q = { d, tam, 0 };
        for (i = 0; i < n; i++) {
            if (cg_cargar_de(leer_mem, &q, tam, "", m, sizeof m)) { printf("cargable ciclos: la carga %u falla: %s\n", i, m); mal++; break; }
            cg_descargar();
            ui_arena_vaciar();
        }
        printf("cargable ciclos: %u cargas y descargas sin abrir; estado=%d, monticulo %u -> %u, elementos %u -> %u\n", i, cg_estado(), mon0, ui_med.monticulo_usado, el0, ui_med.elementos);
        for (i = 0; i < n && !mal; i++) {
            Widget *w = cg_abrir_ruta(ruta);
            int k;
            if (!w) { mal++; break; }
            ui_empujar(w);
            for (k = 0; k < 3; k++) { pc_reloj_ms += 20; ui_paso(); }
            if (cg_estado() != CG_VIVA) { printf("cargable ciclos: en la vuelta %u la app no queda viva\n", i); mal++; break; }
            ui_sacar();
            for (k = 0; k < 30 && cg_estado() != CG_LIBRE; k++) { pc_reloj_ms += 20; ui_paso(); }
            if (cg_estado() != CG_LIBRE) { printf("cargable ciclos: en la vuelta %u la app no se descarga\n", i); mal++; break; }
        }
        for (i = 0; i < 40; i++) { pc_reloj_ms += 20; ui_paso(); }
        printf("cargable ciclos: %s; abrir y cerrar x%u; estado=%d, monticulo %u -> %u, elementos %u -> %u, cargas=%u descargas=%u\n",
               mal || ui_med.monticulo_usado != mon0 || ui_med.elementos != el0 || cg_med.cargas != cg_med.descargas ? "MAL" : "BIEN",
               n, cg_estado(), mon0, ui_med.monticulo_usado, el0, ui_med.elementos, cg_med.cargas, cg_med.descargas);
        free(d);
    } else if (sscanf(orden, "fuzz %u %u %127s", &n, &semilla, ruta) == 3) {
        u32 tam, i, aceptadas = 0, motivos[16] = { 0 }, k;
        u8 *bueno = fichero(ruta, &tam), *f = malloc(tam + 64);
        static const char *const clave[] = { "No es una app", "formato", "(cabecera)", "otro procesador", "API", "Perfil", "Necesita", "pantalla", "imposible", "grande",
                                             "alinear", "pila", "montón", "entrada", "tamaño no cuadra", "cortado" };
        u32 otros = 0, reub = 0, suma = 0, lect = 0;
        azar_s = semilla * 2654435761u + 1;
        for (i = 0; i < n; i++) {
            u32 t = tam, cambios = 1 + azar() % 4, modo = azar() % 10;
            Mem q;
            memcpy(f, bueno, tam);
            while (cambios--) {
                u32 donde = modo < 5 ? azar() % W5X_TAM_CAB : modo < 8 ? W5X_TAM_CAB + azar() % (tam - W5X_TAM_CAB) : tam - 1 - azar() % (tam > 400 ? 400 : tam - 1);
                u32 que = azar() % 6;
                if (que == 0) f[donde] ^= (u8)(1u << (azar() & 7));
                else if (que == 1) f[donde] = (u8)azar();
                else if (que == 2) f[donde] = 0xFF;
                else if (que == 3) f[donde] = 0;
                else if (que == 4 && donde + 4 <= tam) { u32 v = azar() % 3 == 0 ? 0xFFFFFFFFu : azar() % 3 == 1 ? 0x7FFFFFF0u + azar() % 32 : azar() << 4; memcpy(f + (donde & ~3u), &v, 4); }
                else if (que == 5) t = azar() % 8 == 0 ? azar() % (tam + 1) : t;      /* fichero cortado */
            }
            if (azar() & 1) rehacer_crc(f, t);                 /* la mitad pasan los CRC: lo que se prueba es lo de detrás */
            q = (Mem){ f, t, 0 };
            m[0] = 0;
            if (cg_cargar_de(leer_mem, &q, t, "", m, sizeof m) == 0) { aceptadas++; cg_descargar(); }
            else {
                for (k = 0; k < 16 && !strstr(m, clave[k]); k++) ;
                if (k < 16) motivos[k]++; else if (strstr(m, "reubicaci") || strstr(m, "dirección fuera")) reub++; else if (strstr(m, "suma")) suma++; else if (strstr(m, "leer")) lect++; else { otros++; if (otros < 4) printf("  motivo sin clasificar: %s\n", m); }
            }
            if (cg_estado() != CG_LIBRE) { printf("cargable fuzz: tras la variante %u el cargador no queda libre\n", i); exit(1); }
            ui_arena_vaciar();
        }
        printf("cargable fuzz: %u variantes de %s (semilla %u): %u aceptadas (cambios sin efecto, o con los CRC rehechos y contenido aún válido), %u rechazadas\n",
               n, ruta, semilla, aceptadas, n - aceptadas);
        printf("  rechazos:");
        for (k = 0; k < 16; k++) if (motivos[k]) printf(" [%s] %u", clave[k], motivos[k]);
        printf(" [reubicación] %u [suma del cuerpo] %u [lectura] %u [otros] %u\n", reub, suma, lect, otros);
        free(bueno); free(f);
    } else printf("cargable: orden desconocida: %s\n", orden);
    fflush(stdout);
}
#endif
