#include "xag/Check.h"
#include "xag/Lexer.h"
#include "xag/Mir.h"
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

struct Built {
  xag::Source source;
  xag::LexResult lexed;
  xag::ParseResult parsed;
  xag::CheckResult checked;
  xag::MirResult built;
  std::string text;

  bool clean() const { return lexed.ok() && parsed.ok() && checked.ok(); }
  bool has(const std::string &needle) const { return text.find(needle) != std::string::npos; }
  // The printout holds every body, so a claim about one has to say which.
  bool hasIn(const std::string &fn, const std::string &needle) const {
    const size_t from = text.find("fn " + fn + " ");
    if (from == std::string::npos)
      return false;
    size_t to = text.find("\nfn ", from + 1);
    if (to == std::string::npos)
      to = text.size();
    return text.substr(from, to - from).find(needle) != std::string::npos;
  }
  unsigned count(const std::string &needle) const {
    unsigned n = 0;
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1))
      ++n;
    return n;
  }
  const xag::Body &body(unsigned i) const { return built.mir.bodies[i]; }
};

Built run(const std::string &text, bool elaborate = true) {
  Built b{xag::Source("test.xag", text), {}, {}, {}, {}, {}};
  b.lexed = xag::lex(b.source);
  b.parsed = xag::parse(b.source, b.lexed.tokens);
  b.checked = xag::check(b.source, b.parsed.program);
  b.built = xag::build(b.source, b.parsed.program, b.checked);
  if (elaborate)
    xag::elaborate(b.built.mir);
  std::ostringstream out;
  xag::print(b.built.mir, out);
  b.text = out.str();
  return b;
}

Built inStart(const std::string &body) { return run("START {\n" + body + "\n}\n"); }
Built raw(const std::string &body) { return run("START {\n" + body + "\n}\n", false); }
const char *kKeep = "fn.nothing keep [str 'text'] { print.stdout['text' \n]; }\n";

void everyBodyBecomesBlocks() {
  const Built b = run("fn.i64 twice [i64 'n'] { give ['n' + 'n']; }\nSTART { }\n");
  CHECK(b.clean());
  CHECK(b.built.mir.bodies.size() == 2);
  CHECK(b.has("fn twice -> i64"));
  CHECK(b.has("fn START -> nothing"));
  CHECK(b.body(0).parameters == 1);
  // Local 0 is the answer, so a one-parameter body has at least two named slots.
  CHECK(b.body(0).locals.size() >= 2);
  CHECK(b.has("return"));
}

void expressionsBecomeTemporaries() {
  const Built b = inStart("var.i64 'n' = [*1* + *2* x *3*];");
  CHECK(b.clean());
  // The nested multiply lands in its own slot before the addition reads it.
  CHECK(b.has("*2* x *3*"));
  CHECK(b.has("+ copy"));
}

void anIfBecomesASwitchAndAJoin() {
  const Built b = inStart("var.mut.i64 'n' = [*0*];\n"
                          "    if 'n' == *0* { set 'n' = [*1*]; } else { set 'n' = [*2*]; }");
  CHECK(b.clean());
  CHECK(b.has("switch "));
  CHECK(b.count("goto block") >= 2); // both arms rejoin
}

void aLoopBecomesABackEdge() {
  const Built b = inStart("var.mut.i64 'total' = [*0*];\n"
                          "    loop.range.i64 'i' = [*1*, *3*] { set 'total' = ['total' + 'i']; }");
  CHECK(b.clean());
  CHECK(b.has("switch "));      // the header asks whether to run again
  CHECK(b.has("<== copy"));     // counter against its last value
  CHECK(b.has("+ *1*"));        // and steps on
}

void breakLeavesTheLoop() {
  const Built b = inStart("var.i64 'n' = [*0*];\n"
                          "    loop.while 'n' == *0* { break; }");
  CHECK(b.clean());
  CHECK(b.count("goto block") >= 2);
}

void switchIsGeneralFromTheStart() {
  // One value per target rather than a true/false pair, so a decision tree fits.
  const Built b = inStart("var.mut.i64 'n' = [*0*];\n    if 'n' == *0* { set 'n' = [*1*]; }");
  CHECK(b.clean());
  CHECK(b.has("[true -> block"));
  CHECK(b.has("[else -> block"));
}

void typesAreSymbolic() {
  const Built b = run("fn.str greet [ref.str 'who'] { give [*hi*]; }\nSTART { }\n");
  CHECK(b.clean());
  // A body carries its own table, and a loan is a type in it like any other.
  CHECK(!b.body(0).types.empty());
  CHECK(b.has(": ref str"));
}

void whatIsOwnedIsDropped() {
  const Built b = inStart("var.str 's' = [*hi*];");
  CHECK(b.clean());
  CHECK(b.has("drop "));
  // Small values are not owned in that sense, so nothing is dropped.
  CHECK(!inStart("var.i64 'n' = [*1*];").has("drop "));
}

void elaborationSettlesEveryDrop() {
  // Lowering places a drop at every scope end; elaboration decides each one.
  const Built before = raw(std::string("var.str 's' = [*hi*];\n    keep[move 's'];"));
  (void)before;

  // Certainly moved: nothing is left there to end.
  const Built gone = run(std::string(kKeep) +
                         "START { var.str 's' = [*hi*]; keep[move 's']; }\n");
  CHECK(gone.clean());
  CHECK(!gone.hasIn("START", "drop "));

  // Certainly not moved: the drop stands, and asks nothing first.
  const Built kept = inStart("var.str 's' = [*hi*];");
  CHECK(kept.has("drop "));
  CHECK(!kept.has(" if "));

  // Moved down one path only: a flag carries the answer to the drop.
  const Built maybe = run(std::string(kKeep) +
                          "START {\n"
                          "  var.str 's' = [*hi*];\n"
                          "  var.i64 'n' = [*1*];\n"
                          "  if 'n' == *1* { keep[move 's']; }\n"
                          "}\n");
  CHECK(maybe.clean());
  CHECK(maybe.hasIn("START", "drop "));
  CHECK(maybe.hasIn("START", " if "));
  CHECK(maybe.hasIn("START", "*false*"));
  CHECK(maybe.hasIn("START", "*true*"));
}

void aParameterTakenByValueEndsWithItsCallee() {
  const Built b = run("fn.nothing keep [str 'text'] { print.stdout['text' \n]; }\nSTART { }\n");
  CHECK(b.clean());
  CHECK(b.has("drop _1'text'"));
}

void joiningIsItsOwnStep() {
  const Built b = inStart("var.str 'a' = [*x*];\n    var.str 's' = [*hello * 'a' *!*];");
  CHECK(b.clean());
  CHECK(b.has("join("));
}

void aCallKeepsItsArguments() {
  const Built b = run("fn.i64 twice [i64 'n'] { give ['n' + 'n']; }\n"
                      "START { var.i64 'a' = [twice[*2*]]; }\n");
  CHECK(b.clean());
  CHECK(b.has("twice(*2*)"));
}

} // namespace

int main() {
  everyBodyBecomesBlocks();
  expressionsBecomeTemporaries();
  anIfBecomesASwitchAndAJoin();
  aLoopBecomesABackEdge();
  breakLeavesTheLoop();
  switchIsGeneralFromTheStart();
  typesAreSymbolic();
  whatIsOwnedIsDropped();
  elaborationSettlesEveryDrop();
  aParameterTakenByValueEndsWithItsCallee();
  joiningIsItsOwnStep();
  aCallKeepsItsArguments();

  if (failures == 0)
    std::cout << "all mir tests passed\n";
  return failures == 0 ? 0 : 1;
}
