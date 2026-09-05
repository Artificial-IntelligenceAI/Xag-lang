# SafetyBolt syntax — v0 working draft

Status: the shape below is settled. Everything under **Open** is not.

SafetyBolt is a compiled language with Rust-style ownership, no garbage collector,
and two ahead-of-time execution paths (native code, and a form run by an AOT
interpreter). This document covers only how it is written.

## A value is a list of items

Items sit next to each other and are used in order. Nothing is concatenated into a
third thing, so there is no `+` for text.

```
var.str ['greeting'] = [*Hello, * 'name' *!*];
```

A comma says where one value stops, which is what lets a value run to as many items
as it likes — and what makes declaring several names on one line fall out for free
rather than needing a special form.

## Two marks, and only two

| written | is |
| --- | --- |
| `'…'` | a **name** |
| `*…*` | a **written value** |

A quoted thing is a name wherever you meet it. It never has to be re-read as a value
because of where it happens to sit — position is never consulted.

There is no third mark for "text versus number", because the type already answers
that: `*1000*` is a number under `i64` and four characters under `str`.

### Escapes stand outside

```
var.str ['s'] = [*line one* \n *line two*];
```

`\n` is an item sitting beside the text, not hidden inside it. So `*a\nb*` is a
backslash and an `n`. Reading a piece of SafetyBolt text never means working out
which of its characters were secretly instructions.

Escapes: `\n`, `\t`, `\r`, `\\`. Inside a mark, `\*` and `\'` write the closing mark
itself — the one character that could not otherwise appear there.

## Declarations are chains

Each segment answers one question, and the segment nearest the name is always the
type.

```
var.mut.i64 ['total'] = [*0*];
fn.export.i64 ['add'] [i64 'a', i64 'b'] { give ['a' + 'b']; }
loop.perm.range.i64 ['i'] = [*1*, *100*] { ... }
```

### Every segment but the type has a default

**The default is always the least-powerful option, and writing a word always means
asking for something.**

| segment | default | written when |
| --- | --- | --- |
| mutability | `immut` | `mut` — asking to change it |
| ownership | `own` | `ref` / `refmut` — asking to borrow |
| visibility | `file` | `export` / `program` — asking for exposure |
| loop counter | `temp` | `perm` — asking to keep it after the loop |
| type | *none* | always |

So `var.str ['greeting']` is immutable, owned, and visible in this file only. A bare
chain is the safest chain: nothing risky can hide in an omission, and verbosity
scales with how unusual a declaration is.

**There is exactly one spelling.** Writing a default is an error, not an allowed
redundancy:

```
var.immut.str ['s'] = [*hi*];
```
```text
`immut` is what a name is when nothing says otherwise.
Rule(s) broken: a chain says what is unusual, and says nothing else
Tip(s): write `var.str ['s']`.
```

## Ownership

A name owns its value until the value is moved, and then it holds nothing.

```
fn.i64 ['size'] [ref.str 'text'] { ... }        # borrowed, read-only
fn.nothing ['excite'] [refmut.str 'text'] { ... }  # borrowed, writable
fn.nothing ['keep'] [str 'text'] { ... }        # takes it — `own` is the default
```

### A transfer is always spelled at the call site

Declarations default; transfers never do.

```
call 'size'[ref 'greeting'];
call 'excite'[refmut 'greeting'];
call 'keep'[move 'greeting'];
```

A declaration describes a thing, but a call site *acts*, and the consequence here is
that `'greeting'` stops existing. That is worth a word every time.

### A lifetime is a name

Names are already marked, so a lifetime needs no notation of its own:

```
fn.ref.'life'.str ['longer'] [ref.'life'.str 'a', ref.'life'.str 'b'] { ... }
```

`'life'` is spelled like `'greeting'` because it is a name. Nothing about it is a
special form.

## Open

- **File extension.** `.sb` is a placeholder.
- **Multiplication.** `*` is spent on written values, so it cannot also mean multiply
  without a stateful lexer. Quench answered `x`; SafetyBolt has not answered.
- **Entry point.** `START` is borrowed from Quench pending a decision.
- **Whether v0 ships borrowing at all.** Move-only semantics — one owner, passing
  hands it over, using it afterwards is an error — is a sound memory model on its
  own, needs no lifetimes or region analysis, and is perhaps a fifth of the work.
  Borrowing could be an addition to a working language rather than a prerequisite.
- **Multi-declare against moves.** `var.str ['a', 'b'] = [*x*, 'a']` has to say what
  each name ends up owning.
- **Turning a number into text.** Pieces side by side join, and nothing converts on
  its own, so something has to do it and be named.
