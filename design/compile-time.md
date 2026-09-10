# ITMT — running a loop to find out what it does

**ITMT**, which Tankun named on 2026-09-07 and says *I'm Taking My Time*. It is
a joke about the one thing this feature spends without limit, and it is the
name.


**Built, for sums.** The design was written down before any of it, so it did not
get built the wrong way round; what is here now says what happens rather than
what was intended. **Open** at the end is what is still only intended.

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

The compiled form is **built the way anything is built** — an object out of
`Native.cpp`, linked against the runtime by `cc`, run as a program of its own.
Not a JIT. Xag is ahead-of-time to native, and a JIT would be a third way of
making code beside the two that already exist, which is exactly what the second
run is meant to rule out. Three things follow from it being a separate program
rather than code called in-process: it is the very backend that ships, a crash
in it is a result rather than the compiler falling over, and a loop that will not
finish can be killed. Only the interpreter, then, needs to count its steps.

The cost is a link and a program started. Every loop in a file worth running
goes into one such program, built once and run once, so it is a cost per
compilation rather than per loop.

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

It still cannot hand back a program. The compiled run *is* the shipping backend,
so the interpreter's answer would be acted on while the same code, compiled
normally, does the other thing. So it stops.

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

## What is built

`src/Ahead.cpp` runs a program that reads nothing, twice, and compares two
things about the runs: **what each wrote**, and **where each says a sum came
round**.

- Reading it: the test interpreter, watching (`interpretWatching`). A sum came
  round exactly where cutting it to the type changed it.
- Running it: an object from `Native.cpp`, linked by `cc`, started as a program
  of its own. Built with `Watching::Yes` — the only build anywhere with checked
  arithmetic in it: `llvm.sadd.with.overflow` and its five relatives, and a call
  to `xag_came_round` where one of them says the answer did not fit.

**Nothing a reader is handed is built that way.** `xagc build` emits a machine's
own add, and a sum that does not fit comes round in it as it always has. The
checks are how the compiler watches itself, not how the language behaves.

Comparing the output alone was written first and taken out again. A sum coming
round in a value nothing prints leaves *both* runs silent, and two engines
silent in the same way is not two engines agreeing — the same hole found in
`E0516` the same day, where three engines printed nothing for a struct and the
oracle read it as agreement. Asking both where a sum came round is what makes
agreeing mean something.

| what happened | what is said |
| --- | --- |
| they wrote different things | `Severity::Mine` — ours, and it says so |
| they wrote the same, and disagree about the sums | `Severity::Mine` as well |
| they agree, and a bound was right | the bound stands as it was |
| they agree, and a bound was wrong | the bound is dropped |
| they agree, and a sum came round nothing suspected | `E0537` |
| they agree, and the program stopped | `E0538` |
| they agree, and a bound was right | `E0534`, and only here |
| only one engine ran | bounds may be dropped, and nothing else is said |

The last row is what happens with no runtime to link against. One engine may let
something through, which is what already happens wherever a bound gives up, but
it may not refuse anybody's program.

The bounds' own two diagnostics, `E0534` and `W0001`, are held back by the
checker in `CheckResult::aboutSums` rather than reported: a refusal that has
already stopped compilation cannot be overturned by a run that has not happened
yet, and the first attempt at a test for this ran straight into that.

### As far as the first look outside

A program that reads cannot be run to the end here — what it does on what it was
given is not what it does on nothing. It can be run *up to* the read, and
everything before that point actually happened.

There are two ways of looking outside and both stop the run: `read.stdin[]`, and
`arguments[]`. What a program is given on the line that starts it is no more a
fact about the program than what it is handed on its input, and the compiler was
started with its own arguments or with none.

Both engines stop in the same place, or nothing they say can be compared. The
interpreter stops when it meets either while watching; the built program calls
`xag_would_read`, which says so and stops, because a watching build lowers both
that way. A reader's build reads.

Only `read.stdin` said so until 2026-09-10. `arguments[]` answered with whatever
the compiler happened to be holding, and two programs came apart on it: one that
prints its first argument was refused for reaching into a `many` that holds none
— true of the compiler's run and of nothing else — and one that counts them had
the two engines disagree, the interpreter seeing the compiler's arguments and
the built program, started with none, seeing none. The second is the shape the
first should have taken: a `Mine`, loudly, rather than a refusal that reads like
the reader's fault.

What that adds over lifting a loop out is every loop before a read that *cannot*
be lifted — one walking a `many`, or one that calls out:

```
fn.int8 'twice' [int8 'n'] { give ['n' x *2*]; }
START {
    var.mut.int8 'sum' = [*0*];
    loop.range.int8 'i' = [*1*, *5*] { set 'sum' = ['sum' + twice['i']]; }
    loop.while read.stdin[] holds 'line' { print.stdout['line' \n]; }
}
```

The bounds give up on that loop (`W0001`, it calls out) and lifting will not
take it (it calls out). Running the program as far as the read answers it.

A partial run only answers for what it **reached**, which a whole run does not
have to think about: a statement a program with nothing to read never reaches is
a statement that never runs, and a bound about it is a bound about nothing. A
statement past a read is a different matter, and its bound stands — after which
the loop-lifting above gets its turn at it.

### A loop taken out of the program it was written in

A program that reads cannot be run here — what it does depends on what it is
given. Its loops still can: **what a loop is entered with is written down even
where the program's input is not.**

```
START {
    var.mut.int8 'total' = [*0*];
    loop.while read.stdin[] holds 'line' { print.stdout['line' \n]; }
    loop.range.int8 'i' = [*1*, *100*] {
        set 'total' = ['total' + 'i' / *50*];      # bounded at 200, reaches 52
    }
}
```

`src/Loops.cpp` finds the counted loop, sees `*1*`, `*100*` and `*0*` written
down for everything it is entered with, and builds it into a program of its own
called `START`. That program is run both ways like any other.

A loop qualifies on two rules, each about being *sure* rather than about being
*able*:

- **It calls nothing.** A call reaches code with its own state and its own
  reads, and following it is following the whole program again.
- **Every value it is entered with is changed exactly once outside the loop, and
  that change is one that can be made again out of what is written in it.** One
  change outside means no other value can reach it, which settles the question
  without asking which paths run.

The second rule used to be about *types* — plain numbers and `bool`s only,
because text or a `many` "would have to be built up again outside the loop, and
handing a loop something it does not really own is how a compile-time run starts
freeing what a program still holds". That worry was misplaced. Nothing is
handed over: a `many` of written numbers is **built again**, so the lifted loop
owns a copy of its own and shares nothing. What matters is whether the value can
be made again, not what type it is.

### What "changed" means, which cost two bugs

A name is changed three ways and only one of them is an assignment:

```cpp
bool couldChange(const Statement &s, unsigned id) {
  if ((s.kind == StatementKind::Assign || s.kind == StatementKind::Store) &&
      s.place == id) return true;
  return s.value.kind == RValueKind::Ref && s.value.op == "loanmut" &&
         s.value.local == id;
}
```

Writing one place of a `many` is a `Store` and leaves the name alone. Lending a
name out for writing hands the changing to somebody else. Counting only
assignments made both invisible, in both directions:

```
var.mut.many.int64 'xs' = [*0* *0* *0*];
set 'xs'[*1*] = [*7*];                      # invisible
loop … { set 'sum' = ['sum' + 'xs'['i']]; }
```

was lifted with the array as first written, so the compiler folded the loop into
a sum of zeroes while both interpreters said 15. **The oracle found that one**,
and the report said `native is the one out of step` — which is what a bad
rewrite looks like, native being the only engine given rewritten code.

The other direction is the same mistake: a loop writing through a `loanmut`
never assigns to the name behind it, so folding the loop threw the writes away.
`n = 5` interpreted, `n = 0` built. Nothing found that; it was sitting beside
the first one.

**A lifted loop says what happens when the loop runs, and nothing about whether
it does** — that is a question about the program around it, which was not run.
It may drop a bound on those terms without qualification: a loop that is
harmless when it runs is harmless if it never runs either.

Raising one says only as much as it knows (decided by Tankun, 2026-09-08):

```text
Tip(s): I took this loop out of the program and ran it on its own, both ways I
        have of running one, and both watched the sum come round — so it does,
        every time this loop runs. Whether it runs at all is a question about
        the program around it, which I did not run.
```

### A program that stops

A program can stop: it divides by zero, or asks for a place a `many` does not
have. Both engines can see that happen, and a stop they agree about is `E0538` —
the reason and the place, before the program was ever run.

```text
this stops the program: place 3 was asked for, and the `many` has 3

  5 |         if 'xs'['i'] > 'best' { set 'best' = ['xs'['i']]; }
    |            ^^^^^^^^^ here

Error code: E0538
```

Two things had to be built for that. The built program says *where* it stopped:
generated code in a watching build stores the place into `xag_where` — a store,
not a call — and `xag_stop` prints it when it is not zero, which it only ever is
in that build. And the interpreter had to survive a stop at all.

**A stop used to end the compiler.** `xag_stop` calls `std::exit`, and ITMT runs
the program in the compiler's own process — so `xagc check` on a program that
divides by zero printed a runtime message with no file, no line and no code, and
checking a file had quietly run it. `xag_stop` now takes a handler and the
in-process run installs one that comes back. Nothing a reader runs installs one.

Coming back skips the destructors of everything the run had in hand, so that run
leaks. Two things follow, and the second is what the tests caught: the memory is
gone, and the *count* of it would go on counting — so the next run in the same
process is told it ended holding what the last one dropped. `xag_forget_allocations`
puts the count back. A program that is fine was being accused.

### An estimate does not refuse anybody's program

**Decided by Tankun, 2026-09-08.** A bound says *at most*, and at most turns away
programs that are fine. So `E0534` leaves the checker as a **warning**:

```text
`'sum'` may reach past what a `int8` holds.
Tip(s): this is worked out from the loop's ends rather than by running it, so it
        says how far this could get and not how far it does.
```

It becomes a refusal in one place and no other: both engines ran the loop and
both watched the sum come round. Then it says `reaches` rather than `may reach`,
and the tip says how it knows.

### A faster answer while you are writing

`xagc check --no-itmt` does everything except run the program. The whole cost of
a check is that run — 1.87 seconds against 0.01 on the same file — because the
front end is free and building and starting a program is not.

It still finds spelling, types, ownership, loans, showing, dividing by a written
zero, and the bounds as the estimates they are. It stops finding exactly the
three things that need a program to have run: `E0537`, `E0538`, and a bound
turning from *may reach* into *reaches*.

**Only `check` takes it.** On `build` it would be shipping something nothing ever
ran, and the same word in the source needs an `UNSAFE` block around it to say so
— a flag has none, so `xagc build --no-itmt` is refused and says where the word
belongs instead.

### A way past the compiler contradicting itself

**Decided by Tankun, 2026-09-08.** `--anyway` turns a disagreement into a warning
and lets the build through, for somebody who cannot wait for it to be fixed. The
disagreement is still reported and everything the two runs agreed about still
holds; what stops is the refusing.

It also **switches off writing a loop's answer in**. That is the part that
matters: a rewrite worked out from an answer the compiler cannot stand behind is
the one thing that must not reach anybody, and `--anyway` is exactly the case
where it cannot stand behind it.

### Saying what it is about to spend

A run may take as long as the program does, so a build can be quiet for a long
time. Before a long one it says so, using the trip count, which is free — both
ends are written down:

```text
xagc: about to run this program to find out what it does. A loop at line 3 goes
      round 20000000 times, so this may take a moment.
      `no-itmt` on that loop, inside `UNSAFE`, says not to bother.
```

Not for a loop that already said `no-itmt`, where the advice would be to write
the word that is already there.

### Which sums are anybody's business

`E0537` is only ever said about a sum whose answer becomes a **name** that did
not say `wrapping` — `CheckResult::intoPlainNames`. Both runs watch every sum,
including the ones meant to come round, because a checksum coming round is the
checksum working and the run is not the place to decide that.

The narrowing is not tidiness. `wrapping` is written on a name, and a sum
happens between values, so a sum whose answer never becomes a name has nowhere
for the word to go:

```
var.mut.bool 'b' = [(int16:*234*) >== ('n' x *4*)];
```

That multiply may come round, and there is nothing anybody could write to say it
is meant to. Refusing it would be handing the reader a diagnostic they cannot
answer. The oracle put a number on how common that is: reported everywhere, 82
of 200 generated programs were refused, nearly all of them for sums exactly like
that one.

**So `wrapping` not reaching every sum is an open question about the language,
not a gap in this pass.** Until it is answered, a sum with no name at the end of
it is watched, agreed about, and said nothing about.

## A `loop.range` is not limited

**Decided by Tankun, 2026-09-07: not limited.** Not the running, and not the
loop either — a range is never refused for being large. An ITMT run has no step
budget, where a reader's run gives up after fifty million: twenty million
rounds with a branch in them used to be three and a half seconds of running
followed by nothing, and is now eight seconds and an answer. A `loop.range` has its
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

One thing does follow from it: **checking finishes before any loop runs.** A file
with a mistake on one line and a very long loop on another has to report the
mistake, and it cannot if it is still counting. Every diagnostic the checker
would give is given first, and running is what happens to a program that has
already been read and found sound.

The trip count is worth computing anyway, because it is free: two written
numbers, multiplied through any nesting, before a single iteration runs. It is
the same on every machine, and it is what any message about a long run would be
built from.

A `while` is the other case, and it keeps the budget. Its trip count is not
written down, so it may never finish, and then waiting is the compiler hanging
with nothing to show. One anywhere in the program puts the budget back over the
whole of it — coarse, and the cheap way to be sure, since which loop a run is
*inside* is not something the walk keeps track of.

Nothing tested that until 2026-09-08, because the generator had never written a
`loop.while` at all. It does now. The test interpreter already
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

## A loop that says not to bother

Decided and built 2026-09-08. See `design/syntax.md` for the spelling. A loop marked `no-itmt`, inside an `UNSAFE` block, is one ITMT
does not run: not taken out on its own, not folded, and nothing raised about it
— no `E0537`, no `E0538`, no `E0534` confirmed by a run. The bounds still apply,
because they reason rather than run, and `wrapping` is still the word for saying
a sum is meant to come round.

The word names ITMT because ITMT is what it turns off. `no-run` would read as a
loop that never executes, when it runs at runtime exactly as written, and a word
like `no-verif` would borrow a concept nothing else here is called.

The whole program stops there too, the way it stops at a read — both engines at
the same block, or nothing they say could be compared. Otherwise the run would
take the time the loop asked it not to, on its way past.

It exists because a run has no limit. A loop that costs minutes at build is a
loop somebody will want to skip, and the honest way to let them is to make the
skipping visible rather than to put the limit back.

Everything that does not involve running still happens: types and sizes,
ownership, loans, showing, dividing by a written zero, and the bounds — `E0534`
and `W0001` reason from the loop's written ends and run nothing, so a `no-itmt`
loop still gets its estimate.

**It can only make the compiler say more, never less.** Dropping a bound takes a
run: that is how the loop bounded at 200 and reaching 52 gets cleared. Take the
run away and the estimate stands, as a warning. So what `no-itmt` gives up is
learning anything further, and never staying quiet about something already
known — which is the right shape for a word reached for to save time.

What it costs is the check that would have caught the bug — which is why it is
`UNSAFE` and why the word is greppable.

## Open

- What else a run should look for. Statements a run never reached was written and
  taken out again: a run only reaches the end for a program that reads nothing,
  and those are exactly the programs where every condition is decided, so every
  `if` and `when` has an arm it does not take. It found two in one example and
  both were the example working. The version worth having — *no path can reach
  this* — needs no run and is not this pass's question.

### What it costs the oracle

Every program is now built twice while being checked, and the oracle went from
three cases a second to one. Worse, the oracle asks `xagc` three times per case
— `run`, `fast` and `build` — and each of those goes through `ready()`, so each
does a build of its own. Four modules per case where there was one, and the
skip above does not help: a generated program does arithmetic. That is the compile-time bill this was always going
to run up, and it is not a surprise. It is worth writing down anyway, because
the number it slows down is the rate at which the oracle finds bugs — the thing
that caught the 128-bit hole the same day the hole appeared.

A run of 200 also refuses nine cases it used to run, all of them genuine
overflows into a plain name. That is the feature working, and it is still nine
fewer programs the three engines get compared on.
