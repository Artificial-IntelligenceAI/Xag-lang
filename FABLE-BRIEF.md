# Brief: optimise Xag's fast interpreter

You are working on **Xag**, a compiled language in `/Users/ts/SafetyBolt language`.
Your job is one thing: **make `src/Fast.cpp` faster without changing what any
program answers.**

Work on the branch **`fast-interpreter`**, which is where you already are. Another
session is on `main` at the same time, adding to the front end; it will not touch
`src/Fast.cpp`. Do not merge or rebase without saying so — ask first, because the
two lines of work touch different files and there is no hurry.

Use **your own build directory**, not `build/`: that one belongs to the other
session and reconfiguring it would pull the ground out from under them.

---

## The rule that matters more than speed

Xag has **three engines** — a test interpreter (`src/Interpret.cpp`), a fast
interpreter (`src/Fast.cpp`), and an AOT native backend (`src/Native.cpp`). They
must agree on every program, always. Two engines can say something is wrong;
three can say which one is.

A change that makes the fast interpreter quicker and wrong is worse than no
change, and the whole project is built around catching exactly that. Before and
after every change:

```bash
cmake --build /tmp/rel -j8 && (cd /tmp/rel && ctest)      # 15 suites
./generator/target/release/xag-oracle --xagc /tmp/rel/xagc \
    --cases 150 --seed $RANDOM --size 150                 # "every engine agreed"
```

`--size` matters more than `--cases`: macOS scans every freshly linked binary,
which costs about 170 ms and is paid per *binary* rather than per statement, so
a few large cases test far more per second than many small ones.

The oracle writes random Xag programs, runs all three engines, and reports any
that disagree. **Run it with several different seeds.** If it reports a
disagreement, you broke something — do not proceed.

---

## Measure against an optimised build, or you will optimise nothing

`build/` is configured `CMAKE_BUILD_TYPE=Debug`. Measured on an arithmetic loop:

| build | user time |
| --- | --- |
| `build/` (Debug, `-O0`) | 0.31 s |
| `-DCMAKE_BUILD_TYPE=RelWithDebInfo` | **0.06 s** |

**5x, with no code change.** Every profile taken against `build/` is a profile of
unoptimised code and will point at the wrong things. Make your own:

```bash
cmake -S . -B /tmp/rel -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /tmp/rel -j8
```

(Ignore the first run of any freshly linked binary — macOS scans it, which costs
about a second and is not your code.)

---

## Where the headroom is, measured

Same programs, `RelWithDebInfo`, user time:

| shape | test interp | fast interp | ratio |
| --- | --- | --- | --- |
| arithmetic loop, 3M iterations | 0.80 s | 0.06 s | 13x |
| function calls, 1M | — | 0.05 s | — |
| text: join + count, 200k | 0.11 s | 0.04 s | **2.75x** |

One thing worth knowing rather than rediscovering: text is nearly runtime-bound.
Both interpreters call the same C runtime (`xag_str_join`, `xag_str_count`), so
that ratio is mostly the runtime and not the interpreter. The arithmetic and
call ratios are the interpreter.

Beyond that the measurements are yours to take and yours to read. Nobody has
profiled this; there is no received wisdom about where the time goes, and no
list of things somebody already thinks you should change.

For reference the native backend runs the arithmetic loop in ~0.00 s — it is
compiled through LLVM at `-O3`. You are not chasing that; you are making the
interpreter that runs without a compiler as quick as it reasonably can be.

---

## How the fast interpreter is built

`src/Fast.cpp`, ~900 lines, two halves:

- **`Builder`** turns the MIR (`include/xag/Mir.h`) into `Routine`s: a flat
  `std::vector<Code>` plus a constant pool. Done once, before anything runs.
- **`Machine`** runs them: a `stack_` of `Slot`, a frame per call, `switch` on
  the opcode.

It **shares nothing with the test interpreter but the C runtime**, on purpose:
two engines that borrow from each other agree about what they borrowed, and a
vote between them proves nothing. Keep it that way.

## Do not touch

- **`runtime/`** — one C ABI shared by all three engines. Changing it changes
  what every engine answers. If something there is slow, say so; do not fix it
  as part of this.
- **`include/xag/Mir.h`** — the other two engines read the same shapes.
- Semantics of any kind. `Xag-Config.toml` records the decisions (`division`,
  `overflow`, `logic`, `characters`, `no-number`, `out-of-range`); none of them
  are yours to reinterpret.

## House style

- Comments say **why**, not what. Match the surrounding prose; it is written in
  plain English sentences, not note form.
- No exceptions (`-fno-exceptions`), no new dependencies.
- Commits: author `Tankun Sriket <tankun.sriket@safetyboltlang.invalid>`, and
  a `Co-Authored-By:` trailer for the model doing the work.

## Where to start

1. Build `RelWithDebInfo` **into your own directory** (`cmake -S . -B /tmp/rel
   -DCMAKE_BUILD_TYPE=RelWithDebInfo`). Confirm `ctest` and the oracle are clean
   *before* you change anything, so a later failure is definitely yours — and
   note that `ctest` must then be run from `/tmp/rel`, not `build/`.
2. Profile the three benchmark shapes above. `/tmp/bench.xag`, `/tmp/bench2.xag`
   and `/tmp/bench3.xag` exist, or write your own.
3. Change one thing. Measure. Run `ctest` and the oracle. Keep or revert.
