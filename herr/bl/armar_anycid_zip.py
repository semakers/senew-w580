#!/usr/bin/env python3
"""armar_anycid_zip.py — Crea el zip R8BE001 del AnyCID con nuestro executor.b (BL installer) para TODOS los CXC del W580.

Uso:
  python3 -I herr/bl/armar_anycid_zip.py [--destino DIR]

Lee build/installer/executor.b (256 KiB, producido por empaquetar_installer.py) y crea un zip idéntico al original de SE
pero con nuestro executor.b adentro, para cada variante R8BE001 que seftool-senew tiene. Así seftool upload-anycid funciona
sea cual sea el CXC del teléfono.

El zip de plataforma (DB2020.zip) NO se toca — es el genérico de SE con executor.jar y customize_upgrade.xml.
"""
import os
import sys
import zipfile

AQUI = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXECUTOR_B = os.path.join(AQUI, 'build', 'installer', 'executor.b')
SEFTOOL_ANYCID = os.environ.get('SEFTOOL_ANYCID', os.path.join(AQUI, '..', 'seftool-senew', 'anycid'))   # la carpeta anycid de seftool-senew
DESTINO_DEF = SEFTOOL_ANYCID


def main():
    destino = DESTINO_DEF
    if '--destino' in sys.argv:
        destino = sys.argv[sys.argv.index('--destino') + 1]

    if not os.path.isfile(EXECUTOR_B):
        sys.exit(f'Falta {EXECUTOR_B}. Corre: python3 -I herr/bl/empaquetar_installer.py')
    eb = open(EXECUTOR_B, 'rb').read()
    if len(eb) != 256 * 1024:
        sys.exit(f'executor.b tiene {len(eb)} B, se esperan {256*1024} B')

    originales = sorted(f for f in os.listdir(SEFTOOL_ANYCID)
                        if f.startswith('R8BE001') and f.endswith('.zip'))
    if not originales:
        sys.exit(f'No hay zips R8BE001 en {SEFTOOL_ANYCID}')

    print(f'executor.b: {len(eb)} B')
    print(f'{len(originales)} variantes R8BE001 a reescribir:')
    respaldados = 0
    for nombre in originales:
        ruta = os.path.join(destino, nombre)
        bak = ruta + '.original-senew'
        if not os.path.isfile(bak):
            os.rename(ruta, bak)
            respaldados += 1
        with zipfile.ZipFile(ruta, 'w', zipfile.ZIP_STORED) as zf:
            zf.mkdir('tpa/')
            zf.mkdir('tpa/preset/')
            zf.mkdir('tpa/preset/custom/')
            zf.writestr('tpa/preset/custom/executor.b', eb)
        print(f'  {nombre} ({os.path.getsize(ruta)} B)')
    print(f'\n{respaldados} originales respaldados como .original-senew')
    print(f'DB2020.zip NO se tocó (es el genérico de SE con executor.jar)')
    print(f'\nPara restaurar los originales:')
    print(f'  for f in {destino}/R8BE001*.original-senew; do mv "$f" "${{f%.original-senew}}"; done')


if __name__ == '__main__':
    main()
