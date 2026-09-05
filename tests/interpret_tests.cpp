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
  SAYS("START { var.i64 'n' = [*2* + *3* x *4*]; print.stdout['n' \\n]; }\n", "14\n");
  SAYS("START { var.i64 'n' = [*2* ^ *10*]; print.stdout['n' \\n]; }\n", "1024\n");
  // `division = \"truncated\"`, decided once in Xag-Config.toml.
  SAYS("START { var.i64 'a' = [*0* - *7*]; var.i64 'n' = ['a' / *2*];"
       " print.stdout['n' \\n]; }\n", "-3\n");
  SAYS("START { var.i64 'a' = [*0* - *7*]; var.i64 'n' = ['a' mod *2*];"
       " print.stdout['n' \\n]; }\n", "-1\n");
}

void itDecides() {
  SAYS("START { var.i64 'n' = [*5*];\n"
       "  if 'n' > *3* { print.stdout[str:*big* \\n]; } else { print.stdout[str:*small* \\n]; } }\n",
       "big\n");
  SAYS("START { var.i64 'n' = [*1*];\n"
       "  if 'n' > *3* { print.stdout[str:*big* \\n]; }\n"
       "  else-if 'n' == *1* { print.stdout[str:*one* \\n]; }\n"
       "  else { print.stdout[str:*small* \\n]; } }\n",
       "one\n");
}

void itLoops() {
  SAYS("START { loop.range.i64 'i' = [*1*, *5*] { print.stdout['i' str:* *]; } }\n",
       "1 2 3 4 5 ");
  SAYS("START { var.mut.i64 'total' = [*0*];\n"
       "  loop.range.i64 'i' = [*1*, *10*] { set 'total' = ['total' + 'i']; }\n"
       "  print.stdout['total' \\n]; }\n",
       "55\n");
  SAYS("START { var.mut.i64 'n' = [*3*];\n"
       "  loop.while 'n' > *0* { print.stdout['n' str:* *]; set 'n' = ['n' - *1*]; } }\n",
       "3 2 1 ");
  SAYS("START { loop.range.i64 'i' = [*1*, *100*] {"
       " if 'i' > *3* { break; } print.stdout['i' str:* *]; } }\n",
       "1 2 3 ");
}

void itCalls() {
  SAYS("fn.i64 sum-to [i64 'n'] {\n"
       "  var.mut.i64 'total' = [*0*];\n"
       "  loop.range.i64 'i' = [*1*, 'n'] { set 'total' = ['total' + 'i']; }\n"
       "  give ['total'];\n}\n"
       "START { print.stdout[sum-to[*10*] \\n]; }\n",
       "55\n");
  // Two functions may call each other, since every signature is read first.
  SAYS("fn.i64 down [i64 'n'] { if 'n' <== *0* { give [*0*]; } give [up['n' - *1*]]; }\n"
       "fn.i64 up [i64 'n'] { give [down['n']]; }\n"
       "START { print.stdout[down[*3*] \\n]; }\n",
       "0\n");
}

void itKnowsItsConstants() {
  // A constant is a body that answers with its value, so naming one is a call.
  SAYS("const.i64 'LIMIT' = [*10*];\n"
       "START { print.stdout['LIMIT' \\n]; }\n", "10\n");
  SAYS("const.str 'GREETING' = [*Hello*];\n"
       "START { print.stdout['GREETING' str:*!* \\n]; }\n", "Hello!\n");
  // Used above where it stands, and used as an argument.
  SAYS("fn.i64 twice [i64 'n'] { give ['n' + 'n']; }\n"
       "const.i64 'LIMIT' = [*21*];\n"
       "START { print.stdout[twice['LIMIT'] \\n]; }\n", "42\n");
  // And written as an expression rather than only as a literal.
  SAYS("const.i64 'AREA' = [*3* x *4*];\n"
       "START { print.stdout['AREA' \\n]; }\n", "12\n");
}

void itLendsAndTakes() {
  SAYS("fn.i64 size [ref.str 'text'] { give [count['text']]; }\n"
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
                    "  var.i64 'n' = [*1*];\n"
                    "  if 'n' == *1* { keep[move 'b']; }\n"
                    "  print.stdout['a' \\n];\n"
                    "}\n");
  CHECK(r.compiled);
  CHECK(r.ran);
  CHECK(r.said == "two\none\n");
  CHECK(r.leaked == 0);
}

void aRunawayProgramIsStopped() {
  const Ran r = run("START { var.mut.i64 'n' = [*1*];"
                    " loop.while 'n' > *0* { set 'n' = ['n' + *1*]; } }\n");
  CHECK(r.compiled);
  CHECK(!r.ran);
  CHECK(r.trouble.find("longer than") != std::string::npos);
}

} // namespace

int main() {
  itPrints();
  itCounts();
  itDecides();
  itLoops();
  itCalls();
  itKnowsItsConstants();
  itLendsAndTakes();
  itEndsHoldingNothing();
  aRunawayProgramIsStopped();

  if (failures == 0)
    std::cout << "all interpreter tests passed\n";
  return failures == 0 ? 0 : 1;
}
