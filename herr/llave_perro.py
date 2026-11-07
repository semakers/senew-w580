#!/usr/bin/env python3
"""(R3; auditoría AUD6, M2) ¿Lleva este main la LLAVE del perro de hardware? Busca en el binario la secuencia ARM de hal/arranque.S
(paso 9): mov r1,#0xAA · str r1,[r0,#0x10] · mov r1,#0x5A · str r1,[r0,#0x24] · mov r1,#0xA5 · str r1,[r0,#0x2C]
(F9010010 <- AA, F9010024 <- 5A, F901002C <- A5). Sale con 0 si está, 1 si no.   llave_perro.py MAIN.bin"""
import struct, sys
LLAVE = struct.pack('<6I', 0xE3A010AA, 0xE5801010, 0xE3A0105A, 0xE5801024, 0xE3A010A5, 0xE580102C)
def tiene_llave(d):
    return LLAVE in d[:0x2000]                            # está en el arranque, en los primeros KiB de la imagen
if __name__ == '__main__':
    sys.exit(0 if tiene_llave(open(sys.argv[1], 'rb').read()) else 1)
