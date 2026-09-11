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
Either **the slot it goes into** was declared, or **the value says so itself**:

```
var.str 's' = [*a* *b*];              # the chain said it
sum-to[*10*];                          # the parameter said it
set 'total' = ['total' + *1*];         # the name it goes into said it
loop.range.int8 'i' = [*1*, *10*];     # the counter's chain said it
print.stdout[str:*x = * 'n' \n];       # nothing said it, so the value does
```

A slot is a thing that was *declared* — a name, a parameter, a field, a loop's
counter — and the type reaches down through whatever is written into it, so the
`*1*` in `['total' + *1*]` is an `int64` because `'total'` is.

**A comparison is not a slot.** It declares nothing, so it tells neither side
anything, and a written value in one says its own type:

```
if 'n' > int8:*0* { … }
loop.while 'left' > int8:*0* { … }
if ('n' x int8:*4*) > 'limit' { … }
```

The right side used to take whatever the left turned out to be. That was the one
place in the language where a value's type came from the thing standing *beside*
it rather than from the slot it goes into — and `*0*` in `'n' > *0*` has no slot
at all. It was dropped on 2026-09-11.

**`any:*0*` is how a value says its type inside a generic.** A comparison there
wants a type the author cannot name, because naming it is the caller's:

```
fn.any.number 'down' [any.number 'n'] {
    if 'n' <== any:*0* { give ['n']; }
    give [down['n' - any:*1*]];
}
```

`any` is filled in with everything else when the copy is written, so
`down$int64` holds `int64:*0*`. It is the same notation as `str:*hi*`, naming a
type that is not pinned down yet.

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

## A loan is a loan

```
fn.int64 'size' [loan.str 'text'] { ... }     # the parameter borrows
size[loan 'greeting'];                         # the caller lends
```

The word was `ref` until 2026-09-07, and it was the only word in the language
naming a thing the language never talks about: the diagnostics say *loan* 127
times, *lent* and *lends* 97, *borrowed* 51, and *reference* not once.

It has to be a noun, and that is what `ref` was quietly getting right. The chain
is a noun phrase — `loan.str 'text'` is "a loan of str" — and the call is an
imperative — `size[loan 'greeting']` is "loan greeting", beside `move` in the
same position. A verb would be right on one side and backwards on the other,
because the two sides are opposite: the parameter borrows, the caller lends.
`loan` is both the thing and the act, and the act is the lender's.

## A declaration marks what it names

A **name** wears marks, so it may hold anything at all — spaces, punctuation,
emoji — because the marks say where it stops.

A **word** wears none, so it has to be plainer: letters, digits and `_`, joined by
`-`. Words are chain segments and types, and they are how a function is called.

```
fn.int64 'sum-to' [int64 'n'] { ... }
struct 'point' [int64 'x', int64 'y']

sum-to['LIMIT'];
var.point 'p' = [*1* *2*];
var.str 'a name with spaces 🙂' = [*fine*];
```

Every declaration marks the thing it names, and there is nothing to remember
about which ones: `var`, `const`, `fn`, `struct`, a loop's counter, a parameter
and a field are all the same. `'…'` means a name wherever you meet it, which is
what the table above promised, and it was only ever true of some of them.

Naming a function and calling one are different acts, and only the first is
naming — so `'longer'` is what it is called and `longer[…]` is calling it. The
same goes for a struct: `'point'` names it, `point` is the type afterwards. That
is not position being consulted, which the marks never do: it is two different
things being written, and they look different.

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

Arithmetic answers with what it was given, and the type wanted of the sum is
wanted of both its sides — which is how a written value in a sum gets a size at
all. Where nothing wants anything, the value says it itself, in the notation it
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
you named. Under it, `+ - x / ^ mod` on a `bin` of any width stops the program
the moment its answer is an infinity or a not-a-number, in the words `a `bin`
had no number to give back` — whether the answer came from a zero divisor, a
sum run past the largest `bin32`, or an infinity that was already there.
Comparisons never stop; they answer. It is a `bin` setting: the manifest says
so, and a `deci` is left as it is. Per unit, like every `[defaults]` setting.

A narrower `bin` is cut back to its width after **every** step, not only when it
is stored, which is what makes a `bin32` sum a `bin32` sum:

```
var.bin32 'a' = [*0.1*];   ('a' x bin32:*3*)   # 0.300000011920928955078125
var.bin64 'b' = [*0.1*];   ('b' x bin64:*3*)   # 0.3000000000000000444089209850062616169452667236328125
```

### A `bin` is printed exactly

Every digit of it. A binary float is a whole number times a power of two, so
its decimal always ends, and what is written is that decimal and nothing else:

```
print.stdout[bin16:*0.1* \n];   # 0.0999755859375
print.stdout[bin32:*0.1* \n];   # 0.100000001490116119384765625
print.stdout[bin64:*0.1* \n];   # 0.1000000000000000055511151231257827021181583404541015625
```

**None of those is a tenth**, because no binary float is, and each is a
different number. Printed as the shortest spelling that reads back — which is
what most languages write and what this wrote until 2026-09-10 — all three said
`0.1`, and a print that cannot tell three different numbers apart is not showing
the value.

It is long exactly where the value was never representable, which is where it is
worth seeing. A half, a quarter, `2.5` and every whole number are as short as
they always were, and `1e30` says the `1000000000000000019884624838656` it is
rather than the `1e30` it is not.

The long ones are long. The smallest `bin64` there is runs to a thousand
characters and the smallest `bin128` to sixteen thousand, and every one of them
is the number. Printing takes whatever room it takes.

`infinity`, `-infinity` and `not-a-number` are spellings too, and may be written
as well as printed. Everything printed can be read back, which is a rule rather
than a courtesy — the exact spelling of a small `bin128` is eleven hundred
characters, and the reader had to learn to take them.

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
var.bin64  'x' = [*0.1*];  var.bin64  'y' = [*0.2*];   ('x' + 'y')
                              # 0.3000000000000000444089209850062616169452667236328125
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

### `/` and `mod` round the way the unit said

`division = "truncated"` is the default in `Xag-Config.toml`: the quotient
rounds toward zero and the remainder takes the dividend's sign, which is what
the processor's instruction does. `floored` rounds the quotient toward negative
infinity and the remainder takes the divisor's sign, which is what makes
`'n' mod *k*` land in `0..k` for a positive `k`:

```
                 truncated      floored
*-7* / *2*          -3            -4
*-7* mod *2*        -1             1
*7* mod *-2*         1            -1
*-7.5* mod *2.0*    -1.5           0.5
```

It reaches every number family: a `bin` and a `deci` have only `mod` to round,
and under `floored` that remainder follows the divisor too, as Python's `%`
does. Unsigned numbers, exact quotients and the one quotient that does not fit
answer the same either way. A division the compiler works out before anything
runs — a place in a `many` written as `*-9* / *4*` — is worked out under the
setting as well.

Per unit, like every `[defaults]` setting: a library's `mod` does what the
library's manifest says, whichever program calls it (`design/units.md`).

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

### `and` asks its right side only when it has to

`logic = "stops-early"` is the default in `Xag-Config.toml`, and it means what
C, Rust and Python all mean by `&&` and `||`: `and` asks its right side only
when the left was true, and `or` only when it was false. The setting exists
for the guard that every language has:

```
if ('n' !== int64:*0*) and ((int64:*100* / 'n') > int64:*5*) { … }
```

Under `stops-early` a zero `'n'` settles the `and` on the left and the division
is never asked. Under `asks-both` — the other value — both sides are always
asked, and that division by zero stops the program.

It is a setting rather than a rule because the two are two languages: a call on
the right side prints under one and not the other. Like every `[defaults]`
setting it is per unit — a library's `and` does what the library's manifest
says, whichever program calls it — and a value that is neither is refused
(`E0609`) rather than read as the default.

Until 2026-09-11 the compiler asked both sides whatever the manifest said.

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
fn.loan.str 'longer' [loan.str 'a', loan.str 'b'] { ... }
loop.perm.range.int64 'i' = [*1*, *100*] { ... }
```

### Every segment but the type has a default

**The default is always the least-powerful option, and writing a word always means
asking for something.**

| segment | default | written when |
| --- | --- | --- |
| mutability | `immut` | `mut` — asking to change it |
| ownership | `own` | `loan` / `loanmut` — asking to borrow |
| loop counter | `temp` | `perm` — asking to keep it after the loop |

Visibility — `export` / `program` against a default of `file` — is where it will
go, and is refused for now (`E0206`): a program is one file, so there is nothing
outside it for anything to be visible to. A word that cannot change the answer is
not written.

`perm` keeps the counter, and what it holds afterwards is what it last took: the
value a `break` left behind, or the last one when the loop simply ran out.
Wanting it after a `break` is the only reason to keep one at all.

It was one *past* the last until 2026-09-10, because the loop stepped the
counter and then asked whether it had gone too far. Asking first and stepping
after makes the counter never pass the end — which is what the rest of this
page already said about it, and what the bounds are built on.

```
loop.perm.range.int64 'i' = [*1*, *100*] {
    if 'i' x 'i' > *10* { break; }
}
print.stdout[str:*stopped at * 'i' \n];      # stopped at 4
```
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

### The vocabulary is closed, and so is the order

A chain is a run of answers to questions, and both which questions get asked and
the order they come in depend on what is being declared:

```
var    . [mut] . [loan|loanmut]              . [many] . type
fn     . [loan|loanmut] . ['loan']           . [many] . type
const                                      . [many] . type
loop   . [perm] . range                    .          type
loop   . while
param    [mut] . [loan|loanmut] . ['loan']   . [many] . type
```

`many` is not one of the questions: it stands with the type, because it says
what the type is rather than something about the name holding it.

A word outside that list answers nothing, and is refused where it stands rather
than passed over on the way to the type:

```text
`arr` answers no question a chain asks.

  1 | var.arr.int64 'xs' = [*1*];
    |     ^^^ here

Error code: E0202
Rule(s) broken: every segment of a chain answers a question the language asks
```

The rest follows from there. `mut` on a `fn` is a real word in a chain that never
asks whether it changes (`E0203`); `var.mut.mut.int64` answers one question twice
(`E0204`); and `var.loan.mut.str` says the same thing as `var.mut.loan.str`, which
is one spelling too many (`E0205`):

```text
this chain answers whether it owns or borrows before whether it changes, and
they are read the other way round.

  1 | var.loan.mut.str 's' = [*hi*];
    |         ^^^ answered here
    |     ^^^ and this one before it

Error code: E0205
Rule(s) broken: there is exactly one spelling
```

`range` and `while` have no quieter one between them, so a `loop` writes one of
them (`E0207`).

A diagnostic says what happened, points at it, names the rule, and explains why
the rule exists. It does not say what to type instead — the reader knows their
intent and the compiler does not.

## Holding more than one value

A `many` holds a fixed number of values of one type. Its length is settled when
it is made and never changes after; growing is a different type, and does not
exist yet.

```
var.many.int64 'xs' = [*1* *2* *3*];
```

### The type is a chain segment

`many` sits where a chain says what is unusual, and it is unusual: it says the
name holds several of something rather than one. Everything else about a chain
is unchanged, so it works wherever a type does.

```
var.many.int64 'xs'                     # a name
fn.many.int64 'first-few' [int64 'n']     # an answer
fn.int64 'total' [loan.many.int64 'xs']    # a parameter, borrowed
```

`many.many.int64` is refused for now (`E0210`). One `many` is one level, and
the second level is a real feature rather than something to half-support.

### Making one needs no new notation

A value is already a list of items sitting next to each other and used in order.
The type says what "used in order" means, and that is the only difference:

```
var.str 's' = [*a* *b*];                # two items, joined into one
var.many.int64 'xs' = [*1* *2* *3*];    # three items, kept as three
var.many.str 'words' = [*one* *two*];   # two items, kept as two
var.many.int64 'none' = [];             # no items
```

A length that is not known until the program runs is `fill`:

```
var.many.int64 'zeroes' = [fill[*0*, 'n']];
```

`fill` writes one value into every place, so it asks for a value that can be
copied — a number or a `bool`. There is no copying a `str` in Xag, so there is
nothing for `fill` to put in each place, and it says so (`E0515`).

### A `many` of a `many`

```
var.many.many.int64 'grid' = [[*1* *2* *3*] [*4* *5*]];
print.stdout['grid'[*1*][*3*] \n];
```

**Brackets where an item goes make one.** Nothing new is written: `[…]` already
bounds a list, and at the start of an item there is no name in front of it, so it
cannot be an index. `many[…]` was considered — it is how a struct inside a struct
is written — but that naming exists because `point[…]` had to be told apart from
an index, and here there is nothing to tell apart.

**Reaching in is the same brackets again.** `'grid'[*1*][*2*]` reaches into what
was just reached.

A written value is one value, so `[*1* *2*]` into a `many.many.int64` is refused:
each item has to be several, and the brackets that make one are what goes there.

Deeper than two is written the same way and costs nothing extra — nothing says
two, so nothing stops at two.

**What it took.** `many` stopped being a yes-or-no and became a count, in the
checker and in the middle layer alike. `elementOf` takes one level off instead of
all of them. That is the whole of the change: an earlier note here guessed a
second level would want a table of types, and it does not — the combination that
would is `many.or-nothing.T`, which is refused for its own reasons.

### A `many` that grows

```
var.mut.many-growing.str 'lines' = [];
add 'lines' = [move 'line'];
print.stdout[(count[loan 'lines']) str:*|* 'lines'[*1*] \n];
```

**A second type, not a mode of `many`.** A `many` holds the places it was made
with; a `many-growing` may hold more. Adding to a `many` is `E0546`, and it says
which word grows.

**`add` is written like `set`**, because it is the same kind of act: something
happens to a name that already exists, and what happens is spelled where it
happens. Growing is changing, so a name that does not change cannot be grown —
the same `E0508`, in the same words.

**Reading one is reading a `many`.** `count`, an index, a borrowed parameter, a
loop over it: all unchanged. The room it keeps is the last field of it, so
everything that reads the places and the length reads one of these exactly as it
reads a `many`.

**Growing while it is lent is `E0547`**, and this is the whole reason for the
second type:

```text
`'w'` grows while it is lent.

  7 |     add 'w' = [*b*];
    |     ^^^^^^^^^^^^^^^^ grown here
  6 |     var.loan.str 'r' = [loan 'w'[*1*]];
    |                              ^^^^^^^^ and lent here, still in use after this

Rule(s) broken: what is lent stays where it is
```

Worth its own words rather than folding into `E0409`: what is wrong is not that
the value changed, but that every place may have **moved**, so a borrow into one
would point at where they used to be.

An earlier note here guessed this would be a rule Regions had to learn. It half
was. Regions already refuses changing what is lent and already tracks every
loan; what it did not know was that growing is a change. One line, and the rest
was already there.

**It only grows.** Taking a place out from the middle either leaves a gap, which
Xag refuses everywhere (`E0412`), or shifts what comes after, which moves values
other names may be borrowing. Taking one off the end does neither, and is a
separate question — going from cannot to can breaks nothing written before it.

### An element is reached with the name's own brackets

```
print.stdout['xs'[*1*] \n];
set 'xs'[*3*] = [*99*];
var.int64 'n' = [count['xs']];
```

A bare *word* followed by `[` is a call; a *name* followed by `[` is an element,
and the two can never be read for each other because marks say which is which
before the bracket is reached.

`count` answers how many, for a `str` and for a `many` alike — it is the same
question, and the type already says what is being counted. Asking a name that
holds one value for its first is `E0514`: a name holding one value **is** that
value, and there is no first of it.

What one character of a `str` is, is the unit's `characters` setting. The
default, `clusters`, is a grapheme cluster as UAX #29 defines it — what a person
counts: `count[str:*🧑‍🧑‍🧒‍🧒*]` is 1. `letters` is one Unicode scalar, which needs no
table and never changes with a Unicode version: the same family is 7. `café` is
4 either way. A `many` is unaffected; it has places, not characters. Per unit,
like every `[defaults]` setting.

An index is an `int64`, because that is what `count` answers with and two sizes
never meet on their own.

**Places are counted from one.** The first is `*1*` and the last is
`count[…]`, so walking a `many` is written with the ends the language uses
everywhere else:

```
loop.range.int64 'i' = [*1*, count[loan 'xs']] { … }
```

`range` is inclusive at both ends and every other loop in the language starts at
`*1*`; counting places from zero was the one construct that disagreed, and it
showed up written down in every walk over an array as `[*0*, (count[…] - *1*)]`.
That `- *1*` was two decisions failing to line up. `*0*` and every negative are
out of range and say so by name — *place 0 was asked for, and the first place is
1* — rather than being one off and quiet about it.

It changed on 2026-09-10, and it changed what existing programs mean rather than
refusing them, which is why it was worth being sure about. Most of it is loud:
an index written down against a length written down is refused at build
(`E0532`), so a program that indexed from zero stops building rather than
reading the wrong place.

### An element is a place, not a value

`'xs'[*3*]` says *where* a value is, and what happens there depends on what is
asked of it:

```
print.stdout['xs'[*1*] \n];        # read it, and leave it where it is
set 'xs'[*1*] = [*99*];            # write it, ending what was there
size[loan 'xs'[*1*]];               # lend it
move 'xs'[*1*]                     # refused — E0412
```

Taking a value out would leave a hole in the middle of the array, and nothing
in Xag holds a hole. So a `many.str` is read, written and lent, and never
taken apart.

A loan of an element is **a loan of the whole array**. Which element `'xs'['i']`
names is not known until the program runs, so no loan can be narrower than the
thing the index is read out of. That means lending one element and handing the
array over is `E0408` exactly as lending the array itself would be, and writing
one place of a lent array is `E0409` — the same rules, read the same way, with
nothing new to learn.

### Out of range

Reaching past either end stops: which index, how long the array was, and where,
the same in every engine. Below the first place it says so in its own words —
*the first place is 1* — because somebody who wrote `*0*` was counting from
somewhere this language does not count from, and the length is no help to them.
An empty `many` stops whatever is asked of it, because it has no places at all.

Most of the time nothing gets that far. An index written down against a length
written down is `E0532` and never builds, and one a counted loop walks off the
end of is `E0538`, found by running the program at build — see
`design/compile-time.md`. What stops at runtime is what nothing could know: an
index worked out from what the program was given.

**There was a second answer and it is gone.** `out-of-range = "wraps"` in
`Xag-Config.toml` took the index around the length instead, so that `*-1*` was
the last place and nothing ever stopped. It went out on 2026-09-10. Compile-time
running had taken the half of its job that was worth having; what was left was
reading the wrong element in silence where stopping would have said which index
and how long the array was. It also sat in a file away from the code and changed
what every `'xs'['i']` in the program meant, which is the one thing this
language otherwise refuses to do — a property belongs in the spelling of the
declaration it is about. Nothing ever tested it and the oracle never once turned
it on.

Wanting a ring is a real thing to want, and it can be written where anybody can
see it: `'xs'[('i' mod count['xs']) + *1*]`. If that earns a word of its own it
belongs on the declaration — `var.many.wrapping.int64 'ring'` — and going from
cannot to can breaks nothing written before it.

### What it costs

The rule lives in the runtime and every engine asks it, but native code writes
the half of it that says *yes* as a compare and a branch, because a call the
optimiser cannot see into is a call it cannot remove — and this one sits in the
middle of every loop over a `many`.

In the shape every walk over an array is written in, there is no compare at
all, and that is settled where the program is read rather than left to the
optimiser:

```
loop.range.int64 'i' = [*1*, count['xs']] { … 'xs'['i'] … }
```

A `many` is a fixed length once it is made, so `count[…]` does not move under
the loop, and the counter never leaves what it counted. The checker says so,
and native writes the access with nothing in front of it. This is the whole of
a loop adding a `many` up, at `-O3`:

```llvm
%5 = getelementptr [8 x i8], ptr %places, i64 %i
%6 = getelementptr i8, ptr %5, i64 -8
%7 = load i64, ptr %6, align 8
%8 = add i64 %7, %sum
%exitcond = icmp eq i64 %i, %length
%9 = add nuw i64 %i, 1
```

The address, the load, the add, the loop's own test and the step. The `-8` is
counting from one, folded into the addressing where it costs nothing.

Reaching into a *different* array with that counter, or with anything but the
counter itself, is not the same question and keeps its check.

That reading is written against the shape above, which is worth saying because
it once was not: it went on looking for `[*0*, (count['xs'] - *1*)]` after
places began counting from one, so it recognised nothing anybody writes and
every walk carried a check its own end had already answered. Nothing was wrong
with the program that came out — only slower, which is the kind of wrong that
says nothing until somebody measures it.

## A type may say it holds nothing

Some things have no answer. Reading when there is nothing left to read, turning
text that is not a number into one — and until there was somewhere to put that,
the only answers were to stop the program or to settle it once in a file. Now a
type can say it.

```
var.or-nothing.str 'line' = [nothing];

fn.or-nothing.int64 'half' [int64 'n'] {
    if 'n' == *0* { give [nothing]; }
    give ['n' / *2*];
}
```

`or-nothing` stands with the type, as `many` does, and outside it: an
`or-nothing.many.int64` is an array or nothing. An array *of* them is not
written yet (`E0209`), and neither is `or-nothing.or-nothing` (`E0211`), because
one absence is every absence.

### Putting something in takes no word

There is no choice about what a `str` means where a `str`-or-nothing is wanted,
and a word is written where there is a choice — the same reason `give` has none:

```
var.or-nothing.str 's' = [*hi*];        # held
var.or-nothing.str 'e' = [nothing];     # not
```

`nothing` is the one value spelled as a word rather than marked, because there
is no mark for an absence and nothing else it could mean. It takes its type from
what was expected of it: a missing `str` and a missing `int64` do not look
different, so something has to have said which (`E0518`).

### Choosing between the cases

```
when half['i'] {
    is 'value'  { print.stdout['value' \n]; }
    is nothing  { print.stdout[str:*none* \n]; }
}
```

A `when` is made of `is` and nothing else, and the compiler insists that every
case is written and each of them once. A case nobody wrote is a case nobody
thought about, and leaving it out would be found by the program running rather
than by reading it:

```text
this `when` says nothing about what to do with nothing.

  3 |     when 's' { is 't' { print.stdout['t' \n]; } }
    |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ this leaves a case out

Error code: E0522
Rule(s) broken: a `when` covers every case a value could be
Tip(s): `is nothing` is the case that is missing.
```

Saying one twice is `E0521`, and it points at both. Asking it about something
with only one shape is `E0520` — there is nothing to choose between, and an `if`
says it better.

The subject takes no brackets, because `when` bounds it on the left and `{` on
the right — the same reason a condition takes none.

Two cases is all there is to choose between today. The construct is not built
for two: what it lowers to is the middle layer's `switch`, which carries a value
per target rather than a true-or-false pair, and was written that way from the
start so that a decision tree could use it unchanged.

### Getting at it lends, and cannot be skipped

```
if 'h' holds 'value' {
    print.stdout['value' \n];
}

loop.while read.stdin[] holds 'line' {
    print.stdout['line' \n];
}
```

`holds` runs the arm when there is something there, and lends it to the name for
as long as the arm runs. Lends, not gives: what held it goes on holding it, and
taking it away is `E0404` — the same answer as taking an element out of a `many`,
for the same reason.

There is no way to reach what is inside without asking first. Asking a `bool` is
`E0519`, because a `bool` is never absent; using something that may be missing
where a `bool` was wanted is `E0506`.

## A type that is one of several things

A struct holds all of its fields at once. A `one-of` is exactly one of them at a
time — "and" against "or". It is declared the same way, because what is written
is the same: a name, then a list of typed names.

```
one-of 'answer' [int64 'ok', bool 'flag', deci64 'money', nothing 'gave-up']
```

Each case has a name and a type of its own, and a case that carries nothing says
`nothing`. A choice between one is that one, so fewer than two cases is `E0527`;
a `one-of` that can be itself has no size a machine could give it, and is
`E0526` for the same reason a struct that holds itself is.

**A case holds one thing.** A case that wants to carry two names a struct — the
language already has the word for a group of named things, and letting a case
hold several would put a list inside the declaration, a list of names inside
every `is`, and the same again inside every `holds`.

### Which one it is, is written where the value is made

```
var.answer 'a' = [ok:*7*];
var.answer 'b' = [money:*1.25*];
var.answer 'c' = [gave-up];
```

`ok:` is the same notation `int32:*161*` is: the left says how to read the
right. Writing a type there says what a written value is; writing a case says
which of the things a `one-of` may be this one is. A case that holds nothing is
written bare, the way `nothing` is.

What the value is going into says which `one-of` a case belongs to. Where two of
them share a case name and nothing says which is meant, that is `E0503` rather
than a guess.

### Getting at it is a `when`

```
when 'a' {
    is ok 'n'    { print.stdout[str:*ok * 'n' \n]; }
    is flag 'b'  { print.stdout[str:*flag * 'b' \n]; }
    is money 'd' { print.stdout[str:*money * 'd' \n]; }
    is gave-up   { print.stdout[str:*gave up* \n]; }
}
```

The word after `is` says which case, and the name after that is what the case
holds, lent for as long as the arm runs. A case holding nothing has no name
after it, the same way `is nothing` has none.

This is what `when` was built for. Every case written and each of them once —
`E0522` when one is left out, `E0521` when one is written twice — which with two
cases was a courtesy and with five is the reason to write a `when` rather than a
chain of asks. The rule is the same one `or-nothing` has had all along, counted
over as many cases as the type names.

**Showing one is refused** (`E0536`), and so is writing one out (`E0535`) and
comparing two (`E0506`). All three ask the same impossible question: what a
value is, without asking which of the things it is. `when` is the asking, and
inside an arm the case's own value can be shown, written and compared like
anything else.

### A case may hold something with an owner

Text, a `many`, a struct holding either — a case holds whatever a name could.
What the live case holds goes when the value does, and which case that is is
read where it ends:

```
one-of 'thing' [str 'text', many.str 'words', pair 'both', int64 'n', nothing 'no']
```

**The letting go is an ask at the end of a value's life and nowhere else.**
Making one, reading one and a `when` are unchanged. Where the value ends —
a scope closing, a `set` writing over it, a hand-over — the tag is read and the
case that has something to give back gives it. A `one-of` whose cases all copy
emits nothing at all, because there is nothing to let go of.

That is the shape an `or-nothing` has always paid, which is one branch in front
of the free:

```llvm
br i1 %is-it-there, label %letgo, label %kept
letgo:
  call void @xag_str_drop(ptr %text)
```

A `one-of` is the same with a compare on the tag instead of a truth, and an arm
per case that owns something. C makes you write that switch by hand; C++'s
`std::variant` and Rust's enums generate it, and this generates it.

### What it costs to keep one

One run of memory: the room from the front, and the number saying which case it
is in sitting just past where the widest case can reach.

```llvm
%xag.small = type { [2 x i8] }     ; a `bool` case and an empty one — 2 bytes
%xag.mid   = type { [2 x i64] }    ; an `int64` case — 16
%xag.mixed = type { [2 x i128] }   ; a `str` case and a `deci128` case — 32
```

The number is the narrowest one that can tell the cases apart, and the room is
as wide as the widest case, in units of the strictest alignment any case wants.
Nothing writes past the widest case but the number itself: a case's value goes
in at the front and is at most that wide.

**Two things this was, and why neither is it.** Counting the room in `i128`
whatever it held made every one of these at least twenty-four bytes. Then the
number was a field of its own in front of the room — and a field costs a whole
alignment unit, because what follows it has to start aligned, so `mixed` above
was forty-eight bytes to hold thirty-two bytes' worth. Past the end of the
widest case is space the alignment was going to round up to anyway.

### Sometimes there is no number at all

Where exactly one case holds something, and that something leaves a byte with
room in it, the empty cases are written into the values that byte cannot hold:

```llvm
%xag.inner = type { [1 x i8] }   ; [bool 'a', nothing 'b', nothing 'c'] — 1 byte
%xag.outer = type { [1 x i8] }   ; [inner 'i', nothing 'none'] — 1 byte
%xag.boxed = type { [2 x i64] }  ; [flagged 'f', nothing …] — 16, not 24
```

A `bool` lives in a byte and uses two of its two hundred and fifty-six, so `b`
is written as 2 and `c` as 3. Reading which case it is in is then a look at that
byte: anything below the mark is the case that holds something, and anything
above it counts off the others in the order the type declares them.

**What leaves room, and what does not.** A `bool` does, and so does a `one-of`'s
own number — which is why `outer` above costs what `inner` costs, and why this
compounds as they nest. A struct leaves room wherever one of the things it holds
does. Text and a `many` leave none: an empty one keeps a null pointer, so even
that is a value they hold.

**What this costs to get wrong** is worth saying out loud, because it is not how
anything else here fails. A type that was thought to leave a byte spare, and
does not, is a value read as the wrong case — quietly, with no refusal
anywhere. So the list above is short on purpose, and everything not on it keeps
a number of its own.

**Where Rust is still smaller.** It puts the tag inside a case's *own* padding
even when two cases hold something, which needs the payload written field by
field rather than in one go — written whole, the store may clobber the padding
the tag is living in. `Option<String>` is twenty-four bytes there and thirty-two
here.

`or-nothing` stays its own thing rather than becoming a two-case `one-of`. It is
in the type chain rather than declared, it needs no name for its cases, and
`holds` reads it in one line; folding the two together would cost all of that to
save a table.

## A group of named things

A `struct` gives a name to a group of things, each with a name and a type of its
own.

```
struct 'point' [int64 'x', int64 'y']
```

It is written where a function is, and reads the same way, because it asks the
same question: what is in here, in what order, and called what. A struct that
holds nothing is `E0525` — a group of none is `nothing`, which the language
already has — and one that holds itself is `E0526`, because however many times
it were laid out there would always be one more of it inside.

Two structs may name each other, so the order they are written in does not
matter:

```
struct 'line' [point 'from', point 'to']
struct 'point' [int64 'x', int64 'y']
```

### Making one needs no new notation either

Items side by side, in the order the struct holds them — the same list as
everywhere else, read the way this type reads it.

```
var.point 'p' = [*1* *2*];
```

Leaving one out is `E0529` rather than a shorter struct: they go in by position,
so a missing one would silently move every one after it. A value of the wrong
type is `E0506`, as anywhere.

### One of them is named with a dot

```
print.stdout['p'.x \n];
set 'p'.y = [*9*];
```

The mark stays on the name, because the name is the variable and `x` is not one
— it is one of the things `'p'` holds, which is a word like any other. A field
that the struct does not hold is `E0528`; asking a type that has no fields at
all for one is `E0527`.

### One inside another is named where it is made

```
struct 'point' [int64 'x', int64 'y']
struct 'line' [point 'from', point 'to']

var.line 'l' = [point[*0* *0*] point[*1* *1*]];
```

The obvious spelling would have been the same brackets one level down —
`[[*0* *0*] [*1* *1*]]` — and it cannot be had. A name before a bracket is
already indexing, so `['ns' [*0*]]` reads as `'ns'[*0*]`: one item where two
were written, silently and with no error anywhere. Juxtaposition leaves the
parser nothing to tell them apart by.

A *word* before a bracket is a call, and a word is never a name. So naming the
struct settles it with the two marks that were already there, rather than with
whitespace or a lookahead. It also says which struct is being made without the
reader having to know the field order of the one around it.

Answering with a struct needs no name, because the chain already said what the
answer is: `give [*0* *9*]`.

### It is handed over, never copied

Like a `many`, and for the same reason: the places it holds are its own, and
there is only ever one of them. This is true however little is in it.

```
var.point 'a' = [*1* *2*];
var.point 'b' = [move 'a'];
```

Whether the *items going in* need `move` is each field's own question, though,
the same as an element of a `many`:

```
struct 'mixed' [int64 'n', str 's']

var.str 'text' = [*hi*];
var.mixed 'm' = [*7* move 'text'];    # the number copies, the text does not
```

### One of them may go on its own

This is what a struct has that a `many` does not. Which field is written is known
where it is written, so one can be handed over while the rest stay:

```
var.tag 't' = [*ada* *36*];
keep[move 't'.name];
print.stdout['t'.runs \n];           # the rest of it is still here
```

What went is gone: asking for it again is `E0413`, and asking for the whole after
a part has left is `E0414`. When the struct ends, it lets go of what it still
holds and nothing else.

Lending one of them lends the struct, because what the loan points at lives
inside it and goes wherever it goes — so handing the struct over while a field is
lent is `E0408`, and writing that field behind the loan's back is `E0409`.

## Converting, on purpose

Nothing converts on its own — that is the rule `E0506` enforces everywhere. So
converting is a thing you ask for, by name:

```
var.int64 'n' = [*42*];
var.str 's' = [str:*x = * convert-to-str['n']];        # "x = 42"

var.or-nothing.int64 'back' = [convert-to-number[loan 's']];
```

The two are not mirror images, and the names say which is which. Every number
has a way of being written, so `convert-to-str` answers a `str` and cannot fail.
Text that is not a number has no number in it, so `convert-to-number` answers
`or-nothing` of whichever number the chain asked for — `holds` or `when` opens
it.

That is also why neither is spelled after a type. `str['n']` would sit one
character away from `str:*hello*`, which does something else entirely; and
`int64['s']` would promise an `int64` it cannot always deliver. A word that says
`convert-to-…` promises only what it is aiming at.

### It writes what a print writes

`convert-to-str['n']` gives exactly the characters `print.stdout['n']` would
write, because it is the same code writing them. `1.10` keeps its trailing
zero, `0.1` comes back as `0.1`, a `uint128` gets all thirty-nine digits, and a
`bool` is `true` or `false`. Two ways of putting a number in front of somebody
cannot drift apart.

### What has no one way of being written

Text is refused (`E0535`): it is already text. A value that may hold nothing is
refused too, wherever the absence sits — inside a struct or under a `many`, the
message says which field it found — because an absence is not a value and there
is nothing to write for it.

A `many` and a struct are not refused. They are written the way a print writes
them, which is the next section.

## Showing what holds several things

A print writes one piece after another. A `many` and a struct hold several
values, so showing one writes each of them, in order, with nothing between:

```
var.many.int64 'xs' = [*1* *2* *3*];
print.stdout['xs' \n];                 # 123
```

**Nothing stands between two of them, and that is not a decision dodged.**
Writing a `many` out is the same as writing its places side by side in the
print — `print.stdout['xs'[*1*] 'xs'[*2*] 'xs'[*3*]]` — and nothing stands
between two pieces of a print anywhere else either. A comma, a bracket or a
space would be characters the program never asked for. What goes between them
is what the program writes:

```
loop.range.int64 'i' = [*1*, count[loan 'xs']] {
    print.stdout['xs'['i'] str:*, *];
}
```

It goes all the way down. A `many` of a `many` writes every place of every row,
a struct writes every field, and a struct holding either writes what those
hold. Nothing is skipped and nothing is summarised. A borrowed field writes what
it borrows — the value, not where it lives.

It always ends: a struct that holds itself is `E0526`, so the depth is fixed by
the type before the program runs, and the only part not bounded by the type is
how long a `many` is — which is exactly as long as you asked for.

**An absence is still refused** (`E0536`), wherever it sits. A `str` that may
hold nothing and an empty one would write the same characters, and nothing
reaches inside one without asking — `holds` and `when` are the asking. A struct
with such a field is refused for that field, and the message names it.

They wrote nothing at all until 2026-09-07 and were refused until 2026-09-10 —
and nothing at all in all three engines is three engines agreeing, which is why
the oracle never saw the hole.

## A sum that does not fit

A sum that does not fit **stops the program**, unless something said it was meant
to come round. That is the only answer for one nobody could work out ahead of
time: coming round quietly is how a wrong number gets all the way to the end
looking like a right one.

What *can* be worked out is refused or reported before anything runs, and those
sums cost nothing at run time. So the compiler works out how far a counted loop
gets and says so when it can:

```
var.mut.int8 'sum' = [*0*];
loop.range.int64 'i' = [*1*, *10*] {
    set 'sum' = ['sum' + *20*];      # E0534 — this reaches 200
}
```

Both ends of the loop are written down, so how many times it runs is settled
before the program runs, and so is where the sum gets to. That is certain rather
than suspected, and it is refused.

Where it cannot be worked out, it is said rather than refused (`W0001`), and the
program still builds. Refusing there would turn away a correct program because
the compiler was not clever enough, and saying nothing would let a wrong answer
through in silence.

### `wrapping` says it is meant

```
var.mut.wrapping.int8 'sum' = [*0*];
```

Then nothing is said about it and nothing checks it. A checksum is meant to come
round, and the word says so where the name is declared rather than at every sum
that touches it.

It is written wherever a thing is declared: a `var`, a `const`, a parameter, and
one of the things a struct holds.

```
fn.nothing 'mix' [loanmut.wrapping.uint32 'state', loan.uint32 'by'] { … }
struct 'digest' [wrapping.uint32 'state', int64 'length']
```

**What it buys is the machine's own instruction.** Without it a sum is asked
whether it fitted, and stops where it did not; with it the add is an add. That
is the whole difference, and it is why the word is worth writing where a program
means it — and why writing it where a program does not mean it is worse than not
writing it at all.

`checked` is the other side and is refused (`E0201`): it is what a name is when
nothing says otherwise, and a word that cannot change the answer is not written.

### Where it cannot be said

A sum that never reaches a declaration has nowhere to carry the word:

```
if ('n' x int8:*4*) > 'limit' { … }
```

That one is checked and there is no opting out of it. A sum written into a name
can say `wrapping`; one written into a condition can be given a name first.

### What it can follow

The counter never passes where the loop stops, so anything built out of it is
bounded too — `'i'`, `'i' x *3*`, `'i' / *7*`, and `'i' mod *7*`, which never
reaches 7 whatever it was taken from. These are the shapes real programs write,
and a warning on every one of them would be a warning nobody reads.

## What comes in

```
loop.while read.stdin[] holds 'line' {
    var.or-nothing.deci64 'read' = [convert-to-number['line']];
    when 'read' {
        is 'value' { ... }
        is nothing { print.stdout[str:*not a number: * 'line' \n]; }
    }
}
```

`read.stdin` answers a `str` **or nothing**. The end of the input is not an
empty line — an empty line is something a program may legitimately read, and
telling the two apart is what the type is for. Whatever ended the line is not
part of it.

`convert-to-number` reads a number out of text, which is where text stops being
text. It answers `or-nothing` of whichever number was asked for, because text
that is not a number has no number in it. Which number is a question something
else has to have answered, the same way `fill` knows what it is filling — so it
cannot stand on its own with nothing beside it to say, and that is `E0523`:

```text
nothing here says what number this would be.

  3 |     when convert-to-number[loan 'line'] {
    |          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ here

Error code: E0523
Rule(s) broken: a size is always written, and only sizes the standard defines
```

That is the size rule reaching input: a `when` asks for no particular number, so
the answer is named before it is chosen between.

`arguments` answers a `many.str` — what the program was given, without the name
it was run under, which is not something anybody passed. `xagc` hands on
whatever followed `--`:

```
xagc run adder.xag -- 3 4
```

Neither of these needed a rule of its own. Reading has no answer at the end and
a parse has no answer when the text is not a number, and both of those are the
same thing the type already says.

## Ownership

A name owns its value until the value is moved, and then it holds nothing.

```
fn.int64 'size' [loan.str 'text'] { ... }           # borrowed, read-only
fn.nothing 'excite' [loanmut.str 'text'] { ... }  # borrowed, writable
fn.nothing 'keep' [str 'text'] { ... }           # takes it — `own` is the default
```

### A transfer is always spelled at the call site

Declarations default; transfers never do.

```
size[loan 'greeting'];
excite[loanmut 'greeting'];
keep[move 'greeting'];
```

A declaration describes a thing, but a call site *acts*, and the consequence here is
that `'greeting'` stops existing. That is worth a word every time.

A word is written where there is a **choice**, though, and `give` has none — there
is nothing else it could mean — so the answer is handed over without one:

```
fn.str 'greet' [] {
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
fn.loan.str 'broken' [loan.str 'other'] {
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
  13 |     var.loan.str 'winner' = [longer[loan 'greeting', loan 'reply']];
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
fn.loan.str 'echo' [loan.str 'text'] {
    give ['text'];
}
```

When two are, there is a choice, and the compiler does not get to make it. The
loan is given a name, and everything on that loan is written with it:

```
fn.loan.'life'.str 'longer' [loan.'life'.str 'a', loan.'life'.str 'b'] {
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
fn.loan.'as long as both inputs'.str longer [...] { ... }
```

Leaving it out where it is needed is its own error, and the compiler does not
guess:

```text
this answer is borrowed, and so are two of the parameters.

  1 | fn.loan.str 'longer' [loan.str 'a', loan.str 'b'] {
    |    ^^^ here

Error code: E0402
Rule(s) broken: a borrow that is given back says which loan it belongs to
Tip(s): with one borrowed parameter there is only one loan the answer could be
        on, so nothing is written; with two there is a choice.
```

## Open

- **A number in room the cases do not reach.** Done, and in two ways. Where
  exactly one case holds something and that something has a value it never
  takes, the number is written there and there is no number of its own: a
  `bool` uses two of its byte, and a `str`'s pointer is somewhere for every
  `str` there is, so nowhere means the other case. And where two cases hold
  something, the number goes in padding a case was going to have anyway.

  `one-of [str 'some', nothing 'none']` and `or-nothing str` are twenty-four
  bytes, which is a `str` — the same as Rust's `Option<String>`. `one-of [p 'x',
  int64 'y']`, where `p` is an `int64` and a `bool`, is sixteen rather than
  twenty-four.

  This said the gap was about padding and it was not: `Option<String>` was
  thirty-two bytes here because an empty `str` and an absent one were both a
  null pointer, so no pattern was free to tell them apart. The runtime keeps
  every empty `str` pointing at one static byte now, and the pattern is free.

  It also said the padding case wants the payload written field by field.
  It does not — it wants it written *first*. A store of the whole value writes
  its own padding as it likes, so the number goes in after it and survives.
  Nothing writes a value into a `one-of` that already stands: one is built whole
  and read after, never filled in twice, and that is the assumption the layout
  rests on.

  What is left is a case that reaches every byte it has and leaves nothing
  over — `one-of [str 'text', int64 'number']` is thirty-two, honestly, because
  a `str` uses all twenty-four of its own.
- **Visibility.** `export` and `program` wait on there being more than one file.
- **`wrapping` on a sum with no name.** The word is written where a name is
  declared, and a sum happens between values. `('n' x *4*)` inside a comparison
  may come round and there is nothing anybody could write to say it is meant to,
  so nothing is said about it — see `design/compile-time.md`.
- **`UNSAFE`.** Capitals, like `START`, and it has a job now — the first thing
  that needed permitting turned up before calling out to C did. Built.

  A run the compiler does has no limit, so a long loop can cost real time at
  build. `no-itmt` is a loop saying not to bother:

  ```
  UNSAFE {
      loop.no-itmt.range.int64 'i' = [*1*, *1000000000*] {
          set 'total' = ['total' + churn['i']];
      }
  }
  ```

  **The word is on the loop, and `UNSAFE` is the region.** A chain says what is
  unusual about the thing being declared, and not being run at build time is
  about *this loop*; `UNSAFE` around it is what a reader greps for. The chain
  word asks and `UNSAFE` grants, and neither alone does anything.

  **Not `no-run`.** The loop does run — at runtime, every time, exactly as
  written. It is only the compiler that does not run it, and a word built on
  *run* in that position reads as a loop that never executes.

  It names ITMT because ITMT is what it turns off, and because a word for the
  thing itself borrows nothing: `no-verif` was tried first and imported
  "verification", which nothing else in the language is called.

  The chain asks it first — `loop . [no-itmt] . [perm] . range . type` — because
  it is about the loop, where `perm` is about the counter the loop declares. A
  `while` may ask too, and is the one that most needs to: its ends are not
  written down, so a run of it may never finish. Asking outside `UNSAFE` is
  `E0212`, and there is no word for the ordinary case, because not writing one
  is it.

  Twenty million rounds with a branch in them: nine seconds of build without it,
  three hundredths with. The same program either way, and the same answer.

## A file is one of two shapes

```
READ_ME {                  READ_ME {

}                          }

PREP {                     LIBRARY {

}                          }

START {                    ITMT {

}                          }

ITMT {

}
```

A program on the left, a library on the right. Every block in the shape is
written, in that order, whether or not there is anything in it. A shape that is
sometimes there is a shape a reader has to look for; this one is always in the
same place. The second block says which shape it is.

**A library has no `START`** because it has no moment of its own. Everything in
it is a declaration, and what runs is what a program that imports it calls. A
library that needed something done before it was used — a table built, a file
opened — would want a `START`; that is initialisation, and it was left out on
purpose. Three libraries each with one, and one depending on another's having
run first, is a problem C++ has a name for. A table that depends on nothing is a
`const`; a file that has to be opened is opened by a function the program calls,
where a reader sees it.

**`ITMT` is run while building and never ships.** Both shapes have one. A
program's `START` is run while building too, and does ship; `ITMT` is for what
should only ever run here — and it is the whole of what a library can run, which
is how `xagc build` on a library alone means something. Everything in it goes
through the same two engines as everything else, and a disagreement refuses the
build.

**`import 'text';`** in `PREP` or `LIBRARY` says this file uses a unit the
manifest knows by that name. Marks on the name because it is one: it comes back
as the prefix on every name reached through it. See `design/units.md`.

**`PREP` is everything outside `START`** — the structs, the constants, the
functions. It reads like a recipe: what the dish is made of, then the method.
The split it names is not given-against-not — a `var` in `START` is given too —
but **lasts the whole program** against **lasts while it runs**.

`GIVEN` and `NAMES` were tried and dropped: a `var` in `START` is given a name
as much as a `const` is, so both would name the block after something it shares
with the thing it is meant to be the opposite of.

**`READ_ME` is prose**, kept exactly and read by nobody. It is there so that what
a file is for lives in the file rather than beside it.

Markdown uses every mark Xag does — braces, backticks, `*`, `'` — so it cannot
be read as Xag and then passed over. The reader stops reading: `READ_ME` is the
one word that turns the lexer off, and what follows arrives as a single token.

It ends **at a `}` standing at the start of a line**, which is where every block
at the top of a file ends. On one line — `READ_ME { }`, which is what an empty
one looks like — it ends at the first `}`, because there is nowhere else it
could.

The cost is exact and worth writing down: a `}` at the start of a line inside the
prose ends the block early, fenced code included. Indent it by one space and it
is prose again. The alternative was a delimiter nothing else in the language
uses, and a second thing to remember is worse than a rule with one edge.

Errors: `E0111` when a block is missing, and it says which and for which shape;
`E0110` for anything standing outside the blocks; `E0104` when a `var` is written
in `PREP` or `LIBRARY`, which is the one mistake the split invites.

## The tree with the questions answered

Between the checker and everything below it there is one more tree. It is the
program still — statements and expressions, no basic blocks and no drops — with
every question the checker answered written into it rather than into a table
beside it.

```
declare 't' : thing
  case both #0 : thing
    made pair : pair
      written p : str
      written 9 : int64
when
  name t : thing
  arm both #0 'p' : pair
```

Every expression carries its type. A field is a number, a `when` arm is a case
number, a word before a bracket has already been told apart — a call, or a
struct made where it stands — and `str:*hi*` and `text:'s'` are different nodes
rather than one node and a lookup. Brackets that only group are gone, because
grouping is the shape of the tree.

**Why there is one at all.** Everything below the checker used to work the type
of a thing out again. The ownership pass kept a model of its own built from
chains as text; the middle layer wrote types out with one function and read them
back with another. Two answers to one question, and the two bugs that cost most
on 2026-09-10 were the two disagreeing: a case the checker knew about and the
ownership pass did not, which let a value be let go of twice; and a type written
to text and parsed back against the wrong table, which gave a thirty-two byte
struct eight bytes to live in.

`xagc typed <file>` prints it.

**What it is not.** It is not where anything is decided. Where building it looks
something up rather than working it out, that is the point — a second opinion is
the thing it exists to remove.

## Capitals

`READ_ME`, `PREP`, `START` and `UNSAFE` are written in capitals so they can be
found. That is the whole reason. The shape of a file, where a program begins,
and where it stops being checked are what a reader scans for and a reviewer
greps for, and capitals make them impossible to miss in a file of lower-case
words.

It is not a system, and nothing else is capitalised by it. `export` and
`program` are lower case in **Open** above and stay that way unless there is the
same reason to change them.
