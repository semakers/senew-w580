#!/usr/bin/env python3
"""camdriver_expandir.py — expande el guion del sensor OV2640 del ORIGINAL (camdriver0.dat) a listas planas de pasos para el driver de senew.

El guion (`/ifs/settings/camera/camdriver0.dat`, sin comentarios en work/h2/camdriver0_limpio.txt; formato de cam_ctrllib) es un programa con macros:
  NOMBRE=cuerpo;     definición          #NOMBRE / #NOMBRE(a , b …)   llamada (los argumentos del macro se llaman !0, !1…)
  `RR VV $f`         escribe el valor VV (hex) en el registro RR del sensor       `$tNNNN` / `$dNNNN`   espera NNNN ms (hex)
  `$cN VV … $cl … $cx`  si el registro interno N == VV … si no … fin           (se resuelve con las constantes de `--contexto`)
Solo se implementa lo que usan seq_ping, seq_init, seq_vf_config_N y seq_vf_start/stop; el resto ($u = ganchos al C de la biblioteca, $s/$m/$z/$b/$p = registros
internos, lecturas) se ignora o se resuelve con el contexto, y lo que no se entiende se IMPRIME como aviso (nunca se traga). Salida: lista de
(tipo, a, b) con tipo 'w' (escribir banco/registro), 'd' (esperar ms), 'r' (leer registro y comparar: solo el ping).

Contexto (lo que la biblioteca decide en tiempo de ejecución):  modulo = 0 («Premier», #SELECT_PREMIER_OR_FLEX toma la 1.ª rama) o 1 («Flex», la 2.ª)
según la lectura de P0.7 con el pin como ENTRADA al encender; rev = 'REV2B' si el registro 0x0B del banco 1 no vale 0x40, si no 'REV1A'.
Uso:  camdriver_expandir.py SECUENCIA [--modulo 0|1] [--rev REV2B|REV1A] [--c]   (SECUENCIA = seq_init, seq_vf_config_23, seq_vf_start, …)"""
import argparse
import os
import re
import sys

FICHERO = os.environ.get('CAMDRIVER', '')   # el /ifs/settings/camera/camdriver0.dat de TU teléfono (no se distribuye: es del fabricante)


def cargar(fichero=FICHERO):
    """devuelve {nombre: cuerpo} de todas las definiciones NOMBRE=cuerpo; (el prefijo «NNN:» de las líneas es el nº de línea original)"""
    txt = open(fichero, encoding='utf-8', errors='replace').read()
    txt = re.sub(r'(?m)^\s*\d+:', ' ', txt)
    txt = re.sub(r'//[^\n]*', ' ', txt)
    defs = {}
    for m in re.finditer(r'(?s)(?<![\w$#!])([A-Za-z_][A-Za-z_0-9]*)\s*=([^;]*);', txt):
        defs.setdefault(m.group(1), m.group(2))
    return defs


def separar_args(s):
    """parte «a , b» respetando paréntesis anidados"""
    out, d, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            d += 1
        if ch == ')':
            d -= 1
        if ch == ',' and d == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    out.append(cur)
    return [x.strip() for x in out]


def expandir_macros(txt, defs, args=(), prof=0):
    """sustituye #NOMBRE y #NOMBRE(args) recursivamente (y !n por los argumentos del macro en curso)"""
    if prof > 20:
        raise RuntimeError('recursión de macros demasiado profunda')
    out = ''
    i = 0
    while i < len(txt):
        c = txt[i]
        if c == '#':
            m = re.match(r'#([A-Za-z_][A-Za-z_0-9]*)', txt[i:])
            if not m:
                out += c
                i += 1
                continue
            nombre = m.group(1)
            j = i + m.end()
            a = []
            if j < len(txt) and txt[j] == '(':
                d, k = 1, j + 1
                while k < len(txt) and d:
                    d += (txt[k] == '(') - (txt[k] == ')')
                    k += 1
                a = [expandir_macros(x, defs, args, prof + 1) for x in separar_args(txt[j + 1:k - 1])]
                j = k
            if nombre not in defs:
                raise RuntimeError('macro desconocido #%s' % nombre)
            out += ' ' + expandir_macros(defs[nombre], defs, tuple(a), prof + 1) + ' '
            i = j
        elif c == '!' and i + 1 < len(txt) and txt[i + 1].isdigit():
            out += ' ' + args[int(txt[i + 1])] + ' '
            i += 2
        else:
            out += c
            i += 1
    return out


def ejecutar(txt, ctx):
    """recorre los tokens ya sin macros y devuelve los pasos; resuelve $c/$cl/$cx con ctx['reg'] (registros internos 5 = módulo, 6 = ID)"""
    tok = txt.replace('(', ' ').replace(')', ' ').replace(',', ' ').split()
    pasos, avisos, pila, vivo = [], [], [], True
    i = 0
    prev = []
    while i < len(tok):
        t = tok[i]
        if t == '$f':
            if len(prev) >= 2 and vivo:
                pasos.append(('w', int(prev[-2], 16), int(prev[-1], 16)))
            prev = []
        elif re.fullmatch(r'\$[td][0-9a-fA-F]+', t):
            if vivo:
                pasos.append(('d', int(t[2:], 16), 0))
        elif re.fullmatch(r'\$c[0-9a-fA-F]', t):
            n = int(t[2], 16)
            val = int(tok[i + 1], 16)
            i += 1
            cond = ctx['reg'].get(n)
            if cond is None:
                avisos.append('condición $c%d sin valor en el contexto (se toma la rama «si no»)' % n)
                cond = None
            pila.append((vivo, cond == val, False))     # (vivo antes, la condición vale, ya cambió a «si no»)
            vivo = vivo and (cond == val)
        elif t == '$cl':
            v0, ok, _ = pila.pop()
            pila.append((v0, ok, True))
            vivo = v0 and not ok
        elif t == '$cx':
            v0, _, _ = pila.pop()
            vivo = v0
        elif re.fullmatch(r'\$[a-z][0-9a-fA-F]*', t) or t.startswith('$'):
            if re.fullmatch(r'\$[u]\d*', t):
                pass                                    # gancho al C de la biblioteca (AE/AWB, tablas JPEG…): no es escritura del sensor
            elif t.startswith(('$s', '$m', '$z', '$b', '$p', '$h', '$w', '$g', '$q', '$r', '$<', '$>', '$-', '$+')):
                avisos.append('token de máquina interna ignorado: %s (siguiente: %s)' % (t, tok[i + 1] if i + 1 < len(tok) else ''))
            else:
                avisos.append('token desconocido: ' + t)
            prev = []
        else:
            prev.append(t)
        i += 1
    if pila:
        avisos.append('$c sin $cx al final')
    return pasos, avisos


def secuencia(defs, nombre, modulo=0, rev='REV2B'):
    if nombre not in defs:
        raise SystemExit('no existe %s' % nombre)
    cuerpo = expandir_macros(defs[nombre], defs)
    # #SELECT_PREMIER_OR_FLEX(A , B) ya vino expandido como «$c5 00 A $cl B $cx» (reg 5 = módulo); seq_init usa reg 6 = byte 0x0B del banco 1 (== 0x40 -> REV1A)
    ctx = {'reg': {5: modulo, 6: 0x40 if rev == 'REV1A' else 0x42}}
    return ejecutar(cuerpo, ctx)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('secuencia')
    ap.add_argument('--modulo', type=int, default=0)
    ap.add_argument('--rev', default='REV2B')
    ap.add_argument('--c', action='store_true', help='salida como tabla C')
    a = ap.parse_args()
    defs = cargar()
    pasos, avisos = secuencia(defs, a.secuencia, a.modulo, a.rev)
    for av in avisos:
        print('AVISO:', av, file=sys.stderr)
    if a.c:
        print('/* %s (modulo %d, %s): %d pasos; {tipo, reg|ms, val} tipo 0 = escribir, 1 = esperar ms */' % (a.secuencia, a.modulo, a.rev, len(pasos)))
        for t, x, y in pasos:
            print('    { %d, 0x%02X, 0x%02X },' % (0 if t == 'w' else 1, x, y))
    else:
        for t, x, y in pasos:
            print('%s %02X %02X' % (t, x, y) if t == 'w' else 'd %d ms' % x)
        print('# %d pasos' % len(pasos))


if __name__ == '__main__':
    main()
