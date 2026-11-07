/* escritorio_rec.h — la forma de los recursos «pre-renderizados» del estilo Escritorio (shell/menu_escritorio.c).
 * En el árbol de desarrollo hay un shell/escritorio_rec.c GENERADO (iconos, paisaje y textos rasterizados). En este repositorio
 * público NO viene: usaba tipografías y emblemas de terceros. Va un shell/escritorio_rec.c vacío y el escritorio se dibuja entero
 * con código (rec() == 0): el paisaje, los iconos y el texto con las letras del sistema (Roboto). */
#ifndef ESCRITORIO_REC_H
#define ESCRITORIO_REC_H
#include "ui.h"
/* una imagen: pix en el Color del panel (NULL = solo alfa: una máscara que se pinta de un color); alfa 0..255, recto */
typedef struct { u16 w, h; const u16 *pix; const u8 *alfa; } ErImg;
enum { ER_DOOM, ER_MUSICA, ER_VIDEO, ER_N };
typedef struct {
    ErImg ico_g[ER_N], ico_p[ER_N];          /* los iconos: 32x32 (escritorio) y 20x20 (menú Inicio) */
    ErImg etiqueta[ER_N];                    /* su nombre (máscara, en una o dos líneas) */
    ErImg inicio, usuario_txt, apagar_txt;   /* los textos del botón Inicio, del usuario y de «Apagar» */
    ErImg bandera, usuario, apagar;          /* el emblema del botón, la foto del usuario, el botón de apagar */
    ErImg digito[11];                        /* la hora de la bandeja: 0..9 y «:» (máscaras de la misma altura) */
    u16 fondo_an, fondo_al; const u16 *fondo_pal; const u8 *fondo_idx;   /* el paisaje: 256 colores, 1 byte por píxel */
    u16 nubes_an, nubes_al; const u8 *nubes; const u16 *nubes_col;   /* las nubes: alfa<<4 | luz (4+4 bits); da la vuelta en x */
} ErRec;
extern const ErRec er;
#endif
