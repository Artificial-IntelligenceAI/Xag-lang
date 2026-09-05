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
const char *kLook = "fn.i64 look [ref.str 'text'] { give [count['text']]; }\n";
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

void aTransferIsSpelled() {
  // Handing an owned `str` to a function that takes it needs the word.
  CHECK(withHelpers("var.str 's' = [*hi*];\n    keep['s'];").code(0) == "E0406");
  // And the word has to be the one the parameter asked for.
  CHECK(withHelpers("var.str 's' = [*hi*];\n    keep[ref 's'];").code(0) == "E0406");
  CHECK(withHelpers("var.str 's' = [*hi*];\n    look[move 's'];").code(0) == "E0406");
  CHECK(withHelpers("var.str 's' = [*hi*];\n    var.i64 'n' = [look[ref 's']];").ok());
}

void smallValuesAreNotMoved() {
  // An `i64` is handed over by being copied, so nothing is spelled.
  CHECK(run("fn.i64 twice [i64 'n'] { give ['n' + 'n']; }\n"
            "START { var.i64 'a' = [*2*]; var.i64 'b' = [twice['a']];"
            " print.stdout['a' \n]; }\n").ok());
  CHECK(run("fn.i64 twice [i64 'n'] { give ['n' + 'n']; }\n"
            "START { var.i64 'a' = [*2*]; var.i64 'b' = [twice[move 'a']]; }\n")
            .code(0) == "E0405");
}

void aBorrowIsNotYoursToGiveAway() {
  CHECK(run(std::string(kKeep) +
            "fn.nothing pass [ref.str 'text'] { keep[move 'text']; }\nSTART { }\n")
            .code(0) == "E0404");
  // Passing a loan along is not a transfer, so nothing is spelled again.
  CHECK(run(std::string(kLook) +
            "fn.i64 pass [ref.str 'text'] { give [look['text']]; }\nSTART { }\n").ok());
}

void aLoanIsLentBeforeItIsWritten() {
  CHECK(run(std::string(kEdit) +
            "fn.nothing pass [ref.str 'text'] { edit[refmut 'text']; }\nSTART { }\n")
            .code(0) == "E0407");
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
                              "    loop.range.i64 'i' = [*1*, *3*] {\n"
                              "        keep[move 's'];\n    }");
  CHECK(p.code(0) == "E0403");
}

void giveIsAlreadyTheWord() {
  CHECK(run("fn.i64 total [] { var.i64 'n' = [*1*]; give ['n']; }\nSTART { }\n").ok());
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

} // namespace

int main() {
  aMovedNameHoldsNothing();
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

  if (failures == 0)
    std::cout << "all ownership tests passed\n";
  return failures == 0 ? 0 : 1;
}
