#pragma once

#include "xag/Ast.h"
#include "xag/Check.h"

namespace xag {

// Writes every generic out once per type it is called with, and points each
// call at the one it meant.
//
// After this there are no blanks left anywhere: the generic itself is gone,
// replaced by its copies, and everything downstream — ownership, the middle
// layer, the backend — reads ordinary functions and needs to know nothing about
// any of this. That is the whole point of doing it here and not there. Whether
// a thing copies or is handed over is a question with an answer again.
//
// A copy is named `<what>$<type>`, which no program can collide with: `$` is not
// a word character, so nothing a reader writes can be spelled that way.
//
// Answers whether anything changed. Nothing does when a file has no generic in
// it, which is most of them, and then the program is left where it was.
// The program is written into: every call to a generic is pointed at its copy
// before anything is copied, because what the checker worked out is keyed by the
// nodes it walked, and a clone's nodes are not those. Repointing the copies
// instead found nothing and left every call naming a function that was no longer
// there.
bool expand(Program &program, const CheckResult &checked, Program &out);

// Replaces every `whichever` with the arm that was chosen, so that nothing after
// this meets the word — the same erasure a generic gets, and for the same
// reason: the arms nobody chose are written against types this copy does not
// have, and reading them would refuse a program that is correct.
//
// The program is written into rather than copied, because what the checker
// worked out is keyed by the statements it walked and a copy's statements are
// not those. Answers how many were replaced.
//
// A `whichever` the checker never reached is left standing. That is a statement
// inside an arm nobody chose, or in a generic body nobody called, and it is
// carried out with the arm it sits in.
unsigned prune(Program &program, const CheckResult &checked);

} // namespace xag
