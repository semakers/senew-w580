/* nor_placa.h — LA DESCRIPCIÓN DE LA PLACA en lo que toca a la NOR: qué chip se ESPERA encontrar soldado. (R3, propuesta P1 de la
 * pista S7.) Es el ÚNICO sitio donde está escrito; hal/nor.c lo compara con lo que el chip dice de sí mismo y de ahí sale el
 * PERMISO de escritura. Para otra placa (otro modelo del mismo SoC, otro lote con otro chip) se cambia ESTE fichero, o se
 * compila con -DNOR_PLACA_ID=0x0089XXXX.
 * La GEOMETRÍA (cuántos bloques, de qué tamaño, qué búfer) NO sale de aquí: se lee de la tabla CFI del chip (sirve para leer
 * y para portar). Esto es solo «lo que tiene que ser para que nos atrevamos a borrar y programar».
 * OJO: 0x887E es lo que contesta el EMULADOR ([inferido]: M18 de 512 Mbit). [sin saber] qué contesta el W580i del mantenedor: si es
 * otro, el sistema arrancará SIN ESCRITURA (leyendo lo que haya) y dirá el id leído; entonces se pone aquí. */
#ifndef NOR_PLACA_H
#define NOR_PLACA_H
#ifndef NOR_PLACA_ID
#define NOR_PLACA_ID       0x0089887Eu   /* fabricante 0x0089 (Intel), dispositivo 0x887E */
#endif
#define NOR_PLACA_ORDENES  0x0200u       /* juego de órdenes primario (CFI 0x13/0x14): el del M18 */
#define NOR_PLACA_TAM_LOG2 0x1Au         /* CFI 0x27: 2^26 = 64 MiB */
#define NOR_PLACA_BUF_LOG2 0x0Au         /* CFI 0x2A: búfer / región de programación de 2^10 = 1 KiB */
#define NOR_PLACA_BLOQUES  256u          /* CFI 0x2D/0x2E + 1, en UNA sola región de borrado (0x2C = 1) */
#define NOR_PLACA_BLOQUE   0x40000u      /* CFI 0x2F/0x30 << 8: 256 KiB */
#endif
