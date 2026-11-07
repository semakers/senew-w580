#!/usr/bin/env python3
"""Comprueba que lo que programa la NOR puede ejecutarse con la NOR en modo de órdenes:
las funciones de la sección .ramfn (hal/nor.c) viven en RAM (0x20……) y no saltan ni leen nada de la
flash (0x44……..0x47……): ni BL a memcpy/libgcc, ni tablas en .rodata. Además, con NOR_ESCRITURA=0 no debe
quedar ninguna rutina de escritura.   Uso: comprobar_ramfn.py sistema.elf [--sin-escritura]"""
import os, re, subprocess, sys
# (I1; W6F) las zonas escribibles empiezan ahora en W6F_INI (del Makefile, en el entorno; 0x44500000 por omisión): las rutinas de
# RAM comparan contra esos límites, así que son literales permitidos (fs2/INTEGRACION.md §1)
ESCR_INI = int(os.environ.get('W6F_INI', '0x44500000'), 16)
elf = sys.argv[1]
sim = subprocess.run(['arm-none-eabi-nm', '-n', elf], capture_output=True, text=True, check=True).stdout
fn = {m.group(2): int(m.group(1), 16) for m in re.finditer(r'^([0-9a-f]+) \w (nor_ram_\w+)$', sim, re.M)}
if '--sin-escritura' in sys.argv:
    mal = [n for n in fn if n not in ('nor_ram_leer', 'nor_ram_estado')]   # (R3) las de solo lectura (90/98/70/FF); la guarda de S7 ya no existe   # (S7) las dos de solo lectura: la de hal/nor.c y la de la guarda (plat/nor_guarda.c)
    print('sin escritura:', 'BIEN, no hay rutinas de borrado/programa' if not mal else 'MAL: quedan ' + ', '.join(mal))
    sys.exit(1 if mal else 0)
if not fn:
    print('.ramfn: no hay rutinas nor_ram_* (¿un hito?)'); sys.exit(0)
des = subprocess.run(['arm-none-eabi-objdump', '-d', '-j', '.data', elf], capture_output=True, text=True, check=True).stdout
ini, fin, mal, n = min(fn.values()), None, [], 0
dentro = False
for lin in des.splitlines():
    m = re.match(r'^([0-9a-f]{8}) <(\w+)>:', lin)
    if m:
        dentro = m.group(2) in fn
        continue
    m = re.match(r'^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)\s*(.*)$', lin)
    if not dentro or not m:
        continue
    dir_, palabra, op, args = int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)
    n += 1; fin = dir_
    if not 0x20000000 <= dir_ < 0x21000000:
        mal.append('%08x: la rutina no está en RAM' % dir_)
    if re.match(r'^(bl|b|blx)(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?$', op):
        d = re.match(r'^([0-9a-f]+)', args)
        if not d or not ini <= int(d.group(1), 16) <= dir_ + 0x1000 or not 0x20000000 <= int(d.group(1), 16) < 0x21000000:
            mal.append('%08x: salto fuera de RAM: %s %s' % (dir_, op, args))
    if op in ('.word', 'andmi', 'strbmi', 'ldrbmi') or op.startswith('.'):
        if 0x44000000 <= palabra < 0x48000000 and palabra not in (0x44000000, 0x48000000) and not (ESCR_INI <= palabra <= 0x45E00000):
            mal.append('%08x: literal que apunta a la flash: %08x' % (dir_, palabra))
    if 'swi' in op or 'svc' in op:
        mal.append('%08x: %s' % (dir_, op))
print('.ramfn: %d instrucciones/literales en %s, 0x%08x..0x%08x' % (n, ', '.join(sorted(fn)), ini, fin or 0))
for x in mal:
    print('MAL', x)
print('BIEN: nada de lo que programa la NOR toca la flash' if not mal else 'HAY FALLOS')
sys.exit(1 if mal else 0)
