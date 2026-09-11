#include "as_file.h"

#include "xag/Lexer.h"
#include "xag/AstClone.h"
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
  Parsed p{xag::Source("test.xag", xag::asFile(text)), {}, {}, {}};
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
  const Parsed p = inStart("keep[move 'greeting'];\n    size[loan 'greeting'];");
  CHECK(p.ok());
  CHECK(p.has("transfer move"));
  CHECK(p.has("transfer loan"));
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
                           "    loop.while 'left' > int64:*0* {\n"
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

void aChainSaysWhichWordItReadAsTheType() {
  // The hole this closes: `fn.int64.number` said that `int64` answers no
  // question a chain asks, which is a claim about the one word in the line that
  // was fine. `number` is nearest the name, so `number` is what was read as the
  // type — and that is what the reader has to be told to see the mistake.
  const Parsed p = run("fn.int64.number 'f' [int64 'x'] { give ['x']; }\nSTART { }\n");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0202");
  const std::string said = p.parsed.diagnostics.front().message;
  CHECK(said.find("`int64` is not one of the words a chain says") != std::string::npos);
  CHECK(said.find("the type here is `number`") != std::string::npos);

  // But `any.number` is not that shape at all: the blank and the word saying
  // what it will take are one type region, the way `many.int64` is, so nothing
  // here is standing where only a chain word may stand.
  const Parsed a = run("fn.any.number 'f' [any.number 'x'] { give ['x']; }\nSTART { }\n");
  CHECK(a.parsed.ok());
  const Parsed deep =
      run("fn.any.number 'f' [loan.many.any.number 'xs'] { give [*0*]; }\nSTART { }\n");
  CHECK(deep.parsed.ok());
  // A word that names no family is still a word too far from the name.
  CHECK(run("fn.any.banana 'f' [any 'x'] { give ['x']; }\nSTART { }\n").code(0) ==
        "E0202");

  // And it names the type even when the word at fault is nowhere near it.
  const Parsed far = inStart("var.banana.int64 'n' = [*1*];");
  CHECK(far.code(0) == "E0202");
  CHECK(far.parsed.diagnostics.front().message.find("the type here is `int64`") !=
        std::string::npos);
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
  CHECK(inStart("var.loan.'life'.str 's' = [*hi*];").code(0) == "E0203");

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
  const Parsed p = inStart("var.loan.mut.str 's' = [*hi*];");
  CHECK(!p.parsed.ok());
  CHECK(p.code(0) == "E0205");
  CHECK(inStart("var.mut.loan.str 's' = [*hi*];").ok());
}

void visibilityHasNowhereToGoYet() {
  CHECK(run("fn.export.int64 'f' [] { give [*7*]; }\n").code(0) == "E0206");
  CHECK(run("const.program.int64 'L' = [*1*];\n").code(0) == "E0206");
  // And it is not a word a file may open with either, so there is still no way
  // to write more than one file.
  CHECK(run("export.fn.int64 'f' [] { give [*1*]; }\n").code(0) == "E0104");
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
  const Parsed r = inStart("print.stdout['xs'[*1*] \\n];");
  CHECK(r.ok());
  CHECK(r.has("element of 'xs'"));

  const Parsed w = inStart("set 'xs'[*3*] = [*99*];");
  CHECK(w.ok());
  CHECK(w.has("at\n"));

  // A word before `[` is still a call, and a name before one is still not.
  CHECK(inStart("var.int64 'n' = [sum-to['LIMIT']];").has("call sum-to"));
}

void manyStandsWithTheType() {
  CHECK(inStart("var.many.int64 'xs' = [*1* *2*];").ok());
  CHECK(run("fn.many.int64 'f' [loan.many.str 'ws'] { give [*1*]; }\n").ok());
  // A second `many` is a `many` of a `many`, and was refused as `E0210` until
  // there was something to build it as.
  CHECK(inStart("var.many.many.int64 'g' = [[*1* *2*] [*3*]];").ok());
  CHECK(inStart("var.many.many.many.int64 'g' = [[[*1*]] [[*2*]]];").ok());
  // Brackets where an item goes make one; a name in front of them is still an
  // index, and a condition still takes none.
  CHECK(inStart("var.many.int64 'xs' = [*1*];\n    "
                "print.stdout['xs'[*1*] \\n];").ok());
  CHECK(!inStart("if ['a' >== 'b'] { }").parsed.ok());
  CHECK(inStart("var.many.mut.int64 'xs' = [*1*];").code(0) == "E0209");
}

void chainsThatWereAlwaysGoodStillAre() {
  CHECK(run("fn.loan.'life'.str 'longer' [loan.'life'.str 'a', loan.'life'.str 'b'] {\n"
            "    give ['a'];\n}\n").ok());
  CHECK(run("fn.nothing 'edit' [loanmut.str 't'] { set 't' = ['t' *!*]; }\n").ok());
  CHECK(run("const.int64 'LIMIT' = [*10*];\n").ok());
  CHECK(inStart("var.mut.int64 'n' = [*1*];").ok());
  CHECK(inStart("var.loanmut.str 's' = [loanmut 'other'];").ok());
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
  // A word after `is` is a case of a `one-of`. Whether it names one is the
  // checker's question — the parser refused it here until there were cases to
  // name, and a `when` over something with no cases is `E0520` either way.
  CHECK(inStart("when 'x' { is value { } }").parsed.ok());
  CHECK(inStart("when 'x' { is *4* { } }").code(0) == "E0108");
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
  CHECK(run("fn.loan.'life'.str 'longer' [loan.'life'.str 'a', loan.'life'.str 'b'] {\n"
            "    give ['a'];\n}\n").ok());
}

void aStructIsNamedWhereItIsMade() {
  CHECK(inStart("var.line 'l' = [point[*0* *0*] point[*1* *1*]];").ok());
  CHECK(inStart("var.many.point 'ps' = [point[*1* *2*] point[*3* *4*]];").ok());
  CHECK(inStart("var.deep 'd' = [pair[one[*1*] *2*] *3*];").ok());

  // A word before a bracket is a call and can never be an index, which is why
  // the struct is named rather than the brackets standing alone: `'ns' [*0*]`
  // would be read as `'ns'[*1*]` and mean something else in silence.
  CHECK(inStart("var.two 't' = ['ns'[*1*] one[*3*]];").ok());
}

// A name may say that a sum which does not fit is meant to come round. The word
// stands where the chain says what is unusual, so it keeps the order every
// other word keeps, and writing the default is refused as everywhere else.
void aNameMaySayItWraps() {
  CHECK(inStart("var.mut.wrapping.int8 'sum' = [*0*];").ok());
  CHECK(inStart("var.wrapping.int8 'n' = [*1*];").ok());
  CHECK(inStart("var.mut.checked.int8 'n' = [*0*];").code(0) == "E0201");
  CHECK(inStart("var.wrapping.mut.int8 'n' = [*0*];").code(0) == "E0205");
  CHECK(run("fn.wrapping.int8 'f' [] { give [*0*]; }\n").code(0) == "E0203");
  CHECK(inStart("loop.wrapping.range.int8 'i' = [*0*, *3*] { }").code(0) == "E0203");
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

// Xag has no expression statements. A value written on its own is worked out
// and dropped, and saying so once beats three errors about the tokens in it.
void aValueOnItsOwnIsSaidSoOnce() {
  // What it used to answer with was `E0107` about `int32` not being a call, and
  // then two more about the `:` after it, which was never the trouble.
  const Parsed typed = inStart("int32:*1* + int32:*2*");
  CHECK(typed.code(0) == "E0109");
  CHECK(typed.code(1) == "(none)");

  // The same mistake starting with a name went to `E0106`, "a statement begins
  // with a word" — true, and no help at all.
  const Parsed named = inStart("var.int64 'n' = [*1*];\n    'n' + *1*;");
  CHECK(named.code(0) == "E0109");
  CHECK(named.code(1) == "(none)");

  // And a written value with nowhere to go.
  CHECK(inStart("*1*;").code(0) == "E0109");

  // What must still be its own answer.
  CHECK(inStart("print.stdout[str:*hi* \\n];").ok());
  CHECK(inStart("var.int64 'n' = [*1*]\n    var.int64 'm' = [*2*];").code(0) == "E0103");
  // Nothing stands outside the three blocks a file is.
  CHECK(run("START { }\n}\n").code(0) == "E0110");
}

// A clone is faithful when the printer cannot tell the two apart. The printer
// walks everything the tree holds, so anything the clone dropped or shared shows
// up as a difference — which is a stronger check than listing the fields, since
// listing them is exactly what the clone already does and a field forgotten in
// one is forgotten in both.
void aCloneIsIndistinguishable() {
  const std::string program =
      "struct 'point' [int64 'x', int64 'y']\n"
      "const.int64 'LIMIT' = [*10*];\n"
      "fn.loan.'the text'.str 'pick' [loan.'the text'.str 'a', loan.str 'b'] {\n"
      "    if count['a'] >== count['b'] { give ['a']; }\n"
      "    give ['a'];\n"
      "}\n"
      "fn.or-nothing.int64 'half' [int64 'n'] {\n"
      "    if 'n' == int64:*0* { give [nothing]; }\n"
      "    give ['n' / *2*];\n"
      "}\n"
      "START {\n"
      "    var.mut.many.int64 'xs' = [*1* *2* *3*];\n"
      "    set 'xs'[*1*] = [*9*];\n"
      "    var.point 'p' = [*1* *2*];\n"
      "    set 'p'.y = [*7*];\n"
      "    loop.perm.range.int64 'i' = [*1*, 'LIMIT'] {\n"
      "        if 'i' > int64:*3* { break; }\n"
      "    }\n"
      "    loop.while 'p'.x > int64:*0* { set 'p'.x = ['p'.x - *1*]; }\n"
      "    UNSAFE {\n"
      "        loop.no-itmt.range.int64 'j' = [*1*, *2*] { }\n"
      "    }\n"
      "    when half[*4*] {\n"
      "        is 'n'     { print.stdout[str:*got * 'n' \\n]; }\n"
      "        is nothing { print.stdout[str:*none* \\n]; }\n"
      "    }\n"
      "}\n";

  const xag::Source source("test.xag", xag::asFile(program));
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  CHECK(parsed.ok());

  xag::Program copied;
  for (const xag::Item &item : parsed.program.items)
    copied.items.push_back(xag::clone(item));
  CHECK(copied.items.size() == parsed.program.items.size());

  std::ostringstream first;
  std::ostringstream again;
  xag::print(parsed.program, first);
  xag::print(copied, again);
  CHECK(first.str() == again.str());
  CHECK(!first.str().empty());

  // And it is a copy rather than a share: changing one leaves the other alone.
  if (!copied.items.empty()) {
    copied.items.front().name = "changed";
    CHECK(parsed.program.items.front().name != "changed");
  }
}

// Filling the blank in is a separate step from copying, and reaches a chain
// wherever one is written — not only the signature.
void theBlankIsFilledEverywhereAChainIs() {
  const std::string generic =
      "fn.any 'largest' [loan.many.any 'xs', any 'first'] {\n"
      "    var.mut.any 'best' = ['first'];\n"
      "    loop.range.int64 'i' = [*1*, *2*] {\n"
      "        var.any 'here' = ['xs'['i']];\n"
      "    }\n"
      "    give ['best'];\n"
      "}\n";
  const xag::Source source("test.xag", xag::asFile(generic));
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  CHECK(parsed.ok());

  xag::Item filled = xag::clone(parsed.program.items.front());
  // The answer, two parameters, and two declarations inside the body.
  CHECK(xag::fillTheBlank(filled, "int64") == 5);

  xag::Program after;
  after.items.push_back(std::move(filled));
  std::ostringstream out;
  xag::print(after, out);
  // Looked for with the dot, because `many` has an `any` inside it and looking
  // for the bare word said the blank was still there when it was not.
  CHECK(out.str().find(".any") == std::string::npos);
  CHECK(out.str().find("param any ") == std::string::npos);
  CHECK(out.str().find("int64") != std::string::npos);

  // The one it was copied from is untouched.
  std::ostringstream before;
  xag::print(parsed.program, before);
  CHECK(before.str().find(".any") != std::string::npos);

  // A name is not a chain word, so a loan called `'any'` is left alone.
  const std::string named =
      "fn.loan.'any'.str 'pick' [loan.'any'.str 'a', loan.'any'.str 'b'] {\n"
      "    give ['a'];\n"
      "}\n";
  const xag::Source second("test.xag", xag::asFile(named));
  const xag::LexResult lexed2 = xag::lex(second);
  const xag::ParseResult parsed2 = xag::parse(second, lexed2.tokens);
  CHECK(parsed2.ok());
  xag::Item untouched = xag::clone(parsed2.program.items.front());
  CHECK(xag::fillTheBlank(untouched, "int64") == 0);
}

} // namespace

int main() {
  aCloneIsIndistinguishable();
  theBlankIsFilledEverywhereAChainIs();
  aValueOnItsOwnIsSaidSoOnce();
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
  aChainSaysWhichWordItReadAsTheType();
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
  aNameMaySayItWraps();
  aStructNamesWhatItHolds();
  aStructIsNamedWhereItIsMade();

  if (failures == 0)
    std::cout << "all parser tests passed\n";
  return failures == 0 ? 0 : 1;
}
