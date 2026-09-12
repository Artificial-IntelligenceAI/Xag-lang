# Units — libraries, programs, and what crosses between them

Decided 2026-09-11 and 2026-09-12. Built in slices; each says here when it lands.

## A program is its manifest's files; a library is one file

Decided 2026-09-12, replacing the first cut in which both were directories with
manifests.

**A program** is the `.xag` files its `Xag-Config.toml` names. `main` is the one
whose `START` runs — a program has one door — and `files` is the rest of it:

```toml
[unit]
main = "main.xag"
files = ["helpers.xag", "parsing.xag"]
```

Every file has all four blocks. A file that is not `main` has its `START`
written empty; anything in it is refused (`E0619`), because it would never run
and a reader would think it does. Every file's `ITMT` runs, `main`'s first and
then list order, as one body — a file's `ITMT` is about that file's
declarations and belongs beside them. Naming any of the program's files on the
command line builds the program; naming a `.xag` in that directory that the
manifest does not list is refused (`E0618`), since the manifest speaks for the
directory. A `.xag` with no manifest above it is a program of one file, which
is what every program was before this and what most still are.

**A library** is one `.xaglib` file, with no manifest. Everything it says about
itself is on its `LIBRARY` line:

```
LIBRARY.floored 'text' called 't' uses [*../net.xaglib*] {
```

`'text'` is what an importer writes (`import 'text';`), `t` what a use site
writes (`t.count-of[…]`), `uses` the libraries it needs, by path against its
own directory, and the chain words its settings — the other value of each
`[defaults]` setting, the default being what is not said. The author names
their own library; renaming the file changes nothing.

Why the asymmetry: a program is the thing being built, and its manifest is
where a build is described. A library is a thing handed around, and one file
with its account of itself on its first line is the thing that can be handed
around.

## Three levels of seeing

The words were parsed and refused (`E0206`) while a program was one file. They
mean what their order says:

| word | visible to |
| --- | --- |
| `file` (the default) | this file |
| `program` | everything built together as one unit |
| `export` | whoever uses the unit from outside |

A word is written where there is a choice, and the default is the narrowest.

## The manifest lists every library reached

```toml
[uses]
paths = ["../text.xaglib", "../net.xaglib"]
```

The program's manifest is the authority on what the program *could* use;
`import 'text';` in a file is what says *this file* uses it, and a file that
reaches `t.` without saying so is refused (`E0608`). The list is complete: it
names every library the program reaches, the ones its libraries use included.
A library reached only through another library's `uses` is written into the
manifest by the compiler, which says so on standard error — the library carried
the path, so there was nothing a reader would have had to work out, and a
manifest that lists everything is one a reader can trust. Rust's `Cargo.lock`
is the same instinct: the full closure, written down, by the tool.

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
on its own" stands: both sides must be the same `big.uer`.

Built 2026-09-11: `fn.export.uer '+' [loan.uer 'a', loan.uer 'b']`, the twelve
value operators and `convert-to-str`, structs and `one-of`s alike, answered
only by the unit that declared the type and seen under the function's own
visibility word. The full rule is in `design/syntax.md`, "A declared type
answers operators with functions". `tests/units/ratio` is a library whose
fraction type answers them.

## Settings are per-unit

A unit's code runs under its own settings — a program's `[defaults]`, a
library's `LIBRARY` line — wherever it is called from.
`text`'s `mod` is floored if `text` said so, and a program on `truncated` that
calls it gets the floored answer — the author tested one thing and shipped that
thing. Settings attach to the item rather than the build, and an expanded generic
keeps its home unit's, so a library's source is never compiled two ways.

The program's manifest may say otherwise, two ways (decided 2026-09-12):

```toml
[defaults]
division = "floored everywhere"   # this program's division, in every library too
logic = "asks-both"               # per unit, as above: each library's own
different = "refused"             # a library whose settings differ from these is refused
```

`everywhere` after a value makes that one setting the program's in every
library it reaches; the library's own word for it is overridden. `different =
"refused"` refuses a library whose settings still differ from the program's
(`E0620`, pointing at the library's `LIBRARY` line, naming the setting and
both values) — it is asked of the settings without `everywhere`, since an
overridden setting cannot differ. `"refused"` is its only value: not saying it
is the other answer. Neither said, each library runs as it said.

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

## Errors

| code | when |
| --- | --- |
| `E0601` | a manifest line is not `key = value`, or a value is not quoted |
| `E0602` | a library's name or call name cannot be a name, or is a word a chain already reads |
| `E0603` | a path in `[uses]`, or in a library's `uses`, has no `.xaglib` at it |
| `E0604` | the walk came round: a unit is used by something it uses |
| `E0605` | two libraries answer to one import name |
| `E0606` | `import` names a library the manifest does not reach |
| `E0607` | a `.xaglib` is a program, or a program's file is a library |
| `E0608` | a file reaches into `t.` without `import 'text';` |
| `E0609` | a `[defaults]` value is neither of the two a setting has |
| `E0615` | a word after `LIBRARY.` is not a setting |
| `E0616` | a `LIBRARY` line lacks its name or its call name |
| `E0617` | a path in a library's `uses` is not written as text |
| `E0618` | `files` without `main`; a listed file missing; a file named that the manifest does not list |
| `E0619` | a file other than `main` has something in its `START` |
| `E0620` | `different = "refused"`, and a library's setting differs from the program's |

The ones about a manifest point into it; the ones about a `LIBRARY` line point
at the line.

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

**A constant is `t.'LIMIT'`** (decided 2026-09-12). A constant is a name and
wears its marks, where a function and a type are bare words — so the library's
call name goes in front of the marks, the same `t.` as before `t.twice[…]`. It
goes wherever a name goes: a loop's end, a sum, a print, a `const` of the
program's own, a field reached into (`t.'ORIGIN'.x`). The parser hands it on as
the one name `t.LIMIT`, which is what `const.export.int64 'LIMIT'` was renamed
to, so nothing below has a second spelling; `importsCover` sees the prefix in
it. Two spellings were set aside: `'t.LIMIT'`, which hides the prefix inside the
marks where nothing else in the language puts one and cannot be told from a
local named that; and none at all, sharing numbers through `t.limit[]`, which
costs a call where a number was meant and takes a foldable constant away from
the checker.

## Within a unit

A program's files are renamed together, because what a name becomes depends on
who may see it. A program exports nothing — nothing imports a program — so
`export` and `program` both mean the whole program and leave the name as
written; a name that says nothing is its file's own:

| declared as | becomes | reachable from |
| --- | --- | --- |
| `fn.program.int64 'shared'` | `shared` | every file of the program |
| `fn.int64 'helper'` | `$1$helper` | the file that declared it — the second |

So two files may each have a `helper`, and a file reaching for another file's
`helper` is told it is not a function, in the name it wrote. (A library is one
file now, so the three-way table above applies to it with only `export` and
"nothing said" in play.)

A file uses what it imports. A file reaching for `t.` without `import 'text';`
is refused (`E0608`), in the program and in a library alike.

## Not yet enforced

Nothing in this document. What is decided is built.

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
- **2026-09-11, `division`** — read per unit and carried on the `/` and `mod`
  operation (`RValue.floored`), so the folding pass, both interpreters and the
  native code each answer the unit's way. The runtime gained `_floored`
  twins of `xag_int_div`, `xag_int_mod`, `xag_bin_mod`, `xag_bin128_mod` and
  `xag_deci_mod`; native whole-number division stays inline and adjusts the
  instruction's answer by one when the remainder disagrees with the divisor.
  `tests/units/flooring` is a library on `floored` under a program on the
  default, checked against expected output on all three engines. The oracle
  tosses `division` per unit too.
- **2026-09-11, `characters`** — read per unit and carried on the `count`
  call (`RValue.letters`). `xag_str_count_letters` counts scalars — every
  byte that is not a continuation byte — beside `xag_str_count`, which is
  the cluster count. `tests/units/lettering` counts the family emoji, a flag
  and `café` both ways. The oracle tosses it per unit; its texts already had
  both emoji in them.
- **2026-09-12, files** — a program is its manifest's `main` and `files`; a
  library is one `.xaglib` with its names, its `uses` and its settings on the
  `LIBRARY` line. The manifest is completed by the compiler for libraries
  reached through libraries. `tests/units/twofile` is a two-file program
  (private `helper`s, a `program` one, empty second `START`, merged `ITMT`);
  `tests/units/chain` is a library using a library, run on a copy so the
  written-in line can be checked. The oracle writes two-file programs some of
  the time and every library as a `.xaglib`.
- **2026-09-12, override** — `everywhere` on a `[defaults]` value and
  `different = "refused"`. `tests/units/override-*`: the floored library
  truncating under `everywhere`, refused under `different`, and running as it
  said where the program agrees. The oracle says `everywhere` on a setting a
  fifth of the time.
- **2026-09-12, constants** — `t.'LIMIT'` parses to the name `t.LIMIT`;
  `tests/units/limits` shares two constants (one a struct) and keeps one; the
  oracle's library exports constants and the program reads them.
- **2026-09-11, `no-number`** — the last of the four. Carried on every `bin`
  arithmetic operation (`RValue.noNumberStops`); the test interpreter and the
  runtime's `xag_bin_number` / `xag_bin128_number` stop through `xag_stop`,
  native checks `v - v` for order and branches to `xag_no_number`, fast hands
  the stop back the way it hands a sum that came round. A library on `stops`
  whose ITMT reaches an infinity is refused (`tests/units/no-number`); a
  program on the default beside a library on `stops` prints its own
  infinities (`tests/units/stopping`). `fast_tests` runs every setting at its
  other value through both interpreters, stops included.
