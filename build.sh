#!/usr/bin/env bash
# Build out/gate16.iso: a bootable CD/USB image that starts the GATE16
# logic gate editor. The image boots on x86-64 PCs in legacy BIOS (CSM) mode
# and in UEFI mode, including with Secure Boot on (Microsoft-signed shim ->
# Canonical-signed GRUB -> Canonical-signed kernel).
#
# Needs Ubuntu 24.04 (or the ubuntu:24.04 container) with:
#   apt-get install build-essential grub-pc-bin grub-efi-amd64-bin grub-common \
#     grub-efi-amd64-signed shim-signed xorriso mtools dosfstools busybox-static \
#     zstd kmod cpio
set -euo pipefail

TOP=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$TOP/work}
OUT=${OUT:-$TOP/out}
ISO=$OUT/gate16.iso
VOLID=GATE16

for cmd in gcc grub-mkimage xorriso mkfs.fat mmd mcopy cpio gzip zstd depmod apt-get dpkg-deb; do
	command -v "$cmd" >/dev/null || { echo "missing tool: $cmd (see the top of build.sh)" >&2; exit 1; }
done
BUSYBOX=/bin/busybox
SHIM=$(readlink -f /usr/lib/shim/shimx64.efi.signed)
MOKMGR=/usr/lib/shim/mmx64.efi
GRUBEFI=/usr/lib/grub/x86_64-efi-signed/gcdx64.efi.signed
for f in "$BUSYBOX" "$SHIM" "$MOKMGR" "$GRUBEFI" /usr/lib/grub/i386-pc/boot_hybrid.img; do
	[ -f "$f" ] || { echo "missing file: $f (see the top of build.sh)" >&2; exit 1; }
done
if ldd "$BUSYBOX" >/dev/null 2>&1; then
	echo "$BUSYBOX is not static; install busybox-static" >&2
	exit 1
fi

rm -rf "$WORK/rootfs" "$WORK/iso" "$WORK/kernel"
mkdir -p "$WORK/deb" "$WORK/kernel" "$OUT"

echo "==> kernel"
KPKG=$(apt-cache depends linux-image-generic | sed -n 's/.*Depends: \(linux-image-[0-9].*-generic\)$/\1/p' | head -n1)
KVER=${KPKG#linux-image-}
(cd "$WORK/deb" && { ls "$KPKG"_*.deb >/dev/null 2>&1 || apt-get download "$KPKG" "linux-modules-$KVER"; })
for deb in "$WORK/deb/$KPKG"_*.deb "$WORK/deb/linux-modules-$KVER"_*.deb; do
	dpkg-deb -x "$deb" "$WORK/kernel"
done
KMOD=$WORK/kernel/lib/modules/$KVER
echo "    $KVER"

echo "==> gate16 editor"
gcc -O2 -Wall -Wextra -Wno-format-truncation -static -s -o "$WORK/gate16" "$TOP/src/gate16.c"
"$WORK/gate16" --selftest

echo "==> initramfs"
R=$WORK/rootfs
mkdir -p "$R"/{bin,dev,proc,sys,tmp,root,etc,lib/modules}
ln -s bin "$R/sbin"
cp "$BUSYBOX" "$R/bin/busybox"
for app in $("$BUSYBOX" --list); do
	[ "$app" = busybox ] || ln -sf busybox "$R/bin/$app"
done
cp "$WORK/gate16" "$R/bin/gate16"
cp -a "$TOP/rootfs/." "$R/"

# Kernel modules for input devices and the buses they sit on, plus whatever
# they depend on. Graphics drivers are left out on purpose: the firmware
# framebuffer (BIOS VGA text / UEFI GOP) works on every motherboard.
depmod -b "$WORK/kernel" "$KVER"
want=$(cd "$KMOD" && find kernel/drivers/hid kernel/drivers/usb/host kernel/drivers/usb/common \
	kernel/drivers/input kernel/drivers/i2c kernel/drivers/mfd kernel/drivers/pinctrl \
	-name '*.ko.zst' ! -name evbug.ko.zst 2>/dev/null | sort || true)
mods=$( (for m in $want; do
	echo "$m"
	{ grep "^$m:" "$KMOD/modules.dep" || true; } | cut -d: -f2 | tr " " "\n"
done) | grep . | sort -u)
DST=$R/lib/modules/$KVER
for m in $mods; do
	mkdir -p "$DST/$(dirname "$m")"
	zstd -q -d "$KMOD/$m" -o "$DST/${m%.zst}"
done
cp "$KMOD"/modules.{builtin,builtin.modinfo,order} "$DST/"
depmod -b "$R" "$KVER"
echo "    $(echo "$mods" | wc -l) modules"

(cd "$R" && find . | LC_ALL=C sort | cpio -o -H newc --owner=0:0 --quiet | gzip -9) >"$WORK/initrd.img"

echo "==> ISO tree"
I=$WORK/iso
mkdir -p "$I/boot/grub" "$I/EFI/BOOT" "$I/.disk"
install -m 644 "$WORK/kernel/boot/vmlinuz-$KVER" "$I/boot/vmlinuz"
install -m 644 "$WORK/initrd.img" "$I/boot/initrd.img"
install -m 644 "$TOP/boot/grub.cfg" "$I/boot/grub/grub.cfg"
echo "GATE16 Logic Gate Editor (kernel $KVER)" >"$I/.disk/info"

# Legacy BIOS: El Torito GRUB image with every module it needs built in
grub-mkimage -O i386-pc-eltorito -p /boot/grub -o "$I/boot/grub/bios.img" \
	biosdisk iso9660 part_msdos part_gpt normal configfile linux echo test \
	search search_fs_file search_label sleep reboot halt minicmd ls cat \
	all_video vbe vga video_bochs video_cirrus gfxterm

# UEFI: shim (Microsoft-signed) loads grubx64.efi (Canonical-signed), which
# finds /.disk/info on the disc and reads /boot/grub/grub.cfg.
cp "$SHIM" "$I/EFI/BOOT/BOOTX64.EFI"
cp "$GRUBEFI" "$I/EFI/BOOT/grubx64.efi"
cp "$MOKMGR" "$I/EFI/BOOT/mmx64.efi"
cp "$TOP/boot/grub.cfg" "$I/EFI/BOOT/grub.cfg"

EFIIMG=$WORK/efi.img
rm -f "$EFIIMG"
mkfs.fat -C -n GATE16EFI "$EFIIMG" 8192 >/dev/null
mmd -i "$EFIIMG" ::/EFI ::/EFI/BOOT
mcopy -i "$EFIIMG" "$I"/EFI/BOOT/* ::/EFI/BOOT/

echo "==> $ISO"
rm -f "$ISO"
xorriso -as mkisofs -quiet -r -J -joliet-long -iso-level 3 -V "$VOLID" \
	--grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
	-partition_offset 16 --mbr-force-bootable \
	-append_partition 2 0xef "$EFIIMG" -appended_part_as_gpt \
	-c boot/boot.cat \
	-b boot/grub/bios.img -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info \
	-eltorito-alt-boot -e '--interval:appended_partition_2:all::' -no-emul-boot \
	-o "$ISO" "$I"
(cd "$OUT" && sha256sum gate16.iso >gate16.iso.sha256)
ls -lh "$ISO"
