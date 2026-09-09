# Generics, and looking at a type

**All of this is built**, on 2026-09-08 and 2026-09-09. Decided in conversation
first and written down so it did not get built the wrong way round, which is why
this reads as a plan; each part says underneath it what building it settled, and
what building it found.

Nothing here is outstanding. What a blank takes, what a `whichever` chooses by,
what a turn of `loop.parts` hands over — all of it runs, and the generator writes
programs that use it.

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

  15 |     print.stdout[str:*pts = * (largest[loan 'pts']) \n];
     |                                        ^^^^^^^^^^ here
```

**Built 2026-09-09.** Three things had to know about it, and nothing else did:

- The chain reader takes `any.number` as **one type region**, the way it already
  takes `many.int64` — the blank, and the word saying what it will take standing
  nearest the name. It is told the shape and nothing more; whether `number` means
  anything is still the checker's question.
- The checker carries the family on the blank (`Ty::asks`) and asks it **at the
  call that fills the blank in** — the one place where the caller and the type
  they brought are both in view. That is the whole of what the word buys.
- Filling a blank in **takes the word with it**. `any.number` at `int64` is
  `int64`, not `int64.number`: the question has been answered, and the word
  asking it has nothing left to say.

A refused call answers an unknown rather than the blank it could not fill, so
everything built on it folds underneath the refusal instead of being reported as
news of its own.

**Ten of the twelve words are built.** `loan` and `loanmut` are missing, and not
because they do not belong: this document says one list read both ways, and it
already answered the objection — `many.int64` says *make one*, `is many` says
*it is one*, and a borrow behaves the same. It is the same for `any.many`, which
is built.

They are missing because **nothing in the checker could answer them**. A `Ty`
knows a kind, an element, a struct and whether it may hold nothing; it does not
know whether it is a borrow. To the checker a `loan.int64` field *is* an
`int64` — `struct 'holder' [loan.int64 'x']` checks clean and comes out as one —
and how a thing is held is `Own.cpp`'s to say, in a `Mode` the checker never
sees.

`is loan` wants exactly the same answer. So it is one piece of work rather than
two: teach `Ty` how a thing is held, and both words arrive together. It waits
for `whichever`.

### A blank is filled in with a chain, not a word

Filling a blank in wrote **one segment**, which is every type a scalar and no
type else. A blank filled in with a `many` came out spelled `unknown`; one
filled in with an `or-nothing` came out spelled as the thing inside it, so the
copy took a plain `int64` and refused the very call that had asked for it.

A type is a chain fragment: `many.int64` is two segments, `or-nothing.many.point`
is three. Both ends know that now — what a type is spelled as, and what filling
a blank writes — and the copies are named for it: `echo$many.int64`,
`echo$or-nothing.str`.

This was there from the day generics were built and was found by asking whether
`any.many` worked. It did not, and neither did bare `any` handed a `many`.

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

A loan name is checked the same way. It declares itself by first appearance
too, and today nothing checks the rest — `fn.int64 'size' [loan.'typo-here'.str
'a']` compiles, and the label means nothing. Harmless, because with one borrow
there is nothing to tell apart, and still a word that changes nothing, which is
what `E0201` already refuses elsewhere. One rule in one position, enforced.

### Type parameters stay in the chain

Declaring them elsewhere was considered — `[type 'held', loan.many.'held' 'xs']`,
a type parameter as an ordinary parameter — and dropped. It does not remove the
two-quoted-words case, because that is about *uses* and not about where a thing
was declared. What it would add is a second place for a declaration to live,
when the chain is already where everything about one lives.

## Open

- Nothing about the design. Everything asked has been answered.

## What it costs to build

Tried on 2026-09-09 and backed out, because the shape is worth knowing before
anybody starts again.

Adding `any` as a type word and binding it at a call is an afternoon: the first
argument whose parameter has a blank says what the blank is, every other `any`
in the signature is then that, and the answer's type is filled in the same way.
That much worked.

Then the call fails on **ownership**, and it is not a bug:

```text
`'n'` is handed over here, and nothing says so.        E0406
```

`Own.cpp` decides whether a thing copies or is handed over by looking at the
type word, and `any` is not a type it knows. Whether a generic's parameter
copies **depends on what it is called with** — `int64` copies, `str` is handed
over — so ownership cannot be settled until the type is. The same is true of
`MirBuild`, and of everything else that asks whether something copies.

So generics are not a front-end feature. They reach four passes, and the way
that keeps them out of all four is the one this document already assumes: a
generic is **expanded away before anything runs**, once per type it is called
with, so `Check`, `Own`, `MirBuild` and `Native` never meet a blank at all.

That needs the binding, which is the checker's to work out, and then a **deep
copy of an item's tree** with the blank replaced — and the tree is held in
`unique_ptr`s with no clone. Writing that clone is the first real piece of work,
and nothing else can start without it.

## A generic calling a generic

Expansion happens in rounds, not once.

The checker deliberately does not read a generic's body — whether a thing copies
is the very question the blank has not answered yet. So a call *inside* a
generic body is not read either, and nothing knows what it was asked for until
that body has been written out at a type and read for the first time.

One round is therefore not enough. `twice` calls `same`; reading the file finds
`twice` at `int64`, and writing `twice$int64` out is what lets the next reading
find `same` at `int64`. Each round writes what the last one learned.

Two things make that settle:

- **A generic still called by name stays.** It is written out again, blank and
  all, at the end of the round, because the call naming it sits in a copy that
  has only just been written and has never been read. On the round after nothing
  names it, and it goes. A generic nothing ever calls is dropped on the first
  round, since nothing names it then either.
- **A copy already standing is not written twice.** A generic that calls itself
  asks for its own type again on the round that reads its body — and the copy
  doing the asking is the one it is asking for. Recursion works, and produces one
  copy per type rather than a new one every round.

Sixty-four rounds is the ceiling. A file that has not settled by then is not a
file anybody wrote; it is expansion failing to settle, so it is reported in the
voice that says the fault is ours.

## `whichever`, built

**Built 2026-09-09**, for the ten kinds that name a type. `is loan` and
`is loanmut` are the same one piece of work as `any.loan`, and wait with it.

Decided while building, in one place:

- **The checker chooses.** By the time it reads a `whichever` the subject has a
  concrete type — a generic has already been written out once per type it was
  called with — so which arm is meant has one answer. It reads **only that arm**.
  That is the point of them: `is str` may call `count` on something that is a
  `str` only in the copy where it is one.
- **A prune pass erases the word.** The arm stands where the statement stood,
  and nothing after that meets a `whichever` — ownership, the middle layer and
  the backend needed to learn nothing about it. The same erasure a generic gets,
  and for the same reason.
- **A `whichever` is not a scope.** It is a choice about which lines are here at
  all, so the arm's statements are lifted out rather than kept as a block.

Two things the design had not settled, decided in the building:

**Nothing covering the type is refused** — `E0542`, `nothing here covers a
`bool``. It is settled while compiling against a type that is known, so an
uncovered one is not a case that might never come up: it is this program, now,
with nothing to do. Falling through silently is the opposite of how the rest of
this compiler behaves.

**A function answers when the arm that is here answers.** Asking every arm would
refuse the shape everybody writes — `show`, giving from all of them — because an
arm that is not here cannot be read and an arm that is here is the whole of it.

```
fn.str 'show' [any 'v'] {
    whichever 'v' {
        is number { give [convert-to-str['v']]; }
        is str    { give ['v']; }
        is bool   { give [str:*a bool*]; }
    }
}
```

`show['n'] show['d'] show['s'] show['b']` builds `show$int64`, `show$deci64`,
`show$str` and `show$bool` — four functions, each holding one arm and no trace
of the other two.

## Two questions, not one list

**Built 2026-09-09.** The design said one list of kind words, read both ways.
Building it found that the list is really **two**, and that they cannot be mixed
in one `whichever`:

```
struct 'holder' [loan.int64 'x']
```

`'h'.x` is a number. It is also a borrow. Both, at once — and the overlap rule
refuses two arms that could both answer. That rule was written for `number`
against `int`, where one sits inside the other and *pick a level* is a real
instruction. `number` against `loan` is not that: every type word overlaps both
borrow words, for every borrowed value, and there is no level to pick.

So a `whichever` asks **one question**:

```
whichever 'h'.x {          # what it is
    is number { … }        is str { … }        is many { … }
}

whichever 'h'.x {          # how it is held
    is owned { … }         is loan { … }       is loanmut { … }
}
```

An arm from each list in one statement is `E0543`. Both at once is written by
nesting, which costs a block and reaches everything.

**`owned` is the eleventh word**, and it has to exist: without it the second
question has no arm for a plain value, so every such `whichever` would end at
"nothing here covers it". It is not a chain word — owned is what a name is when
nothing says otherwise — and that is exactly why it is free to name a kind, the
same reason `int` is: *there is no `owned` on its own*.

Refusing the mix is the safe direction, by this document's own argument: going
from refused to allowed later breaks nothing written before it.

### How a thing is held is part of what a copy takes

`Ty` gained it. A `loan.int64` was an `int64` to the checker, and how it was held
was `Own.cpp`'s alone — so nothing could answer `is loan`.

It is never compared: a `loan.int64` is still an `int64` everywhere it was one
before, and only a program that asks can tell. But it **is** part of what a blank
was filled in with, so `howHeld['n']`, `howHeld[loan 'n']` and
`howHeld[loanmut 'n']` build three functions rather than one:

```
fn howHeld$int64          fn howHeld$loan.int64          fn howHeld$loanmut.int64
```

They have to be three. One copy would have to choose one arm and give the same
answer to all three callers.

`move` is the third word beside `loan` and `loanmut` and is not a borrow: it
hands the value over for good, so what comes out is held outright and asks for no
copy of its own. Marking it as one built `show$loan.str` and then refused the
`move` that had asked for it.

## `loop.parts`, built

**Built 2026-09-09.** The whole design, working at once:

```
fn.str 'show' [loan.any 'v'] {
    whichever 'v' {
        is number { give [convert-to-str['v']]; }
        is struct {
            var.mut.str 'out' = [str:*<*];
            loop.parts 'part' = ['v'] {
                set 'out' = ['out' 'part'.name str:*: * (show[loan 'part'.value]) str:* *];
            }
            give ['out' str:*>*];
        }
    }
}
```

`show[loan 'l']` on a `line` made of `point`s made of `int64`s:

```text
<from: <x: 1 y: 2 > to: <x: 3 y: 4 > >
```

`show` has never seen either struct. It walks what it is given, asks what each
field is, and calls itself when the answer is `struct`.

**It is not a loop, and it is not a scope.** The body is written out once per
field, and the copies stand where the statement stood — the same as a
`whichever`'s arm, and for the same reason: what a field holds is a different
type on every turn, so there is no one body to build and run several times.

**A turn is a real value with a real type**, as this document said it should be:
a struct of a `str` called `name` and what the field holds, called `value`,
declared at the top of its own copy of the body. So `'part'.name` and
`'part'.value` are ordinary field reads, and the turn can be handed about like
anything else:

```
fn.str 'label' [loan.any 'part'] {
    give ['part'.name str:*=* (convert-to-str['part'.value])];
}
```

`label` is a generic that has never seen `point`, handed a turn of one.

Three things it needed:

- **`value` is lent, not held.** The thing being walked keeps its places —
  taking one out would leave a hole, and there is no taking anything out of
  something borrowed in the first place.
- **Each turn names its own.** The turns stand side by side in one scope, so two
  of them both called `'part'` would be one name declared twice. Each is
  `part$0`, `part$1`, and every mention inside that turn is renamed with it.
- **The struct is written into the program**, one per field of each struct
  walked, called `part$point$x`. With a **`$`** between and never a dot: a type
  is spelled with dots, and a blank filled in with one of these is written back
  into a chain by splitting on them — `part$point.x` came back as a `part$point`
  holding an `x`.

Writing the two reads in directly was built first and does less: it cannot hand
the turn over, because there is nothing to hand.

`E0544` for walking anything but a struct.

### The rounds settle three things, not one

Expansion, arm-choosing and struct-walking all happen in the same rounds, and
all three had to. A `whichever` inside a generic is not reached until that
generic has been written out at a real type; a `loop.parts` inside the arm that
was chosen is not reached until that arm has been chosen.

Choosing had been left to the end, and that was wrong in a way only recursion
showed: **an arm nobody chose still holds calls.** `show`'s `is struct` arm calls
`show`, and a call nothing repoints keeps the generic it names standing — so
`show` kept itself alive round after round and stopped at the sixty-fourth, in
the voice that says the fault is ours. Which it was.
