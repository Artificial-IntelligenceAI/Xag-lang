# Xag syntax — v0 working draft

Status: the shape below is settled. Everything under **Open** is not.

Xag is a compiled language with Rust-style ownership, no garbage collector,
and two ahead-of-time execution paths (native code, and a form run by an AOT
interpreter). This document covers only how it is written.

Source files are `.xag` — Xag source.

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
that: `*1000*` is a number under `int64` and four characters under `str`.

### A written value states its type where nothing else does

A written value means nothing on its own, so something has to say what it is.
Usually something already has:

```
var.str 's' = [*a* *b*];              # the chain said it
sum-to[*10*];                          # the parameter said it
print.stdout[str:*x = * 'n' \n];       # nothing said it, so the value does
```

A print list has no chain and no declared parameter types, so **every** written
value in one carries its own type — there is no inheriting it from the item
before, because that would be guessing at what seems likely:

```
print.stdout[str:*Hello, * 'name' str:*!* \n];
```

A name needs no annotation anywhere: its declaration gave it one already.

### Escapes stand outside

```
var.str 's' = [*line one* \n *line two*];
```

`\n` is an item sitting beside the text, not hidden inside it. So `*a\nb*` is a
backslash and an `n`. Reading a piece of Xag text never means working out
which of its characters were secretly instructions.

Escapes: `\n`, `\t`, `\r`, `\\`. Inside a mark, `\*` and `\'` write the closing mark
itself — the one character that could not otherwise appear there.

## A word is a function, a name is a variable

A **name** wears marks, so it may hold anything at all — spaces, punctuation,
emoji — because the marks say where it stops.

A **word** wears none, so it has to be plainer: letters, digits and `_`, joined by
`-`. Words are function names, chain segments and types.

```
fn.int64 sum-to [int64 'n'] { ... }

sum-to['LIMIT'];
var.str 'a name with spaces 🙂' = [*fine*];
```

That is one more thing a reader never has to work out from context: marks say
*variable*, bareness says *function*, everywhere and always.

`-` joins a word only when it sits between two word characters, which is what
keeps it apart from subtraction — subtraction's operands are marked or bracketed,
so `sum-to` is one word and `count - *1*` is three tokens.

Nothing outside ASCII is a word character, though it is perfectly good inside a name.

## The types, and their sizes

```
int8  int16  int32  int64  int128
uint8 uint16 uint32 uint64 uint128
bin16 bin32  bin64  bin128
deci32 deci64 deci128
bool  str  nothing
```

**A size is always written**, and it is always one the standard defines. There is
no `int` on its own, because there is no size to assume. `bin` is IEEE 754's
binary interchange formats and `deci` is its decimal ones; nothing off-format
exists.

A written value has to be one of the things its type holds, and because the size
is written that can be settled before the program runs:

```text
`*128*` does not fit in a `int8`.

  1 | var.int8 'n' = [*128*];
    |                 ^^^^^ here

Error code: E0509
Rule(s) broken: a written value has to be one of the things its type holds
```

**Two sizes never meet on their own.** Nothing converts, here as anywhere:

```
var.int32 'a' = [*1*];
var.int64 'b' = ['a' + *1*];    # a `int32` and a `int64` are not added together
```

Arithmetic answers with what it was given, and the right side of a sum takes
whatever the left turned out to be — which is how a written value in a sum gets
a size at all. Where nothing says, the value says it itself, in the notation it
always could:

```
print.stdout[(int32:*161* + *0*) \n];
```

`str:*hello*` and `int32:*161*` are one notation and not two.

Under `overflow = "wrap"` a sum that does not fit wraps at **the width that was
written**, not at the width of whatever carried it:

```
var.mut.uint8 'n' = [*255*];
set 'n' = ['n' + *1*];          # 0
```

### What `bin` does when there is no number to give back

A `bin` **is** IEEE 754 binary, so `infinity` and `not-a-number` are values of
the type rather than accidents of it. Nothing stops:

```
var.bin64 'z' = [*0*];
print.stdout[(bin64:*1* / 'z') \n];     # infinity
print.stdout[('z' / 'z') \n];           # not-a-number
```

A not-a-number is equal to nothing at all, itself included. Asking to stop
instead is `no-number = "stops"` — asking for something narrower than the type
you named.

A narrower `bin` is cut back to its width after **every** step, not only when it
is stored, which is what makes a `bin32` sum a `bin32` sum:

```
var.bin32 'a' = [*0.1*];  print.stdout[('a' x bin32:*3*) \n];   # 0.3
var.bin64 'b' = [*0.1*];  print.stdout[('b' x bin64:*3*) \n];   # 0.30000000000000004
```

A number is printed as the shortest spelling that reads back as the same value,
and those spellings — `infinity`, `-infinity`, `not-a-number` — may be written
as well as printed.

`bin128` is written out in software, because this machine's compiler has no
binary128 type at all — no `__float128`, no `mode(TF)`, and `long double` is a
`double`. It is where the extra precision stops being theoretical:

```
var.bin64 'a' = [*1e30*];   ('a' + bin64:*1* - bin64:*1e30*)     # 0
var.bin128 'b' = [*1e30*];  ('b' + bin128:*1* - bin128:*1e30*)   # 1
```

### A `deci` counts in tens, and keeps the places it was given

```
var.deci64 'a' = [*0.1*];  var.deci64 'b' = [*0.2*];   ('a' + 'b')   # 0.3
var.bin64  'x' = [*0.1*];  var.bin64  'y' = [*0.2*];   ('x' + 'y')   # 0.30000000000000004
```

A decimal number is a whole-number coefficient and a power of ten, so `1.10` is
`110` scaled by `10^-2` and `1.1` is `11` scaled by `10^-1`. **They are equal
and they are not the same**, and telling them apart is the point of the type:

```
var.deci64 'price' = [*1.10*];
print.stdout[('price' + deci64:*2.00*) \n];      # 3.10, not 3.1
```

Each operation keeps the exponent the standard prefers — the smaller of the two
for a sum, their total for a product, and for an exact quotient the one nearest
`q1 - q2`, so `1 / 8` is `0.125` and `10 / 2` is `5`.

The encoding is BID, the coefficient stored as an ordinary binary integer,
because the wide arithmetic underneath already speaks that language.

## The operators

```
+   -   x   /   ^   mod        and   or   not
```

`*` is spent on written values, so **multiplication is the letter `x`**, and `^`
raises to a power.

That works for the same reason `-` in `sum-to` works: operands are always marked
or bracketed, so a bare `x` standing between two of them can only be the
operator, while `xs` is a single word rather than an operator and a name. The
lexer does not need to know — `x`, `mod`, `and`, `or` and `not` all arrive at the
parser as ordinary words, and position tells them apart.

Those five words are reserved. No function may be named one of them.

### Precedence is kept where mathematics settled it, and invented nowhere

| | |
| --- | --- |
| `^` | binds tightest, and leans **right** — `*2* ^ *3* ^ *2*` is `*2* ^ (*3* ^ *2*)` |
| `x` `/` | then these, leaning left |
| `+` `-` | then these, leaning left |
| comparison | looser than all of it |

That is the table everybody learns before they meet a keyboard, and it is what
makes `ax² + bx + c` readable unbracketed. Mathematics' own famous ambiguity —
`8 ÷ 2(2+2)` — comes entirely from *implicit* multiplication, which Xag
does not have, because `x` is written.

Everything programming added has no agreed order, because nothing outside
programming ever needed one. `mod` written infix, `and` against `or`: C put `&`
looser than `==` and Python put it tighter, and both choices produced a famous
trap. So there is no answer to inherit, and Xag does not invent one —
**those need brackets**, and writing them without is an error that names both
readings rather than picking one.

```text
`mod` and `+` have no agreed order, so this could be read as
`(*a* mod *b*) + *c*` or as `*a* mod (*b* + *c*)`.

  3 | var.int64 'x' = [*a* mod *b* + *c*];
    |                 ^^^^^^^^^^^^^^^^^ which of these first?

Error code: E0301
Rule(s) broken: precedence is kept where mathematics settled it, and invented nowhere
```

The two readings sit in the message because they *are* the message: the compiler
is not advising, it is saying what it cannot decide.

Repeating `and` or `or` is the one thing that needs no brackets, because both are
associative and the brackets would say nothing:

```
['p' and 'q' and 'r']        # fine
['p' and 'q' or 'r']         # brackets — these are different operators
[*9* mod *5* mod *3*]        # brackets — `mod` is not associative
```

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

  1 | if 'a' <= 'b' {
    |        ^^ here

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

Parameters and arguments are lists too, running up against a `{` or a `;` that is
too far away to help, so they are bracketed as well.

A bare word followed by `[` is a call — `sum-to['LIMIT']`, `count['text']`,
`print.stdout[…]` — so no word announces one. The single other place a word meets
a `[` is a function being declared, and the `fn` chain opening that line has
already said so.

A condition is not a list either — `if` bounds it on the left and `{` on the
right — and neither is a declared name, which its own marks already bound. So
neither takes brackets:

```
var.mut.int64 'total' = [*0*];
set 'total' = ['total' + *1*];
set 'xs'[*2*] = [*99*];
loop.range.int64 'i' = [*1*, 'n'] { ... }
if 'a' >== 'b' { ... }
loop.while 'left' > *0* { ... }
```

The range keeps its brackets because it is two values with a comma between them.

One name is declared at a time. Nothing about the notation forbids several, but
declaring several at once has to say what each of them ends up owning, and that
question is not worth the line it saves.

## Declarations are chains

Each segment answers one question, and the segment nearest the name is always the
type.

```
var.mut.int64 'total' = [*0*];
fn.export.int64 add [int64 'a', int64 'b'] { give ['a' + 'b']; }
loop.perm.range.int64 'i' = [*1*, *100*] { ... }
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
fn.int64 size [ref.str 'text'] { ... }           # borrowed, read-only
fn.nothing excite [refmut.str 'text'] { ... }  # borrowed, writable
fn.nothing keep [str 'text'] { ... }           # takes it — `own` is the default
```

### A transfer is always spelled at the call site

Declarations default; transfers never do.

```
size[ref 'greeting'];
excite[refmut 'greeting'];
keep[move 'greeting'];
```

A declaration describes a thing, but a call site *acts*, and the consequence here is
that `'greeting'` stops existing. That is worth a word every time.

A word is written where there is a **choice**, though, and `give` has none — there
is nothing else it could mean — so the answer is handed over without one:

```
fn.str greet [] {
    var.str 's' = [*hi*];
    give ['s'];
}
```

Nor is a small value ever moved. An `int64` or a `bool` is handed over by being
copied and the original stays where it was, so `move` on one is refused: it would
say a name stops holding what it held, and it does not.

### What was moved holds nothing

```text
`'greeting'` was moved, and holds nothing now.

  8 |     print.stdout['greeting' \n];
    |                  ^^^^^^^^^^ used here
  7 |     keep[move 'greeting'];
    |          ^^^^^^^^^^^^^^^ but it was handed over here

Error code: E0403
Rule(s) broken: a name holds its value until it is moved, and then holds nothing
```

A mistake that happened in one place and showed up in another has two places,
and a diagnostic points at both — otherwise the reader is left to go and find
the half that was not shown.

A name given away down **any** arm of an `if` is gone after it, because the
compiler does not get to assume which arm ran. A name given away inside a **loop**
is an error where it stands: the second pass round would find nothing there.

### A borrow never outlasts what it borrows from

```
fn.ref.str broken [ref.str 'other'] {
    var.str 'text' = [*hello*];
    give ['text'];
}
```

```text
`'text'` stops existing when this function ends, and the answer would outlive it.

  3 |     give ['text'];
    |           ^^^^^^ here

Error code: E0401
Rule(s) broken: a borrow never outlasts what it borrows from
Tip(s): the value belongs to this function, so the only thing that can leave here
        with it is the value itself.
```

### A loan lasts until nobody is holding it

How long a loan lasts is a question the control-flow graph answers, so it is
asked of the graph rather than of the text. A loan lives from where it is taken
until the last place anything holding it is looked at, and in between, what it
borrows from may not be handed over, changed behind its back, ended, or lent
again for writing.

```text
`'greeting'` is handed over while it is still lent.

  15 |     keep[move 'greeting'];
     |     ^^^^^^^^^^^^^^^^^^^^^ handed over here
  13 |     var.ref.str 'winner' = [longer[ref 'greeting', ref 'reply']];
     |                                    ^^^^^^^^^^^^^^ and lent here, still in use after this

Error code: E0408
Rule(s) broken: what is lent stays where it is until the loan is done with
```

The same reading catches `E0409` (changed while lent), `E0410` (one loan for
writing, or any number for reading, and never both) and `E0411` (ended while
lent). A loan nobody is holding any more costs nothing, so lending, looking, and
then handing the value over is perfectly ordinary.

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

- **Turning a number into text.** Pieces side by side join, and nothing converts on
  its own, so something has to do it and be named.
