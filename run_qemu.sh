#!/usr/bin/env bash
# Run twice with the SAME writable OVMF variable flash to test persistent HWM.
set -euo pipefail
cd "$(dirname "$0")"
QEMU=${QEMU:-qemu-system-x86_64}
QEMU_ACCEL=${QEMU_ACCEL:-tcg}
QEMU_CPU=${QEMU_CPU:-max}
if ! command -v "$QEMU" >/dev/null 2>&1; then
  echo 'ERROR: qemu-system-x86_64 missing. Debian: sudo apt install qemu-system-x86 ovmf' >&2
  exit 2
fi
if [[ ! -f esp/EFI/BOOT/BOOTX64.EFI ]]; then
  make
fi
find_ovmf() {
  for p in \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF_CODE_4M.fd \
    /usr/share/ovmf/OVMF_CODE.fd \
    /usr/share/edk2/x64/OVMF_CODE.4m.fd \
    /usr/share/edk2/x64/OVMF_CODE.fd; do
    if [[ -f "$p" ]]; then echo "$p"; return 0; fi
  done
  return 1
}
CODE=${OVMF_CODE:-}
if [[ -z "$CODE" ]]; then CODE=$(find_ovmf) || { echo 'ERROR: OVMF firmware not installed' >&2; exit 2; }; fi
VARS_SOURCE=${OVMF_VARS_TEMPLATE:-}
if [[ -z "$VARS_SOURCE" ]]; then
  suffix=$(basename "$CODE")
  VARS_SOURCE="${CODE%/*}/${suffix/CODE/VARS}"
  [[ -f "$VARS_SOURCE" ]] || VARS_SOURCE=/usr/share/OVMF/OVMF_VARS_4M.fd
  [[ -f "$VARS_SOURCE" ]] || VARS_SOURCE=/usr/share/OVMF/OVMF_VARS.fd
fi
if [[ ! -f "$VARS_SOURCE" ]]; then
  echo "ERROR: can't find OVMF VARS template; set OVMF_VARS_TEMPLATE" >&2
  exit 2
fi
mkdir -p build/qemu
VARS=build/qemu/OVMF_VARS-persistent.fd
[[ -f "$VARS" ]] || cp "$VARS_SOURCE" "$VARS"
CODE=$(realpath "$CODE")
VARS=$(realpath "$VARS")
ESP=$(realpath esp)
for run in 1 2; do
  log="build/qemu/boot${run}.log"
  raw="build/qemu/boot${run}.raw.log"
  echo "[host] Boot ${run}/2 (${QEMU_ACCEL})"
  set +e
  "$QEMU" \
    -machine "q35,accel=${QEMU_ACCEL}" -cpu "$QEMU_CPU" -m 512M \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,unit=1,file="$VARS" \
    -drive if=virtio,format=raw,readonly=on,file="fat:ro:$ESP" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -display none -monitor none -serial stdio \
    -net none -no-reboot \
    2>&1 | tee "$raw" | awk '
      { sub(/\r$/, "", $0) }
      /^zkernel Frame Pool \| UEFI x86-64/ { started=1 }
      started { print; fflush() }
    ' | tee "$log"
  status=${PIPESTATUS[0]}
  set -e
  # isa-debug-exit: exit status = (IO port value << 1) | 1
  if [[ $status -ne 33 ]] || ! grep -q '^FRAME_POOL_UEFI_TEST_PASSED$' "$log"; then
    echo "[host] Boot ${run}/2 FAILED (exit=$status)" >&2
    echo "[host] Last 35 raw lines (firmware and exception details):" >&2
    tail -n 35 "$raw" >&2
    echo "[host] Complete diagnostic log: $raw" >&2
    exit 1
  fi
  echo "[host] Boot ${run}/2 PASS"
done
if ! grep -Eq '^\[boot\] saved=[1-9][0-9]* pages,' build/qemu/boot2.log; then
  echo '[host] FAIL: second boot did not restore a positive high-water mark' >&2
  exit 1
fi
echo '[host] PASS: two boots, persistent high-water restored'
echo '[host] Clean logs: build/qemu/boot{1,2}.log'
echo '[host] Full logs: build/qemu/boot{1,2}.raw.log'
