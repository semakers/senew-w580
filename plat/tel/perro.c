/* plat/tel/perro.c — (P600, paso 6 de docs/portabilidad.md §3.7) EL PERRO DE SOFTWARE Y LA PULSACIÓN LARGA, COMUNES a todo
 * teléfono. Es el bloque de plat/hw_w580.c (pistas S7, R2, R3; docs/sistema.md §15.5, §15.6) movido tal cual, con lo que era
 * del W580 cambiado por las funciones tel_* de la placa (plat/tel/tel.h). Lo que hace, como antes:
 *
 * El perro de HARDWARE (F9010000) lo desarma la llave del arranque: NO se arma. Esto es otra cosa: la IRQ del tic (cada 64 tics
 * = 256 ms) mira cuánto hace que el bucle principal dio señales.
 *  - hw_perro_latido(): el bucle del sistema en cada vuelta, y app_latido() por las apps externas. La primera llamada lo ARMA.
 *  - sin latido en `perro_plazo` segundos (10; 30 con una app externa en pantalla) -> MUERDE.
 *  - cada operación de flash acabada da PRÓRROGA, pero sin latido del bucle en PERRO_TOPE_S (60 s) muerde igual.
 *  - con una operación de flash EN CURSO no se muerde ni se apaga a la fuerza: se espera a que acabe, con tope de 20 s más.
 *  - en reposo profundo el tic no late y el perro tampoco cuenta; en la pantalla de fallo, apagado.
 * AL MORDER: tel_sin_bucle() (gancho del tic fuera, tono callado, carga a 0, vibrador parado, desde la IRQ) y la PANTALLA DE
 * FALLO con el PC interrumpido (de la pila de la IRQ: arranque.S guarda {r0-r3, r12, lr} y lr es el PC) y el LR del bucle;
 * reinicio a los 15 s (no en modo seguro) o con una tecla. NO PUEDE sacar de un cuelgue con las interrupciones cortadas.
 *
 * LA TECLA DE ENCENDIDO se mira aquí por la celda 30 (tel_kbd_masa: lectura pura, sin barrer) con las reglas de api/enc.h.
 * Pulsada 8 s seguidos: hw_encendido_largo() = 1 (el shell apaga con orden). 11 s con el bucle 3 s sin latir: APAGADO FORZADO
 * desde aquí (tel_sin_bucle + tel_apagado_forzado), salvo en modo seguro y salvo que el PMU, si es vía probada, diga «suelta». */
#include "tel.h"

#ifndef PERRO_PLAZO_S
#define PERRO_PLAZO_S 10                           /* (P600) -DPERRO_PLAZO_S=30: SOLO para la prueba del apagado forzado (que el perro no llegue antes) */
#endif
#define PERRO_TOPE_S 60
#define PERRO_LARGA_S 300
extern char _pila_irq[];
EncEstado tel_enc;
static volatile u32 perro_real_tic;                /* (R3) el último latido DE VERDAD del bucle (o de la app): el que no estira nadie más */
static volatile u32 perro_tic, perro_bucle_tic;    /* el último latido o prórroga; el último latido DEL BUCLE */
static volatile u8 perro_armado, enc_largo, forzar_pendiente;
static u8 perro_plazo = PERRO_PLAZO_S, nor_espera_enc, nor_espera_perro;   /* (R2; M4) DOS contadores: con uno solo el tope de 20 s no existía */
static u32 perro_mordidas;

#ifdef SENEW_RAM    /* (27-oct, senew en RAM) el BL armó el perro de HARDWARE (4 s); la 1.ª vuelta del bucle lo DESARMA con la llave del crt0 original */
void tz_etapa(u32 n);
static void perro_hw_desarmar(void)
{
    *(volatile u32 *)0xF9010010 = 0xAA; *(volatile u32 *)0xF9010024 = 0x5A; *(volatile u32 *)0xF901002C = 0xA5;
    tz_etapa(100);
}
#endif
void hw_perro_latido(void)
{
    u32 t = tics;
#ifdef SENEW_RAM
    if (!perro_armado) perro_hw_desarmar();
#endif
    perro_tic = t; perro_bucle_tic = t; perro_real_tic = t; perro_armado = 1; tel_latido_placa();
}
/* (R3; AUD3-M1) UNA OPERACIÓN LARGA Y LEGÍTIMA DE FLASH NO ES UN CUELGUE: quien hace el trabajo avisa de que AVANZA y eso cuenta
 * como latido del bucle... ACOTADO: PERRO_LARGA_S desde el último latido de verdad, y el tope de PERRO_TOPE_S después. */
void hwo_progreso(void) { u32 t = tics; perro_tic = t; if (t - perro_real_tic < PERRO_LARGA_S * 250u) perro_bucle_tic = t; }
void hw_perro_plazo(int s) { perro_plazo = (u8)(s < 5 ? 5 : s > 120 ? 120 : s); }
void tel_perro_prorroga(void) { perro_tic = tics; }
int  hw_encendido_largo(void) { return enc_largo; }
u32  tel_perro_mordidas(void) { return perro_mordidas; }

__attribute__((target("arm"), noinline)) static u32 lr_del_bucle(void)   /* el LR del modo SVC (el del bucle), visto desde la IRQ (ARM926) */
{
    u32 v;
    __asm__ volatile("mrs r1, cpsr\n bic r2, r1, #0x1F\n orr r2, r2, #0x13\n msr cpsr_c, r2\n mov r0, lr\n msr cpsr_c, r1\n mov %0, r0"
                     : "=r"(v) : : "r0", "r1", "r2", "cc");
    return v;
}
static void hex8(char *s, u32 v) { int i; for (i = 7; i >= 0; i--) { u32 d = v & 15; s[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v >>= 4; } s[8] = 0; }
static void perro_muerde(const char *por_que)
{
    static char det[176];
    u32 pc = *(const u32 *)((u32)_pila_irq - 4u), lr = lr_del_bucle();
    static const char a[] = "pc = ", b[] = "  lr = ";
    int n = 0, i;
    perro_mordidas++;
    tel_sin_bucle();
    for (i = 0; a[i]; i++) det[n++] = a[i];
    hex8(det + n, pc); n += 8;
    for (i = 0; b[i]; i++) det[n++] = b[i];
    hex8(det + n, lr); n += 8;
    det[n++] = '.'; det[n++] = ' ';
    for (i = 0; por_que[i] && n < (int)sizeof det - 1; i++) det[n++] = por_que[i];
    det[n] = 0;
    /* reinicia solo a los 15 s; (R2) en MODO SEGURO no: la salida de emergencia no puede entrar en un bucle de reinicios */
    tel_fallo("EL SISTEMA NO RESPONDE", det, TEL_FALLO_PERRO, pc, lr, !hw_modo_seguro());
}
static void apagado_forzado(void)                  /* no vuelve... salvo que el PMU desmienta al teclado (R2) */
{
    u32 e = irq_cerrar();
    /* (R2; A3) Si en ESTE aparato la tecla llega también por el PMU (se le ha visto suelta y pulsada), se le pregunta AHORA. Si dice
     * «suelta», lo del teclado era la foto clavada: no se apaga, la foto queda desarmada hasta que se la vuelva a ver suelta. */
    if (enc_pmu_probado(&tel_enc) && tel_pmu_tecla() == 0) {
        hw_log("apagado forzado: el PMU dice que la tecla esta SUELTA: no se apaga (la foto del teclado se desarma)\n");
        tel_enc.foto_suelta = 0; tel_enc.vigias = 0; tel_enc.kbd = 0; if (tel_enc.descartes < 255) tel_enc.descartes++;
        if (!e) irq_abrir();
        return;
    }
    hw_log("\n*** APAGADO FORZADO por pulsacion larga de la tecla de encendido\n");
    tel_sin_bucle();
    tel_apagado_forzado();
}
void tel_tras_nor(void) { if (forzar_pendiente) { forzar_pendiente = 0; apagado_forzado(); } }
void tel_perro_vigia(void)
{
    u32 t = tics;
    int nivel, foto;
    if (tel_en_panico()) return;
    /* LA TECLA DE ENCENDIDO (R2; A3): por la celda 30, como NIVEL de ahora y como foto, creída según api/enc.h. El apagado FORZADO,
     * además, solo si el bucle lleva 3 s sin latir —si late, el camino es el ordenado de los 8 s (shell)— y nunca en modo seguro. */
    if (tel_kbd_masa(&nivel, &foto) == 0) {
        if (enc_vigia(&tel_enc, foto < 0 ? -1 : (foto & 1), nivel < 0 ? -1 : (nivel & 1))) {
            if (tel_enc.vigias >= ENC_LARGO_VIGIAS) enc_largo = 1;
            if (enc_forzar(&tel_enc, t - perro_bucle_tic, hw_modo_seguro())) {
                if (!tel_nor_en_curso() || ++nor_espera_enc > 80) apagado_forzado();
                else forzar_pendiente = 1;         /* hay una operación de flash a medias: en cuanto acabe */
            }
        } else { enc_largo = 0; forzar_pendiente = 0; nor_espera_enc = 0; }
    }
    if (!perro_armado) return;
    if (!(t - perro_tic > (u32)perro_plazo * 250u || t - perro_bucle_tic > PERRO_TOPE_S * 250u)) nor_espera_perro = 0;
    else {
        if (tel_nor_en_curso() && ++nor_espera_perro <= 80) return;   /* operación de flash en curso: se espera, 20 s como mucho */
        /* (C) quien no vuelve es una app CARGABLE: se la saca a ella y el sistema sigue; (AUD6, M1) PERO NO a media operación de flash */
        if (!tel_nor_en_curso() && cg_perro_irq()) return;
        perro_muerde(t - perro_bucle_tic > PERRO_TOPE_S * 250u ? "El bucle principal lleva mas de 60 s sin dar senales (solo flash)."
                                                              : "El bucle principal (o la app externa) no da senales.");
    }
}
