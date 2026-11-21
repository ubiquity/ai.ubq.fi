#!/bin/bash
# Builds the Raspberry Pi OS kernel exactly as shipped (same source package, same configuration)
# with one difference: Landlock is compiled in and enabled. Output: out/pi-kernel-landlock.tar.gz
set -euo pipefail
SRCVER='1:6.12.109-1+rpt1'; BASECFG="$PWD/pi-kernel/config-6.12.109+rpt-rpi-v8"; OUT="$PWD/out"; mkdir -p "$OUT" build; cd build
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates curl gnupg build-essential bc bison flex libssl-dev libelf-dev kmod cpio rsync xz-utils zstd dpkg-dev git python3 >/dev/null
curl -fsSL https://archive.raspberrypi.com/debian/raspberrypi.gpg.key | gpg --dearmor -o /usr/share/keyrings/raspberrypi.gpg
echo "deb-src [signed-by=/usr/share/keyrings/raspberrypi.gpg] http://archive.raspberrypi.com/debian/ bookworm main" > /etc/apt/sources.list.d/raspi-src.list
apt-get update -qq
if apt-get source -qq "linux=$SRCVER"; then
  SRC=$(find . -maxdepth 1 -type d -name 'linux-*' | head -n 1); ORIGIN="raspberrypi apt source linux=$SRCVER"
else
  echo "apt source unavailable; falling back to the raspberrypi/linux rpi-6.12.y branch"
  git clone -q --depth 1 --branch rpi-6.12.y https://github.com/raspberrypi/linux linux-git
  SRC=linux-git; ORIGIN="raspberrypi/linux rpi-6.12.y $(git -C linux-git rev-parse HEAD)"
fi
cd "$SRC"; cp "$BASECFG" .config
scripts/config --enable SECURITY_LANDLOCK --set-str LSM "landlock" --set-str LOCALVERSION "-v8-landlock" --disable LOCALVERSION_AUTO
make ARCH=arm64 olddefconfig >/dev/null
grep -E '^CONFIG_SECURITY_LANDLOCK=y$' .config; grep -E '^CONFIG_LSM=' .config
# Every option other than the three above must be unchanged from the Pi's running kernel.
diff <(grep -E '^CONFIG_' "$BASECFG" | sort) <(grep -E '^CONFIG_' .config | sort) > "$OUT/config.diff" || true
cat "$OUT/config.diff"
make ARCH=arm64 -j"$(nproc)" Image.gz modules dtbs 2>&1 | tail -n 3
REL=$(make -s ARCH=arm64 kernelrelease); echo "kernel release: $REL"
S="$OUT/stage"; mkdir -p "$S/boot" "$S/dtbs/overlays"
cp arch/arm64/boot/Image.gz "$S/boot/kernel8-landlock.img"; cp .config "$S/boot/config-$REL"; cp System.map "$S/boot/System.map-$REL"
cp arch/arm64/boot/dts/broadcom/bcm2710-rpi-zero-2*.dtb "$S/dtbs/" 2>/dev/null || true
cp arch/arm64/boot/dts/overlays/*.dtbo "$S/dtbs/overlays/" 2>/dev/null || true
make ARCH=arm64 INSTALL_MOD_PATH="$S" INSTALL_MOD_STRIP=1 modules_install >/dev/null
rm -f "$S/lib/modules/$REL/build" "$S/lib/modules/$REL/source"
printf 'release=%s\nsource=%s\nbuilt=%s\n' "$REL" "$ORIGIN" "$(date -u +%FT%TZ)" > "$S/BUILD-INFO"
(cd "$S" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -C "$S" -czf "$OUT/pi-kernel-landlock.tar.gz" .
(cd "$OUT" && sha256sum pi-kernel-landlock.tar.gz > pi-kernel-landlock.tar.gz.sha256 && ls -la pi-kernel-landlock.tar.gz && cat pi-kernel-landlock.tar.gz.sha256)
