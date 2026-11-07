/* Lo que el guion del PC (pc_main.c) le hace a la simulación (hw_pc.c). */
#ifndef PC_H
#define PC_H
extern u32 pc_reloj_ms;
extern u16 pc_panel[];
extern u32 pc_volcados, pc_filas_volcadas;
void pc_tecla(int k);                 /* pulsar y soltar */
void pc_mantener(int k, int si);      /* dejar pulsada / soltar */
void pc_serie_rx(const char *s);      /* bytes que «llegan» por la UART */
const char *pc_serie_tx(void);        /* todo lo que el sistema ha enviado */
/* energía y tapa simuladas (órdenes «bateria», «cargador» y «tapa» del guion) */
extern int pc_bateria_mv, pc_cargador_bits, pc_tapa_cerrada;
void pc_bateria(int n);               /* n <= 100: porcentaje; si no, milivoltios */
void pc_descarga(int mv_por_s);       /* la batería baja sola con el reloj virtual */
extern int pc_apagable, pc_apagado, pc_apagados_fallidos, pc_cargas_fuera;   /* (S7) hw_apagar simulado y las veces que se pidió «carga fuera» */
extern u32 pc_dormidas, pc_ms_dormido;
void pc_tapa(int cerrada);
void pc_bomba(void (*f)(void));       /* quien sigue leyendo el guion cuando el bucle no es del guion */
int  pc_pantalla_dormida(void);
/* sonido simulado (pista S5): la traza de lo que se ha pedido al generador, y los WAV que escribe el PC
 * con el mismo modelo del emulador (W600_WAV_TONO / W600_WAV_PCM; W600_PCM=1 finge que el DSP contesta). */
void pc_sonido_avanzar(u32 hasta_ms);
extern char pc_tono_traza[];
extern u32  pc_tono_marcos, pc_tono_sonando_marcos;
/* acelerómetro simulado (W580i; órdenes «acel» y «andar» del guion) */
void pc_acel(int x, int y);
void pc_andar(int decihz, int amplitud);
extern u32 pc_acel_lecturas, pc_acel_sin_alimentar;
extern u32 pc_w6f_violaciones, pc_w6f_programas, pc_w6f_borrados;   /* (I1) el modelo de la zona de la Memoria (plat/hw_pc.c) */
#endif
