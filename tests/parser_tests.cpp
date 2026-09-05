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
  const std::string &code(unsigned i) const { return parsed.diagnostics[i].code; }
  const std::string &message(unsigned i) const { return parsed.diagnostics[i].message; }
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

fn.int64 sum-to [int64 'n'] {
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
  CHECK(!run("fn.mut.wat.int64 f [] { give [*7*]; }\n").parsed.ok());
}

void eachChainAsksItsOwnQuestions() {
  // `mut` is a real word in the wrong chain: a function's answer is a value,
  // and a value does not change.
  const Parsed f = run("fn.mut.int64 f [] { give [*7*]; }\n");
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
  CHECK(run("fn.export.int64 f [] { give [*7*]; }\n").code(0) == "E0206");
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

void indexingIsRefusedRatherThanIgnored() {
  // It used to parse, throw the index away, and assign to the whole name.
  const Parsed p = inStart("var.mut.int64 'xs' = [*1*];\n    set 'xs'[*2*] = [*99*];");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0208");
}

void chainsThatWereAlwaysGoodStillAre() {
  CHECK(run("fn.ref.'life'.str longer [ref.'life'.str 'a', ref.'life'.str 'b'] {\n"
            "    give ['a'];\n}\n").ok());
  CHECK(run("fn.nothing edit [refmut.str 't'] { set 't' = ['t' *!*]; }\n").ok());
  CHECK(run("const.int64 'LIMIT' = [*10*];\n").ok());
  CHECK(inStart("var.mut.int64 'n' = [*1*];").ok());
  CHECK(inStart("var.refmut.str 's' = [refmut 'other'];").ok());
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
  indexingIsRefusedRatherThanIgnored();
  chainsThatWereAlwaysGoodStillAre();

  if (failures == 0)
    std::cout << "all parser tests passed\n";
  return failures == 0 ? 0 : 1;
}
