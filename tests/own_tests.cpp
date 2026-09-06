#include "xag/Check.h"
#include "xag/Lexer.h"
#include "xag/Own.h"
#include "xag/Parser.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #cond "\n";            \
      ++failures;                                                                        \
    }                                                                                    \
  } while (false)

struct Owned {
  xag::Source source;
  xag::LexResult lexed;
  xag::ParseResult parsed;
  xag::CheckResult checked;
  xag::OwnResult owned;

  bool ok() const {
    return lexed.ok() && parsed.ok() && checked.ok() && owned.ok();
  }
  std::string code(unsigned i) const {
    return i < owned.diagnostics.size() ? owned.diagnostics[i].code : "(none)";
  }
  const xag::Diagnostic *find(const std::string &wanted) const {
    for (const xag::Diagnostic &d : owned.diagnostics)
      if (d.code == wanted)
        return &d;
    return nullptr;
  }
  bool reports(const std::string &wanted) const {
    for (const xag::Diagnostic &d : owned.diagnostics)
      if (d.code == wanted)
        return true;
    return false;
  }
};

Owned run(const std::string &text) {
  Owned o{xag::Source("test.xag", text), {}, {}, {}, {}};
  o.lexed = xag::lex(o.source);
  o.parsed = xag::parse(o.source, o.lexed.tokens);
  o.checked = xag::check(o.source, o.parsed.program);
  o.owned = xag::own(o.source, o.parsed.program);
  return o;
}

const char *kKeep = "fn.nothing keep [str 'text'] { print.stdout['text' \n]; }\n";
const char *kLook = "fn.int64 look [ref.str 'text'] { give [count['text']]; }\n";
const char *kEdit = "fn.nothing edit [refmut.str 'text'] { set 'text' = ['text' *!*]; }\n";

Owned withHelpers(const std::string &body) {
  return run(std::string(kKeep) + kLook + kEdit + "START {\n" + body + "\n}\n");
}

void aMovedNameHoldsNothing() {
  CHECK(withHelpers("var.str 's' = [*hi*];\n    keep[move 's'];").ok());
  const Owned twice = withHelpers("var.str 's' = [*hi*];\n"
                                  "    keep[move 's'];\n"
                                  "    keep[move 's'];");
  CHECK(twice.code(0) == "E0403");
  const Owned after = withHelpers("var.str 's' = [*hi*];\n"
                                  "    keep[move 's'];\n"
                                  "    print.stdout['s' \n];");
  CHECK(after.code(0) == "E0403");
}

void aMistakeWithTwoPlacesPointsAtBoth() {
  const Owned twice = withHelpers("var.str 's' = [*hi*];\n"
                                  "    keep[move 's'];\n"
                                  "    print.stdout['s' \n];");
  const xag::Diagnostic *moved = twice.find("E0403");
  CHECK(moved != nullptr);
  if (moved) {
    CHECK(moved->label == "used here");
    CHECK(moved->notes.size() == 1);
    // The second place is where it went, and it comes earlier in the file.
    CHECK(moved->notes[0].span.begin < moved->span.begin);
  }

  // The loan rule points at every parameter it counted.
  const Owned loan =
      run("fn.ref.str longer [ref.str 'a', ref.str 'b'] { give ['a']; }\nSTART { }\n");
  const xag::Diagnostic *named = loan.find("E0402");
  CHECK(named != nullptr);
  if (named)
    CHECK(named->notes.size() == 2);
}

void aTransferIsSpelled() {
  // Handing an owned `str` to a function that takes it needs the word.
  CHECK(withHelpers("var.str 's' = [*hi*];\n    keep['s'];").code(0) == "E0406");
  // And the word has to be the one the parameter asked for.
  CHECK(withHelpers("var.str 's' = [*hi*];\n    keep[ref 's'];").code(0) == "E0406");
  CHECK(withHelpers("var.str 's' = [*hi*];\n    look[move 's'];").code(0) == "E0406");
  CHECK(withHelpers("var.str 's' = [*hi*];\n    var.int64 'n' = [look[ref 's']];").ok());
}

void smallValuesAreNotMoved() {
  // An `int64` is handed over by being copied, so nothing is spelled.
  CHECK(run("fn.int64 twice [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.int64 'a' = [*2*]; var.int64 'b' = [twice['a']];"
            " print.stdout['a' \n]; }\n").ok());
  CHECK(run("fn.int64 twice [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.int64 'a' = [*2*]; var.int64 'b' = [twice[move 'a']]; }\n")
            .code(0) == "E0405");
}

void aBorrowIsNotYoursToGiveAway() {
  CHECK(run(std::string(kKeep) +
            "fn.nothing pass [ref.str 'text'] { keep[move 'text']; }\nSTART { }\n")
            .code(0) == "E0404");
  // Passing a loan along is not a transfer, so nothing is spelled again.
  CHECK(run(std::string(kLook) +
            "fn.int64 pass [ref.str 'text'] { give [look['text']]; }\nSTART { }\n").ok());
}

void aLoanIsLentBeforeItIsWritten() {
  CHECK(run(std::string(kEdit) +
            "fn.nothing pass [ref.str 'text'] { edit[refmut 'text']; }\nSTART { }\n")
            .code(0) == "E0407");
  // A name that does not change cannot be lent for writing either.
  CHECK(withHelpers("var.str 's' = [*hi*];\n    edit[refmut 's'];").code(0) == "E0407");
  CHECK(withHelpers("var.mut.str 's' = [*hi*];\n    edit[refmut 's'];").ok());
}

void aBorrowNeverOutlastsWhatItBorrowsFrom() {
  // One parameter is lent, so the loan is not in question — only the answer is.
  CHECK(run("fn.ref.str broken [ref.str 'other'] {"
            " var.str 'text' = [*hello*]; give ['text']; }\nSTART { }\n")
            .code(0) == "E0401");
  CHECK(run("fn.ref.str echo [ref.str 'text'] { give ['text']; }\nSTART { }\n").ok());

  // With nothing lent at all, the signature is wrong before the body is read.
  const Owned both = run("fn.ref.str broken [] {"
                         " var.str 'text' = [*hello*]; give ['text']; }\nSTART { }\n");
  CHECK(both.reports("E0402"));
  CHECK(both.reports("E0401"));
}

void aLoanIsNamedWhenThereIsAChoice() {
  // One borrowed parameter: only one loan the answer could be on.
  CHECK(run("fn.ref.str echo [ref.str 'a'] { give ['a']; }\nSTART { }\n").ok());
  // Two: the compiler does not get to choose.
  CHECK(run("fn.ref.str longer [ref.str 'a', ref.str 'b'] { give ['a']; }\nSTART { }\n")
            .code(0) == "E0402");
  CHECK(run("fn.ref.'life'.str longer [ref.'life'.str 'a', ref.'life'.str 'b']"
            " { give ['a']; }\nSTART { }\n").ok());
  // A loan nobody was lent on.
  CHECK(run("fn.ref.'life'.str odd [ref.str 'a'] { give ['a']; }\nSTART { }\n")
            .code(0) == "E0402");
  // Nothing lent at all.
  CHECK(run("fn.ref.str nothingLent [] { give [*x*]; }\nSTART { }\n").code(0) == "E0402");
}

void anArmThatMovesMovesForEveryArm() {
  const Owned p = withHelpers("var.str 's' = [*hi*];\n"
                              "    if *true* == *true* {\n        keep[move 's'];\n    }\n"
                              "    print.stdout['s' \n];");
  CHECK(p.code(0) == "E0403");
}

void aLoopWouldMoveItTwice() {
  const Owned p = withHelpers("var.str 's' = [*hi*];\n"
                              "    loop.range.int64 'i' = [*1*, *3*] {\n"
                              "        keep[move 's'];\n    }");
  CHECK(p.code(0) == "E0403");
}

void giveIsAlreadyTheWord() {
  CHECK(run("fn.int64 total [] { var.int64 'n' = [*1*]; give ['n']; }\nSTART { }\n").ok());
  CHECK(run("fn.str greet [] { var.str 's' = [*hi*]; give ['s']; }\nSTART { }\n").ok());
  // Spelling `move` there would be a second way to write one thing.
  CHECK(run("fn.str greet [] { var.str 's' = [*hi*]; give [move 's']; }\nSTART { }\n")
            .code(0) == "E0406");
  // What was given is gone, the same as anything else handed over.
  CHECK(run("fn.str greet [] { var.str 's' = [*hi*]; give ['s']; give ['s']; }\n"
            "START { }\n").code(0) == "E0403");
}

void joiningReadsItsPieces() {
  // Building a new piece of text reads what it is built from and takes nothing.
  CHECK(withHelpers("var.str 's' = [*hi*];\n"
                    "    var.str 'greeting' = [*say * 's' *!*];\n"
                    "    keep[move 's'];").ok());
}

void aManyIsHandedOverRatherThanCopied() {
  // Every place holds a value, so an array is never copied out from under
  // itself — even one whose places would each copy.
  CHECK(withHelpers("var.many.int64 'xs' = [*1* *2*];\n"
                    "    var.many.int64 'ys' = ['xs'];").code(0) == "E0406");
  CHECK(withHelpers("var.many.int64 'xs' = [*1* *2*];\n"
                    "    var.many.int64 'ys' = [move 'xs'];").ok());
  CHECK(withHelpers("var.many.int64 'xs' = [*1*];\n"
                    "    var.many.int64 'ys' = [move 'xs'];\n"
                    "    print.stdout['xs'[*0*] \\n];").code(0) == "E0403");
}

void everyPlaceIsFilledOnce() {
  // Items under a `many` are handed over, not read: two places cannot hold the
  // same text, and the same name cannot fill both.
  CHECK(withHelpers("var.str 's' = [*hi*];\n"
                    "    var.many.str 'ws' = [*a* 's'];").code(0) == "E0406");
  CHECK(withHelpers("var.str 's' = [*hi*];\n"
                    "    var.many.str 'ws' = [*a* move 's' *b* move 's'];").code(0) ==
        "E0403");
  CHECK(withHelpers("var.str 's' = [*hi*];\n"
                    "    var.many.str 'ws' = [*a* move 's'];").ok());
}

void nothingIsTakenOutOfAMany() {
  CHECK(withHelpers("var.many.str 'ws' = [*a* *b*];\n"
                    "    keep[move 'ws'[*0*]];").code(0) == "E0412");
  CHECK(withHelpers("var.many.str 'ws' = [*a* *b*];\n"
                    "    keep['ws'[*0*]];").code(0) == "E0412");
  // Reading one and lending one are both fine, because neither leaves a hole.
  CHECK(withHelpers("var.many.str 'ws' = [*a* *b*];\n"
                    "    print.stdout[(look[ref 'ws'[*0*]]) \\n];").ok());
}

void lendingAnElementLendsTheWholeArray() {
  // Which place `'ws'['i']` names is not known until the program runs, so the
  // loan is of the array and E0408 reads exactly as it always did.
  CHECK(withHelpers("var.many.str 'ws' = [*a* *b*];\n"
                    "    var.int64 'n' = [look[ref 'ws'[*0*]]];\n"
                    "    print.stdout['n' \\n];").ok());
  CHECK(withHelpers("var.many.str 'ws' = [*a*];\n"
                    "    keep[move 'ws'];\n"
                    "    print.stdout['ws'[*0*] \\n];").code(0) != "(none)");
}

void whatHoldsLendsIsNotYoursToGiveAway() {
  CHECK(withHelpers("var.or-nothing.str 's' = [*hi*];\n"
                    "    if 's' holds 'text' { keep[move 'text']; }").code(0) == "E0404");
  // Reading it and lending it are both fine.
  CHECK(withHelpers("var.or-nothing.str 's' = [*hi*];\n"
                    "    if 's' holds 'text' { print.stdout[(look[ref 'text']) \n]; }").ok());
}

} // namespace

int main() {
  aMovedNameHoldsNothing();
  aMistakeWithTwoPlacesPointsAtBoth();
  aTransferIsSpelled();
  smallValuesAreNotMoved();
  aBorrowIsNotYoursToGiveAway();
  aLoanIsLentBeforeItIsWritten();
  aBorrowNeverOutlastsWhatItBorrowsFrom();
  aLoanIsNamedWhenThereIsAChoice();
  anArmThatMovesMovesForEveryArm();
  aLoopWouldMoveItTwice();
  giveIsAlreadyTheWord();
  joiningReadsItsPieces();
  aManyIsHandedOverRatherThanCopied();
  everyPlaceIsFilledOnce();
  nothingIsTakenOutOfAMany();
  lendingAnElementLendsTheWholeArray();
  whatHoldsLendsIsNotYoursToGiveAway();

  if (failures == 0)
    std::cout << "all ownership tests passed\n";
  return failures == 0 ? 0 : 1;
}
