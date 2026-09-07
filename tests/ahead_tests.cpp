#include "xag/Ahead.h"
#include "xag/Check.h"
#include "xag/Fold.h"
#include "xag/Lexer.h"
#include "xag/Mir.h"
#include "xag/Own.h"
#include "xag/Parser.h"

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

// A second engine that is not one: it says what the test wants it to say, so
// what is tested here is what `ahead` does with two answers rather than whether
// the backend produces the right one. Every program below prints nothing, so
// agreeing means saying nothing.
xag::Building agrees() {
  return [](const xag::Mir &) {
    xag::Compiled out;
    out.asked = true;
    out.ran = true;
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
  const xag::Source source("test.xag", text);
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
      xag::ahead(source, built.mir, checked.aboutSums, building);
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

// Nothing is run when what it would do depends on what it is given.
void aProgramThatReadsIsNotRun() {
  const Settled s = settle("fn.int8 'twice' [int8 'n'] { give ['n' x *2*]; }\n"
                           "START {\n"
                           "    var.mut.int8 'sum' = [*0*];\n"
                           "    loop.range.int8 'i' = [*1*, *5*] {\n"
                           "        set 'sum' = ['sum' + twice['i']];\n"
                           "    }\n"
                           "    loop.while read.stdin[] holds 'line' {\n"
                           "        print.stdout['line' \\n];\n"
                           "    }\n}\n");
  CHECK(!s.ran);
  CHECK(s.said.size() == 1);
  CHECK(s.code(0) == "W0001");
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

} // namespace

int main() {
  aBoundThatWasWrongIsDropped();
  aBoundThatWasRightStands();
  aWarningTheRunAnswersGoesAway();
  aProgramThatReadsIsNotRun();
  aRunThatStoppedChangesNothing();
  aProgramWithNothingHeldIsNotRun();
  twoAnswersIsOurMistake();
  aBuiltProgramThatStoppedIsOurMistakeAsWell();

  if (failures == 0)
    std::cout << "all ahead tests passed\n";
  return failures == 0 ? 0 : 1;
}
