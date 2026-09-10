#include "xag/Ahead.h"
#include "xag/Interpret.h"
#include "xag/Loops.h"
#include "xag/Check.h"
#include "xag/Fold.h"
#include "as_file.h"

#include "xag/Lexer.h"
#include "xag/Mir.h"
#include "xag/Own.h"
#include "xag/Parser.h"
#include "xag_runtime.h"

#include <cstdio>
#include <iostream>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                        \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #cond "\n";            \
      ++failures;                                                                        \
    }                                                                                    \
  } while (false)

struct Settled {
  bool built = false;
  bool ran = false;
  bool compared = false;
  std::vector<xag::Diagnostic> held;  // what the bounds worked out
  std::vector<xag::Diagnostic> said;  // what stood after running
  std::string code(unsigned i) const {
    return i < said.size() ? said[i].code : "(none)";
  }
};

// A second engine that is not one: it runs the same interpreter, so it always
// agrees. What that tests is the plumbing — which answers are compared, and what
// is done with the outcome — not whether two real engines agree, which no fake
// can tell you.
//
// It has to actually run, rather than report nothing. A fake that always says
// "no sums came round" disagrees with the interpreter about every program that
// overflows, and a test then passes because the two disagreed rather than
// because the answer was right. One did.
xag::Building agrees() {
  return [](const xag::Mir &mir) {
    xag::Compiled out;
    out.asked = true;
    std::FILE *sink = std::tmpfile();
    xag_set_output(sink);
    const xag::InterpretResult said = xag::interpretWatching(mir);
    xag_set_output(nullptr);
    std::fflush(sink);
    std::rewind(sink);
    char buffer[4096];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), sink)) > 0)
      out.said.append(buffer, got);
    std::fclose(sink);
    out.ran = said.ran;
    out.cameRound = said.cameRound;
    out.wouldRead = said.wouldRead;
    if (!said.ran && said.theirFault) {
      out.stopped = true;
      out.why = said.trouble;
      out.stoppedAt = said.stoppedAt;
    }
    return out;
  };
}

xag::Building disagrees(const std::string &said) {
  return [said](const xag::Mir &) {
    xag::Compiled out;
    out.asked = true;
    out.ran = true;
    out.said = said;
    return out;
  };
}

xag::Building stops() {
  return [](const xag::Mir &) {
    xag::Compiled out;
    out.asked = true;
    out.trouble = "it stopped.";
    return out;
  };
}

Settled settle(const std::string &text, const xag::Building &building = {}) {
  Settled out;
  const xag::Source source("test.xag", xag::asFile(text));
  const xag::LexResult lexed = xag::lex(source);
  if (!lexed.ok())
    return out;
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (!parsed.ok())
    return out;
  const xag::CheckResult checked = xag::check(source, parsed.program);
  if (!checked.ok())
    return out;
  const xag::OwnResult owned = xag::own(source, parsed.program);
  if (!owned.ok())
    return out;
  xag::MirResult built = xag::build(source, parsed.program, checked);
  if (!built.ok())
    return out;
  xag::elaborate(built.mir);
  out.built = true;
  out.held = checked.aboutSums;

  const xag::AheadResult ahead =
      xag::ahead(source, built.mir, checked.aboutSums, checked.intoPlainNames,
                 building);
  out.ran = ahead.ran;
  out.compared = ahead.compared;
  out.said = ahead.diagnostics;
  return out;
}

// The one that started this. The bound reasons "a hundred trips, at most two
// each", reaches 200, and refuses an `int8`. The loop reaches 52.
void aBoundThatWasWrongIsDropped() {
  const Settled s = settle("START {\n"
                           "    var.mut.int8 'total' = [*0*];\n"
                           "    loop.range.int8 'i' = [*1*, *100*] {\n"
                           "        set 'total' = ['total' + 'i' / *50*];\n"
                           "    }\n}\n");
  CHECK(s.built);
  CHECK(s.held.size() == 1);
  CHECK(!s.held.empty() && s.held[0].code == "E0534");
  CHECK(s.ran);
  CHECK(s.said.empty());
}

// A bound that was right stands, and says the same thing it always said.
void aBoundThatWasRightStands() {
  const Settled s = settle("START {\n"
                           "    var.mut.int8 'sum' = [*0*];\n"
                           "    loop.range.int8 'i' = [*1*, *10*] {\n"
                           "        set 'sum' = ['sum' + *20*];\n"
                           "    }\n}\n");
  CHECK(s.ran);
  CHECK(s.said.size() == 1);
  CHECK(s.code(0) == "E0534");
}

// A warning is a bound giving up, and running is exactly what settles it.
void aWarningTheRunAnswersGoesAway() {
  const Settled s = settle("fn.int8 'twice' [int8 'n'] { give ['n' x *2*]; }\n"
                           "START {\n"
                           "    var.mut.int8 'sum' = [*0*];\n"
                           "    loop.range.int8 'i' = [*1*, *5*] {\n"
                           "        set 'sum' = ['sum' + twice['i']];\n"
                           "    }\n}\n");
  CHECK(s.held.size() == 1);
  CHECK(!s.held.empty() && s.held[0].code == "W0001");
  CHECK(s.ran);
  CHECK(s.said.empty());
}

// A program that reads cannot be run, and a loop inside it still can: what the
// loop is entered with is written down even where the program's input is not.
void aLoopAfterAReadStandsOnItsOwn() {
  const std::string reads =
      "START {\n"
      "    var.mut.int8 'total' = [*0*];\n"
      "    loop.while read.stdin[] holds 'line' { print.stdout['line' \\n]; }\n"
      "    loop.range.int8 'i' = [*1*, *100*] {\n"
      "        set 'total' = ['total' + 'i' / *50*];\n"
      "    }\n}\n";
  // The bound says at most 200 and refuses an `int8`. The loop reaches 52.
  CHECK(settle(reads).held.size() == 1);
  CHECK(settle(reads).code(0) == "E0534");   // one engine may not clear it
  CHECK(settle(reads, agrees()).said.empty()); // two may

  // And one that really does come round is still refused, read or no read.
  const std::string over =
      "START {\n"
      "    var.mut.int8 'total' = [*0*];\n"
      "    loop.while read.stdin[] holds 'line' { print.stdout['line' \\n]; }\n"
      "    loop.range.int8 'i' = [*1*, *10*] {\n"
      "        set 'total' = ['total' + *20*];\n"
      "    }\n}\n";
  CHECK(settle(over, agrees()).said.size() == 1);
  CHECK(settle(over, agrees()).code(0) == "E0534");
}

// A program that reads is run as far as the read, and no further: what it does
// on what it was given is not what it does on nothing. The loop here is before
// the read, so it happened.
void aProgramThatReadsIsRunUpToTheRead() {
  const Settled s = settle("fn.int8 'twice' [int8 'n'] { give ['n' x *2*]; }\n"
                           "START {\n"
                           "    var.mut.int8 'sum' = [*0*];\n"
                           "    loop.range.int8 'i' = [*1*, *5*] {\n"
                           "        set 'sum' = ['sum' + twice['i']];\n"
                           "    }\n"
                           "    loop.while read.stdin[] holds 'line' {\n"
                           "        print.stdout['line' \\n];\n"
                           "    }\n}\n");
  CHECK(s.ran);
  CHECK(s.said.empty());
}

// What a program was given is not a fact about the program, so looking at it is
// as far as a run at compile time goes — the same place a read stops.
//
// Nothing said so until 2026-09-10: `arguments[]` answered with whatever the
// compiler was holding, so this program, which prints its first argument, was
// refused for reaching into a `many` that holds none.
void aProgramThatLooksAtWhatItWasGivenIsRunUpToThat() {
  const std::string first = "START {\n"
                            "    var.many.str 'given' = [arguments[]];\n"
                            "    print.stdout['given'[*0*] \\n];\n}\n";
  const Settled s = settle(first, agrees());
  CHECK(s.ran);
  CHECK(s.said.empty());
  // And one engine on its own says nothing about it either.
  CHECK(settle(first).said.empty());
}

// A run that stopped saw only part of the program, so it vouches for none of
// it. This one never finishes, and the engine gives up counting.
void aRunThatStoppedChangesNothing() {
  const Settled s = settle("START {\n"
                           "    var.mut.int8 'sum' = [*0*];\n"
                           "    loop.range.int8 'i' = [*1*, *5*] {\n"
                           "        set 'sum' = ['sum' + 'i'];\n"
                           "    }\n"
                           "    var.mut.int64 'n' = [*0*];\n"
                           "    loop.while 'n' >== *0* {\n"
                           "        set 'n' = ['n' + *1*];\n"
                           "    }\n}\n");
  CHECK(s.built);
  CHECK(!s.ran);
  CHECK(s.said.size() == s.held.size());
}

// Nothing to settle means nothing to run: a program the bounds said nothing
// about is not run at all.
void aProgramWithNothingHeldIsNotRun() {
  const Settled s = settle("START {\n"
                           "    var.mut.int64 'n' = [*0*];\n"
                           "    set 'n' = ['n' + *1*];\n}\n");
  CHECK(s.built);
  CHECK(s.held.empty());
  CHECK(!s.ran);
  CHECK(s.said.empty());
}





// Two answers, so no answer. Nothing about the reader's code is reported —
// everything said about a program the compiler cannot agree with itself about
// is worth nothing until that is fixed.
void twoAnswersIsOurMistake() {
  const Settled s = settle("START {\n"
                           "    var.mut.int8 'sum' = [*0*];\n"
                           "    loop.range.int8 'i' = [*1*, *10*] {\n"
                           "        set 'sum' = ['sum' + *20*];\n"
                           "    }\n}\n",
                           disagrees("something else entirely\n"));
  CHECK(s.ran);
  CHECK(!s.compared);
  CHECK(s.said.size() == 1);
  CHECK(s.code(0) == "");
  CHECK(!s.said.empty() && s.said[0].severity == xag::Severity::Mine);
  // It stops, the way a refusal stops, without being one.
  CHECK(xag::anyErrors(s.said));
}

// A built program that did not finish is a disagreement too: reading it, the
// program ran to the end.
void aBuiltProgramThatStoppedIsOurMistakeAsWell() {
  const Settled s = settle("START {\n"
                           "    var.mut.int8 'sum' = [*0*];\n"
                           "    loop.range.int8 'i' = [*1*, *10*] {\n"
                           "        set 'sum' = ['sum' + *20*];\n"
                           "    }\n}\n",
                           stops());
  CHECK(!s.compared);
  CHECK(s.said.size() == 1);
  CHECK(!s.said.empty() && s.said[0].severity == xag::Severity::Mine);
}


// A program that stops is something both engines can see happen, and knowing it
// before the program is run is the whole point of running it.
void aProgramThatStopsIsSaidSo() {
  // The divisor is worked out in a loop, so the fold cannot see it and this
  // reaches the run.
  const std::string zero = "START {\n"
                           "    var.mut.int64 'd' = [*5*];\n"
                           "    loop.range.int64 'i' = [*1*, *5*] {\n"
                           "        set 'd' = ['d' - *1*];\n"
                           "    }\n"
                           "    var.int64 'n' = [*10* / 'd'];\n}\n";
  CHECK(settle(zero, agrees()).code(0) == "E0538");
  // One engine may not refuse anybody's program, here as anywhere.
  CHECK(settle(zero).said.empty());

  // Reaching past the end of a `many`, which is a stop today and knowable now.
  const std::string past = "START {\n"
                           "    var.many.int64 'xs' = [*5* *9* *2*];\n"
                           "    var.mut.int64 'best' = ['xs'[*0*]];\n"
                           "    loop.range.int64 'i' = [*1*, *3*] {\n"
                           "        if 'xs'['i'] > 'best' { set 'best' = ['xs'['i']]; }\n"
                           "    }\n}\n";
  CHECK(settle(past, agrees()).code(0) == "E0538");
  // The same program that stays inside is left alone.
  CHECK(settle("START {\n"
               "    var.many.int64 'xs' = [*5* *9* *2*];\n"
               "    var.mut.int64 'best' = ['xs'[*0*]];\n"
               "    loop.range.int64 'i' = [*1*, *2*] {\n"
               "        if 'xs'['i'] > 'best' { set 'best' = ['xs'['i']]; }\n"
               "    }\n}\n",
               agrees())
            .said.empty());

  // Stopping in one and not the other is ours, not theirs.
  const Settled apart = settle(zero, disagrees(""));
  CHECK(apart.said.size() == 1);
  CHECK(!apart.said.empty() && apart.said[0].severity == xag::Severity::Mine);
}

// A loop the compiler could run is a loop whose answer it knows, and one LLVM
// cannot see through is one worth writing the answer into.
void aLoopWithAnAnswerIsWrittenAsItsAnswer() {
  const std::string branching =
      "START {\n"
      "    var.mut.int64 'total' = [*0*];\n"
      "    loop.range.int64 'i' = [*1*, *1000*] {\n"
      "        if ('i' mod *7*) == *0* { set 'total' = ['total' + 'i' x 'i']; }\n"
      "        else { set 'total' = ['total' - *3*]; }\n"
      "    }\n"
      "    print.stdout[str:*total = * 'total' \\n];\n}\n";
  const xag::Source source("test.xag", xag::asFile(branching));
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  CHECK(checked.ok());
  const xag::OwnResult owned = xag::own(source, parsed.program);
  CHECK(owned.ok());
  xag::MirResult built = xag::build(source, parsed.program, checked);
  xag::elaborate(built.mir);

  CHECK(xag::writeInWhatTheLoopsAnswer(built.mir) == 1);

  // The answer is written where the loop was, and the block everything used to
  // jump back to now goes forward. The loop's own blocks are still there and
  // nothing reaches them, which is LLVM's to tidy rather than this pass's.
  bool holdsTheAnswer = false;
  bool goesForward = false;
  for (const xag::Body &body : built.mir.bodies)
    for (const xag::BasicBlock &block : body.blocks) {
      bool here = false;
      for (const xag::Statement &s : block.statements)
        for (const xag::Operand &one : s.value.operands)
          if (one.kind == xag::OperandKind::Written && one.written == "47259641")
            here = true;
      if (!here)
        continue;
      holdsTheAnswer = true;
      goesForward = block.terminator.kind == xag::TerminatorKind::Goto &&
                    block.terminator.targets.size() == 1 &&
                    block.terminator.targets[0] > block.id;
    }
  CHECK(holdsTheAnswer);
  CHECK(goesForward);

  // Asked twice, it answers the same loop the same way rather than folding what
  // it already folded into something else.
  CHECK(xag::writeInWhatTheLoopsAnswer(built.mir) == 0);
}


// A read is as far as a run goes, and what happened before it still happened.
// This loop cannot be lifted — it calls out — so running the program up to the
// read is the only thing that could ever answer for it.
void aLoopBeforeAReadIsAnswered() {
  const std::string before =
      "fn.int8 'twice' [int8 'n'] { give ['n' x *2*]; }\n"
      "START {\n"
      "    var.mut.int8 'sum' = [*0*];\n"
      "    loop.range.int8 'i' = [*1*, *5*] {\n"
      "        set 'sum' = ['sum' + twice['i']];\n"
      "    }\n"
      "    loop.while read.stdin[] holds 'line' { print.stdout['line' \\n]; }\n}\n";
  CHECK(settle(before).held.size() == 1);
  CHECK(!settle(before).held.empty() && settle(before).held[0].code == "W0001");
  // Dropping a bound needs one engine; only standing one up needs two.
  CHECK(settle(before).said.empty());
  CHECK(settle(before, agrees()).said.empty());

  // The same loop after the read is one the run never gets to, and one nothing
  // else can answer either — so it stands.
  const std::string after =
      "fn.int8 'twice' [int8 'n'] { give ['n' x *2*]; }\n"
      "START {\n"
      "    loop.while read.stdin[] holds 'line' { print.stdout['line' \\n]; }\n"
      "    var.mut.int8 'sum' = [*0*];\n"
      "    loop.range.int8 'i' = [*1*, *5*] {\n"
      "        set 'sum' = ['sum' + twice['i']];\n"
      "    }\n}\n";
  CHECK(settle(after, agrees()).said.size() == 1);
  CHECK(settle(after, agrees()).code(0) == "W0001");
}


// A loop walking a `many` was the wall: it could not be taken out, because
// everything it touched had to be a plain number. What actually mattered was
// never the type — it was whether the value could be made again out of what is
// written in it. A `many` of written numbers can, and the loop then owns a copy
// of its own and shares nothing with the program it came from.
void aLoopOverAWrittenManyStandsAlone() {
  const std::string walking =
      "START {\n"
      "    loop.while read.stdin[] holds 'line' { print.stdout['line' \\n]; }\n"
      "    var.many.int8 'xs' = [*10* *20* *30* *40*];\n"
      "    var.mut.int8 'sum' = [*0*];\n"
      "    loop.range.int64 'i' = [*0*, *3*] {\n"
      "        set 'sum' = ['sum' + 'xs'['i']];\n"
      "    }\n}\n";
  // The bounds cannot follow `'xs'['i']`, and the loop is past a read, so
  // nothing but lifting it could ever answer.
  CHECK(settle(walking).held.size() == 1);
  CHECK(!settle(walking).held.empty() && settle(walking).held[0].code == "W0001");
  CHECK(settle(walking, agrees()).said.empty());

  // A `many` that came from somewhere rather than being written down cannot be
  // made again, so that loop still cannot be taken out.
  const std::string filled =
      "START {\n"
      "    loop.while read.stdin[] holds 'line' { print.stdout['line' \\n]; }\n"
      "    var.many.int8 'xs' = [fill[*10*, *4*]];\n"
      "    var.mut.int8 'sum' = [*0*];\n"
      "    loop.range.int64 'i' = [*0*, *3*] {\n"
      "        set 'sum' = ['sum' + 'xs'['i']];\n"
      "    }\n}\n";
  CHECK(settle(filled, agrees()).said.empty()); // `fill` is written down too
}

// A loop that writes into a `many` changes something no written answer can say,
// so it is not written away. Counting only whole assignments as changes would
// have folded it into an array that was never filled in.
void aLoopThatFillsAnArrayIsNotWrittenAway() {
  const std::string filling =
      "START {\n"
      "    var.mut.many.int64 'xs' = [*0* *0* *0* *0*];\n"
      "    loop.range.int64 'i' = [*0*, *3*] {\n"
      "        set 'xs'['i'] = ['i' x *11*];\n"
      "    }\n"
      "    print.stdout[str:*b = * 'xs'[*3*] \\n];\n}\n";
  const xag::Source source("test.xag", xag::asFile(filling));
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  CHECK(checked.ok());
  xag::MirResult built = xag::build(source, parsed.program, checked);
  xag::elaborate(built.mir);
  CHECK(xag::writeInWhatTheLoopsAnswer(built.mir) == 0);
}


// A name is changed by more than being assigned to, and both of the other two
// ways were invisible. The oracle found the first; the second was the same
// mistake sitting next to it.
void aNameIsChangedByMoreThanBeingAssignedTo() {
  // Filling places of a `many` before the loop is a `Store`, not an assignment.
  // Counting only assignments, the loop was taken out with the array as it was
  // first written — and the compiler folded the loop into a sum of zeroes while
  // both interpreters said 15.
  const std::string stored =
      "START {\n"
      "    var.mut.many.int64 'xs' = [*0* *0* *0*];\n"
      "    set 'xs'[*0*] = [*7*];\n"
      "    set 'xs'[*1*] = [*8*];\n"
      "    var.mut.int64 'sum' = [*0*];\n"
      "    loop.range.int64 'i' = [*0*, *2*] {\n"
      "        set 'sum' = ['sum' + 'xs'['i']];\n"
      "    }\n}\n";
  const auto mirOf = [](const std::string &text) {
    const xag::Source source("test.xag", xag::asFile(text));
    const xag::LexResult lexed = xag::lex(source);
    const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
    const xag::CheckResult checked = xag::check(source, parsed.program);
    xag::MirResult built = xag::build(source, parsed.program, checked);
    xag::elaborate(built.mir);
    return built;
  };
  xag::MirResult one = mirOf(stored);
  CHECK(xag::writeInWhatTheLoopsAnswer(one.mir) == 0);

  // Writing through a loan changes what the loan points at, and the name it
  // points at is never assigned to. Folding the loop threw the writes away.
  const std::string through =
      "START {\n"
      "    var.mut.int64 'n' = [*0*];\n"
      "    var.mut.int64 'k' = [*0*];\n"
      "    loop.range.int64 'i' = [*1*, *5*] {\n"
      "        var.loanmut.int64 'p' = [loanmut 'n'];\n"
      "        set 'p' = ['p' + *1*];\n"
      "        set 'k' = ['k' + *1*];\n"
      "    }\n}\n";
  xag::MirResult two = mirOf(through);
  CHECK(xag::writeInWhatTheLoopsAnswer(two.mir) == 0);

  // And one with nothing hidden in it still folds, or the fix would just be a
  // way of never folding anything.
  xag::MirResult three = mirOf("START {\n"
                               "    var.mut.int64 'total' = [*0*];\n"
                               "    loop.range.int64 'i' = [*1*, *1000*] {\n"
                               "        if ('i' mod *7*) == *0* {\n"
                               "            set 'total' = ['total' + 'i' x 'i'];\n"
                               "        } else { set 'total' = ['total' - *3*]; }\n"
                               "    }\n"
                               "    print.stdout[str:*t = * 'total' \\n];\n}\n");
  CHECK(xag::writeInWhatTheLoopsAnswer(three.mir) == 1);
}


} // namespace

int main() {
  aBoundThatWasWrongIsDropped();
  aBoundThatWasRightStands();
  aWarningTheRunAnswersGoesAway();
  aLoopAfterAReadStandsOnItsOwn();
  aLoopBeforeAReadIsAnswered();
  aLoopOverAWrittenManyStandsAlone();
  aLoopThatFillsAnArrayIsNotWrittenAway();
  aNameIsChangedByMoreThanBeingAssignedTo();
  aProgramThatReadsIsRunUpToTheRead();
  aProgramThatLooksAtWhatItWasGivenIsRunUpToThat();
  aProgramThatStopsIsSaidSo();
  aLoopWithAnAnswerIsWrittenAsItsAnswer();
  aRunThatStoppedChangesNothing();
  aProgramWithNothingHeldIsNotRun();
  twoAnswersIsOurMistake();
  aBuiltProgramThatStoppedIsOurMistakeAsWell();

  if (failures == 0)
    std::cout << "all ahead tests passed\n";
  return failures == 0 ? 0 : 1;
}
