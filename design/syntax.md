# SafetyBolt syntax — v0 working draft

Status: the shape below is settled. Everything under **Open** is not.

SafetyBolt is a compiled language with Rust-style ownership, no garbage collector,
and two ahead-of-time execution paths (native code, and a form run by an AOT
interpreter). This document covers only how it is written.

Source files are `.sbls` — SafetyBolt Language Source.

## A value is a list of items

Items sit next to each other and are used in order. Nothing is concatenated into a
third thing, so there is no `+` for text.

```
var.str 'greeting' = [*Hello, * 'name' *!*];
```

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
var.str 's' = [*line one* \n *line two*];
```

`\n` is an item sitting beside the text, not hidden inside it. So `*a\nb*` is a
backslash and an `n`. Reading a piece of SafetyBolt text never means working out
which of its characters were secretly instructions.

Escapes: `\n`, `\t`, `\r`, `\\`. Inside a mark, `\*` and `\'` write the closing mark
itself — the one character that could not otherwise appear there.

## A word is a function, a name is a variable

A **name** wears marks, so it may hold anything at all — spaces, punctuation,
emoji — because the marks say where it stops.

A **word** wears none, so it has to be plainer: letters, digits and `_`, joined by
`-`. Words are function names, chain segments and types.

```
fn.i64 sum-to [i64 'n'] { ... }

call sum-to['LIMIT'];
var.str 'a name with spaces 🙂' = [*fine*];
```

That is one more thing a reader never has to work out from context: marks say
*variable*, bareness says *function*, everywhere and always.

`-` joins a word only when it sits between two word characters, which is what
keeps it apart from subtraction — subtraction's operands are marked or bracketed,
so `sum-to` is one word and `count - *1*` is three tokens.

Nothing outside ASCII is a word character, though it is perfectly good inside a name.

## Arithmetic is five things

```
'a' + 'b'    'a' - 'b'    'a' x 'b'    'a' / 'b'    'a' ^ *2*
```

`*` is spent on written values, so **multiplication is the letter `x`**, and `^`
raises to a power.

That works for the same reason `-` in `sum-to` works: arithmetic operands are
always marked or bracketed, so a bare `x` standing between two of them can only
be the operator, while `xs` is a single word rather than an operator and a name.
The lexer does not need to know — `x` arrives at the parser as a word like any
other, and position tells it apart.

`x` is reserved. No function may be called `x`.

## Comparison carries the whole of `==`

```
'a' <  'b'     'a' <== 'b'     'a' == 'b'
'a' >  'b'     'a' >== 'b'     'a' !== 'b'
```

One `=` assigns. Two are the equality token — so a comparison that includes
equality carries it whole, and `<=` would read as *less-than-assign*, which
means nothing.

What that buys is a line you can classify by counting: **one `=` is always an
assignment, two are always a comparison**, and neither has to be told apart from
the other by what surrounds it.

Writing `<=`, `>=` or `!=` out of habit is its own error rather than a quiet
acceptance:

```text
`<=` is not how a comparison is written.

  1 | if ['a' <= 'b'] {
    |         ^^ here

Error code: E0009
Rule(s) broken: one `=` assigns, and equality is written `==`
Tip(s): a comparison that includes equality carries the whole of it, so the
        number of `=` says whether a line assigns or compares without ever
        consulting what is around it.
```

## Brackets bound a list that nothing else bounds

A value is a list of **juxtaposed** items, so nothing inside it says where one item
stops. Brackets supply the bounds, and they are load-bearing:

```
[*Hello, * 'name' *!*]
```

Parameters and call arguments are lists too, running up against a `{` or a `;` that
is too far away to help, so they are bracketed as well.

A declared name is not a list, and it is already bounded by its own marks. So it
takes no brackets:

```
var.mut.i64 'total' = [*0*];
set 'total' = ['total' + *1*];
set 'xs'[*2*] = [*99*];
loop.range.i64 'i' = [*1*, 'n'] { ... }
```

One name is declared at a time. Nothing about the notation forbids several, but
declaring several at once has to say what each of them ends up owning, and that
question is not worth the line it saves.

## Declarations are chains

Each segment answers one question, and the segment nearest the name is always the
type.

```
var.mut.i64 'total' = [*0*];
fn.export.i64 add [i64 'a', i64 'b'] { give ['a' + 'b']; }
loop.perm.range.i64 'i' = [*1*, *100*] { ... }
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

So `var.str 'greeting'` is immutable, owned, and visible in this file only. A bare
chain is the safest chain: nothing risky can hide in an omission, and verbosity
scales with how unusual a declaration is.

**There is exactly one spelling.** Writing a default is an error, not an allowed
redundancy:

```text
`immut` is what a name is when nothing says otherwise.

  1 | var.immut.str 's' = [*hi*];
    |     ^^^^^ here

Error code: E0201
Rule(s) broken: a chain says what is unusual, and says nothing else
```

A diagnostic says what happened, points at it, names the rule, and explains why
the rule exists. It does not say what to type instead — the reader knows their
intent and the compiler does not.

## Ownership

A name owns its value until the value is moved, and then it holds nothing.

```
fn.i64 size [ref.str 'text'] { ... }           # borrowed, read-only
fn.nothing excite [refmut.str 'text'] { ... }  # borrowed, writable
fn.nothing keep [str 'text'] { ... }           # takes it — `own` is the default
```

### A transfer is always spelled at the call site

Declarations default; transfers never do.

```
call size[ref 'greeting'];
call excite[refmut 'greeting'];
call keep[move 'greeting'];
```

A declaration describes a thing, but a call site *acts*, and the consequence here is
that `'greeting'` stops existing. That is worth a word every time.

### A borrow never outlasts what it borrows from

```
fn.ref.str broken [] {
    var.str 'text' = [*hello*];
    give ['text'];
}
```

```text
`'text'` stops existing when this function ends, and the answer would outlive it.

  4 |     give ['text'];
    |           ^^^^^^ here

Error code: E0401
Rule(s) broken: a borrow never outlasts what it borrows from
Tip(s): the value belongs to this function, so the only thing that can leave here
        with it is the value itself.
```

### A lifetime is a name

When one parameter is borrowed, there is only one thing the answer could be
borrowed from, so nothing has to be said:

```
fn.ref.str echo [ref.str 'text'] {
    give ['text'];
}
```

When two are, there is a choice, and the compiler does not get to make it. The
loan is given a name, and everything on that loan is written with it:

```
fn.ref.'life'.str longer [ref.'life'.str 'a', ref.'life'.str 'b'] {
    if ['a' > 'b'] {
        give ['a'];
    } else {
        give ['b'];
    }
}
```

`'life'` is spelled like `'greeting'` because it **is** a name — a name for the
loan rather than for a value. Nothing about it is a special form, and it may be
called whatever says what it is:

```
fn.ref.'as long as both inputs'.str longer [...] { ... }
```

Leaving it out where it is needed is its own error, and the compiler does not
guess:

```text
this answer is borrowed, and so are two of the parameters.

  1 | fn.ref.str longer [ref.str 'a', ref.str 'b'] {
    |    ^^^ here

Error code: E0402
Rule(s) broken: a borrow that is given back says which loan it belongs to
Tip(s): with one borrowed parameter there is only one loan the answer could be
        on, so nothing is written; with two there is a choice.
```

## Open

- **Precedence.** What binds tighter than what, and which way `^` associates.
- **Entry point.** `START` is borrowed from Quench pending a decision.
- **Turning a number into text.** Pieces side by side join, and nothing converts on
  its own, so something has to do it and be named.
