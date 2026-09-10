// A loan lasts from where it is taken to the last place anything holding it is
// looked at. These are the things that may not happen in between.

#include "xag/Check.h"
#include "as_file.h"

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
  const xag::Source source("test.xag", xag::asFile(text));
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (!lexed.ok() || !parsed.ok())
    return out;
  const xag::CheckResult checked = xag::check(source, parsed.program);
  const xag::OwnResult owned = xag::own(source, parsed.program, checked);
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
    "fn.int64 'size' [loan.str 't'] { give [count['t']]; }\n"
    "fn.nothing 'edit' [loanmut.str 't'] { set 't' = ['t' *!*]; }\n"
    "fn.nothing 'keep' [str 't'] { print.stdout['t' \n]; }\n";

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

// A struct is declared where a function is, so a program that has one cannot be
// written as a body alone.
void expectWith(const std::string &shapes, const std::string &body,
                const std::string &wanted, int line) {
  const Held held =
      run(shapes + std::string(kHelpers) + "START {\n" + body + "\n}\n");
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

#define SHAPED_HOLDS(shapes, body) expectWith(shapes, body, "", __LINE__)
#define SHAPED_REFUSES(shapes, body, code) expectWith(shapes, body, code, __LINE__)

void aLoanEndsWhenNobodyIsHoldingIt() {
  // Lent, looked at, done with — and then it may be handed over.
  HOLDS("var.str 's' = [*hi*];\n"
        "    var.int64 'n' = [size[loan 's']];\n"
        "    keep[move 's'];");
  // Lent for writing, and the same.
  HOLDS("var.mut.str 's' = [*hi*];\n"
        "    edit[loanmut 's'];\n"
        "    keep[move 's'];");
  // Two loans for reading at once is no trouble at all.
  HOLDS("var.str 's' = [*hi*];\n"
        "    var.int64 'a' = [size[loan 's']];\n"
        "    var.int64 'b' = [size[loan 's']];\n"
        "    print.stdout['a' 'b' \n];");
}

void aNumberIsNotHoldingALoan() {
  // `count[loan 'a']` answers a number, and the loan it was worked out from
  // ended at the semicolon. Reading the number as still holding it made the
  // pass refuse this, which the generator found within forty programs.
  HOLDS("var.mut.str 'a' = [*hello*];\n"
        "    var.int64 'n' = [count[loan 'a']];\n"
        "    edit[loanmut 'a'];\n"
        "    print.stdout['n' \n];");
  // The same, the other way round.
  HOLDS("var.mut.str 'a' = [*hello*];\n"
        "    edit[loanmut 'a'];\n"
        "    var.int64 'n' = [size[loan 'a']];\n"
        "    keep[move 'a'];\n"
        "    print.stdout['n' \n];");
}

void aLoanHeldInANameLastsWhileItIsLookedAt() {
  // Held, looked at, done with — and what it borrowed is free again.
  HOLDS("var.str 'a' = [*hello*];\n"
        "    var.loan.str 'w' = [loan 'a'];\n"
        "    print.stdout['w' \n];\n"
        "    keep[move 'a'];");
  // Written through the loan, which is what being lent for writing is for.
  HOLDS("var.mut.str 'a' = [*hello*];\n"
        "    var.loanmut.str 'w' = [loanmut 'a'];\n"
        "    set 'w' = ['w' *!*];\n"
        "    print.stdout['w' \n];");
}

void whatIsLentStaysWhereItIs() {
  // The loan is still wanted after the move, which is the whole objection.
  REFUSES("var.str 'a' = [*hello*];\n"
          "    var.loan.str 'w' = [loan 'a'];\n"
          "    keep[move 'a'];\n"
          "    print.stdout['w' \n];",
          "E0408");
}

void whatIsLentIsNotChangedBehindTheLoansBack() {
  REFUSES("var.mut.str 'a' = [*hello*];\n"
          "    var.loan.str 'w' = [loan 'a'];\n"
          "    set 'a' = [*other*];\n"
          "    print.stdout['w' \n];",
          "E0409");
}

void oneLoanForWritingOrAnyNumberForReading() {
  REFUSES("var.mut.str 'a' = [*hello*];\n"
          "    var.loanmut.str 'x' = [loanmut 'a'];\n"
          "    var.loanmut.str 'y' = [loanmut 'a'];\n"
          "    print.stdout['x' 'y' \n];",
          "E0410");
  REFUSES("var.mut.str 'a' = [*hello*];\n"
          "    var.loan.str 'x' = [loan 'a'];\n"
          "    var.loanmut.str 'y' = [loanmut 'a'];\n"
          "    print.stdout['x' 'y' \n];",
          "E0410");
}

void aLoanOfAManyIsALoanOfEveryPlaceInIt() {
  // Which place `'xs'[…]` names is not known until the program runs, so a loan
  // of the array covers all of them and writing one goes round the loan.
  REFUSES("var.mut.many.int64 'xs' = [*1* *2*];\n"
          "    var.loan.many.int64 'w' = [loan 'xs'];\n"
          "    set 'xs'[*1*] = [*9*];\n"
          "    print.stdout[(count['w']) \n];",
          "E0409");
  REFUSES("var.mut.many.str 'ws' = [*a* *b*];\n"
          "    var.loan.many.str 'w' = [loan 'ws'];\n"
          "    var.many.str 'taken' = [move 'ws'];\n"
          "    print.stdout[(count['w']) \n];",
          "E0408");
}

// Lending one of the things a struct holds lends the struct, because what the
// loan points at lives inside it and goes wherever it goes.
void lendingAFieldLendsTheStruct() {
  const char *kTag = "struct 'tag' [str 'name', int64 'runs']\n";

  SHAPED_REFUSES(kTag,
                 "var.tag 'a' = [*ada* *36*];\n"
                 "    var.loan.str 'w' = [loan 'a'.name];\n"
                 "    var.tag 'gone' = [move 'a'];\n"
                 "    print.stdout[(size['w']) \n];",
                 "E0408");
  SHAPED_REFUSES(kTag,
                 "var.mut.tag 'a' = [*ada* *36*];\n"
                 "    var.loan.str 'w' = [loan 'a'.name];\n"
                 "    set 'a'.name = [*bob*];\n"
                 "    print.stdout[(size['w']) \n];",
                 "E0409");

  // Reading one out and being done with it leaves nothing standing.
  SHAPED_HOLDS(kTag,
               "var.tag 'a' = [*ada* *36*];\n"
               "    var.int64 'n' = [size[loan 'a'.name]];\n"
               "    var.tag 'gone' = [move 'a'];\n"
               "    print.stdout['n' \n];");

  // A number that came out of one is a number, not a way back in.
  SHAPED_HOLDS(kTag,
               "var.tag 'a' = [*ada* *36*];\n"
               "    var.int64 'r' = ['a'.runs];\n"
               "    var.tag 'gone' = [move 'a'];\n"
               "    print.stdout['r' \n];");
}

} // namespace

// A struct may hold a borrow, and then it is holding the loan too — for as long
// as the struct lives, rather than until the statement that built it ends.
void aStructHoldingALoanIsHoldingTheLoan() {
  SHAPED_REFUSES("struct 'keeps' [loan.str 's']\n",
                 "var.str 's' = [*hi*];\n"
                 "    var.keeps 'k' = [loan 's'];\n"
                 "    keep[move 's'];\n"
                 "    print.stdout['k'.s \\n];",
                 "E0408");
  // Deeper, through a struct that holds the one that holds it.
  SHAPED_REFUSES("struct 'inner' [loan.str 's']\nstruct 'outer' [inner 'i']\n",
                 "var.str 's' = [*hi*];\n"
                 "    var.outer 'o' = [inner[loan 's']];\n"
                 "    keep[move 's'];\n"
                 "    print.stdout['o'.i.s \\n];",
                 "E0408");
  // And a struct holding nothing borrowed is holding no loan, so what it was
  // built from may go where it likes.
  SHAPED_HOLDS("struct 'plain' [str 's', int64 'n']\n",
               "var.str 's' = [*hi*];\n"
               "    var.plain 'p' = [move 's' *1*];\n"
               "    print.stdout['p'.n \\n];");
}

int main() {
  aStructHoldingALoanIsHoldingTheLoan();
  aLoanEndsWhenNobodyIsHoldingIt();
  aNumberIsNotHoldingALoan();
  aLoanHeldInANameLastsWhileItIsLookedAt();
  whatIsLentStaysWhereItIs();
  whatIsLentIsNotChangedBehindTheLoansBack();
  oneLoanForWritingOrAnyNumberForReading();
  aLoanOfAManyIsALoanOfEveryPlaceInIt();
  lendingAFieldLendsTheStruct();

  if (failures == 0)
    std::cout << "all region tests passed\n";
  return failures == 0 ? 0 : 1;
}
