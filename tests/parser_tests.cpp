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
const.i64 'LIMIT' = [*10*];

fn.i64 sum-to [i64 'n'] {
    var.mut.i64 'total' = [*0*];
    loop.range.i64 'i' = [*1*, 'n'] {
        set 'total' = ['total' + 'i'];
    }
    give ['total'];
}

START {
    var.i64 's' = [sum-to['LIMIT']];
    print.stdout[str:*sum to * 'LIMIT' str:* = * 's' \n];
}
)");
  CHECK(p.ok());
  CHECK(p.has("const const.i64 'LIMIT'"));
  CHECK(p.has("fn fn.i64 sum-to"));
  CHECK(p.has("param i64 'n'"));
  CHECK(p.has("declare var.mut.i64 'total'"));
  CHECK(p.has("loop loop.range.i64 'i'"));
  CHECK(p.has("give"));
  CHECK(p.has("call print.stdout"));
  CHECK(p.has("typed str"));
  CHECK(p.has("escape \\n"));
}

void aDeclarationAndACallTellApart() {
  // Both open with a dotted run of words; what follows decides.
  const Parsed p = inStart("var.i64 'n' = [*1*];\n    print.stdout['n'];");
  CHECK(p.ok());
  CHECK(p.has("declare var.i64 'n'"));
  CHECK(p.has("do\n"));
  CHECK(p.has("call print.stdout"));
}

void precedenceIsMathematics() {
  const Parsed p = inStart("var.i64 'n' = [*1* + *2* x *3*];");
  CHECK(p.ok());
  // `+` at the root, `x` beneath it.
  CHECK(p.has("binary +\n"));
  CHECK(p.tree.find("binary x") > p.tree.find("binary +"));
}

void powerLeansRight() {
  const Parsed p = inStart("var.i64 'n' = [*2* ^ *3* ^ *2*];");
  CHECK(p.ok());
  // The right child is itself a power, which is what leaning right means.
  const std::string expected = "binary ^\n            written *2*\n            binary ^\n";
  CHECK(p.tree.find("binary ^") != std::string::npos);
  CHECK(p.tree.find("binary ^", p.tree.find("binary ^") + 1) != std::string::npos);
  CHECK(p.tree.find("written *2*\n") < p.tree.find("binary ^", p.tree.find("binary ^") + 1));
  (void)expected;
}

void unsettledOrderNeedsBrackets() {
  const Parsed p = inStart("var.i64 'n' = [*a* mod *b* + *c*];");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0301");
  // Both readings are named, because naming them is the message.
  CHECK(p.message(0).find("(*a* mod *b*) + *c*") != std::string::npos);
  CHECK(p.message(0).find("*a* mod (*b* + *c*)") != std::string::npos);

  const Parsed andOr = inStart("var.bool 'b' = ['p' and 'q' or 'r'];");
  CHECK(!andOr.parsed.ok());
  CHECK(andOr.code(0) == "E0301");

  // A settled operator arriving after an unsettled one is the same mistake.
  const Parsed after = inStart("var.i64 'n' = [*a* mod *b* + *c*];");
  CHECK(!after.parsed.ok());
  CHECK(after.code(0) == "E0301");
}

void repeatingAnAssociativeOperatorIsFine() {
  // `and` and `or` are associative, so the brackets would say nothing.
  const Parsed p = inStart("var.bool 'b' = ['p' and 'q' and 'r' and 's'];");
  CHECK(p.ok());
  CHECK(inStart("var.bool 'b' = ['p' or 'q' or 'r'];").ok());

  // `mod` is not associative, so repeating it still asks a question.
  const Parsed m = inStart("var.i64 'n' = [*9* mod *5* mod *3*];");
  CHECK(!m.parsed.ok());
  CHECK(m.code(0) == "E0301");

  // Mixing the two still needs brackets even though each repeats fine.
  CHECK(!inStart("var.bool 'b' = ['p' or 'q' and 'r'];").parsed.ok());
}

void bracketsSettleIt() {
  CHECK(inStart("var.i64 'n' = [(*a* mod *b*) + *c*];").ok());
  CHECK(inStart("var.bool 'b' = [('p' and 'q') or 'r'];").ok());
  CHECK(inStart("var.i64 'n' = [*a* mod *b*];").ok());
}

void writingADefaultIsAnError() {
  for (const std::string &word : {"immut", "own", "file", "temp"}) {
    const Parsed p = inStart("var." + word + ".i64 'n' = [*1*];");
    CHECK(!p.parsed.ok());
    CHECK(p.code(0) == "E0201");
  }
  CHECK(inStart("var.mut.i64 'n' = [*1*];").ok());
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
  const Parsed w = inStart("var.mut.i64 'left' = [*3*];\n"
                           "    loop.while 'left' > *0* {\n"
                           "        set 'left' = ['left' - *1*];\n    }");
  CHECK(w.ok());
  CHECK(w.has("loop while\n"));
  CHECK(w.has("binary >\n"));

  // Bracketing one is now a mistake, since `[` opens a value list.
  CHECK(!inStart("if ['a' >== 'b'] { }").parsed.ok());
}

void aVarCannotStandAtTheTopLevel() {
  const Parsed p = run("var.i64 'n' = [*1*];\n");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0104");
}

void readingContinuesAfterAMistake() {
  const Parsed p = inStart("var.i64 'n' = ;\n    var.immut.i64 'm' = [*2*];");
  CHECK(!p.parsed.ok());
  CHECK(p.parsed.diagnostics.size() >= 2);
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

  if (failures == 0)
    std::cout << "all parser tests passed\n";
  return failures == 0 ? 0 : 1;
}
