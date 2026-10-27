#!/usr/bin/env bash
# Boot out/gate16.iso in QEMU to try it without burning a disc.
#   scripts/run-qemu.sh          legacy BIOS
#   scripts/run-qemu.sh uefi     UEFI
#   scripts/run-qemu.sh secure   UEFI with Secure Boot enforced
set -euo pipefail
ISO=$(cd "$(dirname "$0")/.." && pwd)/out/gate16.iso
MODE=${1:-bios}
shift || true
args=(-m 512 -cdrom "$ISO" -boot d -device qemu-xhci -device usb-kbd -device usb-mouse)
[ -w /dev/kvm ] && args+=(-enable-kvm)
case $MODE in
bios) ;;
uefi | secure)
	code=/usr/share/OVMF/OVMF_CODE_4M.fd vars=/usr/share/OVMF/OVMF_VARS_4M.fd
	if [ "$MODE" = secure ]; then
		code=/usr/share/OVMF/OVMF_CODE_4M.secboot.fd vars=/usr/share/OVMF/OVMF_VARS_4M.ms.fd
		args+=(-machine q35,smm=on -global driver=cfi.pflash01,property=secure,value=on)
	fi
	tmpvars=$(mktemp)
	trap 'rm -f "$tmpvars"' EXIT
	cp "$vars" "$tmpvars"
	args+=(-drive if=pflash,format=raw,readonly=on,file="$code" -drive if=pflash,format=raw,file="$tmpvars")
	;;
*) echo "usage: $0 [bios|uefi|secure] [extra qemu args]" >&2; exit 1 ;;
esac
exec qemu-system-x86_64 "${args[@]}" "$@"
