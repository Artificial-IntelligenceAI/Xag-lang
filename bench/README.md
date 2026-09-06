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

**Quiet the machine first.** Close the dev server, let the build finish, and
run it on its own. The obvious advice — read the ratio and ignore the
milliseconds — is not enough, because load does not cancel out of the ratio
either: the two binaries do not slow down by the same proportion, so a busy
machine reads 1.3x where a quiet one reads 1.15x. Both halves being wrong
together is what makes the ratio look trustworthy when it is not.

## What the two cases are for

`add.xag` is the control. Both compilers fold a billion iterations of
`total + (i x *3*)` to a constant and never run the loop, so if that one is
slow the backend is not being optimised at all and the other number means
nothing.

`loop.xag` and `div.xag` are the ones that move. `mod` and `/` each have a
question in front of them — dividing by zero stops, and the least number over
-1 has no answer a machine will give — and how those questions are answered is
the whole measurement. Answering with a call to the runtime instead of a
compare cost 20.7x against C, because a call is something the optimiser cannot
see through, so `mod *7*` never became the multiply and shift that C's `% 7`
becomes. Emitting the compare and the instruction inline brought both to about
1.1x.

Both are here because both were changed at once, with the same guards. A
benchmark watching only one of them would let the other go back to being a
call without saying anything.

If either goes back to several times its C twin while `add` stays flat, an
operation has gone back to being a call.

## What this is not

It is not a benchmark to quote. It compares one machine against itself on one
afternoon, with two loops, on three operations. It is enough to catch a
regression and nowhere near enough to publish, and nothing on the website
claims a performance number for exactly that reason.

A number here is about the afternoon it was taken on. On 2026-09-06 the `mod`
row read 20.7x in the morning and 1.1x by the afternoon, and both were right
when taken.
