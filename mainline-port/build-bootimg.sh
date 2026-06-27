#!/usr/bin/env bash
#
# build-bootimg.sh — bygg en RAM-bootbar boot.img för gta2xlwifi.
#
# Tar den nyaste byggda kärn-apk:n (linux-postmarketos-qcom-msm8953), plockar ut
# vmlinuz + vår dtb, appendar dtb:n på kärnan (qcom-stil), och paketerar med
# mkbootimg + gta2:s offsets. Resultatet RAM-bootas ofarligt med:
#     fastboot boot images/boot.img        (inget skrivs till plattan)
#
# Förutsätter att kärnan redan är byggd:
#     ./apply-dts.sh
#     pmbootstrap build --src=<kärnträd> linux-postmarketos-qcom-msm8953
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PMOS="${PMOS:-$HOME/pmos}"
DTS_NAME="sdm450-samsung-gta2xllte"
OUT="$HERE/images/boot.img"

# --- device-specifika värden (verifierade mot plattan) ---------------------
# Root/boot-UUID lästa från installen på mmcblk1p47 (se README).
ROOT_UUID="07d2abef-9505-4fc9-a6da-b6e69594ae30"
BOOT_UUID="1298aa1d-1977-4172-a50b-d7aead5d4569"
CMDLINE="console=tty0 ignore_loglevel loglevel=8 fbcon=nodefer \
pmos_root_uuid=$ROOT_UUID pmos_boot_uuid=$BOOT_UUID pmos_rootfsopts=defaults"

# mkbootimg-offsets (gta2 downstream deviceinfo)
BASE=0x80000000
KOFF=0x00008000
ROFF=0x02000000
TOFF=0x01e00000
PAGESIZE=2048

# --- hitta nyaste kärn-apk + initramfs -------------------------------------
APK=$(find "$PMOS/packages" -name "linux-postmarketos-qcom-msm8953-*.apk" | sort | tail -1)
[ -n "$APK" ] || { echo "FEL: hittar ingen kärn-apk — bygg kärnan först." >&2; exit 1; }
echo "kärn-apk:   $APK"

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
tar -xzf "$APK" -C "$WORK" "boot/vmlinuz" "boot/dtbs/qcom/$DTS_NAME.dtb"

# initramfs tas från device-rootfs-chrooten (pmbootstrap install genererar den)
INITRAMFS="$PMOS/chroot_rootfs_qcom-msm8953/boot/initramfs"
[ -f "$INITRAMFS" ] || { echo "FEL: $INITRAMFS saknas — kör 'pmbootstrap install' först." >&2; exit 1; }

# --- appenda dtb + paketera ------------------------------------------------
cat "$WORK/boot/vmlinuz" "$WORK/boot/dtbs/qcom/$DTS_NAME.dtb" > "$WORK/vmlinuz-dtb"
mkdir -p "$HERE/images"
mkbootimg \
	--kernel "$WORK/vmlinuz-dtb" \
	--ramdisk "$INITRAMFS" \
	--base "$BASE" --kernel_offset "$KOFF" --ramdisk_offset "$ROFF" --tags_offset "$TOFF" \
	--pagesize "$PAGESIZE" --header_version 0 \
	--cmdline "$CMDLINE" \
	-o "$OUT"

echo "klart: $OUT  ($(du -h "$OUT" | cut -f1))"
echo "RAM-boota med:  fastboot boot $OUT"
