"""Disagree with the decimal answers, using something that shares no code with them.

Python's `decimal` is libmpdec: an implementation of the same IBM General Decimal
Arithmetic specification that IEEE 754 decimal is drawn from, written by someone
else, from the specification rather than from us. That is the whole reason it is
worth asking — three engines calling one runtime cannot catch a mistake inside
that runtime, and no vote between them ever will.

Reads the lines `decimal_cases` writes: width, operation, left, right, our answer.
"""

import decimal
import sys
from decimal import Decimal

# The three interchange formats, as IEEE 754 defines them. `clamp` keeps an
# exponent inside what the encoding can carry, which is the difference between a
# format and an arbitrary-precision context.
#
# No traps: Xag carries on rather than stopping (`no-number = "carries-on"`),
# because a `deci` **is** IEEE 754 decimal and infinity and not-a-number are
# values of it. Untrapped, Python answers the same way instead of raising.
def context(prec, emax, emin):
    return decimal.Context(prec=prec, Emax=emax, Emin=emin, clamp=1,
                           rounding=decimal.ROUND_HALF_EVEN, traps=[])


CONTEXTS = {32: context(7, 96, -95),
            64: context(16, 384, -383),
            128: context(34, 6144, -6143)}


def parse(text, ctx):
    """Read a value the way Xag writes one."""
    if text == "infinity":
        return Decimal("Infinity")
    if text == "-infinity":
        return Decimal("-Infinity")
    if text == "not-a-number":
        return Decimal("NaN")
    return ctx.create_decimal(text)


def spell(value):
    """Write a value the way Xag writes one.

    Only the spelling is translated, never the value: Xag writes an exponent
    with a small `e`, and the cohort — which digits, at which power — is left
    exactly as it came, because keeping it is the whole point of the type.
    """
    if value.is_nan():
        return "not-a-number"
    if value.is_infinite():
        return "-infinity" if value.is_signed() else "infinity"
    return str(value).replace("E", "e")


def reference(width, op, left, right):
    """What this operation answers, or None where nothing is claimed."""
    ctx = CONTEXTS[width]
    a, b = parse(left, ctx), parse(right, ctx)

    if op == "compare":
        if a.is_nan() or b.is_nan():
            return "unordered"
        return str(int(ctx.compare(a, b)))

    if op == "+":
        return spell(ctx.add(a, b))
    if op == "-":
        return spell(ctx.subtract(a, b))
    if op == "x":
        return spell(ctx.multiply(a, b))
    if op == "/":
        return spell(ctx.divide(a, b))
    if op == "mod":
        return spell(ctx.remainder(a, b))
    if op == "^":
        # Xag has no transcendental functions, so it answers a power only where
        # the exponent is a whole number. Anything else is nobody's claim.
        if a.is_finite() and b.is_finite() and b == b.to_integral_value():
            return spell(ctx.power(a, b))
        return None
    return None


def within_one(ours, want):
    """Whether two answers differ by no more than the last digit.

    Allowed for `^` and nothing else. Raising to a power is the one operation
    the specification does not require to be correctly rounded — it is computed
    by repeated multiplication and is permitted to be out by one in the last
    place. Every other operation here has to match exactly, and does.
    """
    try:
        a, b = Decimal(ours), Decimal(want)
    except decimal.InvalidOperation:
        return False
    if not (a.is_finite() and b.is_finite()):
        return False
    x, y = a.as_tuple(), b.as_tuple()
    if x.exponent != y.exponent or x.sign != y.sign:
        return False
    return abs(int("".join(map(str, x.digits))) -
               int("".join(map(str, y.digits)))) <= 1


def main():
    checked = skipped = 0
    wrong = []
    tally = {}
    for line in sys.stdin:
        parts = line.rstrip("\n").split("\t")
        if len(parts) != 5:
            continue
        width, op, left, right, ours = parts
        want = reference(int(width), op, left, right)
        if want is None:
            skipped += 1
            continue
        checked += 1
        if want != ours and not (op == "^" and within_one(ours, want)):
            wrong.append((width, op, left, right, ours, want))
            key = f"deci{width} {op}"
            tally[key] = tally.get(key, 0) + 1

    for width, op, left, right, ours, want in wrong[:20]:
        print(f"deci{width}: {left} {op} {right}")
        print(f"    xag said  {ours}")
        print(f"    libmpdec  {want}")

    if tally:
        print("\nwhere they disagree:")
        for key in sorted(tally, key=lambda k: -tally[k]):
            print(f"    {tally[key]:6d}  {key}")

    print(f"\n{checked} case(s) checked, {skipped} skipped, "
          f"{len(wrong)} disagreement(s)")
    if len(wrong) > 20:
        print(f"({len(wrong) - 20} more not shown)")
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
