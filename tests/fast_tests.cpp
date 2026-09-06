// The fast interpreter, held against the one that is meant to be believed.
//
// Every program here is run on both, and what they say must match exactly. It
// is a poor test that only checks the fast one against what somebody expected:
// the whole reason it exists is to be a second opinion, and a second opinion
// that was written from the first is not one.

#include "xag/Check.h"
#include "xag/Fast.h"
#include "xag/Interpret.h"
#include "xag/Lexer.h"
#include "xag/Mir.h"
#include "xag/Own.h"
#include "xag/Parser.h"
#include "xag_runtime.h"

#include <cstdio>
#include <iostream>
#include <string>

namespace {

int failures = 0;

struct Said {
  bool ran = false;
  std::string trouble;
  std::string out;
  int64_t leaked = 0;
};

Said capture(const xag::Mir &mir, bool quick) {
  Said said;
  const int64_t before = xag_live_allocations();
  std::FILE *sink = std::tmpfile();
  xag_set_output(sink);
  if (quick) {
    const xag::FastResult result = xag::runFast(mir);
    said.ran = result.ran;
    said.trouble = result.trouble;
  } else {
    const xag::InterpretResult result = xag::interpret(mir);
    said.ran = result.ran;
    said.trouble = result.trouble;
  }
  xag_set_output(nullptr);
  said.leaked = xag_live_allocations() - before;

  std::fflush(sink);
  std::rewind(sink);
  char buffer[8192];
  const size_t got = std::fread(buffer, 1, sizeof(buffer), sink);
  said.out.assign(buffer, got);
  std::fclose(sink);
  return said;
}

void agree(const std::string &text, int line) {
  const xag::Source source("test.xag", text);
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  const xag::OwnResult owned = xag::own(source, parsed.program);
  if (!lexed.ok() || !parsed.ok() || !checked.ok() || !owned.ok()) {
    std::cerr << "FAIL line " << line << ": the program did not compile\n";
    ++failures;
    return;
  }
  xag::MirResult built = xag::build(source, parsed.program, checked);
  xag::elaborate(built.mir);

  const Said slow = capture(built.mir, false);
  const Said quick = capture(built.mir, true);

  if (slow.out != quick.out || slow.ran != quick.ran) {
    std::cerr << "FAIL line " << line << ": the engines disagree\n"
              << "    test: \"" << slow.out << "\" (" << (slow.ran ? "ran" : slow.trouble)
              << ")\n"
              << "    fast: \"" << quick.out << "\" (" << (quick.ran ? "ran" : quick.trouble)
              << ")\n";
    ++failures;
  }
  if (quick.leaked != 0) {
    std::cerr << "FAIL line " << line << ": the fast engine ended holding "
              << quick.leaked << '\n';
    ++failures;
  }
}

#define AGREE(program) agree(program, __LINE__)

void onTheOrdinaryThings() {
  AGREE("START { print.stdout[str:*hello* \\n]; }\n");
  AGREE("START { var.str 'w' = [*world*];"
        " print.stdout[str:*Hello, * 'w' str:*!* \\n]; }\n");
  AGREE("START { var.int64 'n' = [*2* + *3* x *4*]; print.stdout['n' \\n]; }\n");
  AGREE("START { var.mut.int64 't' = [*0*];\n"
        "  loop.range.int64 'i' = [*1*, *10*] { set 't' = ['t' + 'i']; }\n"
        "  print.stdout['t' \\n]; }\n");
  AGREE("START { var.int64 'n' = [*5*];\n"
        "  if 'n' > *3* { print.stdout[str:*big* \\n]; }"
        "  else { print.stdout[str:*small* \\n]; } }\n");
  AGREE("START { loop.perm.range.int64 'i' = [*1*, *100*] {"
        " if 'i' x 'i' > *10* { break; } }"
        " print.stdout['i' \\n]; }\n");
  AGREE("START { loop.range.int64 'i' = [*1*, *100*] {"
        " if 'i' > *3* { break; } print.stdout['i' str:* *]; } }\n");
}

void onEverySizeAndFamily() {
  AGREE("START { var.mut.uint8 'n' = [*255*]; set 'n' = ['n' + *1*];"
        " print.stdout['n' \\n]; }\n");
  AGREE("START { var.mut.int8 'n' = [*127*]; set 'n' = ['n' + *1*];"
        " print.stdout['n' \\n]; }\n");
  AGREE("START { var.uint128 'n' = [*340282366920938463463374607431768211455*];"
        " print.stdout['n' \\n]; }\n");
  AGREE("START { var.bin64 'a' = [*0.1*]; var.bin64 'b' = [*0.2*];"
        " print.stdout[('a' + 'b') \\n]; }\n");
  AGREE("START { var.bin32 'a' = [*0.1*]; print.stdout[('a' x bin32:*3*) \\n]; }\n");
  AGREE("START { var.bin64 'z' = [*0*]; print.stdout[(bin64:*1* / 'z') str:* * ('z' / 'z') \\n]; }\n");
  AGREE("START { var.bin128 'a' = [*1e30*];"
        " print.stdout[('a' + bin128:*1*) \\n]; }\n");
  AGREE("START { var.deci64 'a' = [*0.1*]; var.deci64 'b' = [*0.2*];"
        " print.stdout[('a' + 'b') \\n]; }\n");
  AGREE("START { var.deci64 'p' = [*1.10*];"
        " print.stdout[('p' + deci64:*2.00*) \\n]; }\n");
  AGREE("START { print.stdout[(deci128:*1* / deci128:*3*) \\n]; }\n");
}

void onCallsAndBorrows() {
  AGREE("fn.int64 sum-to [int64 'n'] {\n"
        "  var.mut.int64 't' = [*0*];\n"
        "  loop.range.int64 'i' = [*1*, 'n'] { set 't' = ['t' + 'i']; }\n"
        "  give ['t']; }\n"
        "START { print.stdout[sum-to[*10*] \\n]; }\n");
  AGREE("fn.int64 size [ref.str 't'] { give [count['t']]; }\n"
        "START { var.str 's' = [*café*]; print.stdout[size[ref 's'] \\n]; }\n");
  AGREE("fn.nothing edit [refmut.str 't'] { set 't' = ['t' *!*]; }\n"
        "START { var.mut.str 's' = [*hi*]; edit[refmut 's'];"
        " print.stdout['s' \\n]; }\n");
  AGREE("fn.nothing keep [str 't'] { print.stdout['t' \\n]; }\n"
        "START { var.str 's' = [*taken*]; keep[move 's']; }\n");
  AGREE("fn.nothing keep [str 't'] { print.stdout['t' \\n]; }\n"
        "START { var.str 's' = [*hi*]; var.int64 'n' = [*1*];\n"
        "  if 'n' == *1* { keep[move 's']; } }\n");
  AGREE("fn.ref.'life'.str longer [ref.'life'.str 'a', ref.'life'.str 'b'] {\n"
        "  if count['a'] >== count['b'] { give ['a']; } else { give ['b']; } }\n"
        "START { var.str 'x' = [*hello*]; var.str 'y' = [*hi*];\n"
        "  var.ref.str 'w' = [longer[ref 'x', ref 'y']];\n"
        "  print.stdout['w' \\n]; }\n");
  AGREE("const.int64 'LIMIT' = [*10*];\n"
        "START { print.stdout['LIMIT' \\n]; }\n");
  AGREE("const.str 'GREETING' = [*Hello*];\n"
        "START { print.stdout['GREETING' str:*!* \\n]; }\n");
}

void onHoldingSeveralValues() {
  AGREE("START { var.many.int64 'xs' = [*10* *20* *30*];\n"
        "  print.stdout['xs'[*0*] str:* * 'xs'[*2*] str:* of * (count[ref 'xs']) \\n]; }\n");
  AGREE("START { var.mut.many.int64 'xs' = [fill[*3*, *5*]];\n"
        "  set 'xs'[*4*] = [*9*];\n"
        "  loop.range.int64 'i' = [*0*, *4*] { print.stdout['xs'['i'] \\n]; } }\n");
  AGREE("START { var.mut.many.str 'ws' = [*one* *two* *three*];\n"
        "  set 'ws'[*1*] = [*TWO*];\n"
        "  loop.range.int64 'i' = [*0*, (count[ref 'ws'] - *1*)] {\n"
        "    print.stdout['ws'['i'] str:* * (count['ws'['i']]) \\n]; } }\n");
  AGREE("fn.int64 total [ref.many.int64 'xs'] {\n"
        "  var.mut.int64 't' = [*0*];\n"
        "  loop.range.int64 'i' = [*0*, (count['xs'] - *1*)] {\n"
        "    set 't' = ['t' + 'xs'['i']]; }\n"
        "  give ['t']; }\n"
        "START { var.many.int64 'xs' = [*1* *2* *3* *4*];\n"
        "  print.stdout[total[ref 'xs'] \\n]; }\n");
  AGREE("START { var.many.int64 'none' = [];\n"
        "  print.stdout[(count[ref 'none']) \\n]; }\n");
}

void onHoldingNothing() {
  AGREE("fn.or-nothing.int64 half [int64 'n'] {\n"
        "  if 'n' == *0* { give [nothing]; }\n"
        "  give ['n' / *2*]; }\n"
        "START { loop.range.int64 'i' = [*0*, *4*] {\n"
        "  var.or-nothing.int64 'h' = [half['i']];\n"
        "  if 'h' holds 'v' { print.stdout['v' \\n]; } } }\n");
  AGREE("START { var.or-nothing.str 'a' = [*text*];\n"
        "  if 'a' holds 't' { print.stdout['t' str:* * (count['t']) \\n]; } }\n");
  AGREE("START { var.or-nothing.many.int64 'xs' = [*1* *2* *3*];\n"
        "  if 'xs' holds 'held' { print.stdout[(count[ref 'held']) \\n]; } }\n");
}

void onChoosingBetweenCases() {
  AGREE("fn.or-nothing.int64 half [int64 'n'] {\n"
        "  if 'n' == *0* { give [nothing]; }\n"
        "  give ['n' / *2*]; }\n"
        "START { loop.range.int64 'i' = [*0*, *4*] {\n"
        "  when half['i'] {\n"
        "    is 'v'     { print.stdout['i' str:* -> * 'v' \\n]; }\n"
        "    is nothing { print.stdout['i' str:* -> none* \\n]; } } } }\n");
  AGREE("START { var.or-nothing.str 's' = [*text*];\n"
        "  when 's' {\n"
        "    is nothing { print.stdout[str:*none* \\n]; }\n"
        "    is 't'     { print.stdout['t' str:* * (count['t']) \\n]; } } }\n");
}

void onGroupingNamedThings() {
  // A field read and a field written, with the rest left as it was.
  AGREE("struct point [int64 'x', int64 'y']\n"
        "START { var.mut.point 'p' = [*3* *4*];\n"
        "  print.stdout['p'.x str:* * 'p'.y \\n];\n"
        "  set 'p'.y = [*9*];\n"
        "  print.stdout['p'.x str:* * 'p'.y \\n]; }\n");
  // Text held by a struct is the struct's own, and let go with it.
  AGREE("struct tag [str 'name', int64 'runs']\n"
        "START { var.mut.tag 't' = [*ada* *36*];\n"
        "  print.stdout['t'.name str:* * 't'.runs \\n];\n"
        "  set 't'.name = [*bob*];\n"
        "  print.stdout['t'.name str:* * (count['t'.name]) \\n]; }\n");
  // A struct of structs, read and written down a path.
  AGREE("struct point [int64 'x', int64 'y']\n"
        "struct runner [str 'name', point 'at']\n"
        "fn.nothing bump [refmut.runner 'r'] { set 'r'.at.x = ['r'.at.x + *1*]; }\n"
        "START { var.mut.point 'a' = [*1* *2*];\n"
        "  var.mut.runner 'r' = [*ada* move 'a'];\n"
        "  bump[refmut 'r'];\n"
        "  print.stdout['r'.name str:* * 'r'.at.x str:* * 'r'.at.y \\n]; }\n");
  // One field handed over on its own, with the rest still there to read.
  AGREE("fn.nothing keep [str 't'] { print.stdout['t' \\n]; }\n"
        "struct tag [str 'name', int64 'runs']\n"
        "START { var.tag 't' = [*ada* *36*];\n"
        "  keep[move 't'.name];\n"
        "  print.stdout['t'.runs \\n]; }\n");
  // Structs in a `many`, one of them replaced.
  AGREE("struct tag [str 'name']\n"
        "START { var.tag 'a' = [*ada*];\n  var.tag 'b' = [*bob*];\n"
        "  var.mut.many.tag 'ts' = [move 'a' move 'b'];\n"
        "  var.tag 'c' = [*cy*];\n"
        "  set 'ts'[*0*] = [move 'c'];\n"
        "  print.stdout['ts'[*0*].name str:* * 'ts'[*1*].name \\n]; }\n");
  // A struct behind a loan, through a function.
  AGREE("struct point [int64 'x', int64 'y']\n"
        "fn.int64 across [ref.point 'p'] { give ['p'.x + 'p'.y]; }\n"
        "START { var.point 'p' = [*20* *22*];\n"
        "  print.stdout[(across[ref 'p']) \\n]; }\n");
  // A struct behind `or-nothing`, both ways, under `when`.
  AGREE("struct tag [str 'name']\n"
        "START { var.or-nothing.tag 't' = [*ada*];\n"
        "  when 't' {\n"
        "    is 'one'   { print.stdout['one'.name \\n]; }\n"
        "    is nothing { print.stdout[str:*none* \\n]; } }\n"
        "  var.or-nothing.tag 'u' = [nothing];\n"
        "  when 'u' {\n"
        "    is 'one'   { print.stdout['one'.name \\n]; }\n"
        "    is nothing { print.stdout[str:*none* \\n]; } } }\n");
}

} // namespace

int main() {
  onTheOrdinaryThings();
  onEverySizeAndFamily();
  onCallsAndBorrows();
  onHoldingSeveralValues();
  onHoldingNothing();
  onChoosingBetweenCases();
  onGroupingNamedThings();

  if (failures == 0)
    std::cout << "the two interpreters agree everywhere asked\n";
  return failures == 0 ? 0 : 1;
}
