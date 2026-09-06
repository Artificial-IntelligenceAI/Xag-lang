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

runtime=$(cd "$here/../../runtime" && pwd)
flags="--target=powerpc64-linux-gnu -mabi=elfv2 -mcpu=power8 -ffreestanding \
       -fno-stack-protector -fno-PIC -O1"
cxxflags="$flags -fno-exceptions -fno-rtti"
# The include paths are passed separately, because a directory name may have a
# space in it and an unquoted variable would split it in two.
inc="-I$here/freestanding"
inc2="-I$runtime"

# The runtime itself, built for a machine with a decimal unit. Everything above
# its encoding seam is the same code that runs anywhere; below it, the
# arithmetic is instructions.
$clang $cxxflags "$inc" "$inc2" -DXAG_DECIMAL_HARDWARE -c "$runtime/xag_deci.cpp" -o "$out/deci.o"
$clang --target=powerpc64-linux-gnu -mabi=elfv2 -mcpu=power8 \
       -c "$runtime/xag_deci_power.S" -o "$out/power.o"
$clang $cxxflags "$inc" "$inc2" -c "$here/support_cxx.cpp" -o "$out/support_cxx.o"
$clang $flags "$inc2" -c "$here/support.c" -o "$out/support.o"
$clang $flags "$inc2" -c "$here/decimal_unit.c" -o "$out/decimal_unit.o"
$clang --target=powerpc64-linux-gnu -mabi=elfv2 -c "$here/start.S" -o "$out/start.o"
$lld -T "$here/bare.ld" "$out/start.o" "$out/decimal_unit.o" "$out/deci.o" \
     "$out/power.o" "$out/support.o" "$out/support_cxx.o" -o "$out/decimal_unit.elf"

# Wait for the guest to say it is done rather than for a fixed count of
# seconds: it prints a character per hypercall, so how long it takes depends on
# how much it has to say.
log="$out/said.txt"
: > "$log"
"$qemu" -M pseries -cpu POWER9 -m 1G -nographic -nodefaults \
        -serial mon:stdio -kernel "$out/decimal_unit.elf" > "$log" 2>/dev/null &
pid=$!
waited=0
while [ $waited -lt 600 ]; do
  if grep -q '^end' "$log" 2>/dev/null; then break; fi
  if ! kill -0 $pid 2>/dev/null; then break; fi
  sleep 1
  waited=$((waited + 1))
done
kill -9 $pid 2>/dev/null || true
wait $pid 2>/dev/null || true
said=$(cat "$log")

if ! echo "$said" | grep -q "^end"; then
  echo "the guest did not finish; it said:" >&2
  echo "$said" | tail -20 >&2
  exit 1
fi

# The comparing needs a software build of the same runtime, which the guest
# cannot hold: it has no operating system under it.
if [ -n "$COMPARE" ]; then
  echo "$said" | sed -n '/^cases/,$p' | tail -n +2 | tr -d '\r' | "$COMPARE"
else
  echo "$said" | sed -n '/^cases/,$p' | head -6
fi
