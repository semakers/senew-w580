#!/usr/bin/env python3
"""gen_tabla_c.py — GENERA hal/camara_w580_tab.c: la tabla de pasos del sensor OV2640 para el driver hal/camara_w580.c (176x144, YUV 4:2:2, 7 fps),
expandida del guion del ORIGINAL (work/h2/camdriver0_limpio.txt) con camdriver_expandir.py. NO editar a mano el .c: cambia esto y regenera.
Composición (idéntica a cam_cuadro.py, validada en el aparato 27-oct): seq_init (rama Premier = módulo 0, REV2B; espera tras el reset 30 ms) + ventana/tamaño/D3 del
original (volcado aparato/sensor_original_b0.bin) + seq_vf_config_24 + seq_vf_start + DD = 7F.
Uso: gen_tabla_c.py > hal/camara_w580_tab.c"""
import os, sys
AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
import camdriver_expandir as ce

defs = ce.cargar()
pasos, _ = ce.secuencia(defs, 'seq_init', 0, 'REV2B')
pasos = [('d', 30, 0) if (t == 'd' and x == 1) else (t, x, y) for t, x, y in pasos]
pasos += [('w', 0xFF, 0x00), ('w', 0xE0, 0x04), ('w', 0x50, 0x9B), ('w', 0x51, 0x70), ('w', 0x52, 0x2C), ('w', 0x53, 0x44), ('w', 0x54, 0x0C), ('w', 0x55, 0x88),
          ('w', 0x5A, 0x2C), ('w', 0x5B, 0x24), ('w', 0x5C, 0x00), ('w', 0x86, 0x3D), ('w', 0xE0, 0x00), ('w', 0xD3, 0x04), ('w', 0xE9, 0x67)]
pp, _ = ce.secuencia(defs, 'seq_vf_config_24', 0, 'REV2B'); pasos += pp
pp, _ = ce.secuencia(defs, 'seq_vf_start', 0, 'REV2B'); pasos += pp
pasos += [('w', 0xFF, 0x00), ('w', 0xDD, 0x7F)]
print('/* GENERADO por herr/camara/gen_tabla_c.py desde work/h2/camdriver0_limpio.txt (el camdriver0.dat del original): NO editar a mano. */')
print('#ifdef TELEFONO')
print('#include "hal.h"')
print('/* %d pasos de 3 bytes {tipo, a, b}: tipo 0 = escribir el registro a con el valor b (el banco lo cambia FF); tipo 1 = esperar (a | b << 8) ms */' % len(pasos))
print('const u8 cam_tabla[] = {')
for t, x, y in pasos:
    print('    %d, 0x%02X, 0x%02X,' % (0, x, y) if t == 'w' else '    1, 0x%02X, 0x%02X,' % (x & 0xFF, x >> 8))
print('};')
print('const u32 cam_tabla_n = %d;' % len(pasos))
print('#endif')
