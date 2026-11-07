/* pasos.c — el PODÓMETRO como servicio del sistema (pista S2). Cuenta en segundo plano, con cualquier pantalla o
 * con la pantalla apagada, al ritmo del original (docs/emulador_acelerometro.md §4.1, medido en el firmware R8BE001):
 *
 *   despierto: ENCIENDE el sensor (P1.1) · ~20 ms · convierte Y (canal 4) y X (canal 12) · lo APAGA ·
 *              una iteración del detector (shell/podometro.c) · y otra vez a los 40 ms de la anterior;
 *   dormido (modo «vigilar», tras 46 s sin candidatos a paso): no toca el sensor y da una iteración del detector
 *              cada 40 ms, hasta que el detector pide encender (5 s).
 *
 * Va por el reloj hw_ms() (el tic de 4 ms) desde sh_pasos_paso(), una vez por vuelta del bucle de la plataforma:
 * no hay hilo ni interrupción propia. Lo que eso implica:
 *   - una fase se retrasa lo que tarde la vuelta en curso (pintar un cuadro entero): la muestra sale tarde y la
 *     rejilla se recoloca; `tarde` cuenta cuántas veces;
 *   - despierto no se puede dormir a fondo (el reposo profundo para el tic): sh_dormir_ms() pregunta aquí. Dormido
 *     el sensor sí: las iteraciones sin muestra no dependen de nada de fuera, así que al despertar la CPU se dan
 *     de golpe las que tocaban (dan lo mismo que una a una);
 *   - una app EXTERNA con la pantalla tomada lo mantiene vivo desde app_latido().
 *
 * El ahorro en reposo es un ajuste (aj.pasos_ahorro): NINGUNO (por omisión: el sensor no duerme), FIEL al original
 * o CORREGIDO. Es una DECISIÓN ABIERTA (docs/sistema.md §6.8): medido con señales sintéticas, sin ahorro no se
 * pierde ningún paso al echar a andar; el fiel pierde de media ~4 de 40 y el corregido ~8, a cambio de tener el
 * sensor apagado el 33–70 % y el 98 % del reposo. Lo que cuesta en batería no dormir NO se puede medir aquí.
 *
 * Una pantalla que quiere el sensor en vivo (apps/movimiento) lo RETIENE: mientras tanto no hay modo vigilar (y
 * el servicio corre aunque el ajuste lo tenga apagado). */
#include "shell.h"
#include "placa.h"

static Podometro pod;
static u8 iniciado, fase, retenido, encendido, parado, hay = 1, gesto = 7;
static u8 ult[10][2], ux = HW_ACEL_CERO, uy = HW_ACEL_CERO;
static u32 t_sig, t_rejilla;             /* hw_ms() de la próxima fase / de la próxima vez que toca encender */
static u32 n_muestras, n_dormidas, n_tarde, n_sacudidas, n_fallos, version;

static int activo(void) { return aj.pasos || retenido; }
static void aplicar(void)
{
    int modo = aj.pasos_ahorro == SH_PASOS_FIEL ? POD_FIEL : POD_CORREGIDO;
    int vigilar = aj.pasos_ahorro != SH_PASOS_SIN_AHORRO && !retenido;
    if (pod.modo != modo || pod.vigilar_si != vigilar) pod_despertar(&pod);
    pod.modo = (unsigned char)modo; pod.vigilar_si = (unsigned char)vigilar;
}
void sh_pasos_init(void)
{
    if (!placa_tiene(CAP_ACELEROMETRO)) {           /* (0.23, el mantenedor: «el 600 no tiene acelerómetro») sin sensor: ni podómetro ni
                                                     * menú que se inclina, aunque el ajuste guardado (o el de fábrica) diga que sí */
        aj.pasos = 0;
        if (aj.menu_efecto == SH_EFECTO_INCLINA) aj.menu_efecto = SH_EFECTO_VAIVEN;
    }
    pod_iniciar(&pod, POD_CORREGIDO);
    aplicar();
    t_sig = t_rejilla = hw_ms() + 40;
    iniciado = 1;
}
void sh_pasos_ajuste(int si, int ahorro)
{
    aj.pasos = (u8)!!si; aj.pasos_ahorro = (u8)(ahorro < 0 || ahorro >= SH_PASOS_AHORROS ? 0 : ahorro);
    aplicar();
    sh_guardar_ajustes();
}
void sh_pasos_retener(int si)
{
    retenido = (u8)((retenido & ~1u) | (si ? 1u : 0u));
    aplicar();
}
/* (pista V) El menú 3D también quiere el sensor en vivo mientras se ve. Es OTRO dueño: que el menú lo suelte al
 * quedar tapado por la app Movimiento no puede quitárselo a ella (y al revés). */
void sh_pasos_retener_menu(int si)
{
    retenido = (u8)((retenido & ~2u) | (si ? 2u : 0u));
    aplicar();
}
/* (estética) el giro de la pantalla (shell/giro.c) también lo quiere mientras la pantalla está encendida: OTRO dueño */
void sh_pasos_retener_giro(int si)
{
    if (!!(retenido & 4u) == !!si) return;
    retenido = (u8)((retenido & ~4u) | (si ? 4u : 0u));
    aplicar();
}
void sh_pasos_a_cero(void)
{
    pod_iniciar(&pod, POD_CORREGIDO);
    aplicar();
    n_sacudidas = 0; version++;
}
const Podometro *sh_pasos(void) { return &pod; }
void sh_pasos_info(PasosInfo *p)
{
    p->activo = (u8)activo(); p->hay_sensor = hay; p->dormido = (u8)pod_dormido(&pod); p->gesto = gesto;
    p->x = ux; p->y = uy;
    p->muestras = n_muestras; p->sin_muestra = n_dormidas; p->tarde = n_tarde; p->sacudidas = n_sacudidas; p->fallos = n_fallos;
    p->version = version;
}

/* ¿Cuánto falta para que el servicio necesite la CPU? Para el reposo profundo (shell/energia.c). */
u32 sh_pasos_proximo_ms(void)
{
    u32 t = hw_ms(), falta;
    if (!iniciado || !activo()) return 0xFFFFFFFFu;
    falta = (i32)(t_sig - t) > 0 ? t_sig - t : 0;
    if (pod_dormido(&pod)) falta += 40u * pod.cta_dormir;   /* hasta entonces solo hay iteraciones sin muestra: se dan luego */
    return falta;
}

void sh_pasos_paso(void)
{
    u32 t; int x, y, contado = 0, vueltas;
    if (!iniciado) return;
    sh_giro_paso();                                  /* (estética) la pantalla gira con estas mismas muestras (shell/giro.c) */
    if (!activo()) {
        if (encendido) { hw_acel_alimentar(0); encendido = 0; }
        fase = 0; parado = 1;
        return;
    }
    t = hw_ms();
    if (parado) { parado = 0; t_sig = t_rejilla = t; }   /* vuelve de estar apagado: la rejilla empieza ahora */
    if ((i32)(t - t_sig) < 0) return;
    for (vueltas = 0; pod_dormido(&pod) && (i32)(t - t_sig) >= 0 && vueltas < 200; vueltas++) {
        contado |= pod_sin_muestra(&pod);            /* dormido: una iteración cada 40 ms, sin sensor */
        n_dormidas++; t_sig += 40;
        if (!pod_dormido(&pod)) {                    /* el detector pide encender. El original pasa su temporizador de 40 a
                                                      * 20 ms: enciende 20 ms después de esta iteración y convierte a los 40 */
            t_sig -= 20; t_rejilla = t_sig; fase = 0;
        }
    }
    if (vueltas == 200) t_sig = t + 40;              /* (un salto de reloj de más de 8 s: no se persigue) */
    if (!pod_dormido(&pod) && (i32)(t - t_sig) >= 0) {
        if (!fase) {
            hw_acel_alimentar(1); encendido = 1; fase = 1;
            t_sig = t + 20;                          /* el original convierte ~20,4 ms después de encender */
        } else {
            fase = 0;
            hay = (u8)(hw_acel_leer(&x, &y) == HW_OK);
            hw_acel_alimentar(0); encendido = 0;
            t_rejilla += 40;
            if ((i32)(t_rejilla - (t + 20)) < 0) { t_rejilla = t + 20; n_tarde++; }   /* la vuelta se ha retrasado */
            t_sig = t_rejilla;
            if (!hay) n_fallos++;
            else {
                int i;
                ux = (u8)x; uy = (u8)y; n_muestras++;
                for (i = 0; i < 9; i++) { ult[i][0] = ult[i + 1][0]; ult[i][1] = ult[i + 1][1]; }
                ult[9][0] = ux; ult[9][1] = uy;
                contado |= pod_muestra(&pod, x, y);
                if (pod_dormido(&pod)) t_sig = t + 40;   /* a dormir: el original pasa su temporizador a 40 ms */
                if (n_muestras >= 10 && n_muestras % 5 == 0) {
                    int g = pod_gesto(ult);
                    if (g == 6 && gesto != 6) n_sacudidas++;
                    gesto = (u8)g;
                }
            }
            version++;
        }
    }
    (void)contado;
}
