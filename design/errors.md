
## One mistake is one mistake

A typo in a type word used to produce two refusals: `` `in64` is not a type ``,
and then `nothing here says what this written value is` pointing at `*3*`, a
value that was fine. The second one came with a tip about `print.stdout` and
parameters — a road that leads nowhere, because the value was never the problem.

The cause was that `Type::Unknown` was **anonymous**. When `in64` failed, `'n'`
became unknown, and everything downstream met a type it could say nothing about
and could not tell whether that was the reader's doing or an earlier refusal's.
So it did one of two things, and both were wrong:

- **within a statement** it carried on and complained about a value that was
  fine;
- **across statements** it passed over the check in total silence, so a reader
  could not see how far one mistake had reached.

Now an unknown says where it came from. `Ty::from` is the span of the mistake
that made it unknowable, and it travels: a type that came out unknown because
its operand was unknown keeps the operand's span, so what a diagnostic finally
names is the mistake that **started** the chain rather than the last link in it.

A refusal that only happened because of an earlier one carries `follows`, and is
folded underneath the one it followed from:

```text
`in64` is not a type.

  4 |     var.in64 'n' = [*3*];
    |         ^^^^ here
    |                     ^^^ and because of that, nothing here says what this written value is
  5 |     var.str 's' = ['n'];
    |                    ^^^ and because of that, this value was not checked against a `str`
  6 |     var.int64 'a' = ['n' + *1*];
    |                      ^^^^^^^^^ and because of that, `+` was not checked here
  7 |     var.int64 'c' = [double['n']];
    |                             ^^^ and because of that, this was not checked against what `double` wants
  8 |     var.bool 'ok' = ['n'];
    |                      ^^^ and because of that, this value was not checked against a `bool`

Error code: E0503

1 error.
```

One thing to fix, and the whole of what it broke in one place. Three of those
five used to be said in total silence.

Two rules hold this together:

- **Nothing is hidden.** A consequence is shown, not dropped. Dropping it is
  what a compiler does when it is confident, and this one opens by saying it
  might be wrong.
- **A follow-on does not lecture.** The tip on `E0507` is right in general and
  wrong when the cause is known, so a traced one is given without it.

A follow-on whose root nobody printed is kept where it is rather than lost —
that happens when a pass stopped before the root was reached, and the only thing
said about it should not vanish.
