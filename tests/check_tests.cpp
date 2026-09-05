#include "xag/Check.h"
#include "xag/Lexer.h"
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

struct Checked {
  xag::Source source;
  xag::LexResult lexed;
  xag::ParseResult parsed;
  xag::CheckResult checked;

  bool ok() const { return lexed.ok() && parsed.ok() && checked.ok(); }
  std::string code(unsigned i) const {
    return i < checked.diagnostics.size() ? checked.diagnostics[i].code : "(none)";
  }
};

Checked run(const std::string &text) {
  Checked c{xag::Source("test.xag", text), {}, {}, {}};
  c.lexed = xag::lex(c.source);
  c.parsed = xag::parse(c.source, c.lexed.tokens);
  c.checked = xag::check(c.source, c.parsed.program);
  return c;
}

Checked inStart(const std::string &body) { return run("START {\n" + body + "\n}\n"); }

void aNameMustBeDeclared() {
  CHECK(inStart("print.stdout['nope' \\n];").code(0) == "E0501");
  CHECK(inStart("var.i64 'n' = [*1*];\n    print.stdout['n' \\n];").ok());
}

void aNameIsDeclaredOnce() {
  CHECK(inStart("var.i64 'n' = [*1*];\n    var.i64 'n' = [*2*];").code(0) == "E0502");
}

void aTypeMustExist() {
  CHECK(inStart("var.i65 'n' = [*1*];").code(0) == "E0503");
}

void aWrittenValueSaysWhatItIs() {
  // A print states no parameter types, so each written value must say its own.
  CHECK(inStart("print.stdout[*Hello* \\n];").code(0) == "E0507");
  CHECK(inStart("print.stdout[str:*Hello* \\n];").ok());
  // A chain says it, so nothing is repeated.
  CHECK(inStart("var.str 's' = [*Hello*];").ok());
}

void aWrittenValueFitsItsType() {
  CHECK(inStart("var.i64 'n' = [*abc*];").code(0) == "E0509");
  CHECK(inStart("var.bool 'b' = [*maybe*];").code(0) == "E0509");
  CHECK(inStart("var.i64 'n' = [*-12*];").ok());
  CHECK(inStart("var.bool 'b' = [*true*];").ok());
}

void nothingConvertsOnItsOwn() {
  CHECK(inStart("var.str 's' = [*a*];\n    var.i64 'n' = ['s'];").code(0) == "E0506");
  CHECK(inStart("var.i64 'n' = [*1* + *2*];").ok());
  CHECK(inStart("var.str 's' = [*a*];\n    var.i64 'n' = ['s' + *1*];").code(0) == "E0506");
}

void piecesSideBySideJoin() {
  CHECK(inStart("var.str 'n' = [*Hello, * *world*];").ok());
  // Joining builds text, so an i64 cannot be one of the pieces.
  CHECK(inStart("var.i64 'n' = [*1*];\n    var.str 's' = [*x* 'n'];").code(0) == "E0506");
}

void anImmutableNameDoesNotChange() {
  CHECK(inStart("var.i64 'n' = [*1*];\n    set 'n' = [*2*];").code(0) == "E0508");
  CHECK(inStart("var.mut.i64 'n' = [*1*];\n    set 'n' = [*2*];").ok());
}

void aBorrowSaysWhetherItWrites() {
  CHECK(run("fn.nothing excite [refmut.str 'text'] { set 'text' = ['text' *!*]; }\n"
            "START { }\n").ok());
  // `ref` lends for reading, so writing through it is the same mistake as
  // writing to anything else that does not change.
  CHECK(run("fn.nothing excite [ref.str 'text'] { set 'text' = ['text' *!*]; }\n"
            "START { }\n").code(0) == "E0508");
}

void callsAreChecked() {
  CHECK(run("fn.i64 twice [i64 'n'] { give ['n' + 'n']; }\n"
            "START { var.i64 'a' = [twice[*2*]]; }\n").ok());
  CHECK(run("fn.i64 twice [i64 'n'] { give ['n' + 'n']; }\n"
            "START { var.i64 'a' = [twice[*2*, *3*]]; }\n").code(0) == "E0505");
  CHECK(run("fn.i64 twice [i64 'n'] { give ['n' + 'n']; }\n"
            "START { var.str 's' = [*x*]; var.i64 'a' = [twice['s']]; }\n").code(0) == "E0506");
  CHECK(inStart("nosuch[*1*];").code(0) == "E0504");
}

void signaturesAreReadBeforeBodies() {
  // Two functions may call each other, and a constant may be used above itself.
  CHECK(run("fn.i64 odd [i64 'n'] { give [even['n']]; }\n"
            "fn.i64 even [i64 'n'] { give ['n']; }\n"
            "START { var.i64 'a' = [odd[*3*]]; }\n").ok());
  CHECK(run("fn.i64 limit [] { give ['LIMIT']; }\n"
            "const.i64 'LIMIT' = [*10*];\n"
            "START { var.i64 'a' = [limit[]]; }\n").ok());
}

void giveAnswersItsFunction() {
  CHECK(inStart("give [*1*];").code(0) == "E0511");
  CHECK(run("fn.nothing quiet [] { give [*1*]; }\nSTART { }\n").code(0) == "E0511");
  CHECK(run("fn.str greet [] { give [*hi*]; }\nSTART { }\n").ok());
  CHECK(run("fn.str greet [] { give [*1*]; }\nSTART { }\n").ok()); // *1* is text under str
}

void breakNeedsALoop() {
  CHECK(inStart("break;").code(0) == "E0510");
  CHECK(inStart("loop.range.i64 'i' = [*1*, *3*] { break; }").ok());
}

void conditionsAskABool() {
  CHECK(inStart("var.i64 'n' = [*1*];\n    if 'n' { }").code(0) == "E0506");
  CHECK(inStart("var.i64 'n' = [*1*];\n    if 'n' > *0* { }").ok());
}

void theCounterIsInScopeOnlyInTheLoop() {
  CHECK(inStart("loop.range.i64 'i' = [*1*, *3*] { print.stdout['i' \\n]; }").ok());
  CHECK(inStart("loop.range.i64 'i' = [*1*, *3*] { }\n    print.stdout['i' \\n];")
            .code(0) == "E0501");
}

} // namespace

int main() {
  aNameMustBeDeclared();
  aNameIsDeclaredOnce();
  aTypeMustExist();
  aWrittenValueSaysWhatItIs();
  aWrittenValueFitsItsType();
  nothingConvertsOnItsOwn();
  piecesSideBySideJoin();
  anImmutableNameDoesNotChange();
  aBorrowSaysWhetherItWrites();
  callsAreChecked();
  signaturesAreReadBeforeBodies();
  giveAnswersItsFunction();
  breakNeedsALoop();
  conditionsAskABool();
  theCounterIsInScopeOnlyInTheLoop();

  if (failures == 0)
    std::cout << "all check tests passed\n";
  return failures == 0 ? 0 : 1;
}
