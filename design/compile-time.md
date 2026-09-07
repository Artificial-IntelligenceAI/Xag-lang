# Running Xag while Xag is being compiled

**Status: nothing here is built.** There is no way to ask for code to run at
compile time, and there is no JIT. What is written down is a decision made
before either exists, so that neither gets built the wrong way round.

Xag already *reasons* at compile time — constants are folded, and how far a
counted loop gets is worked out from its written ends (`E0534`, `W0001`). That
is the compiler deciding to, for its own reasons. Running at compile time would
be a program *asking*, and it is a different thing.

## The two ways to run it, and why there are two

The test interpreter could run it. It runs MIR already, it is built to be
obviously correct, it counts its steps (`kBudget` in `src/Interpret.cpp`), and
the oracle already proves it agrees with the native backend. It is also slow on
purpose.

The native backend could run it, by compiling the block and calling it through
LLVM's JIT — which ships in the LLVM already linked. Jai does this and almost
nobody else does; Zig, Rust, C++ and D all interpret instead. It is fast, and
the answer comes from the same backend that compiles the rest of the program.

Neither is strictly better:

- The interpreter works when the target is not the host. A JIT computes the
  *host's* answer and bakes it into a *target's* program. `Native.cpp` uses
  `getDefaultTargetTriple()` today, so there is no cross-compiling yet — but
  there is already a POWER path (`tests/power/`), so this is not hypothetical.
- The interpreter can stop. A budget counts steps; native code does not, not
  cheaply. An endless block under a JIT hangs `xagc` with nothing to say, and a
  bad one takes the compiler down with it.
- Compiling stops being safe. Under a JIT, `xagc check` on a file you did not
  write runs that file's code.

## Decided: both run, every build, and they must agree

Tankun, 2026-09-07. The check is not a flag and not only an oracle test. Every
build that runs code at compile time runs it both ways and compares.

The reason it can be a default rather than an option is the rule for defaults —
*most safety, with no runtime performance cost*. Running it twice costs compile
time and nothing else, and slow compilation is already the price this language
says it is paying:

> Excellent Runtime Performance / Slow Compilation Time —
> it builds slowly *because* of what it does to run quickly.

## A disagreement is the compiler's fault, and has to say so

**Tankun's point, and the reason this document exists:** if the two answers
differ, the program did nothing wrong. Xag contradicted itself. Refusing the
program in the ordinary voice would blame the reader for our bug.

It still cannot produce a program. The JIT and the native backend are the same
code path, so the interpreter's answer would be baked in while the same
expression, compiled normally, computes the other one at runtime — the program
would hold both answers for one piece of code. That is worse than either.

So it stops, and **stopping is not blaming**. This needs a kind of message Xag
does not have: today every word it prints is about the reader's code, including

```
If I am wrong about any of that, please tell me: <issues>
```

which assumes the compiler is probably right. A mismatch is the opposite case —
the compiler is certainly wrong and knows it. That message says so in its first
sentence, carries no `E0…` code (those name a rule the reader's code broke),
shows both answers and the block that produced them, and puts the issue link at
the middle of it rather than the foot. A mismatch a reader hits is a program the
generator never wrote, finding a disagreement the oracle never found; the report
is the most useful thing the compiler could ask for.

## Open

- Whether code may be asked to run at compile time at all, and how that is
  spelled.
- Whether there is a way to carry on past a mismatch with the interpreter's
  answer, for somebody who cannot wait for the fix. Raised, not decided.
- The budget stops being an engine limit and becomes an answer. Today the oracle
  sets aside any case that reaches it. If a compile-time block can reach it,
  running out has to become a diagnostic pointing at a real loop.
- A compile-time run has to give the same answer on every machine, or the same
  source builds into different programs. There is no FFI, so this is nearly free
  today — which is the moment to write it down, not later.
- What this is *for*. The first thing that needs it is showing a struct or a
  `many` (`E0516`), where what stands between the pieces is a decision the
  compiler should not be making for everyone.
