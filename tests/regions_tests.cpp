// A loan lasts from where it is taken to the last place anything holding it is
// looked at. These are the things that may not happen in between.

#include "xag/Check.h"
#include "xag/Lexer.h"
#include "xag/Mir.h"
#include "xag/Own.h"
#include "xag/Parser.h"
#include "xag/Regions.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

struct Held {
  bool compiled = false;
  std::string code;
};

Held run(const std::string &text) {
  Held out;
  const xag::Source source("test.xag", text);
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (!lexed.ok() || !parsed.ok())
    return out;
  const xag::CheckResult checked = xag::check(source, parsed.program);
  const xag::OwnResult owned = xag::own(source, parsed.program);
  if (!checked.ok() || !owned.ok())
    return out;
  out.compiled = true;

  xag::MirResult built = xag::build(source, parsed.program, checked);
  xag::elaborate(built.mir);
  const xag::RegionResult held = xag::regions(source, built.mir);
  out.code = held.diagnostics.empty() ? "" : held.diagnostics.front().code;
  return out;
}

const char *kHelpers =
    "fn.int64 size [ref.str 't'] { give [count['t']]; }\n"
    "fn.nothing edit [refmut.str 't'] { set 't' = ['t' *!*]; }\n"
    "fn.nothing keep [str 't'] { print.stdout['t' \n]; }\n";

void expect(const std::string &body, const std::string &wanted, int line) {
  const Held held = run(std::string(kHelpers) + "START {\n" + body + "\n}\n");
  if (!held.compiled) {
    std::cerr << "FAIL line " << line << ": the program did not reach this pass\n";
    ++failures;
    return;
  }
  if (held.code != wanted) {
    std::cerr << "FAIL line " << line << ": got \"" << held.code << "\", wanted \""
              << wanted << "\"\n";
    ++failures;
  }
}

#define HOLDS(body) expect(body, "", __LINE__)
#define REFUSES(body, code) expect(body, code, __LINE__)

void aLoanEndsWhenNobodyIsHoldingIt() {
  // Lent, looked at, done with — and then it may be handed over.
  HOLDS("var.str 's' = [*hi*];\n"
        "    var.int64 'n' = [size[ref 's']];\n"
        "    keep[move 's'];");
  // Lent for writing, and the same.
  HOLDS("var.mut.str 's' = [*hi*];\n"
        "    edit[refmut 's'];\n"
        "    keep[move 's'];");
  // Two loans for reading at once is no trouble at all.
  HOLDS("var.str 's' = [*hi*];\n"
        "    var.int64 'a' = [size[ref 's']];\n"
        "    var.int64 'b' = [size[ref 's']];\n"
        "    print.stdout['a' 'b' \n];");
}

void aNumberIsNotHoldingALoan() {
  // `count[ref 'a']` answers a number, and the loan it was worked out from
  // ended at the semicolon. Reading the number as still holding it made the
  // pass refuse this, which the generator found within forty programs.
  HOLDS("var.mut.str 'a' = [*hello*];\n"
        "    var.int64 'n' = [count[ref 'a']];\n"
        "    edit[refmut 'a'];\n"
        "    print.stdout['n' \n];");
  // The same, the other way round.
  HOLDS("var.mut.str 'a' = [*hello*];\n"
        "    edit[refmut 'a'];\n"
        "    var.int64 'n' = [size[ref 'a']];\n"
        "    keep[move 'a'];\n"
        "    print.stdout['n' \n];");
}

void aLoanHeldInANameLastsWhileItIsLookedAt() {
  // Held, looked at, done with — and what it borrowed is free again.
  HOLDS("var.str 'a' = [*hello*];\n"
        "    var.ref.str 'w' = [ref 'a'];\n"
        "    print.stdout['w' \n];\n"
        "    keep[move 'a'];");
  // Written through the loan, which is what being lent for writing is for.
  HOLDS("var.mut.str 'a' = [*hello*];\n"
        "    var.refmut.str 'w' = [refmut 'a'];\n"
        "    set 'w' = ['w' *!*];\n"
        "    print.stdout['w' \n];");
}

void whatIsLentStaysWhereItIs() {
  // The loan is still wanted after the move, which is the whole objection.
  REFUSES("var.str 'a' = [*hello*];\n"
          "    var.ref.str 'w' = [ref 'a'];\n"
          "    keep[move 'a'];\n"
          "    print.stdout['w' \n];",
          "E0408");
}

void whatIsLentIsNotChangedBehindTheLoansBack() {
  REFUSES("var.mut.str 'a' = [*hello*];\n"
          "    var.ref.str 'w' = [ref 'a'];\n"
          "    set 'a' = [*other*];\n"
          "    print.stdout['w' \n];",
          "E0409");
}

void oneLoanForWritingOrAnyNumberForReading() {
  REFUSES("var.mut.str 'a' = [*hello*];\n"
          "    var.refmut.str 'x' = [refmut 'a'];\n"
          "    var.refmut.str 'y' = [refmut 'a'];\n"
          "    print.stdout['x' 'y' \n];",
          "E0410");
  REFUSES("var.mut.str 'a' = [*hello*];\n"
          "    var.ref.str 'x' = [ref 'a'];\n"
          "    var.refmut.str 'y' = [refmut 'a'];\n"
          "    print.stdout['x' 'y' \n];",
          "E0410");
}

void aLoanOfAManyIsALoanOfEveryPlaceInIt() {
  // Which place `'xs'[…]` names is not known until the program runs, so a loan
  // of the array covers all of them and writing one goes round the loan.
  REFUSES("var.mut.many.int64 'xs' = [*1* *2*];\n"
          "    var.ref.many.int64 'w' = [ref 'xs'];\n"
          "    set 'xs'[*0*] = [*9*];\n"
          "    print.stdout[(count['w']) \n];",
          "E0409");
  REFUSES("var.mut.many.str 'ws' = [*a* *b*];\n"
          "    var.ref.many.str 'w' = [ref 'ws'];\n"
          "    var.many.str 'taken' = [move 'ws'];\n"
          "    print.stdout[(count['w']) \n];",
          "E0408");
}

} // namespace

int main() {
  aLoanEndsWhenNobodyIsHoldingIt();
  aNumberIsNotHoldingALoan();
  aLoanHeldInANameLastsWhileItIsLookedAt();
  whatIsLentStaysWhereItIs();
  whatIsLentIsNotChangedBehindTheLoansBack();
  oneLoanForWritingOrAnyNumberForReading();
  aLoanOfAManyIsALoanOfEveryPlaceInIt();

  if (failures == 0)
    std::cout << "all region tests passed\n";
  return failures == 0 ? 0 : 1;
}
