# Generics, and looking at a type

**Nothing here is built.** Decided in conversation on 2026-09-08; written down so
it does not get built the wrong way round.

Xag already has generics — a program just cannot write them. `many.int64`,
`many.point`, `or-nothing.str`: one type parameterised by another, working over
every type including structs, carried in `MirType::held`. The question was never
whether to have them. It is whether a program can write what `many` already is.

## Decided

**Both functions and structs may be generic.** Structs are what a growable
`many` would need, and a growable `many` is what would stop `many` being
special.

**Types are values.** Not the whole of Zig's `comptime` — what is wanted is the
part that lets a program look at a type it was never told about, because that is
what answers `E0516`, and `E0516` is blocked today.

### Walking a struct's parts

A struct's fields each have a name and a type, written down where the struct is
declared. A generic function that has never seen the struct cannot write `'p'.x`,
because it does not know there is an `x`. So it walks:

```
fn.str 'show' [loan.any 'x'] {
    var.mut.str 'out' = [*<*];
    loop.parts 'part' = ['x'] {
        set 'out' = ['out' 'part'.name str:*: * ... 'part'.value ...];
    }
    give ['out' *>*];
}
```

**A turn hands you the field, not the value.** `'part'.name` is what it is
called, as text; `'part'.value` is what it holds. Handing over the value alone
was tried on paper and cannot write `x: 1` at all — a value has no name, only
the field does.

It is not really a loop. `'part'.value` is an `int64` on one turn and a `str` on
the next, so the body cannot be compiled once and run twice: it is unrolled
while compiling, one copy per field. Zig calls the same thing `inline for`.

`loop.parts` walks a struct and nothing else. A `many` holds one type in every
place, so a counted loop already reaches them and there is nothing to unroll —
and a place has a position rather than a name, so `'part'.name` would have
nothing to hold. The two only look alike.

### Asking what kind of thing something is

Walking a `struct 'line' [point 'from', point 'to']`, a field is itself a struct,
and `convert-to-str` does not take one. So the writer can ask:

```
whichever 'part'.value {
    is number { ... }
    is str    { ... }
    is struct { show[loan 'part'.value] }
    is many   { ... }
}
```

**Families, not exact types.** Naming exact types would mean twenty-odd branches
doing the same thing and an `else` catching every struct anybody will ever write
— where nothing useful could be done, because nothing is known. `is struct` is
what lets `show` call itself, which is how a `line` made of `point`s is shown at
all.

**`whichever`, not `when`.** They look alike and are not: `when` is a choice the
program makes while running, with both arms in it. `whichever` is decided while
compiling and only the chosen arm survives into the program. A word appears
where there is a choice, and that is a real one — and the word says the choosing
rather than the asking, which is what is actually happening.

### The words

```
is number      is int   is uint   is bin   is deci
is str         is bool
is many        is or-nothing       is struct
is loan        is loanmut
```

Only five of those are new vocabulary, and they are the number words. The rest
are words the language already has, which follows a rule worth stating:

- A family with **many members** gets a word of its own — `number`, and `int`,
  `uint`, `bin`, `deci` under it.
- A family with **one member** is called by the type's own name — `str`, `bool`.
- Everything else already had a word — `many`, `or-nothing`, `struct`, `loan`,
  `loanmut`.

`int` is free to name a family precisely because it is not a type: *there is no
`int` on its own, because there is no size to assume*. Not a type, and a family,
which is exactly what a word here has to be.

**`loan` and `loanmut` are two words, not one.** They are not interchangeable —
one may be written through and the other may not — so a generic that collapsed
them would have to find out some other way. There is no collision in reusing
the chain's words: `many.int64` says *make one* and `is many` says *it is one*,
and a borrow behaves the same.

All of them can turn up. A struct's field may be a `many`, a `bool`, an
`or-nothing` or a borrow, which was checked rather than assumed.

### A part is an ordinary struct

`'part'` is a real value with a real type: a struct of a `str` called `name` and
whatever the field holds, called `value`. It can be handed about like anything
else.

Its type differs from turn to turn — `[str 'name', int64 'value']` walking one
field, `[str 'name', str 'value']` walking the next — and that needs nothing new,
because the loop is unrolled. Each turn is its own copy of the body with one
concrete struct in it. Nothing in the type system has to hold a type that
changes; the *copies* differ, and each copy is ordinary.

### Two arms that overlap are refused

A field holding an `int64` answers to both `is int` and `is number`, so one
`whichever` may not ask both. Pick a level: `is number`, or the four families.

Most-specific-wins was argued for first and withdrawn. The case for it was
"handle numbers generally, except decimals" — and that case does not exist:
`convert-to-str` already answers every family correctly, keeping a decimal's
trailing zero. A rule was being added for a need nobody had.

Refusing costs nothing today, adds no idea of one word being *more specific*
than another — which the language has nowhere else — and is the least powerful
answer, which is the rule. It is also the safe direction: going from refused to
allowed later breaks nothing written before it, and going the other way breaks
everything.

Nobody writes an overlap deliberately. It arrives when somebody *extends* a
`whichever` written months earlier, which is exactly the moment nothing else
would say anything.

Other languages allow it because for them overlap is the mechanism rather than
the mistake: `Some(0)` before `Some(x)` is how a pattern says "this case, then
the general one", and a wildcard overlaps everything on purpose. They check
exhaustiveness instead. `whichever` asks a closed question with a fixed set of
answers and no values in it, so there is no "this particular one, then any" to
express.

### A generic may say what it asks for

The same words, in the other direction:

```
fn.any.number 'largest' [loan.many.any.number 'xs']
```

`is number` asks what something is; `any.number` says what will be taken. One
list, learned once, read both ways.

`whichever` came first and nearly made this unnecessary — a generic that can ask
what it was handed can cope with anything, so nothing has to be refused in
advance. What is left is that some generics do not *want* everything. The
largest `point` is not a thing, and the author would rather say so than write a
branch for it.

What it buys is where the failure lands. Without it, `largest` uses `>` and a
caller passing points is told about a line inside `largest` that they did not
write. With it:

```text
`point` is not a number, and `largest` asks for one.
```

Bare `any` stays the floor: it takes anything, and what can be done with it is
what can be done with every type — hold it, move it, lend it, hand it back.
Every word added buys one thing more.

### A blank is named only when there are two of them

`any` covers the single blank, the way nothing at all covers a single loan.
A name appears where there is a choice about *which*, and with one blank there
is none:

```
fn.loan.'the list'.any 'first' [loan.'the list'.many.any 'xs']
        ^^^^^^^^^^ ^^^
        quoted: yours    bare: Xag's
```

One quoted word and one bare word, told apart by looking rather than counting.

Two quoted words side by side needs a generic over *two* types that also hands
back a borrow on a named loan — and that is allowed rather than refused. It is a
corner, the compiler reads it by slot without trouble, and refusing a shape
because it is hard to read is a cost paid by everyone to spare a few.

### The first appearance declares it

Reading the whole signature left to right, the first `'held'` introduces the
blank and every one after has to be it:

```
fn.'held' 'largest' [loan.many.'held' 'xs']
   ^^^^^^ declares            ^^^^^^ must be that one
```

Which makes a typo an error rather than a second blank nobody asked for:

```
fn.'held' 'largest' [loan.many.'hedl' 'xs']
                              ^^^^^^ names no type this function has
```

Without that check the two ends are unrelated, the function still compiles, and
it does not mean what was written — a generic that does not tie its ends
together is a comment rather than a generic.

**First in reading order, not first in the chain.** The answer's type is not
always written first: `fn.int64 'count-of' [loan.many.'held' 'xs']` gives back a
plain `int64`, and `'held'` appears first among the parameters.

### Type parameters stay in the chain

Declaring them elsewhere was considered — `[type 'held', loan.many.'held' 'xs']`,
a type parameter as an ordinary parameter — and dropped. It does not remove the
two-quoted-words case, because that is about *uses* and not about where a thing
was declared. What it would add is a second place for a declaration to live,
when the chain is already where everything about one lives.

## Open

- Whether a loan name should be tightened the same way. It declares itself by
  first appearance too, and nothing checks the rest: `'typo-here'` written once
  on a single parameter compiles and means nothing. Harmless there, because with
  one borrow there is nothing to tell apart — but it is the same rule, enforced
  in one place and not the other.
