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
  CHECK(inStart("var.int64 'n' = [*1*];\n    print.stdout['n' \\n];").ok());
}

void aNameIsDeclaredOnce() {
  CHECK(inStart("var.int64 'n' = [*1*];\n    var.int64 'n' = [*2*];").code(0) == "E0502");
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
  CHECK(inStart("var.int64 'n' = [*abc*];").code(0) == "E0509");
  CHECK(inStart("var.bool 'b' = [*maybe*];").code(0) == "E0509");
  CHECK(inStart("var.int64 'n' = [*-12*];").ok());
  CHECK(inStart("var.bool 'b' = [*true*];").ok());
}

void aSizeIsAlwaysWritten() {
  // There is no `int` on its own, because there is no size to assume.
  CHECK(inStart("var.int 'n' = [*1*];").code(0) == "E0503");
  CHECK(inStart("var.uint 'n' = [*1*];").code(0) == "E0503");
  for (const char *type : {"int8", "int16", "int32", "int64", "int128", "uint8",
                           "uint16", "uint32", "uint64", "uint128"})
    CHECK(inStart(std::string("var.") + type + " 'n' = [*1*];").ok());
}

void aWrittenNumberHasToFit() {
  CHECK(inStart("var.int8 'n' = [*127*];").ok());
  CHECK(inStart("var.int8 'n' = [*128*];").code(0) == "E0509");
  CHECK(inStart("var.int8 'n' = [*-128*];").ok());
  CHECK(inStart("var.int8 'n' = [*-129*];").code(0) == "E0509");
  CHECK(inStart("var.uint8 'n' = [*255*];").ok());
  CHECK(inStart("var.uint8 'n' = [*256*];").code(0) == "E0509");
  // Unsigned holds nothing below zero, whatever its width.
  CHECK(inStart("var.uint64 'n' = [*-1*];").code(0) == "E0509");
  CHECK(inStart("var.uint128 'n' = [*340282366920938463463374607431768211455*];").ok());
}

void binaryHoldsWhatIEEESaysItHolds() {
  for (const char *type : {"bin16", "bin32", "bin64"})
    CHECK(inStart(std::string("var.") + type + " 'n' = [*1.5*];").ok());
  CHECK(inStart("var.bin64 'n' = [*-0.25*];").ok());
  CHECK(inStart("var.bin64 'n' = [*1e300*];").ok());
  CHECK(inStart("var.bin64 'n' = [*3*];").ok()); // a whole number is a fine `bin`
  CHECK(inStart("var.bin64 'n' = [*abc*];").code(0) == "E0509");
  // A value that would arrive as infinity was not the value written down.
  CHECK(inStart("var.bin64 'n' = [*1e400*];").code(0) == "E0509");
  CHECK(inStart("var.bin32 'n' = [*1e300*];").code(0) == "E0509");
  // What a print writes, a program may write back.
  CHECK(inStart("var.bin64 'n' = [*infinity*];").ok());
  CHECK(inStart("var.bin64 'n' = [*not-a-number*];").ok());
  // And a `bin` is not a whole number, here as anywhere.
  CHECK(inStart("var.bin64 'a' = [*1.5*];\n    var.int64 'b' = ['a' + *1*];").code(0) ==
        "E0506");
}

void aTypeWithNothingBehindItSaysSo() {
  CHECK(inStart("var.bin128 'n' = [*1*];").ok()); // written out in software now
  CHECK(inStart("var.bin128 'n' = [*1e4000*];").ok());
  CHECK(inStart("var.bin128 'n' = [*abc*];").code(0) == "E0509");
  CHECK(inStart("var.deci64 'n' = [*1*];").code(0) == "E0512");
  CHECK(inStart("var.deci32 'n' = [*1*];").code(0) == "E0512");
}

void sizesDoNotMixOnTheirOwn() {
  CHECK(inStart("var.int32 'a' = [*1*];\n    var.int64 'b' = ['a' + *1*];").code(0) ==
        "E0506");
  CHECK(inStart("var.int64 'a' = [*1*];\n    var.uint64 'b' = ['a' + *1*];").code(0) ==
        "E0506");
  CHECK(inStart("var.int32 'a' = [*1*];\n    var.int32 'b' = ['a' + *1*];").ok());
  // A comparison takes its type from its left side, so the right side fits it.
  CHECK(inStart("var.uint8 'a' = [*200*];\n    var.bool 'b' = ['a' > *100*];").ok());
  CHECK(inStart("var.uint8 'a' = [*200*];\n    var.bool 'b' = ['a' > *300*];").code(0) ==
        "E0509");
}

void nothingConvertsOnItsOwn() {
  CHECK(inStart("var.str 's' = [*a*];\n    var.int64 'n' = ['s'];").code(0) == "E0506");
  CHECK(inStart("var.int64 'n' = [*1* + *2*];").ok());
  CHECK(inStart("var.str 's' = [*a*];\n    var.int64 'n' = ['s' + *1*];").code(0) == "E0506");
}

void piecesSideBySideJoin() {
  CHECK(inStart("var.str 'n' = [*Hello, * *world*];").ok());
  // Joining builds text, so an int64 cannot be one of the pieces.
  CHECK(inStart("var.int64 'n' = [*1*];\n    var.str 's' = [*x* 'n'];").code(0) == "E0506");
}

void anImmutableNameDoesNotChange() {
  CHECK(inStart("var.int64 'n' = [*1*];\n    set 'n' = [*2*];").code(0) == "E0508");
  CHECK(inStart("var.mut.int64 'n' = [*1*];\n    set 'n' = [*2*];").ok());
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
  CHECK(run("fn.int64 twice [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.int64 'a' = [twice[*2*]]; }\n").ok());
  CHECK(run("fn.int64 twice [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.int64 'a' = [twice[*2*, *3*]]; }\n").code(0) == "E0505");
  CHECK(run("fn.int64 twice [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.str 's' = [*x*]; var.int64 'a' = [twice['s']]; }\n").code(0) == "E0506");
  CHECK(inStart("nosuch[*1*];").code(0) == "E0504");
}

void signaturesAreReadBeforeBodies() {
  // Two functions may call each other, and a constant may be used above itself.
  CHECK(run("fn.int64 odd [int64 'n'] { give [even['n']]; }\n"
            "fn.int64 even [int64 'n'] { give ['n']; }\n"
            "START { var.int64 'a' = [odd[*3*]]; }\n").ok());
  CHECK(run("fn.int64 limit [] { give ['LIMIT']; }\n"
            "const.int64 'LIMIT' = [*10*];\n"
            "START { var.int64 'a' = [limit[]]; }\n").ok());
}

void giveAnswersItsFunction() {
  CHECK(inStart("give [*1*];").code(0) == "E0511");
  CHECK(run("fn.nothing quiet [] { give [*1*]; }\nSTART { }\n").code(0) == "E0511");
  CHECK(run("fn.str greet [] { give [*hi*]; }\nSTART { }\n").ok());
  CHECK(run("fn.str greet [] { give [*1*]; }\nSTART { }\n").ok()); // *1* is text under str
}

void breakNeedsALoop() {
  CHECK(inStart("break;").code(0) == "E0510");
  CHECK(inStart("loop.range.int64 'i' = [*1*, *3*] { break; }").ok());
}

void conditionsAskABool() {
  CHECK(inStart("var.int64 'n' = [*1*];\n    if 'n' { }").code(0) == "E0506");
  CHECK(inStart("var.int64 'n' = [*1*];\n    if 'n' > *0* { }").ok());
}

void theCounterIsInScopeOnlyInTheLoop() {
  CHECK(inStart("loop.range.int64 'i' = [*1*, *3*] { print.stdout['i' \\n]; }").ok());
  CHECK(inStart("loop.range.int64 'i' = [*1*, *3*] { }\n    print.stdout['i' \\n];")
            .code(0) == "E0501");
}

} // namespace

int main() {
  aNameMustBeDeclared();
  aNameIsDeclaredOnce();
  aTypeMustExist();
  aWrittenValueSaysWhatItIs();
  aWrittenValueFitsItsType();
  aSizeIsAlwaysWritten();
  aWrittenNumberHasToFit();
  binaryHoldsWhatIEEESaysItHolds();
  aTypeWithNothingBehindItSaysSo();
  sizesDoNotMixOnTheirOwn();
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
