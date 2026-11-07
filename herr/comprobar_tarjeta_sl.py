#!/usr/bin/env python3
"""Pista T; (MSW) puesto al día para la ESCRITURA de sectores: comprueba SOBRE EL BINARIO por dónde se puede escribir en la
tarjeta (docs/tarjeta.md §5, docs/tarjeta_escritura.md §6).  make puertas-tarjeta  (antes: make sin-escritura-tarjeta)
  comprobar_tarjeta_sl.py DIR_BUILD        (DIR_BUILD/sistema.elf y DIR_BUILD/w580/…/*.o)
 1. nm de los objetos de la tarjeta (hal/ms.c, hal/ms_db2020.c, api/fat.c, api/tarjeta.c, plat/tarjeta_w580.c): ningún
    símbolo con nombre de formato o borrado; y los de escritura, SOLO los de la lista (MSW: hwo_tarjeta_escribir, ms_escribir,
    escribir_orden); se listan todos.
 2. desensamblado del ELF: las únicas funciones que nombran los registros del anfitrión (0xFC010000..0C) son las de
    hal/ms_db2020.c; nadie más puede arrancar una transacción.
 3. desensamblado de tpc() (la única que escribe el registro de orden, +0): está la puerta «orden == 0x20 || orden ==
    0x24» (el compilador la deja como (orden & 0xFB) == 0x20) o, con la escritura (MSW), «orden - 0x20 <= 1 || orden ==
    0x24» = 0x20, 0x21 y 0x24; nunca 0x10 (formato).
Esto NO sustituye a la prueba dinámica (el emulador cuenta 0 sectores escritos y la imagen queda idéntica): la acompaña."""
import os, re, subprocess, sys
B = sys.argv[1]
OBJ = ['hal/ms.c.o', 'hal/ms_db2020.c.o', 'api/fat.c.o', 'api/tarjeta.c.o', 'plat/tarjeta_w580.c.o']
run = lambda *a: subprocess.run(a, capture_output=True, text=True, check=True).stdout
mal = 0
print('== 1. símbolos de los objetos de la tarjeta')
vedado = re.compile(r'escrib|write|format|borra|erase|grab|program', re.I)
ESCRITURA = ('hwo_tarjeta_leer', 'hwo_tarjeta_escribir', 'ms_escribir', 'escribir_orden')   # (MSW) las únicas puertas de escritura
# (FATW) la FAT que escribe (api/fat.c, api/tarjeta.c) borra FICHEROS, no la tarjeta: sus nombres se admiten EN ESOS DOS objetos, y
# a cambio se comprueba (abajo) que fat.c no nombra ningún hwo_* (solo escribe por el puntero que le da tarjeta.c) y que en
# tarjeta.c el único hwo_* de escritura es hwo_tarjeta_escribir (más vaciar/protegida); ni formato ni borrado de bloques en ningún sitio.
FATW = {'api/fat.c.o': ('fat_borrar', 'rev_ent_escribir', 'rev_ent_escribir.constprop.0'),
        'api/tarjeta.c.o': ('hw_tj_borrar', 'hw_tj_escribible', 'tj_borrar', 'tj_escribir')}
for o in OBJ:
    sim = [l.split() for l in run('arm-none-eabi-nm', '--defined-only', os.path.join(B, 'w580', o)).splitlines()]
    nombres = sorted(s[2] for s in sim if len(s) == 3 and s[1] in 'TtDdBbRr' and not s[2].startswith('$'))
    print('%-22s %s' % (o, ' '.join(nombres)))
    for n in nombres:
        if vedado.search(n) and n not in ESCRITURA and n not in FATW.get(o, ()): print('   *** símbolo sospechoso:', n); mal += 1
    ext = sorted(l.split()[-1] for l in run('arm-none-eabi-nm', '--undefined-only', os.path.join(B, 'w580', o)).splitlines())
    print('%-22s usa de fuera: %s' % ('', ' '.join(ext)))
    for n in ext:
        if re.search(r'nor_|hw_usr_escribir|hw_fs_escribir|hw_guardar|alm_', n): print('   *** llama a algo que escribe:', n); mal += 1
        if o == 'api/fat.c.o' and n.startswith('hwo_'): print('   *** (FATW) fat.c nombra la plataforma:', n); mal += 1
        if o == 'api/tarjeta.c.o' and n.startswith('hwo_') and n not in ('hwo_tarjeta_escribir', 'hwo_tarjeta_vaciar', 'hwo_tarjeta_protegida', 'hwo_tarjeta_leer', 'hwo_tarjeta_paso', 'hwo_tarjeta_generacion', 'hwo_tarjeta_sectores', 'hwo_tarjeta_mala', 'hwo_tarjeta_dormida', 'hwo_tarjeta_preparando',
                                                               'hwo_tarjeta_bloque_sectores', 'hwo_tarjeta_cierre_zona'):   # (28-oct) el cierre: leer el bloque y REGISTRAR la zona (no escriben)
            print('   *** (FATW) tarjeta.c usa:', n); mal += 1
print('== 2. quién nombra los registros del anfitrión 0xFC01000x en el ELF')
dis = run('arm-none-eabi-objdump', '-d', os.path.join(B, 'sistema.elf'))
fn, quien = None, {}
for l in dis.splitlines():
    m = re.match(r'^[0-9a-f]+ <(.+)>:$', l)
    if m: fn = m.group(1); continue
    m = re.search(r'\.word\s+0x(fc0100[0-9a-f]{2})', l)
    if m: quien.setdefault(fn, set()).add(m.group(1))
propias = set(l.split()[-1] for l in run('arm-none-eabi-nm', os.path.join(B, 'w580', 'hal/ms_db2020.c.o')).splitlines() if l.split()[-2] in 'Tt')
for f in sorted(quien):
    ok = f.split('.')[0] in propias
    print('   %-16s %s %s' % (f, ' '.join(sorted(quien[f])), '' if ok else '*** NO es de hal/ms_db2020.c'))
    mal += not ok
if not quien: print('   *** nadie: ¿el controlador no está enlazado?'); mal += 1
print('== 3. las constantes de tpc() (hal/ms_db2020.c) y de hal/ms.c')
for o, quiero in (('hal/ms_db2020.c.o', ('tpc',)), ('hal/ms.c.o', None)):
    d = run('arm-none-eabi-objdump', '-d', os.path.join(B, 'w580', o))
    trozo, dentro = [], quiero is None
    for l in d.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', l)
        if m: dentro = quiero is None or m.group(1) in quiero
        elif dentro: trozo.append(l)
    cmps = sorted(set(int(x, 0) for x in re.findall(r'\bcmp\s+\w+, #(\d+|0x[0-9a-f]+)', '\n'.join(trozo))))
    movs = sorted(set(int(x, 0) for x in re.findall(r'\bmovs?\s+\w+, #(\d+|0x[0-9a-f]+)', '\n'.join(trozo))))
    print('   %-20s comparaciones con: %s' % (o, ' '.join('0x%X' % c for c in cmps)))
    print('   %-20s constantes cargadas: %s' % ('', ' '.join('0x%X' % c for c in movs)))
    if quiero:                                               # la puerta de tpc(): «orden == 0x20 || orden == 0x24». El compilador la deja como
        t = '\n'.join(trozo)                                 # (orden & 0xFB) == 0x20, que es exactamente eso: se enseña y se exige una de las dos formas
        m = re.search(r'and\s+(\w+), \1, #251.*\n.*cmp\s+\1, #32', t)
        m2 = re.search(r'sub\s+(\w+), (\w+), #32\n.*cmp\s+\2, #36.*\n.*cmpne\s+\1, #1', t)   # (MSW) 0x20, 0x21 o 0x24
        if m: print('   la puerta de órdenes en tpc():  ' + ' ; '.join(x.split('\t', 2)[-1].replace('\t', ' ') for x in m.group(0).splitlines()) + '   = solo 0x20 y 0x24')
        elif m2: print('   la puerta de órdenes en tpc():  ' + ' ; '.join(x.split('\t', 2)[-1].replace('\t', ' ') for x in m2.group(0).splitlines()) + '   = solo 0x20, 0x21 (escribir) y 0x24')
        else: print('   *** no encuentro la puerta de órdenes (0x20 / 0x21 / 0x24) en tpc()'); mal += 1
print('BIEN: en el binario solo se escribe en la tarjeta por hwo_tarjeta_escribir -> ms_escribir -> orden 0x21; ni formato ni borrado' if not mal else 'MAL: %d cosas que mirar' % mal)
sys.exit(1 if mal else 0)
