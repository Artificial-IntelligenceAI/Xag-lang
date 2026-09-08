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

### Asking what kind of thing something is

Walking a `struct 'line' [point 'from', point 'to']`, a field is itself a struct,
and `convert-to-str` does not take one. So the writer can ask:

```
whichever 'part'.value {
    is number { ... }
    is text   { ... }
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

### The number families are asked about both ways

`is number` covers all twenty-odd of them, and `is int`, `is uint`, `is bin` and
`is deci` each cover one family. Both, because one branch usually suffices and
sometimes it does not — a whole number and a decimal are not handled alike.

`int` is free to mean this because it is deliberately not a type: *there is no
`int` on its own, because there is no size to assume*. It is not a type and it
is a family, which is exactly what a word here has to be.

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

## Open

- Which family words there are beyond the numbers: `text`, `struct`, `many`,
  and whether `bool`, `or-nothing` and a loan are asked about the same way.
- Whether the same words constrain a generic — `loan.many.any.int 'xs'` — so the
  vocabulary for *what a thing is* and for *what a generic asks of it* is one
  list, learned once.
- Whether constraints are wanted at all now that `whichever` exists. A generic that
  can ask what it was given can handle anything; a constraint says what it
  refuses in advance, and buys an error at the call rather than inside the body.
- Whether `loop.parts` also walks a `many`, where every element is the same type
  and an ordinary loop would do.
- What `'part'` is, exactly. It is reached like a struct, and no struct has a
  field whose type changes per turn.
- How a type parameter is spelled where it needs a name, and the collision that
  comes with it — `fn.loan.'life'.'held'` puts a loan name and a type name side
  by side, told apart by counting, which is a syntax nobody could read.
