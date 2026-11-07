#!/usr/bin/env python3
"""emu.py — senew en el emulador (github.com/semakers/senew-emu, máquina «w580» de QEMU) por QMP: arrancar, pulsar teclas, capturar.
Solo la biblioteca estándar de Python 3.8+. Funciona en Linux y en macOS.

  python3 herr/emu.py arrancar --qemu RUTA/qemu-system-arm [--flash build/flash-w580.bin] [--opts k=v,...]
  python3 herr/emu.py tecla NOMBRE [VECES]       pulsa y suelta (NOMBRE: ver TECLAS; p. ej. ok, arriba, abajo, izq, der, sel_izq,
                                                 sel_der, atras, c, 0..9, *, #; también en inglés: up, down, left, right, back…)
  python3 herr/emu.py captura salida.png         la pantalla (240x320) en PNG (o .ppm, tal cual la da QEMU)
  python3 herr/emu.py espera SEGUNDOS
  python3 herr/emu.py parar                      cierra la instancia (QMP «quit»)
  python3 herr/emu.py --qemu Q --flash F guion G.txt [--captura final.png]
        arranca, ejecuta el guion (una orden por línea: «espera S», «tecla N [veces]», «captura F.png», «# comentario») y para.

La instancia es UNA por usuario (su socket QMP va en el directorio temporal del sistema); «arrancar» se niega si ya hay una.
Al arrancar, senew enseña el menú de encendido del teléfono (el PMU emulado dice «encendido con la tecla»): «tecla sel_der» lo quita."""
import argparse, json, os, socket, struct, subprocess, sys, tempfile, time, zlib

DIR = os.path.join(tempfile.gettempdir(), 'senew-emu-%d' % os.getuid())
QMP = os.path.join(DIR, 'qmp.sock')
PID = os.path.join(DIR, 'qemu.pid')
MAQUINA = 'w580,i2c=0.0x4a:1.0x40,gpio-in=2.6=0'

# celda del teclado = columna*5 + fila (así las numera la máquina w580: propiedad kp-cell; pulsar = celda | 0x100, soltar = celda)
TECLAS = {'3': 0, '6': 1, '1': 2, '8': 5, 'sel_der': 6, 'atras': 7, 'c': 8, '2': 10, '0': 11, 'sel_izq': 12, '5': 13, '9': 15,
          '4': 16, '7': 17, '*': 18, '#': 19, 'vol-': 21, 'vol+': 24, 'abajo': 25, 'izq': 26, 'arriba': 27, 'ok': 28, 'der': 29}
INGLES = {'softright': 'sel_der', 'right_soft': 'sel_der', 'back': 'atras', 'softleft': 'sel_izq', 'left_soft': 'sel_izq',
          'down': 'abajo', 'left': 'izq', 'up': 'arriba', 'right': 'der', 'clear': 'c', 'star': '*', 'hash': '#'}


class Qmp:
    def __init__(self, ruta, espera=20.0):
        t0 = time.time()
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(ruta)
                break
            except OSError:
                self.s.close()
                if time.time() - t0 > espera:
                    sys.exit('no hay emulador escuchando en %s (¿«emu.py arrancar»?)' % ruta)
                time.sleep(0.1)
        self.s.settimeout(60)                  # QMP atiende a UN cliente a la vez: si otro lo tiene, no colgarse para siempre
        self.f = self.s.makefile('rw')
        self.f.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, nombre, **args):
        self.f.write(json.dumps({'execute': nombre, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                if 'error' in r:
                    raise RuntimeError('%s: %s' % (nombre, r['error'].get('desc')))
                return r['return']


def ppm_a_png(ppm, png):
    d = open(ppm, 'rb').read()
    _, dims, _, px = d.split(b'\n', 3)
    w, h = map(int, dims.split())
    filas = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def trozo(t, x):
        return struct.pack('>I', len(x)) + t + x + struct.pack('>I', zlib.crc32(t + x) & 0xFFFFFFFF)
    open(png, 'wb').write(b'\x89PNG\r\n\x1a\n' + trozo(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                          trozo(b'IDAT', zlib.compress(filas, 9)) + trozo(b'IEND', b''))
    no_negros = sum(1 for i in range(0, w * h * 3, 3) if px[i] or px[i + 1] or px[i + 2])
    return w, h, no_negros


def vivo():
    try:
        pid = int(open(PID).read())
        os.kill(pid, 0)
        return pid
    except (OSError, ValueError):
        return 0


def arrancar(qemu, flash, opts=''):
    if vivo():
        sys.exit('ya hay un emulador de senew en marcha (pid %d): «emu.py parar» primero' % vivo())
    if not os.path.isfile(flash) or os.path.getsize(flash) != 64 << 20:
        sys.exit('%s: hace falta una flash de 64 MiB («make w580» da build/flash-w580.bin)' % flash)
    os.makedirs(DIR, exist_ok=True)
    if os.path.exists(QMP):
        os.remove(QMP)
    cmd = [qemu, '-name', 'senew-emu', '-M', MAQUINA + (',' + opts if opts else ''), '-bios', flash,
           '-display', 'none', '-serial', 'none', '-monitor', 'none', '-qmp', 'unix:%s,server=on,wait=off' % QMP]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(os.path.join(DIR, 'qemu.err'), 'w'), start_new_session=True)
    open(PID, 'w').write(str(p.pid))
    t0 = time.time()
    while not os.path.exists(QMP):
        if p.poll() is not None:
            sys.exit('QEMU no arrancó: ' + open(os.path.join(DIR, 'qemu.err')).read()[:600])
        if time.time() - t0 > 20:
            sys.exit('QEMU no abrió su socket QMP en 20 s')
        time.sleep(0.1)
    print('emulador en marcha (pid %d), QMP en %s' % (p.pid, QMP))


def tecla(q, nombre, veces=1):
    n = INGLES.get(nombre, nombre)
    if n not in TECLAS:
        sys.exit('tecla desconocida «%s»; las que hay: %s' % (nombre, ' '.join(sorted(TECLAS))))
    for _ in range(veces):
        q.cmd('qom-set', path='/machine', property='kp-cell', value=TECLAS[n] | 0x100)
        time.sleep(0.15)
        q.cmd('qom-set', path='/machine', property='kp-cell', value=TECLAS[n])
        time.sleep(0.35)


def captura(q, salida):
    ppm = salida if salida.endswith('.ppm') else os.path.join(DIR, 'captura.ppm')
    q.cmd('screendump', filename=os.path.abspath(ppm))
    if ppm != salida:
        w, h, nn = ppm_a_png(ppm, salida)
        print('%s: %dx%d, %d píxeles no negros' % (salida, w, h, nn))
        return nn
    return -1


def parar(q=None):
    pid = vivo()
    if not pid:
        print('no hay emulador en marcha')
        return
    try:
        (q or Qmp(QMP, espera=2)).cmd('quit')    # con la conexión que ya haya: QMP no atiende a dos clientes a la vez
    except Exception:
        pass
    for _ in range(50):
        if not vivo():
            break
        time.sleep(0.1)
    if vivo():
        os.kill(pid, 9)
    print('emulador parado (pid %d)' % pid)


def guion(q, ruta):
    nn = None
    for linea in open(ruta, encoding='utf-8'):
        p = linea.split('#', 1)[0].split()
        if not p:
            continue
        if p[0] == 'espera':
            time.sleep(float(p[1]))
        elif p[0] == 'tecla':
            tecla(q, p[1], int(p[2]) if len(p) > 2 else 1)
        elif p[0] == 'captura':
            nn = captura(q, p[1])
        else:
            sys.exit('%s: orden desconocida «%s»' % (ruta, p[0]))
    return nn


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--qemu', default=os.environ.get('QEMU', 'qemu-system-arm'))
    ap.add_argument('--flash', default='build/flash-w580.bin')
    ap.add_argument('--opts', default='')
    ap.add_argument('--captura', default=None, help='con «guion»: una captura al final')
    ap.add_argument('orden')
    ap.add_argument('args', nargs='*')
    a = ap.parse_args()
    if a.orden == 'arrancar':
        arrancar(a.qemu, a.flash, a.opts)
    elif a.orden == 'parar':
        parar()
    elif a.orden == 'tecla':
        tecla(Qmp(QMP), a.args[0], int(a.args[1]) if len(a.args) > 1 else 1)
    elif a.orden == 'captura':
        captura(Qmp(QMP), a.args[0])
    elif a.orden == 'espera':
        time.sleep(float(a.args[0]))
    elif a.orden == 'guion':
        arrancar(a.qemu, a.flash, a.opts)
        q = None
        try:
            q = Qmp(QMP)
            guion(q, a.args[0])
            if a.captura:
                nn = captura(q, a.captura)
                if nn == 0:
                    sys.exit('la pantalla está NEGRA')
        finally:
            parar(q)
    else:
        sys.exit('orden desconocida: %s (arrancar, tecla, captura, espera, parar, guion)' % a.orden)


if __name__ == '__main__':
    main()
