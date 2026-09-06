#include "xag/Check.h"
#include "xag/Interpret.h"
#include "xag/Lexer.h"
#include "xag/Mir.h"
#include "xag/Own.h"
#include "xag/Parser.h"
#include "xag_runtime.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #cond "\n";            \
      ++failures;                                                                        \
    }                                                                                    \
  } while (false)

struct Ran {
  bool compiled = false;
  bool ran = false;
  std::string trouble;
  std::string said;
  int64_t leaked = 0;
};

Ran run(const std::string &text) {
  Ran out;
  const xag::Source source("test.xag", text);
  const xag::LexResult lexed = xag::lex(source);
  if (!lexed.ok())
    return out;
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (!parsed.ok())
    return out;
  const xag::CheckResult checked = xag::check(source, parsed.program);
  if (!checked.ok())
    return out;
  const xag::OwnResult owned = xag::own(source, parsed.program);
  if (!owned.ok())
    return out;
  out.compiled = true;

  xag::MirResult built = xag::build(source, parsed.program, checked);
  xag::elaborate(built.mir);

  const int64_t before = xag_live_allocations();
  std::FILE *sink = std::tmpfile();
  xag_set_output(sink);
  const xag::InterpretResult result = xag::interpret(built.mir);
  xag_set_output(nullptr);
  out.leaked = xag_live_allocations() - before;

  std::fflush(sink);
  std::rewind(sink);
  char buffer[4096];
  size_t got = std::fread(buffer, 1, sizeof(buffer), sink);
  out.said.assign(buffer, got);
  std::fclose(sink);

  out.ran = result.ran;
  out.trouble = result.trouble;
  return out;
}

void checkSays(const std::string &program, const std::string &expected, int line) {
  const Ran r = run(program);
  if (!r.compiled) {
    std::cerr << "FAIL line " << line << ": the program did not compile\n";
    ++failures;
    return;
  }
  if (!r.ran) {
    std::cerr << "FAIL line " << line << ": " << r.trouble << '\n';
    ++failures;
    return;
  }
  if (r.said != expected) {
    std::cerr << "FAIL line " << line << ": said \"" << r.said << "\", wanted \""
              << expected << "\"\n";
    ++failures;
  }
  if (r.leaked != 0) {
    std::cerr << "FAIL line " << line << ": ended holding " << r.leaked << '\n';
    ++failures;
  }
}

#define SAYS(program, expected) checkSays(program, expected, __LINE__)

void itPrints() {
  SAYS("START { print.stdout[str:*hello* \\n]; }\n", "hello\n");
  SAYS("START { var.str 'who' = [*world*];\n"
       "  print.stdout[str:*Hello, * 'who' str:*!* \\n]; }\n",
       "Hello, world!\n");
}

void itCounts() {
  SAYS("START { var.int64 'n' = [*2* + *3* x *4*]; print.stdout['n' \\n]; }\n", "14\n");
  SAYS("START { var.int64 'n' = [*2* ^ *10*]; print.stdout['n' \\n]; }\n", "1024\n");
  // `division = \"truncated\"`, decided once in Xag-Config.toml.
  SAYS("START { var.int64 'a' = [*0* - *7*]; var.int64 'n' = ['a' / *2*];"
       " print.stdout['n' \\n]; }\n", "-3\n");
  SAYS("START { var.int64 'a' = [*0* - *7*]; var.int64 'n' = ['a' mod *2*];"
       " print.stdout['n' \\n]; }\n", "-1\n");
}

void itWrapsAtTheWidthItWasWritten() {
  // `overflow = "wrap"`, and the width that wraps is the written one.
  SAYS("START { var.mut.uint8 'n' = [*255*]; set 'n' = ['n' + *1*];"
       " print.stdout['n' \\n]; }\n", "0\n");
  SAYS("START { var.mut.int8 'n' = [*127*]; set 'n' = ['n' + *1*];"
       " print.stdout['n' \\n]; }\n", "-128\n");
  SAYS("START { var.mut.uint16 'n' = [*0*]; set 'n' = ['n' - *1*];"
       " print.stdout['n' \\n]; }\n", "65535\n");
  // A wider one does not wrap where a narrower one would.
  SAYS("START { var.int32 'n' = [*127* + *1*]; print.stdout['n' \\n]; }\n", "128\n");
}

void itComparesAsTheTypeSaysToCompare() {
  // 200 in a uint8 is two hundred, not minus fifty-six.
  SAYS("START { var.uint8 'n' = [*200*];\n"
       "  if 'n' > *100* { print.stdout[str:*bigger* \\n]; }"
       "  else { print.stdout[str:*smaller* \\n]; } }\n",
       "bigger\n");
  SAYS("START { var.int8 'n' = [*-56*];\n"
       "  if 'n' > *100* { print.stdout[str:*bigger* \\n]; }"
       "  else { print.stdout[str:*smaller* \\n]; } }\n",
       "smaller\n");
}

void itCountsPastSixtyFourBits() {
  SAYS("START { var.uint128 'n' = [*340282366920938463463374607431768211455*];"
       " print.stdout['n' \\n]; }\n",
       "340282366920938463463374607431768211455\n");
  SAYS("START { var.int128 'n' = [*-170141183460469231731687303715884105728*];"
       " print.stdout['n' \\n]; }\n",
       "-170141183460469231731687303715884105728\n");
}

void itDoesIEEEBinary() {
  SAYS("START { var.bin64 'a' = [*1.5*]; var.bin64 'b' = [*0.1*];"
       " print.stdout[('a' + 'b') \\n]; }\n", "1.6\n");
  // Nothing stops: infinity and not-a-number are values of the type.
  SAYS("START { var.bin64 'z' = [*0*]; print.stdout[(bin64:*1* / 'z') \\n]; }\n",
       "infinity\n");
  SAYS("START { var.bin64 'z' = [*0*]; print.stdout[('z' / 'z') \\n]; }\n",
       "not-a-number\n");
  SAYS("START { var.bin64 'z' = [*0*]; print.stdout[(bin64:*-1* / 'z') \\n]; }\n",
       "-infinity\n");
  // A not-a-number is equal to nothing at all, itself included.
  SAYS("START { var.bin64 'z' = [*0*]; var.bin64 'n' = ['z' / 'z'];\n"
       "  if 'n' == 'n' { print.stdout[str:*equal* \\n]; }"
       "  else { print.stdout[str:*not equal* \\n]; } }\n",
       "not equal\n");
  // A narrower `bin` is cut back after every step, not only when stored.
  SAYS("START { var.bin32 'a' = [*0.1*]; print.stdout[('a' x bin32:*3*) \\n]; }\n",
       "0.3\n");
  SAYS("START { var.bin64 'a' = [*0.1*]; print.stdout[('a' x bin64:*3*) \\n]; }\n",
       "0.30000000000000004\n");
}

void itHoldsWhatABin64Cannot() {
  SAYS("START { var.bin128 'a' = [*1e30*];"
       " print.stdout[('a' + bin128:*1*) \\n]; }\n",
       "1.000000000000000000000000000001e+30\n");
  // The same sum in a bin64 loses the one, and says so.
  SAYS("START { var.bin64 'a' = [*1e30*];"
       " print.stdout[('a' + bin64:*1* - bin64:*1e30*) \\n]; }\n", "0\n");
  SAYS("START { var.bin128 'a' = [*1e30*];"
       " print.stdout[('a' + bin128:*1* - bin128:*1e30*) \\n]; }\n", "1\n");
  SAYS("START { print.stdout[(bin128:*1* / bin128:*3*) \\n]; }\n",
       "0.3333333333333333333333333333333333\n");
  SAYS("START { print.stdout[(bin128:*1* / bin128:*0*) \\n]; }\n", "infinity\n");
}

void itCountsInTensWhenAsked() {
  SAYS("START { var.deci64 'a' = [*0.1*]; var.deci64 'b' = [*0.2*];"
       " print.stdout[('a' + 'b') \\n]; }\n", "0.3\n");
  SAYS("START { var.bin64 'a' = [*0.1*]; var.bin64 'b' = [*0.2*];"
       " print.stdout[('a' + 'b') \\n]; }\n", "0.30000000000000004\n");
  // A price keeps its places through a sum.
  SAYS("START { var.deci64 'p' = [*1.10*];"
       " print.stdout[('p' + deci64:*2.00*) \\n]; }\n", "3.10\n");
  SAYS("START { print.stdout[(deci64:*1* / deci64:*8*) \\n]; }\n", "0.125\n");
  SAYS("START { print.stdout[deci128:*1234567890123456789012345678901234* \\n]; }\n",
       "1234567890123456789012345678901234\n");
}

void itDecides() {
  SAYS("START { var.int64 'n' = [*5*];\n"
       "  if 'n' > *3* { print.stdout[str:*big* \\n]; } else { print.stdout[str:*small* \\n]; } }\n",
       "big\n");
  SAYS("START { var.int64 'n' = [*1*];\n"
       "  if 'n' > *3* { print.stdout[str:*big* \\n]; }\n"
       "  else-if 'n' == *1* { print.stdout[str:*one* \\n]; }\n"
       "  else { print.stdout[str:*small* \\n]; } }\n",
       "one\n");
}

void itLoops() {
  SAYS("START { loop.range.int64 'i' = [*1*, *5*] { print.stdout['i' str:* *]; } }\n",
       "1 2 3 4 5 ");
  SAYS("START { var.mut.int64 'total' = [*0*];\n"
       "  loop.range.int64 'i' = [*1*, *10*] { set 'total' = ['total' + 'i']; }\n"
       "  print.stdout['total' \\n]; }\n",
       "55\n");
  SAYS("START { var.mut.int64 'n' = [*3*];\n"
       "  loop.while 'n' > *0* { print.stdout['n' str:* *]; set 'n' = ['n' - *1*]; } }\n",
       "3 2 1 ");
  SAYS("START { loop.range.int64 'i' = [*1*, *100*] {"
       " if 'i' > *3* { break; } print.stdout['i' str:* *]; } }\n",
       "1 2 3 ");
}

void aPermCounterKeepsWhatItHad() {
  // What a `break` left behind, which is the only reason to keep a counter.
  SAYS("START { loop.perm.range.int64 'i' = [*1*, *100*] {"
       " if 'i' x 'i' > *10* { break; } }\n"
       "  print.stdout['i' \\n]; }\n", "4\n");
  // And one past the last, when it simply ran out.
  SAYS("START { loop.perm.range.int64 'i' = [*1*, *3*] { }"
       " print.stdout['i' \\n]; }\n", "4\n");
}

void itCalls() {
  SAYS("fn.int64 sum-to [int64 'n'] {\n"
       "  var.mut.int64 'total' = [*0*];\n"
       "  loop.range.int64 'i' = [*1*, 'n'] { set 'total' = ['total' + 'i']; }\n"
       "  give ['total'];\n}\n"
       "START { print.stdout[sum-to[*10*] \\n]; }\n",
       "55\n");
  // Two functions may call each other, since every signature is read first.
  SAYS("fn.int64 down [int64 'n'] { if 'n' <== *0* { give [*0*]; } give [up['n' - *1*]]; }\n"
       "fn.int64 up [int64 'n'] { give [down['n']]; }\n"
       "START { print.stdout[down[*3*] \\n]; }\n",
       "0\n");
}

void itKnowsItsConstants() {
  // A constant is a body that answers with its value, so naming one is a call.
  SAYS("const.int64 'LIMIT' = [*10*];\n"
       "START { print.stdout['LIMIT' \\n]; }\n", "10\n");
  SAYS("const.str 'GREETING' = [*Hello*];\n"
       "START { print.stdout['GREETING' str:*!* \\n]; }\n", "Hello!\n");
  // Used above where it stands, and used as an argument.
  SAYS("fn.int64 twice [int64 'n'] { give ['n' + 'n']; }\n"
       "const.int64 'LIMIT' = [*21*];\n"
       "START { print.stdout[twice['LIMIT'] \\n]; }\n", "42\n");
  // And written as an expression rather than only as a literal.
  SAYS("const.int64 'AREA' = [*3* x *4*];\n"
       "START { print.stdout['AREA' \\n]; }\n", "12\n");
}

void itLendsAndTakes() {
  SAYS("fn.int64 size [ref.str 'text'] { give [count['text']]; }\n"
       "START { var.str 's' = [*café*]; print.stdout[size[ref 's'] \\n]; }\n",
       "4\n");
  SAYS("fn.nothing shout [refmut.str 'text'] { set 'text' = ['text' *!*]; }\n"
       "START { var.mut.str 's' = [*hi*]; shout[refmut 's'];"
       " print.stdout['s' \\n]; }\n",
       "hi!\n");
  SAYS("fn.nothing keep [str 'text'] { print.stdout['text' \\n]; }\n"
       "START { var.str 's' = [*taken*]; keep[move 's']; }\n",
       "taken\n");
}

void itEndsHoldingNothing() {
  // Every one of the above also asserts the balance; this one says so out loud.
  const Ran r = run("fn.nothing keep [str 'text'] { print.stdout['text' \\n]; }\n"
                    "START {\n"
                    "  var.str 'a' = [*one*];\n"
                    "  var.str 'b' = [*two*];\n"
                    "  var.int64 'n' = [*1*];\n"
                    "  if 'n' == *1* { keep[move 'b']; }\n"
                    "  print.stdout['a' \\n];\n"
                    "}\n");
  CHECK(r.compiled);
  CHECK(r.ran);
  CHECK(r.said == "two\none\n");
  CHECK(r.leaked == 0);
}

void aRunawayProgramIsStopped() {
  const Ran r = run("START { var.mut.int64 'n' = [*1*];"
                    " loop.while 'n' > *0* { set 'n' = ['n' + *1*]; } }\n");
  CHECK(r.compiled);
  CHECK(!r.ran);
  CHECK(r.trouble.find("longer than") != std::string::npos);
}

void itHoldsSeveralValues() {
  SAYS("START { var.many.int64 'xs' = [*10* *20* *30*];\n"
       "  print.stdout['xs'[*0*] str:* * 'xs'[*2*] str:* of * (count[ref 'xs']) \\n]; }\n",
       "10 30 of 3\n");
  SAYS("START { var.mut.many.int64 'xs' = [*1* *2*];\n"
       "  set 'xs'[*1*] = [*9*];\n"
       "  print.stdout['xs'[*1*] \\n]; }\n", "9\n");
  SAYS("START { var.many.int64 'xs' = [fill[*7*, *4*]];\n"
       "  print.stdout['xs'[*3*] str:* of * (count[ref 'xs']) \\n]; }\n", "7 of 4\n");
  SAYS("START { var.many.int64 'none' = [];\n"
       "  print.stdout[(count[ref 'none']) \\n]; }\n", "0\n");
}

void itEndsEveryPlaceItHeld() {
  // The balance is checked after every one of these, so a `many` of text that
  // let go of only its buffer would be caught here.
  SAYS("START { var.many.str 'ws' = [*one* *two* *three*];\n"
       "  print.stdout['ws'[*1*] \\n]; }\n", "two\n");
  SAYS("START { var.mut.many.str 'ws' = [*one* *two*];\n"
       "  set 'ws'[*0*] = [*ONE*];\n"
       "  print.stdout['ws'[*0*] str:* * 'ws'[*1*] \\n]; }\n", "ONE two\n");
  SAYS("START { var.str 's' = [*taken*];\n"
       "  var.many.str 'ws' = [*a* move 's'];\n"
       "  print.stdout['ws'[*1*] \\n]; }\n", "taken\n");
}

void itCountsWhatEachPlaceHolds() {
  SAYS("START { var.many.str 'ws' = [*café* *🧑‍🧑‍🧒‍🧒*];\n"
       "  print.stdout[(count['ws'[*0*]]) str:* * (count['ws'[*1*]]) \\n]; }\n",
       "4 1\n");
}

void itHoldsSomethingOrNothing() {
  SAYS("fn.or-nothing.str pick [int64 'n'] {\n"
       "  if 'n' > *0* { give [*yes*]; }\n"
       "  give [nothing]; }\n"
       "START {\n"
       "  var.or-nothing.str 'a' = [pick[*1*]];\n"
       "  if 'a' holds 't' { print.stdout['t' \\n]; }\n"
       "  var.or-nothing.str 'b' = [pick[*0*]];\n"
       "  if 'b' holds 't' { print.stdout[str:*wrong* \\n]; }\n"
       "  print.stdout[str:*done* \\n]; }\n",
       "yes\ndone\n");

  // An absence that holds text lets go of it, and one that holds none has
  // nothing to let go of — both have to leave the balance clear.
  SAYS("START { var.or-nothing.str 'a' = [*held*];\n"
       "  if 'a' holds 't' { print.stdout[(count['t']) \\n]; } }\n", "4\n");
  SAYS("START { var.or-nothing.str 'a' = [nothing];\n"
       "  print.stdout[str:*fine* \\n]; }\n", "fine\n");
}

} // namespace

int main() {
  itPrints();
  itCounts();
  itWrapsAtTheWidthItWasWritten();
  itComparesAsTheTypeSaysToCompare();
  itCountsPastSixtyFourBits();
  itDoesIEEEBinary();
  itHoldsWhatABin64Cannot();
  itCountsInTensWhenAsked();
  itDecides();
  itLoops();
  aPermCounterKeepsWhatItHad();
  itCalls();
  itKnowsItsConstants();
  itLendsAndTakes();
  itEndsHoldingNothing();
  aRunawayProgramIsStopped();
  itHoldsSeveralValues();
  itEndsEveryPlaceItHeld();
  itCountsWhatEachPlaceHolds();
  itHoldsSomethingOrNothing();

  if (failures == 0)
    std::cout << "all interpreter tests passed\n";
  return failures == 0 ? 0 : 1;
}
