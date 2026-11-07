/* podometro.h — contador de pasos de senew (versión propia, simple). Software puro (lo compila también el PC).
 *
 * Es un detector por umbral con histéresis sobre el eje más activo, sin su media lenta (la gravedad):
 * un paso es un cruce de -umbral a +umbral (3 cuentas), y solo se empieza a contar cuando
 * hay una racha regular (cuatro cruces seguidos separados entre 0,28 s y 2 s), para no contar sacudidas sueltas.
 * Las muestras llegan a 25 por segundo (x, y crudos del ADC, 0..255; el cero anda por 125).
 *
 * ESTADO: PENDIENTE DE DECISIÓN. Esta versión sustituye en el árbol público a la que usa el binario publicado de senew,
 * que no se distribuye aquí. Cuenta de forma aproximada y es menos exacta (no mide la intensidad ni la energía). El «ahorro
 * en reposo» (apagar el sensor entre pasos) tampoco está: el sensor no duerme y `pod_dormido()` es siempre falso. */
#ifndef PODOMETRO_H
#define PODOMETRO_H

#define POD_NO_ITERAR_DORMIDO 1      /* (sin efecto en esta versión) */
#define POD_SIN_Z             2      /* (sin efecto en esta versión) */
#define POD_FIEL              0
#define POD_CORREGIDO         (POD_NO_ITERAR_DORMIDO | POD_SIN_Z)

typedef struct {
    /* configuración */
    unsigned char hz;                    /* 25 iteraciones por segundo */
    unsigned char modo;                  /* POD_FIEL / POD_CORREGIDO: se guardan, pero esta versión se comporta igual */
    unsigned char vigilar_si;            /* idem: el ahorro en reposo no existe aquí */
    float estatura, peso;                /* m, kg (la zancada sale de la estatura) */
    /* la señal */
    float g[2];                          /* media lenta de x e y respecto al cero, en cuentas (la gravedad que ve cada eje), recortada a ±20 */
    unsigned char eje;                   /* eje con más gravedad: 1 = X, 2 = Y, 0 = ninguno (teléfono tumbado) */
    /* la detección */
    int bx16, by16;                      /* medias lentas ×16 */
    int ex, ey;                          /* cuánto se mueve cada eje (media de |desviación| ×16) */
    unsigned char primera;               /* la siguiente muestra inicia las medias */
    unsigned char armado;                /* la señal bajó del umbral bajo desde el último cruce */
    unsigned idle;                       /* iteraciones desde el último cruce */
    unsigned c;                          /* cruces (candidatos a paso) vistos desde el arranque */
    unsigned it, ult_it;                 /* contador de iteraciones y la del último cruce */
    unsigned char racha;                 /* cruces regulares seguidos */
    unsigned char andando, inactivo;     /* andando AHORA (racha validada) · lleva ≥ 1 s sin cruces */
    /* resultados */
    unsigned pasos;
    float dist, kmh;                     /* metros desde el arranque · velocidad estimada, km/h */
    /* ahorro en reposo: no existe en esta versión; los campos los lee el servicio de pasos */
    unsigned char vig, ventana, apagar;
    unsigned short cta_dormir;
} Podometro;

void pod_iniciar(Podometro *p, int modo);
void pod_perfil(Podometro *p, float estatura_m, float peso_kg);
/* Una iteración con una muestra nueva (x, y crudos 0..255). Devuelve 1 si en ella se ha contado algún paso. */
int  pod_muestra(Podometro *p, int x_crudo, int y_crudo);
/* Una iteración sin muestra (solo existiría con el ahorro en reposo, que aquí no hay): no cuenta nada. */
int  pod_sin_muestra(Podometro *p);
#define pod_dormido(p) ((p)->apagar != 0)
/* Vuelve a tomar las medias de la siguiente muestra. No toca los pasos contados. */
void pod_despertar(Podometro *p);
/* Clasifica las últimas 10 muestras crudas (x, y): 6 = SACUDIDA, 7 = quieto, 0 / 1 = golpe en X (− / +), 3 / 2 = golpe en Y (− / +). */
int  pod_gesto(const unsigned char m[10][2]);
#endif
