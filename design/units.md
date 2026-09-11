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

## What is built

- **2026-09-11, slice 1** — the two file shapes, `ITMT`, and `import` parse;
  every file has an `ITMT`; `ITMT` lowers to a body but is not yet run.
