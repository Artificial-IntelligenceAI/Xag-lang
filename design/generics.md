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
kind 'part'.value {
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

**`kind`, not `when`.** They look alike and are not: `when` is a choice the
program makes while running, with both arms in it. `kind` is decided while
compiling and only the chosen arm survives into the program. A word appears
where there is a choice, and that is a real one.

## Open

- Which family words there are: `number`, `text`, `struct`, `many` at least —
  and whether `number` splits into `int`, `bin`, `deci`.
- Whether the same words constrain a generic — `loan.many.any.int 'xs'` — so the
  vocabulary for *what a thing is* and for *what a generic asks of it* is one
  list, learned once.
- Whether constraints are wanted at all now that `kind` exists. A generic that
  can ask what it was given can handle anything; a constraint says what it
  refuses in advance, and buys an error at the call rather than inside the body.
- Whether `loop.parts` also walks a `many`, where every element is the same type
  and an ordinary loop would do.
- What `'part'` is, exactly. It is reached like a struct, and no struct has a
  field whose type changes per turn.
- How a type parameter is spelled where it needs a name, and the collision that
  comes with it — `fn.loan.'life'.'held'` puts a loan name and a type name side
  by side, told apart by counting, which is a syntax nobody could read.
