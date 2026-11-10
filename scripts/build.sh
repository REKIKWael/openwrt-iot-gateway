#!/bin/sh
# Reconstruit l'image OpenWrt de la passerelle depuis les sources.
# Usage : ./scripts/build.sh [dossier_openwrt]   (par defaut : ~/openwrt)
set -e

OPENWRT_TAG=v25.12.6
REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
WORK_DIR=${1:-$HOME/openwrt}

[ -d "$WORK_DIR" ] || git clone https://github.com/openwrt/openwrt.git "$WORK_DIR"
cd "$WORK_DIR"
git checkout "$OPENWRT_TAG"

./scripts/feeds update -a
./scripts/feeds install -a

# Paquet iotd et fichiers de l'image, relies depuis ce depot
ln -sfn "$REPO_DIR/package/iotd" package/iotd
ln -sfn "$REPO_DIR/image-files" files

# Configuration minimale, completee par make defconfig
cp "$REPO_DIR/configs/diffconfig" .config
make defconfig

make download -j"$(nproc)"
make -j"$(nproc)"

echo "Image : $WORK_DIR/bin/targets/x86/64/"
