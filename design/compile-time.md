# Running a loop to find out what it does

**Nothing here is built.** The design is written down first so it does not get
built the wrong way round.

Xag already reasons about loops at compile time. It folds constants, and it
works out how far a counted loop gets from its written ends — `E0534` when a sum
will not fit, `W0001` when it cannot tell. That reasoning is *bounds*: it never
learns what a loop computes, only how large the answer could be.

This is the other way of finding out. When a loop's inputs are all written down,
the compiler stops, compiles that loop on its own, runs it, and looks at what
actually happened. **There is nothing to write.** A word appears only where there
is a choice, and there is no choice here: the compiler does it where it can.

That is the whole difference from Zig's `comptime` and Jai's `#run`, which are
things a program asks for. Nobody asks for this.

## Why, when there are already bounds

Bounds are conservative, and conservative in the direction that refuses working
programs. This one is refused today:

```
START {
    var.mut.int8 'total' = [*0*];
    loop.range.int8 'i' = [*1*, *100*] {
        set 'total' = ['total' + 'i' / *50*];
    }
}
```

```text
`'total'` reaches past what a `int8` holds.
Error code: E0534
```

The same loop in `int16` prints `total = 52`, which fits an `int8` with room to
spare. The bound reasons *100 trips, at most 2 each, so at most 200* — and never
notices the step is 0 for the first half of the loop. There was an exact answer
available and the compiler refused on an estimate instead.

So the two are not rivals. Bounds are instant whatever the trip count and cost
nothing, but they only ever say *at most*. Running is exact and costs a step per
iteration. A bound is the right thing to reach for first; it is the wrong thing
to refuse a program on when running it was possible.

It catches more than sums, too. Reaching past the end of a `many` is a runtime
stop today — *"place 3 was asked for, and the `many` has 3"* — and in a loop
whose ends are written down, that is knowable before the program ever runs.

## It is run twice, and the two must agree

The test interpreter runs it, and the compiled form runs it, and their answers
are compared. Every build. Not a flag, and not only a test in the oracle.

Running twice costs compile time and no runtime time, which is the trade this
language already says it is making:

> Excellent Runtime Performance / Slow Compilation Time —
> it builds slowly *because* of what it does to run quickly.

**Tankun's rule for what a disagreement means:**

> If one disagrees, it's OUR problem. If both agree, it's THEIR problem.

Both agreeing is what makes the answer worth acting on. A loop that overflows,
or reaches past the end of a `many`, is then a fact about the program, and it is
reported the way every other mistake is reported.

## A disagreement is the compiler's fault, and says so

If the two differ, the program did nothing wrong — Xag contradicted itself.

It still cannot hand back a program. The compiled run and the shipping backend
are one code path, so the interpreter's answer would be acted on while the same
code, compiled normally, does the other thing. So it stops.

**Stopping is not blaming.** Every word Xag prints today is about the reader's
code, closing with

```
If I am wrong about any of that, please tell me: <issues>
```

which allows that the compiler may be wrong. A mismatch is the opposite case:
certainly wrong, and it knows it. That message says so in its first sentence,
carries no `E0…` code — those name a rule the reader's code broke, and no rule
was broken — shows both answers and the loop that produced them, and puts the
issue link at the middle of it rather than the foot.

A mismatch a reader hits is a program the generator never wrote, finding a
disagreement the oracle never found. The report is the most useful thing the
compiler could ask for.

## A `loop.range` is not limited

**Decided by Tankun, 2026-09-07: no limit by default.** A `loop.range` has its
ends written down, so it always finishes, and there is no halting problem to
defend against — only patience, which is the thing this language already spends.

The case against was this one:

```
loop.range.int64 'i' = [*1*, *9223372036854775807*] { ... }
```

Legal, finite, and it terminates some time after the sun does. Tankun's answer
is that this is exactly right: a loop that takes forever to compile is a loop
that takes forever to run, and finding that out during the build, on your own
machine, beats finding it out after shipping. The writer set the limit when they
wrote the ends; the compiler does not get a second opinion.

The trip count is worth computing anyway, because it is free: two written
numbers, multiplied through any nesting, before a single iteration runs. It is
the same on every machine, and it is what any message about a long run would be
built from.

A `while` is the other case. Its trip count is not written down, so it can fail
to finish for real, and something has to stop it. The test interpreter already
counts its steps (`kBudget` in `src/Interpret.cpp`) — but that budget is an
engine limit today, one the oracle sets aside cases for reaching. If running a
loop can reach it, running out has to become an answer: a diagnostic pointing at
a real loop, saying the compiler gave up rather than that the program is wrong.

## When it cannot be done

Only a loop whose inputs are all known can be run. A loop over a parameter, or
one reading input, cannot — so this sits on top of the bounds rather than
replacing them. Bounds keep doing the work wherever an unknown is involved.

A run also has to give the same answer on every machine, or the same source
builds into different programs. There is no FFI, so that is nearly free today,
which is the moment to write it down rather than later.

## Open

- Whether a bound alone may still refuse a program, or only warn once running is
  possible. `E0534` refuses on an estimate today, and the example above shows it
  refusing a correct program.
- Whether the compiler says what it is doing before a long run, or simply goes
  quiet until it is finished.
- What else a run should look for beyond sums that do not fit and places that do
  not exist.
- Whether a loop that was run, agreed on and found safe should also be
  *replaced* by its answer. That is an optimisation and a separate decision; the
  work is already done by then.
