/* GENERADO por tmp/mp3/gen_datos.py: NO EDITAR. Datos del reproductor MP3 (hal/mp3_w580.c). */
#ifndef MP3_W580_DAT_H
#define MP3_W580_DAT_H
typedef unsigned short u16;
typedef struct { unsigned short canal, nw, off; } Mp3Plan;   /* canal del mensaje, palabras (SIN el canal) y desplazamiento en mp3_pool */
#define MP3_PLAN1_N 87
extern const u16 mp3_pool[1939];
extern const Mp3Plan mp3_plan1[MP3_PLAN1_N];
/* posiciones del plan de la primera reproduccion (los roles) */
#define MP3_P_F1_R6_FILTRO 0
#define MP3_P_F1_R9_A 1
#define MP3_P_F1_R6_DRC 2
#define MP3_P_F1_R9_B 3
#define MP3_P_F1_R9_DRC 4
#define MP3_P_F1_R10 5
#define MP3_P_F1_R4_UNIDAD 6
#define MP3_P_F1_R12 7
#define MP3_P_F1_R4_CERO 8
#define MP3_P_F1_R11 9
#define MP3_P_F1_0800 10
#define MP3_P_F1_R91 11
#define MP3_P_F1_R13 12
#define MP3_P_F1_R3 13
#define MP3_P_F1_R6_BANCO 14
#define MP3_P_F1_R9_C 15
#define MP3_P_GRUPO_0 16
#define MP3_P_GRUPO_1 17
#define MP3_P_GRUPO_2 18
#define MP3_P_GRUPO_3 19
#define MP3_P_GRUPO_4 20
#define MP3_P_RAMPA_PASO 26
#define MP3_P_PRE_R7A 51
#define MP3_P_PRE_R7B 52
#define MP3_P_PRE_R14 53
#define MP3_P_PRE_R91 54
#define MP3_P_PRE_R16 55
#define MP3_P_PRE_R17A 56
#define MP3_P_PRE_R15 57
#define MP3_P_PRE_R7C 58
#define MP3_P_PRE_R17B 59
#define MP3_P_M1101 60
#define MP3_P_F3_R0 62
#define MP3_P_F3_R10A 63
#define MP3_P_F3_R4A 64
#define MP3_P_F3_R10B 65
#define MP3_P_F3_R4B 66
#define MP3_P_F3_R12 67
#define MP3_P_F3_R11 68
#define MP3_P_F3_0800 69
#define MP3_P_F3_R91 70
#define MP3_P_F3_R13 71
#define MP3_P_F3_R3 72
#define MP3_P_F3_R6_BANCO 73
#define MP3_P_F3_R6_FILTRO 74
#define MP3_P_F3_R9_A 75
#define MP3_P_F3_R6_DRC 76
#define MP3_P_F3_R9_B 77
#define MP3_P_M1208 79
#define MP3_P_M120A 80
#define MP3_P_M0E06 81
#define MP3_P_F4_R2 82
#define MP3_P_M0E00 83
#define MP3_P_M0404 84
#define MP3_P_M0E02 86
/* constructores de los mensajes que salen del APF (comprobados contra el original con farfield_closed_play) */
#define MP3_FILTRO_PRE_N 11
#define MP3_DRC6_PRE_N 12
#define MP3_DRC9_PRE_N 9
extern const u16 mp3_filtro_pre[11], mp3_drc6_pre[12], mp3_drc9_pre[9];
#define MP3_FILTRO_N 292   /* palabras de cuerpo del R_6 del filtro RX 44,1 */
#define MP3_DRC6_N 56
#define MP3_DRC9_N 34
#endif
