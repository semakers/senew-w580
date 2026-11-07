#!/bin/sh
# (Fase 3b) Las compilaciones del contrato del canal §8, cada una en su B, con su ELF, su senus_tabla.json y sus sha256 en MANIFIESTO.txt.
#   telefono: canal Y puente (la del paquete de SENUS).
#   publica:  canal, sin puente. Solo emulador hasta que SEN_ENTRAR_BL haya funcionado en el aparato.
# Ninguna lleva -DARRANQUE_VIGILADO. NADA de esto se graba desde aquí: solo por SENUS, con respaldo verificado, y por orden del mantenedor.
cd "$(dirname "$0")/.." || exit 1
PRE=${1:-build/agente_a}
for v in telefono publica; do
  B=${PRE}_$v
  case $v in telefono) D="-DMIGAS -DPUENTE -DMP3_IGNORA_CABLE";; publica) D="";; esac
  nice -n 15 make -s -j3 B=$B DEFS="$D" $B/sistema.bin || exit 1
  t=$(wc -c < $B/sistema.bin)
  [ "$t" -le 2359296 ] || { echo "$B/sistema.bin: $t B no cabe en 9 bloques"; exit 1; }
  python3 herr/hacer_tabla_senus.py $B/sistema.elf $B/sistema.bin $B/senus_tabla.json || exit 1
  { echo "senew $(sed -n 's/^#define SENEW_VERSION "\(.*\)"$/\1/p' api/hw.h) build $(sed -n 's/^#define SENEW_BUILD *\([0-9]*\)$/\1/p' api/hw.h): compilación «$v» de herr/compilar_senus.sh"
    echo "DEFS: $D"; echo "tamaño: $t B de 2359296 (9 bloques)"; echo "git: $(git rev-parse --short HEAD) + cambios sin commit (Fase 3b)"
    sha256sum $B/sistema.bin $B/sistema.elf; [ -f $B/senus_tabla.json ] && sha256sum $B/senus_tabla.json
    echo "NO GRABAR: ni emulador ni aparato desde aquí. [sin probar] en silicio."; } > $B/MANIFIESTO.txt
  cat $B/MANIFIESTO.txt
done
