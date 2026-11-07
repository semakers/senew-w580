#!/usr/bin/env python3
"""(Fase 3b) senus_tabla.json de una compilación CON PUENTE (contrato del canal §8; diseño §4.4): lo que SENUS necesita para entrar al BL por el puente en el
caso de que el aparato aún no tenga canal, sacado del ELF que se grabó. Sin puente no hay tabla: sale sin escribir nada.

  hacer_tabla_senus.py SISTEMA.elf SISTEMA.bin senus_tabla.json

Las direcciones van como enteros JSON y además en texto hexadecimal, para cotejarlas a ojo con cliente_puente.dart."""
import json, os, re, subprocess, sys
elf, bina, sal = sys.argv[1:4]
MAIN_INI = int(os.environ.get('MAIN_INI', '0x44180000'), 16)
sim = {}
for l in subprocess.run(['arm-none-eabi-nm', elf], capture_output=True, text=True, check=True).stdout.split('\n'):
    p = l.split()
    if len(p) == 3:
        sim[p[2]] = int(p[0], 16)
if 'pu_ident' not in sim:
    print('%s: sin puente, sin tabla' % elf)
    sys.exit(0)
b = open(bina, 'rb').read()
d = sim['pu_ident'] - MAIN_INI
assert 0 <= d < len(b), 'pu_ident fuera del binario'
ident = b[d:d + 28].split(b'\0')[0].decode()
assert ident.startswith('PUEN1 '), ident
t = {'id_puente': ident}
h = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'api', 'hw.h'), encoding='utf-8').read()
t['version'] = re.search(r'#define SENEW_VERSION "(.*?)"', h).group(1)
t['build'] = int(re.search(r'#define SENEW_BUILD\s+(\d+)', h).group(1))
for n in ('carga_veto', 'hw_carga_fuera', 'reinicio_caliente'):
    t[n] = sim[n]
    t[n + '_hex'] = '0x%08X' % sim[n]
json.dump(t, open(sal, 'w'), indent=2, ensure_ascii=False)
print(sal, t)
