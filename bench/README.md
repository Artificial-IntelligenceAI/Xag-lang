# Is it fast yet?

Xag is meant to be fast, and native code is what it ships. That is a goal
rather than a measurement, and this says where the goal stands — so that the
answer is a command rather than somebody's recollection.

```sh
./bench/run.py                     # uses ./build/xagc
XAGC=/path/to/xagc ./bench/run.py  # or say where it is
```

It builds the same loop twice, once with `xagc build` and once with
`clang -O3`, runs each seven times after a warm-up, and refuses to report
anything if the two disagree about the answer. Build the compiler
`-DCMAKE_BUILD_TYPE=Release` first; timing a Debug build measures nothing.

## What the two cases are for

`add.xag` is the control. Both compilers fold a billion iterations of
`total + (i x *3*)` to a constant and never run the loop, so if that one is
slow the backend is not being optimised at all and the other number means
nothing.

`loop.xag` is the one that moves. `mod` has a question in front of it —
dividing by zero stops — and how that question is answered is the whole
measurement. Answering it with a call to the runtime instead of a compare cost
20.7x against C, because a call is something the optimiser cannot see through,
so `mod *7*` never became the multiply and shift that C's `% 7` becomes.
Emitting the compare and the instruction inline brought it to about 1.1x.

If `loop` ever goes back to several times `loop.c` while `add` stays flat,
an operation has gone back to being a call.

## What this is not

It is not a benchmark to quote. It compares one machine against itself on one
afternoon, with one loop, on one operation. It is enough to catch a
regression and nowhere near enough to publish, and nothing on the website
claims a performance number for exactly that reason.
