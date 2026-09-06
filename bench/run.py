#!/usr/bin/env python3
"""Times the same loop written in Xag and in C.

Xag is meant to be fast, and native code is what it ships. That is a goal
rather than a measurement, and this is the thing that says how far off it is —
so that the answer is a command rather than somebody's memory of a Tuesday.

    ./bench/run.py                     # uses ./build/xagc
    XAGC=/path/to/xagc ./bench/run.py  # or say where it is

It is a regression check, not a benchmark to quote. It compares one machine
against itself on one afternoon, which is enough to catch an operation going
back to being a call and not enough to publish.
"""

import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"

DEFAULT_XAGC = HERE.parent / "build" / "xagc"

# (stem, what the body does) — `loop` is the one that matters, `add` is the
# control that says whether the backend is being optimised at all.
CASES = [
    ("add", "total + (i x 3)   native"),
    ("loop", "total + (i mod 7)"),
]

RUNS = 7


def die(message: str) -> "typing.NoReturn":  # noqa: F821
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def find_xagc() -> Path:
    named = os.environ.get("XAGC")
    if named:
        path = Path(named)
        if not path.is_file():
            die(f"XAGC is set to {path}, which is not a file")
        return path
    if DEFAULT_XAGC.is_file():
        return DEFAULT_XAGC
    found = shutil.which("xagc")
    if found:
        return Path(found)
    die(
        "cannot find xagc. Build the compiler and either put it on PATH or "
        "set XAGC to it:\n"
        "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build"
    )


def time_it(argv: list[str]) -> tuple[float, float, str]:
    """Median and best of `RUNS`, in milliseconds, after a warm-up.

    The warm-up matters more than it looks: macOS scans a freshly linked binary
    the first time it is run, and that scan is worth more than a short program.
    """
    first = subprocess.run(argv, capture_output=True, cwd=OUT)
    if first.returncode != 0:
        die(f"{argv[0]} exited {first.returncode}: {first.stderr.decode().strip()}")

    times = []
    for _ in range(RUNS):
        started = time.perf_counter()
        done = subprocess.run(argv, capture_output=True, cwd=OUT)
        times.append((time.perf_counter() - started) * 1000)
    return statistics.median(times), min(times), done.stdout.decode().strip()


def main() -> None:
    xagc = find_xagc()
    if not shutil.which("clang"):
        die("cannot find clang, which is what Xag is being compared against")

    OUT.mkdir(exist_ok=True)
    print(f"xagc:  {xagc}")
    print(f"clang: {subprocess.run(['clang', '--version'], capture_output=True).stdout.decode().splitlines()[0]}")
    print()

    rows = []
    for stem, body in CASES:
        # `xagc build` writes the executable beside its source, so the source
        # goes where the executables are meant to end up.
        shutil.copy(HERE / f"{stem}.xag", OUT / f"{stem}.xag")

        subprocess.run(["clang", "-O3", str(HERE / f"{stem}.c"), "-o", str(OUT / f"{stem}_c")], check=True)
        built = subprocess.run([str(xagc), "build", str(OUT / f"{stem}.xag")], capture_output=True)
        if built.returncode != 0:
            die(f"xagc build failed on {stem}.xag: {built.stderr.decode().strip()}")

        c_median, c_best, c_said = time_it([str(OUT / f"{stem}_c")])
        x_median, x_best, x_said = time_it([str(OUT / stem)])

        # A benchmark whose two halves disagree is not measuring anything.
        if c_said != x_said:
            die(f"{stem}: C said {c_said!r} and Xag said {x_said!r}")

        rows.append((body, c_median, c_best, x_median, x_best, c_said))

    width = max(len(r[0]) for r in rows)
    print(f"{'loop body':<{width}}  {'C -O3':>12}  {'Xag build':>12}  {'ratio':>7}")
    print("-" * (width + 38))
    for body, c_median, c_best, x_median, x_best, said in rows:
        ratio = x_median / c_median if c_median else float("nan")
        print(f"{body:<{width}}  {c_median:9.1f} ms  {x_median:9.1f} ms  {ratio:6.1f}x")
    print()
    print(f"median of {RUNS} runs after a warm-up; both sides agreed on every answer")
    print(f"answers: {', '.join(r[5] for r in rows)}")


if __name__ == "__main__":
    main()
