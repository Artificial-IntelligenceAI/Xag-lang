#include "xag/Lexer.h"
#include "xag/Parser.h"

#include <iostream>
#include <sstream>
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

struct Parsed {
  xag::Source source;
  xag::LexResult lexed;
  xag::ParseResult parsed;
  std::string tree;

  bool ok() const { return lexed.ok() && parsed.ok(); }
  bool has(const std::string &needle) const { return tree.find(needle) != std::string::npos; }
  // Asking for a mistake that is not there is how a test says it is out of
  // date, and it should read as a failure rather than walking off the end of
  // the list — which is what it did when a program this file expected to be
  // refused became a good one.
  std::string code(unsigned i) const {
    return i < parsed.diagnostics.size() ? parsed.diagnostics[i].code : "(none)";
  }
  std::string message(unsigned i) const {
    return i < parsed.diagnostics.size() ? parsed.diagnostics[i].message : "(none)";
  }
};

Parsed run(const std::string &text) {
  Parsed p{xag::Source("test.xag", text), {}, {}, {}};
  p.lexed = xag::lex(p.source);
  p.parsed = xag::parse(p.source, p.lexed.tokens);
  std::ostringstream out;
  xag::print(p.parsed.program, out);
  p.tree = out.str();
  return p;
}

Parsed inStart(const std::string &body) { return run("START {\n" + body + "\n}\n"); }

void wholeProgramParses() {
  const Parsed p = run(R"(
const.int64 'LIMIT' = [*10*];

fn.int64 'sum-to' [int64 'n'] {
    var.mut.int64 'total' = [*0*];
    loop.range.int64 'i' = [*1*, 'n'] {
        set 'total' = ['total' + 'i'];
    }
    give ['total'];
}

START {
    var.int64 's' = [sum-to['LIMIT']];
    print.stdout[str:*sum to * 'LIMIT' str:* = * 's' \n];
}
)");
  CHECK(p.ok());
  CHECK(p.has("const const.int64 'LIMIT'"));
  CHECK(p.has("fn fn.int64 sum-to"));
  CHECK(p.has("param int64 'n'"));
  CHECK(p.has("declare var.mut.int64 'total'"));
  CHECK(p.has("loop loop.range.int64 'i'"));
  CHECK(p.has("give"));
  CHECK(p.has("call print.stdout"));
  CHECK(p.has("typed str"));
  CHECK(p.has("escape \\n"));
}

void aDeclarationAndACallTellApart() {
  // Both open with a dotted run of words; what follows decides.
  const Parsed p = inStart("var.int64 'n' = [*1*];\n    print.stdout['n'];");
  CHECK(p.ok());
  CHECK(p.has("declare var.int64 'n'"));
  CHECK(p.has("do\n"));
  CHECK(p.has("call print.stdout"));
}

void precedenceIsMathematics() {
  const Parsed p = inStart("var.int64 'n' = [*1* + *2* x *3*];");
  CHECK(p.ok());
  // `+` at the root, `x` beneath it.
  CHECK(p.has("binary +\n"));
  CHECK(p.tree.find("binary x") > p.tree.find("binary +"));
}

void powerLeansRight() {
  const Parsed p = inStart("var.int64 'n' = [*2* ^ *3* ^ *2*];");
  CHECK(p.ok());
  // The right child is itself a power, which is what leaning right means.
  const std::string expected = "binary ^\n            written *2*\n            binary ^\n";
  CHECK(p.tree.find("binary ^") != std::string::npos);
  CHECK(p.tree.find("binary ^", p.tree.find("binary ^") + 1) != std::string::npos);
  CHECK(p.tree.find("written *2*\n") < p.tree.find("binary ^", p.tree.find("binary ^") + 1));
  (void)expected;
}

void unsettledOrderNeedsBrackets() {
  const Parsed p = inStart("var.int64 'n' = [*a* mod *b* + *c*];");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0301");
  // Both readings are named, because naming them is the message.
  CHECK(p.message(0).find("(*a* mod *b*) + *c*") != std::string::npos);
  CHECK(p.message(0).find("*a* mod (*b* + *c*)") != std::string::npos);

  const Parsed andOr = inStart("var.bool 'b' = ['p' and 'q' or 'r'];");
  CHECK(!andOr.parsed.ok());
  CHECK(andOr.code(0) == "E0301");

  // A settled operator arriving after an unsettled one is the same mistake.
  const Parsed after = inStart("var.int64 'n' = [*a* mod *b* + *c*];");
  CHECK(!after.parsed.ok());
  CHECK(after.code(0) == "E0301");
}

void repeatingAnAssociativeOperatorIsFine() {
  // `and` and `or` are associative, so the brackets would say nothing.
  const Parsed p = inStart("var.bool 'b' = ['p' and 'q' and 'r' and 's'];");
  CHECK(p.ok());
  CHECK(inStart("var.bool 'b' = ['p' or 'q' or 'r'];").ok());

  // `mod` is not associative, so repeating it still asks a question.
  const Parsed m = inStart("var.int64 'n' = [*9* mod *5* mod *3*];");
  CHECK(!m.parsed.ok());
  CHECK(m.code(0) == "E0301");

  // Mixing the two still needs brackets even though each repeats fine.
  CHECK(!inStart("var.bool 'b' = ['p' or 'q' and 'r'];").parsed.ok());
}

void bracketsSettleIt() {
  CHECK(inStart("var.int64 'n' = [(*a* mod *b*) + *c*];").ok());
  CHECK(inStart("var.bool 'b' = [('p' and 'q') or 'r'];").ok());
  CHECK(inStart("var.int64 'n' = [*a* mod *b*];").ok());
}

void writingADefaultIsAnError() {
  for (const std::string &word : {"immut", "own", "file", "temp"}) {
    const Parsed p = inStart("var." + word + ".int64 'n' = [*1*];");
    CHECK(!p.parsed.ok());
    CHECK(p.code(0) == "E0201");
  }
  CHECK(inStart("var.mut.int64 'n' = [*1*];").ok());
}

void transfersAreSpelled() {
  const Parsed p = inStart("keep[move 'greeting'];\n    size[ref 'greeting'];");
  CHECK(p.ok());
  CHECK(p.has("transfer move"));
  CHECK(p.has("transfer ref"));
}

void armsOfAnIf() {
  const Parsed p = inStart(
      "if 'a' >== 'b' {\n        give ['a'];\n    } else-if 'a' == 'b' {\n"
      "        give ['b'];\n    } else {\n        give ['c'];\n    }");
  CHECK(p.ok());
  CHECK(p.has("if\n"));
  CHECK(p.has("otherwise"));
  size_t arms = 0, from = 0;
  while ((from = p.tree.find("arm\n", from)) != std::string::npos) {
    ++arms;
    ++from;
  }
  CHECK(arms == 2);
}

void conditionsWearNoBrackets() {
  const Parsed w = inStart("var.mut.int64 'left' = [*3*];\n"
                           "    loop.while 'left' > *0* {\n"
                           "        set 'left' = ['left' - *1*];\n    }");
  CHECK(w.ok());
  CHECK(w.has("loop while\n"));
  CHECK(w.has("binary >\n"));

  // Bracketing one is now a mistake, since `[` opens a value list.
  CHECK(!inStart("if ['a' >== 'b'] { }").parsed.ok());
}

void aVarCannotStandAtTheTopLevel() {
  const Parsed p = run("var.int64 'n' = [*1*];\n");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0104");
}

void readingContinuesAfterAMistake() {
  const Parsed p = inStart("var.int64 'n' = ;\n    var.immut.int64 'm' = [*2*];");
  CHECK(!p.parsed.ok());
  CHECK(p.parsed.diagnostics.size() >= 2);
}

void aSegmentHasToMeanSomething() {
  // The hole this closes: every one of these used to compile and run, because a
  // chain was read by taking its last segment and passing over the rest.
  for (const std::string &word : {"banana", "arr", "wat", "mtu"}) {
    const Parsed p = inStart("var." + word + ".int64 'n' = [*1*];");
    CHECK(!p.parsed.ok());
    CHECK(p.code(0) == "E0202");
  }
  CHECK(!run("fn.mut.wat.int64 'f' [] { give [*7*]; }\n").parsed.ok());
}

void eachChainAsksItsOwnQuestions() {
  // `mut` is a real word in the wrong chain: a function's answer is a value,
  // and a value does not change.
  const Parsed f = run("fn.mut.int64 'f' [] { give [*7*]; }\n");
  CHECK(!f.parsed.ok());
  CHECK(f.code(0) == "E0203");

  // `perm` belongs to a loop counter, and a `var` has none.
  CHECK(inStart("var.perm.int64 'n' = [*1*];").code(0) == "E0203");

  // A lifetime names the loan an answer is on, and a `var` is not an answer.
  CHECK(inStart("var.ref.'life'.str 's' = [*hi*];").code(0) == "E0203");

  // The kind is said once, and first.
  CHECK(inStart("var.var.int64 'n' = [*1*];").code(0) == "E0203");
}

void oneQuestionIsAnsweredOnce() {
  const Parsed p = inStart("var.mut.mut.int64 'n' = [*1*];");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0204");
  CHECK(p.parsed.diagnostics[0].notes.size() == 1);
}

void aChainHasOneOrder() {
  const Parsed p = inStart("var.ref.mut.str 's' = [*hi*];");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0205");
  CHECK(inStart("var.mut.ref.str 's' = [*hi*];").ok());
}

void visibilityHasNowhereToGoYet() {
  CHECK(run("fn.export.int64 'f' [] { give [*7*]; }\n").code(0) == "E0206");
  CHECK(run("const.program.int64 'L' = [*1*];\n").code(0) == "E0206");
}

void aLoopSaysWhichKindItIs() {
  const Parsed p = inStart("loop.int64 'i' = [*1*, *3*] { }");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0207");
  CHECK(inStart("loop.range.int64 'i' = [*1*, *3*] { }").ok());
  CHECK(inStart("loop.perm.range.int64 'i' = [*1*, *3*] { }").ok());
  CHECK(inStart("loop.while *false* { }").ok());
}

void anElementIsReadAndWritten() {
  const Parsed r = inStart("print.stdout['xs'[*0*] \\n];");
  CHECK(r.ok());
  CHECK(r.has("element of 'xs'"));

  const Parsed w = inStart("set 'xs'[*2*] = [*99*];");
  CHECK(w.ok());
  CHECK(w.has("at\n"));

  // A word before `[` is still a call, and a name before one is still not.
  CHECK(inStart("var.int64 'n' = [sum-to['LIMIT']];").has("call sum-to"));
}

void manyStandsWithTheType() {
  CHECK(inStart("var.many.int64 'xs' = [*1* *2*];").ok());
  CHECK(run("fn.many.int64 'f' [ref.many.str 'ws'] { give [*1*]; }\n").ok());
  CHECK(inStart("var.many.many.int64 'g' = [];").code(0) == "E0210");
  CHECK(inStart("var.many.mut.int64 'xs' = [*1*];").code(0) == "E0209");
}

void chainsThatWereAlwaysGoodStillAre() {
  CHECK(run("fn.ref.'life'.str 'longer' [ref.'life'.str 'a', ref.'life'.str 'b'] {\n"
            "    give ['a'];\n}\n").ok());
  CHECK(run("fn.nothing 'edit' [refmut.str 't'] { set 't' = ['t' *!*]; }\n").ok());
  CHECK(run("const.int64 'LIMIT' = [*10*];\n").ok());
  CHECK(inStart("var.mut.int64 'n' = [*1*];").ok());
  CHECK(inStart("var.refmut.str 's' = [refmut 'other'];").ok());
}

void aTypeMaySayItHoldsNothing() {
  CHECK(inStart("var.or-nothing.str 's' = [*hi*];").ok());
  CHECK(inStart("var.or-nothing.many.int64 'xs' = [*1*];").ok());
  CHECK(run("fn.or-nothing.int64 'f' [] { give [nothing]; }\n").ok());

  // One absence is every absence.
  CHECK(inStart("var.or-nothing.or-nothing.str 's' = [*hi*];").code(0) == "E0211");
  // `many` of them wants a table of types rather than a pair of words.
  CHECK(inStart("var.many.or-nothing.int64 'xs' = [*1*];").code(0) == "E0209");
}

void holdsLendsWhatIsThere() {
  const Parsed p = inStart("if 'a' holds 'text' { print.stdout['text' \\n]; }");
  CHECK(p.ok());
  CHECK(p.has("name 'text'"));

  CHECK(inStart("loop.while 'a' holds 'line' { break; }").ok());
  // It lends to a name, and a word is not one.
  CHECK(inStart("if 'a' holds text { }").code(0) == "E0101");
}

void whenIsMadeOfIs() {
  const Parsed p = inStart("when 'x' {\n"
                           "        is 'value' { print.stdout['value' \\n]; }\n"
                           "        is nothing { break; }\n    }");
  CHECK(p.ok());
  CHECK(p.has("when\n"));
  CHECK(p.has("is 'value'"));
  CHECK(p.has("is nothing"));

  // The subject is bounded by `when` and `{`, so it takes no brackets.
  CHECK(!inStart("when ['x'] { is nothing { } }").parsed.ok());
  // Nothing but `is` goes in one.
  CHECK(inStart("when 'x' { else { } }").code(0) == "E0108");
  CHECK(inStart("when 'x' { is value { } }").code(0) == "E0108");
}

void aDeclarationMarksWhatItNames() {
  // Marked where it is named, bare where it is called.
  CHECK(run("fn.int64 'longer' [int64 'n'] { give ['n']; }\n").ok());
  CHECK(run("fn.int64 longer [int64 'n'] { give ['n']; }\n").code(0) == "E0101");
  CHECK(run("struct 'point' [int64 'x']\n").ok());
  CHECK(run("struct point [int64 'x']\n").code(0) == "E0101");

  // The call site is unchanged, and so is naming a type.
  CHECK(run("fn.int64 'twice' [int64 'n'] { give ['n' + 'n']; }\n"
            "START { print.stdout[(twice[*2*]) \\n]; }\n").ok());
  CHECK(run("struct 'point' [int64 'x']\n"
            "START { var.point 'p' = [*1*]; }\n").ok());

  // The loan name in a chain was already marked, and still is.
  CHECK(run("fn.ref.'life'.str 'longer' [ref.'life'.str 'a', ref.'life'.str 'b'] {\n"
            "    give ['a'];\n}\n").ok());
}

void aStructNamesWhatItHolds() {
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n").ok());
  // The same shape as a function's parameters, because it is the same question:
  // what is in here, in what order, and called what.
  CHECK(run("struct 'pair' [str 'name', many.int64 'runs']\n").ok());

  // A declaration marks what it names, and so do the things it holds.
  CHECK(run("struct point [int64 'x']\n").code(0) == "E0101");
  CHECK(run("struct 'point' [int64 x]\n").code(0) == "E0101");

  // Nothing else goes in the chain.
  CHECK(run("struct.mut point [int64 'x']\n").code(0) == "E0203");

  // Reading and writing one of the things it holds.
  CHECK(inStart("var.point 'p' = [*1* *2*];\n    set 'p'.x = [*9*];").ok());
  CHECK(inStart("print.stdout['p'.x \\n];").ok());
  CHECK(inStart("print.stdout['p'.x.y \\n];").ok());
}

} // namespace

int main() {
  wholeProgramParses();
  aDeclarationAndACallTellApart();
  precedenceIsMathematics();
  powerLeansRight();
  unsettledOrderNeedsBrackets();
  repeatingAnAssociativeOperatorIsFine();
  bracketsSettleIt();
  writingADefaultIsAnError();
  transfersAreSpelled();
  armsOfAnIf();
  conditionsWearNoBrackets();
  aVarCannotStandAtTheTopLevel();
  readingContinuesAfterAMistake();
  aSegmentHasToMeanSomething();
  eachChainAsksItsOwnQuestions();
  oneQuestionIsAnsweredOnce();
  aChainHasOneOrder();
  visibilityHasNowhereToGoYet();
  aLoopSaysWhichKindItIs();
  anElementIsReadAndWritten();
  manyStandsWithTheType();
  chainsThatWereAlwaysGoodStillAre();
  aTypeMaySayItHoldsNothing();
  holdsLendsWhatIsThere();
  whenIsMadeOfIs();
  aDeclarationMarksWhatItNames();
  aStructNamesWhatItHolds();

  if (failures == 0)
    std::cout << "all parser tests passed\n";
  return failures == 0 ? 0 : 1;
}
