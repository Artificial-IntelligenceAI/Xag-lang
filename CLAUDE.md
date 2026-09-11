# Working in this repository

## Commit messages

**Subjects are plain and searchable.** Say the kind of change and what it
touched, in words somebody hunting for it would actually type into a search:

    Fix: a struct holding one struct lost a level in lowering

Not an aphorism. Subjects here used to read like design notes — "A
`many-growing` is several, and is handed over", "A `one-of` is the room, and the
number just past it" — and three things were wrong with that. Stating the fixed
state as though it were timeless means nothing says a thing was ever broken; the
first of those was a miscompile. It is not findable by anyone who knows the
symptom rather than the metaphor, and `git log --grep=layout` is exactly the
search a person bisecting a layout regression would run. And joining two things
with "and" put a correctness fix and a size optimisation under one line.

An aphorism reads well in a list, which is where a subject is read least, and
badly in `git log --oneline` during a hunt, which is where it is read most.
Decided 2026-09-10; `028ea5b` marks the turnover, and commits before it were
left alone.

**Bodies stay long.** Write what broke, the symptom it showed, the reasoning,
what was tried and rejected, and the numbers from whatever verified it. That is
the part worth keeping, and it is the part most commit messages do not have.

## Before a commit

Three things, in this order:

    cmake --build build -j8
    ctest --test-dir build
    ./generator/target/release/xag-oracle --xagc "$PWD/build/xagc" \
        --cases 300 --jobs 8 --seed <a fresh one> --keep-going

The suite has to be green and the oracle has to report no disagreements. Say the
case count and the agreement count in the commit body.

**One oracle run at a time, and nothing is rebuilt while one is running** — it is
testing the binary that is on disk, and replacing that binary underneath it makes
the result mean nothing. Start it in the background and then *wait*: the harness
notifies when it finishes. Do not poll for it.

The nine programs in `examples/` are the other check, and they must answer the
same both ways:

    for f in examples/*.xag; do
      diff <(./build/xagc run "$f" </dev/null) \
           <(./build/xagc fast "$f" </dev/null) || echo "DIFF $f"
    done

`reading.xag` reads standard input, so **redirect it from `/dev/null`**. Without
that the loop waits for a line that never comes — and in a backgrounded shell it
waits forever, which looks like a hung build rather than a program doing exactly
what it was asked.

## The bug this compiler keeps having

Almost every real bug found here has been one shape: **something asked what kind
of thing it had, and got its answer from a list written before that kind
existed.** A `many-growing` that was not recognised as several. A case type
looked up in the table of structs. A `many` of a sum that lost which sum. A lone
struct taken for the whole struct because the question asked was "is it a
struct?" rather than "is it *this* struct?".

The typed tree (`include/xag/Typed.h`) exists to make that impossible: the
checker's answer is carried on the node, and `Own.cpp` and `MirBuild.cpp` read it
rather than working it out again. When adding a pass, read the type off the tree.
If you find yourself matching a name against a list of fields, or spelling a type
out to text to read it back, that is the bug about to happen again.
