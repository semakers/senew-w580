#include <string.h>
/* Prueba de conformidad n.º 1 (PC): TODA descripción de placa es coherente consigo misma y con las reglas de seguridad.
 * Es la primera que tiene que pasar un port nuevo, antes de escribir una línea de controlador. */
#include <stdio.h>
#include "placa.h"
#include "ui.h"
int placa_n_modelos(void); const Placa *placa_modelo(int i);
static int mal;
#define EXIGE(c, ...) do { if (!(c)) { mal++; printf("  MAL: " __VA_ARGS__); printf("\n"); } } while (0)
int main(void)
{
    int m, i, j;
    for (m = 0; m < placa_n_modelos(); m++) {
        const Placa *p = placa_modelo(m); int celdas = p->filas * p->columnas + p->a_masa, enc = 0, seg = 0;
        printf("%s (%s): %ux%u, %d celdas, RAM %u KiB\n", p->modelo, p->soc, p->ancho, p->alto, celdas, p->ram_tam >> 10);
        EXIGE(p->api_placa == PLACA_API, "api_placa %u", p->api_placa);
        EXIGE(p->rotulo && p->rotulo[0] && p->flash_esperada && p->flash_esperada[0], "faltan rotulo o flash_esperada");
        EXIGE(p->celda_corto != 0, "falta celda_corto");
        EXIGE(p->ancho >= 96 && p->alto >= 96 && p->ancho <= 480 && p->alto <= 480, "pantalla fuera de lo que el núcleo sabe pintar");
        for (i = 0; i < celdas; i++) { if (p->celdas[i].tecla == K_ENCENDIDO) enc++; if (p->celdas[i].tecla == p->tecla_modo_seguro) seg++; }
        EXIGE(enc == 1, "tiene que haber UNA celda de encendido (hay %d)", enc);
        EXIGE(seg >= 1, "la tecla del modo seguro no está en el teclado");
        for (i = 0; i < celdas; i++) for (j = i + 1; j < celdas; j++)     /* teclas imprescindibles sin duplicar: 0..9, flechas, OK, selección */
            EXIGE(!(p->celdas[i].tecla && p->celdas[i].tecla <= K_ALM && p->celdas[i].tecla == p->celdas[j].tecla), "tecla %d en dos celdas (%d, %d)", p->celdas[i].tecla, i, j);
        for (i = K_ARRIBA; i <= K_ALM; i++) { for (j = 0; j < celdas && p->celdas[j].tecla != i; j++) ; EXIGE(j < celdas, "falta la tecla lógica %d", i); }
        for (i = 0; i < ZONA_N; i++) for (j = i + 1; j < ZONA_N; j++) {
            const PlacaZona *a = &p->zonas[i], *b = &p->zonas[j];
            if (!a->tam || !b->tam || i == ZONA_MAIN || j == ZONA_MAIN) continue;   /* (el W6FS vive dentro del tramo del main) */
            EXIGE(a->base + a->tam <= b->base || b->base + b->tam <= a->base, "las zonas «%s» y «%s» se pisan", a->nombre, b->nombre);
        }
        for (i = 0; i < ZONA_N; i++) if (p->zonas[i].escribible) {
            const PlacaZona *z = &p->zonas[i], *mn = &p->zonas[ZONA_MAIN], *ff = &p->zonas[ZONA_FS_FABRICA];
            EXIGE(z->bloque_logico && z->tam % z->bloque_logico == 0 && z->base % z->bloque_logico == 0, "«%s» no cae en bloques enteros", z->nombre);
            EXIGE(z->tam / z->bloque_logico >= 4, "«%s»: menos de 4 bloques, no hay sitio para la limpieza atómica", z->nombre);
            EXIGE(z->base >= mn->base + mn->tam || z->base + z->tam <= mn->base, "«%s» se escribe DENTRO del main", z->nombre);
            EXIGE(!ff->tam || z->base + z->tam <= ff->base || z->base >= ff->base + ff->tam, "«%s» se escribe DENTRO del FS de fábrica", z->nombre);
        }
        EXIGE(!p->zonas[ZONA_FS_FABRICA].escribible && !p->zonas[ZONA_MAIN].escribible, "main y FS de fábrica no son escribibles NUNCA");
        EXIGE(p->bat.carga_permitida == 0 || !strcmp(p->modelo, "W580i"), "un port no carga la batería: solo el W580i, cuya carga USB está MEDIDA en su aparato (pista CARGA, docs/bateria.md §12)");
        if (p->capacidades & CAP_BATERIA_MV) {
            EXIGE(p->bat.marcha.pend_umv > 0 && p->bat.mv_corte >= 3000 && p->bat.mv_aviso >= p->bat.mv_corte && p->bat.mv_no_creible < p->bat.mv_corte, "umbrales de batería incoherentes");
        } else EXIGE(p->bat.mv_corte == 0, "sin CAP_BATERIA_MV no puede haber corte por tensión");
        EXIGE((p->luces_rgb > 0) == !!(p->capacidades & CAP_LUZ_RGB), "luces_rgb y CAP_LUZ_RGB no cuadran");
        EXIGE((p->tapa_tipo != TAPA_NO) == !!(p->capacidades & CAP_TAPA), "tapa_tipo y CAP_TAPA no cuadran");
    }
    printf(mal ? "HAY %d FALLOS\n" : "TODAS LAS PLACAS COHERENTES\n", mal);
    return mal != 0;
}
