#!/bin/sh
# Ask a PowerPC's decimal unit whether it says what our software decimal says.
#
# There is no operating system in here and nothing is downloaded: the program is
# cross-compiled, handed to QEMU where a kernel would go, and talks to the
# console through the one PAPR hypercall for it. That is the whole harness.
#
# It is big-endian on purpose. The firmware jumps in big-endian mode and a
# little-endian image is decoded as rubbish from the first instruction; the
# decimal instructions themselves do not care which way round the bytes are.
#
# Needs: clang with the PowerPC target, ld.lld, qemu-system-ppc64.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/xag-power
mkdir -p "$out"

clang=${CLANG:-clang}
lld=${LLD:-ld.lld}
qemu=${QEMU:-qemu-system-ppc64}

flags="--target=powerpc64-linux-gnu -mabi=elfv2 -mcpu=power8 -ffreestanding \
       -fno-stack-protector -fno-PIC -O1"

# No static data anywhere: the firmware loads this wherever it likes, so every
# address baked in at link time would be wrong, and there is no relocation pass
# to fix them. Constants live in the instruction stream instead.
$clang $flags -c "$here/decimal_unit.c" -o "$out/decimal_unit.o"
$clang --target=powerpc64-linux-gnu -mabi=elfv2 -c "$here/start.S" -o "$out/start.o"
$lld -T "$here/bare.ld" "$out/start.o" "$out/decimal_unit.o" -o "$out/decimal_unit.elf"

if llvm-objdump -d "$out/decimal_unit.elf" | grep -q '(2)$'; then
  echo "a table-of-contents load crept in; it would read from nowhere" >&2
  exit 1
fi

said=$("$qemu" -M pseries -cpu POWER9 -m 1G -nographic -nodefaults \
       -serial mon:stdio -kernel "$out/decimal_unit.elf" 2>/dev/null &
       pid=$!; sleep 12; kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null) || true

echo "$said" | sed -n '/dfp here/,$p'
if echo "$said" | grep -q "all good"; then
  exit 0
fi
echo "the decimal unit did not agree" >&2
exit 1
