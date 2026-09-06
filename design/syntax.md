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
fn.ref.str 'longer' [ref.str 'a', ref.str 'b'] { ... }
loop.perm.range.int64 'i' = [*1*, *100*] { ... }
```

### Every segment but the type has a default

**The default is always the least-powerful option, and writing a word always means
asking for something.**

| segment | default | written when |
| --- | --- | --- |
| mutability | `immut` | `mut` — asking to change it |
| ownership | `own` | `ref` / `refmut` — asking to borrow |
| loop counter | `temp` | `perm` — asking to keep it after the loop |

Visibility — `export` / `program` against a default of `file` — is where it will
go, and is refused for now (`E0206`): a program is one file, so there is nothing
outside it for anything to be visible to. A word that cannot change the answer is
not written.

`perm` keeps the counter, and what it holds afterwards is what it last took: the
value a `break` left behind, or one past the last when the loop simply ran out.
Wanting it after a `break` is the only reason to keep one at all.

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
var    . [mut] . [ref|refmut]              . [many] . type
fn     . [ref|refmut] . ['loan']           . [many] . type
const                                      . [many] . type
loop   . [perm] . range                    .          type
loop   . while
param    [mut] . [ref|refmut] . ['loan']   . [many] . type
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
(`E0204`); and `var.ref.mut.str` says the same thing as `var.mut.ref.str`, which
is one spelling too many (`E0205`):

```text
this chain answers whether it owns or borrows before whether it changes, and
they are read the other way round.

  1 | var.ref.mut.str 's' = [*hi*];
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
fn.int64 'total' [ref.many.int64 'xs']    # a parameter, borrowed
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

### An element is reached with the name's own brackets

```
print.stdout['xs'[*0*] \n];
set 'xs'[*2*] = [*99*];
var.int64 'n' = [count['xs']];
```

A bare *word* followed by `[` is a call; a *name* followed by `[` is an element,
and the two can never be read for each other because marks say which is which
before the bracket is reached.

`count` answers how many, for a `str` and for a `many` alike — it is the same
question, and the type already says what is being counted. Asking a name that
holds one value for its first is `E0514`: a name holding one value **is** that
value, and there is no first of it.

An index is an `int64`, because that is what `count` answers with and two sizes
never meet on their own. A negative index is simply out of range.

### An element is a place, not a value

`'xs'[*2*]` says *where* a value is, and what happens there depends on what is
asked of it:

```
print.stdout['xs'[*0*] \n];        # read it, and leave it where it is
set 'xs'[*0*] = [*99*];            # write it, ending what was there
size[ref 'xs'[*0*]];               # lend it
move 'xs'[*0*]                     # refused — E0412
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

Reaching past the end changes what a program answers, so it is a setting, and
both values are real languages every engine has to agree under:

```toml
out-of-range = "stops"   # or "wraps"
```

`stops` says which index, how long the array was, and where, and stops there in
every engine. `wraps` takes the index around the length, so `*-1*` is the last
place and nothing ever stops.

An empty `many` stops under both, because wrapping needs somewhere to land and
there is nowhere.

### What it costs

The rule lives in the runtime and every engine asks it, but native code writes
the half of it that says *yes* as a compare and a branch, because a call the
optimiser cannot see into is a call it cannot remove — and this one sits in the
middle of every loop over a `many`.

What that buys, in a loop counting to `count['xs']`, is that LLVM proves the
index always fits and lifts the check out of the loop entirely. This is the
whole body of `total` at `-O3`:

```llvm
%1 = getelementptr [8 x i8], ptr %places, i64 %i
%2 = load i64, ptr %1, align 8
%3 = add i64 %2, %sum
%4 = add nuw i64 %i, 1
%5 = icmp sgt i64 %4, %last
```

A load, an add, an increment and the loop's own test. Nothing of the check is
left in it.

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

## What comes in

```
loop.while read.stdin[] holds 'line' {
    var.or-nothing.deci64 'read' = [number['line']];
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

`number` reads a number out of text, which is where text stops being text. It
answers `or-nothing` of whichever number was asked for, because text that is not
a number has no number in it. Which number is a question something else has to
have answered, the same way `fill` knows what it is filling — so `number` on its
own, with nothing beside it to say, is `E0523`:

```text
nothing here says what number this would be.

  19 |         when number['line'] {
     |              ^^^^^^^^^^^^^^ here

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
fn.int64 'size' [ref.str 'text'] { ... }           # borrowed, read-only
fn.nothing 'excite' [refmut.str 'text'] { ... }  # borrowed, writable
fn.nothing 'keep' [str 'text'] { ... }           # takes it — `own` is the default
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
fn.ref.str 'broken' [ref.str 'other'] {
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
fn.ref.str 'echo' [ref.str 'text'] {
    give ['text'];
}
```

When two are, there is a choice, and the compiler does not get to make it. The
loan is given a name, and everything on that loan is written with it:

```
fn.ref.'life'.str 'longer' [ref.'life'.str 'a', ref.'life'.str 'b'] {
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

  1 | fn.ref.str 'longer' [ref.str 'a', ref.str 'b'] {
    |    ^^^ here

Error code: E0402
Rule(s) broken: a borrow that is given back says which loan it belongs to
Tip(s): with one borrowed parameter there is only one loan the answer could be
        on, so nothing is written; with two there is a choice.
```

## Open

- **Turning a number into text, and text into a number.** Pieces side by side
  join, and nothing converts on its own, so something has to do it and be named.
  Now that a type can say it holds nothing, the failing half has somewhere to go.
- **A type with more than two shapes.** `when` covers every case a value could
  be, and today a value can be two things. A type that could be several — with
  its own names and its own contents — is what would make the construct earn
  itself, and the middle layer is already shaped for it.
- **A `many` that grows.** `many` is a fixed length, settled when it is made.
  Growing is a second type rather than a mode of this one, because growing may
  move what is held and a loan of it would then point at nowhere — a rule
  Regions would have to learn, and the first place ownership here stops being a
  demonstration.
- **A `many` of a `many`.** One level, and `E0210` says so. A second is where a
  type stops fitting in a pair of words and wants a table of its own.
- **Showing a `many`.** What stands between two of them is a decision nobody has
  made, so `E0516` refuses rather than choosing.
- **Visibility.** `export` and `program` wait on there being more than one file.
