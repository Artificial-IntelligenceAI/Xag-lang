# Units — libraries, programs, and what crosses between them

Decided 2026-09-11. Being built in slices; each says here when it lands.

## A library and a program are the same kind of thing

A set of files with a name. A program is one that has a `START`. Both have a
`Xag-Config.toml`, and the file shapes differ only in that a library has
`LIBRARY` where a program has `PREP` and `START` — see `syntax.md`.

## Three levels of seeing

The words were parsed and refused (`E0206`) while a program was one file. They
mean what their order says:

| word | visible to |
| --- | --- |
| `file` (the default) | this file |
| `program` | everything built together as one unit |
| `export` | whoever uses the unit from outside |

A word is written where there is a choice, and the default is the narrowest.

## Two manifests, split by who owns the fact

**The library's** says what it is called, both ways:

```toml
[unit]
name = "text"        # what an importer writes:  import 'text';
called = "t"         # what a use site writes:   t.count-of[…]
```

The author chooses both. They are separate: the first is how a program refers to
the library, the second is how a line of code reaches into it. The directory and
the filenames are neither, so moving or renaming the library breaks no caller.

**The program's** says only where things are:

```toml
[uses]
paths = ["../text", "../net"]
```

and the compiler reads each library's own manifest to learn what to call it. The
manifest is the authority on what the program *could* use; `import 'text';` in a
file is what says *this file* uses it.

## Bare is reserved for the language

No library's names arrive without a prefix. So a name with no prefix is a
built-in, and a name with one says which library — origin is on the name itself,
with no import line to look back at. Two libraries cannot collide, because two
libraries are two prefixes, and nothing can shadow a built-in.

A standard library written *in* Xag is prefixed too. That draws the line where it
belongs: bare is what the compiler itself knows.

This is where Rust ended up — only `std`'s prelude is bare, and that is the
language's privilege rather than something a crate can claim — reached by a
shorter road.

A prefix cannot be a chain word (`mut`, `loan`, `many`, …), or `var.t.point 'p'`
could not be read. Refused.

## A library type may carry operators

Decided the same day, and independent of the rest: a struct may say that `+` is
a given function, so an unbounded exact number from a library can be arithmetic
rather than `big.add['x', 'y']`. The type already decides what `+` does — `int64`
wraps or stops, `bin64` rounds, `deci64` rounds decimally — so a struct
answering it is one more row rather than a new kind of thing. "Nothing converts
on its own" stands: both sides must be the same `big.uer`. The spelling and which
operators may be answered are not yet chosen.

## Settings are per-unit

A unit's code runs under its own `Xag-Config.toml`, wherever it is called from.
`text`'s `mod` is floored if `text` said so, and a program on `truncated` that
calls it gets the floored answer — the author tested one thing and shipped that
thing. Settings attach to the item rather than the build, and an expanded generic
keeps its home unit's, so a library's source is never compiled two ways.

The program's manifest may instead say the program's settings win everywhere, or
that a library whose settings differ is refused. Spelling not yet chosen.

The C++ disaster this looks like — `-ffast-math` in one translation unit and not
another — is the *same* source compiled two ways in one binary, through a header.
Xag has no headers and no macros, none of the knobs touch layout, and a unit is a
whole compilation of its own source. Different code under different rules is not
that problem; it is what calling a library is.

## Cycles are refused

Imports form a graph with no loops, as Rust's crates do. A cycle is never
designed: two types that refer to each other belong in one unit, a callback
interface belongs in a small third unit both sides import, and the rest is
accretion. Refusing it costs nothing anyone wanted and is what lets a library be
built on its own.

## Which files are a library's

Every `.xag` in the directory holding its manifest, in name order. Not the
directories under it: a unit is one directory, and a directory below it is
somebody else's. This was an assumption made while building rather than a
decision taken; it can be a `files = [...]` under `[unit]` if a library ever
wants to say otherwise.

## Errors

| code | when |
| --- | --- |
| `E0601` | a manifest line is not `key = value`, or a value is not quoted |
| `E0602` | a library's manifest gives no `name` or no `called`, or one that cannot be a name, or one a chain already reads |
| `E0603` | a `[uses]` path has no `Xag-Config.toml` at it |
| `E0604` | the walk came round: a unit is used by something it uses |
| `E0605` | two libraries answer to one import name |
| `E0606` | `import` names a library the manifest does not reach |
| `E0607` | a file of a library has `PREP` and `START` — it is a program |
| `E0608` | a file reaches into `t.` without `import 'text';` |

The first five are about a manifest and point into it; the last is about the
file being compiled.

## How a library's code is read

Its files are read alongside the program's as one program, and nothing below
the parser knows what a unit is. What makes that possible is a rename: every
name a library declares is written under its call name before its items join
the program's.

| declared as | becomes | who can write it |
| --- | --- | --- |
| `fn.export.int64 'twice'` | `t.twice` | anyone — it is what a use site writes |
| `fn.int64 'helper'` | `t$helper` | nobody: `$` is not a word character |

Every reference inside the library follows — a call, the type in a chain, a
written value's type, a constant's name — so the library's own code still reads
itself, and a program writing `t.twice[…]` names the function with no lookup on
the way. The wall between a library's private names and everything outside is
the same wall generics already use: a spelling nothing can produce.

The parser is told each library's call name so it can read `var.t.point 'p'` as
one type, and `t.uer:*…*` as one type before a `:`. A `struct` or `one-of` says
`export` the way a function does — `struct.export 'point' […]`.

## Within a unit

A library's files are renamed together, because what a name becomes depends on
who may see it:

| declared as | becomes | reachable from |
| --- | --- | --- |
| `fn.export.int64 'answer'` | `t.answer` | anywhere |
| `fn.program.int64 'shared'` | `t$shared` | every file of the library |
| `fn.int64 'helper'` | `t$1$helper` | the file that declared it — the second |

So two files may each have a `helper`, and a file reaching for another file's
`helper` is told it is not a function, in the name it wrote. A library built on
its own is all of its files, whichever one was named on the command line, and
with several files their `ITMT` blocks are one body in file order — the
library's `ITMT`, not any one file's.

A file uses what it imports. A file reaching for `t.` without `import 'text';`
is refused (`E0608`), in the program and in a library alike.

## Not yet enforced

- **Constants across a unit.** `t.'LIMIT'` — how a program spells a library's
  constant — has no syntax yet. Functions and types do.
- **Settings per unit.** Built for `logic`; `division`, `characters` and
  `no-number` are decided in the manifest and nothing reads them yet. The
  program's override — its settings winning everywhere, or a library whose
  settings differ refused — has no spelling yet.
- **A program of several files.** A library is all the `.xag` files in its
  directory; a program is still the one file named. Which file has `START`,
  and what several `ITMT`s mean, is not decided.

## What is built

- **2026-09-11, slice 1** — the two file shapes, `ITMT`, and `import` parse;
  every file has an `ITMT`; `ITMT` lowers to a body but is not yet run.
- **2026-09-11, slice 2** — manifests read (`src/Units.cpp`), `[uses]`
  followed, cycles refused, every `import` checked against what was reached.
- **2026-09-11, slice 3** — a library's files are read, qualified under its
  call name, and compiled with the program. `t.twice[…]`, `var.t.point 'p'`,
  `struct.export`. Private names are unreachable. All three engines agree on
  `tests/units/program`, which is now a test.
- **2026-09-11, slice 4** — `ITMT` runs. The watched run — interpreter and
  built binary both — runs `START` and then `ITMT`; a reader's `xagc run` and a
  shipped binary run `START` only. `xagc build` on a library reads it, checks
  it, runs its `ITMT` two ways, and makes no binary. Building a library whose
  `ITMT` trips an overflow inside an exported function is refused, which is
  what `ITMT` is for and is now a test.

  Found on the way: a sum that comes round somewhere with no name — inside a
  `give`, a call, a comparison — was neither reported (`E0537` only fired for
  sums into a named slot) nor stopped (the watched run carries on by design),
  so a build passed and the shipped program would have stopped. Every sum the
  watchers see come round is now a refusal, with a tip that says whether
  `wrapping` can be written or the sum needs a name first. The watchers no
  longer notice a `wrapping` sum at all — it is doing what it was declared to.
- **2026-09-11, settings** — `[defaults]` is read per unit and written onto
  every item of the unit (`Settings` in `Ast.h`, carried on `Item` and
  `TypedItem`), so the middle layer lowers each function under the manifest of
  the unit that wrote it. `logic` is the first setting read. It turned out the
  default itself was not built: `and` and `or` were lowered as one instruction
  on both sides, so both sides were always asked. `stops-early` is now two
  blocks and a switch in `MirBuild.cpp`; `asks-both` is the instruction.
  `tests/units/asking` is a library on `asks-both` imported by a program that
  said nothing, and the test checks the output rather than only that engines
  agree. The oracle tosses `logic` per unit and writes `and`/`or` now, which it
  never had.
