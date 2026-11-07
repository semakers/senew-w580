#!/bin/sh
# traer_btstack.sh — descarga BTstack (BlueKitchen GmbH, https://github.com/bluekitchen/btstack) en bt/terceros/btstack, en el
# commit EXACTO con el que se probó senew (la rama master del 15-sep-2026, posterior a la v1.8.3), y le aplica el parche de senew
# (bt/parches/). BTstack NO se distribuye con senew: su licencia es la suya (BSD con una cláusula de uso NO comercial; para uso
# comercial hay que pedir licencia a BlueKitchen). Ver NOTICE. Sin BTstack se puede compilar senew sin Bluetooth: «make w580 BT=0».
#   sh herr/traer_btstack.sh            (necesita git y acceso a github.com; BTSTACK_URL=… para usar otra copia)
set -e
cd "$(dirname "$0")/.."
D=bt/terceros/btstack
REV=e38553977a25fb0b55b383c72c289be0975f422c
URL=${BTSTACK_URL:-https://github.com/bluekitchen/btstack.git}
if [ -f "$D/src/hci.c" ]; then echo "BTstack ya está en $D (bórralo para volver a traerlo)"; exit 0; fi
rm -rf "$D"; mkdir -p "$D"
git -C "$D" init -q
git -C "$D" fetch -q --depth 1 "$URL" "$REV"
git -C "$D" checkout -q FETCH_HEAD
for p in bt/parches/*.patch; do git -C "$D" apply --whitespace=nowarn "$(pwd)/$p"; echo "aplicado: $p"; done
echo "BTstack listo en $D ($(git -C "$D" rev-parse --short HEAD) + parches de senew). Licencia: $D/LICENSE"
