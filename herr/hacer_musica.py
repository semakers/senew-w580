#!/usr/bin/env python3
"""La música DE CASA (pista S7): escribe por programa unos ficheros MIDI nuestros —melodías de dominio público
(Beethoven, Petzold) y una escala de prueba—, nada de terceros:

  sistema/fs_propio/musica/*.mid     el contenido de /musica de nuestro FS (W6FS)
  sistema/shell/musica_casa.h        UNA de ellas metida en el propio main («casa:…»), para que la app de
                                     Música tenga algo que tocar aunque no haya ningún sistema de ficheros

Uso: python3 herr/hacer_musica.py        (desde sistema/; lo llama «make musica»)

Cada pieza es una lista de voces; cada voz, una lista de (nota MIDI o None = silencio, negras). Formato 1,
división 480, una pista por voz más la de tempo. Las alturas son las de siempre: 60 = do4, 69 = la4.
"""
import os
import struct

AQUI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIV = 480
N = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def n(s):
    """'C4' 'F#5' 'Bb3' -> nota MIDI; '-' = silencio"""
    if s == '-':
        return None
    v = N[s[0]]
    r = s[1:]
    if r[0] == '#':
        v, r = v + 1, r[1:]
    elif r[0] == 'b':
        v, r = v - 1, r[1:]
    return 12 * (int(r) + 1) + v


def voz(texto):
    """'E5:1 F5:1 G5:2 -:1' -> [(nota, negras)]"""
    out = []
    for t in texto.split():
        a, b = t.split(':')
        out.append((n(a), float(b)))
    return out


def vlq(v):
    b = [v & 0x7F]
    v >>= 7
    while v:
        b.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(b))


def pista(eventos):
    """eventos = [(pulso, orden, bytes)] -> trozo MTrk"""
    d, t = b'', 0
    for p, _, e in sorted(eventos):
        d += vlq(p - t) + e
        t = p
    d += vlq(0) + b'\xFF\x2F\x00'
    return b'MTrk' + struct.pack('>I', len(d)) + d


def smf(voces, ppm, nombre, velocidades=None, despegue=0.9):
    """despegue: fracción de la figura que suena (el resto es el respiro entre notas)"""
    pistas = [pista([(0, 0, b'\xFF\x51\x03' + struct.pack('>I', 60000000 // ppm)[1:]),
                     (0, 1, b'\xFF\x03' + vlq(len(nombre.encode())) + nombre.encode())])]
    for i, v in enumerate(voces):
        ev, t = [], 0
        vel = (velocidades or [96, 72, 64, 64])[i % 4]
        for nota, negras in v:
            dur = int(round(negras * DIV))
            if nota is not None:
                ev.append((t, 1, bytes([0x90 | i, nota, vel])))
                ev.append((t + max(1, int(dur * despegue)), 0, bytes([0x80 | i, nota, 0])))
            t += dur
        pistas.append(pista(ev))
    return b'MThd' + struct.pack('>IHHH', 6, 1, len(pistas), DIV) + b''.join(pistas)


# ---- las piezas -------------------------------------------------------------------------------------------
ODA = [   # Beethoven, 9.ª sinfonía, el tema del «Himno a la alegría» (1824), en do mayor: melodía, tercera y bajo
    voz('E5:1 E5:1 F5:1 G5:1  G5:1 F5:1 E5:1 D5:1  C5:1 C5:1 D5:1 E5:1  E5:1.5 D5:0.5 D5:2 '
        'E5:1 E5:1 F5:1 G5:1  G5:1 F5:1 E5:1 D5:1  C5:1 C5:1 D5:1 E5:1  D5:1.5 C5:0.5 C5:2'),
    voz('C5:1 C5:1 D5:1 E5:1  E5:1 D5:1 C5:1 B4:1  G4:1 G4:1 B4:1 C5:1  C5:1.5 B4:0.5 B4:2 '
        'C5:1 C5:1 D5:1 E5:1  E5:1 D5:1 C5:1 B4:1  G4:1 G4:1 B4:1 C5:1  B4:1.5 G4:0.5 G4:2'),
    voz('C3:2 C3:2  G3:2 G3:2  C3:2 G3:2  C3:2 G3:2  C3:2 C3:2  G3:2 G3:2  C3:2 G3:2  G3:2 C3:2'),
]
MINUETO = [   # Christian Petzold, Minueto en sol mayor (c. 1725; del cuaderno de Ana Magdalena Bach), 8 compases
    voz('D5:1 G4:0.5 A4:0.5 B4:0.5 C5:0.5  D5:1 G4:1 G4:1  E5:1 C5:0.5 D5:0.5 E5:0.5 F#5:0.5  G5:1 G4:1 G4:1 '
        'C5:1 D5:0.5 C5:0.5 B4:0.5 A4:0.5  B4:1 C5:0.5 B4:0.5 A4:0.5 G4:0.5  F#4:1 G4:0.5 A4:0.5 B4:0.5 G4:0.5  A4:3'),
    voz('G3:2 A3:1  B3:3  C4:3  B3:3  A3:3  G3:3  D4:1 B3:1 G3:1  D4:1 D3:0.5 C4:0.5 B3:0.5 A3:0.5'),
]
ESCALA = [   # prueba: do mayor de do3 a do6 (las de abajo, por el zumbador, suben de octava) y un acorde final
    voz('C3:1 D3:1 E3:1 F3:1 G3:1 A3:1 B3:1 C4:1 D4:1 E4:1 F4:1 G4:1 A4:1 B4:1 C5:1 D5:1 E5:1 F5:1 G5:1 A5:1 B5:1 C6:2 -:1 C5:4'),
    voz('-:25 E4:4'),
    voz('-:25 G4:4'),
    voz('-:25 C4:4'),
]
PIEZAS = [('Himno a la alegría', ODA, 132), ('Minueto en sol', MINUETO, 126), ('Escala de prueba', ESCALA, 240)]
DE_CASA = 'Himno a la alegría'


def main():
    d = os.path.join(AQUI, 'fs_propio', 'musica')
    os.makedirs(d, exist_ok=True)
    for nombre, voces, ppm in PIEZAS:
        b = smf(voces, ppm, nombre)
        open(os.path.join(d, nombre + '.mid'), 'wb').write(b)
        print('%-22s %5d bytes, %d voces, %d ppm' % (nombre + '.mid', len(b), len(voces), ppm))
        if nombre == DE_CASA:
            filas = [', '.join('0x%02X' % x for x in b[i:i + 16]) for i in range(0, len(b), 16)]
            open(os.path.join(AQUI, 'shell', 'musica_casa.h'), 'w', encoding='utf-8').write(
                '/* GENERADO por herr/hacer_musica.py: no editar. La pista «de casa» de la app de Música: el mismo fichero\n'
                ' * que fs_propio/musica/%s.mid, dentro del main para que haya algo que tocar sin sistema de ficheros. */\n'
                '#define MUSICA_CASA_NOMBRE "%s"\n'
                'static const unsigned char musica_casa[%d] = {\n    %s\n};\n' % (nombre, nombre, len(b), ',\n    '.join(filas)))


if __name__ == '__main__':
    main()
