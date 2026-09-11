#include "xag/Units.h"

#include "xag/Lexer.h"
#include "xag/Parser.h"

#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                        \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #cond "\n";            \
      ++failures;                                                                        \
    }                                                                                    \
  } while (false)

const std::string kUnits = std::string(XAG_TESTS_DIR) + "/units/";

std::string firstCode(const xag::UnitsResult &r) {
  return r.diagnostics.empty() ? "(none)" : r.diagnostics.front().code;
}

// A library's manifest names it both ways, and a program's says where it is.
void aProgramReachesALibrary() {
  const xag::UnitsResult r = xag::unitsFor(kUnits + "program/main.xag");
  CHECK(r.ok());
  CHECK(r.libraries.size() == 1);
  CHECK(r.libraries.front().name == "text");
  CHECK(r.libraries.front().called == "t");
  CHECK(r.libraries.front().files.size() == 1);
  CHECK(xag::unitNamed(r, "text") != nullptr);
  CHECK(xag::unitNamed(r, "t") == nullptr); // the call name is not the import name
  // The program's own unit has no name; nothing imports it.
  CHECK(r.self.name.empty());
}

// A file with no manifest anywhere above it is a unit with nothing to say.
void noManifestIsNotAMistake() {
  const xag::UnitsResult r = xag::unitsFor("/tmp/nowhere-in-particular.xag");
  CHECK(r.ok());
  CHECK(r.libraries.empty());
}

// Two libraries that use each other are one thing wearing two names, and the
// walk refuses to go round.
void aCycleIsRefused() {
  const xag::UnitsResult r = xag::unitsFor(kUnits + "unnamed/main.xag");
  CHECK(!r.ok());
  bool missing = false, looped = false;
  for (const xag::Diagnostic &d : r.diagnostics) {
    missing = missing || d.code == "E0603";
    looped = looped || d.code == "E0604";
  }
  CHECK(missing); // `../nowhere`
  CHECK(looped);  // loop-a uses loop-b uses loop-a
  // Each diagnostic says which manifest it is about.
  CHECK(r.about.size() == r.diagnostics.size());
}

// A library's names are written under its call name: what it exports as
// `t.name`, which is what a use site writes, and what it keeps as `t$name`,
// which nothing can spell. Every reference inside it follows.
void aLibraryIsQualified() {
  const xag::Source source("lib.xag",
                           "READ_ME { }\nLIBRARY {\n"
                           "  struct 'pair' [int64 'a', int64 'b']\n"
                           "  struct.export 'point' [int64 'x', int64 'y']\n"
                           "  const.int64 'K' = [*3*];\n"
                           "  fn.int64 'helper' [int64 'n'] { give ['n' + 'K']; }\n"
                           "  fn.export.int64 'twice' [loan.point 'p'] {\n"
                           "    var.pair 'q' = [*1* *2*];\n"
                           "    give [helper['p'.x] + 'q'.a]; }\n"
                           "}\nITMT { }\n");
  const xag::LexResult lexed = xag::lex(source);
  xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  CHECK(lexed.ok() && parsed.ok());
  xag::Unit unit;
  unit.name = "text";
  unit.called = "t";
  xag::qualify(parsed.program, unit);
  std::ostringstream out;
  xag::print(parsed.program, out);
  const std::string tree = out.str();
  CHECK(tree.find("struct t$pair") != std::string::npos);
  CHECK(tree.find("struct t.point") != std::string::npos);
  CHECK(tree.find("'t$K'") != std::string::npos);
  CHECK(tree.find("fn fn.int64 t$helper") != std::string::npos);
  CHECK(tree.find("fn.export.int64 t.twice") != std::string::npos);
  // The chain that names `point` now names `t.point`, and `pair` `t$pair`.
  CHECK(tree.find("loan.t.point") != std::string::npos);
  CHECK(tree.find("var.t$pair") != std::string::npos);
  // A call to the helper, and the constant it reads.
  CHECK(tree.find("call t$helper") != std::string::npos);
  CHECK(tree.find("name 't$K'") != std::string::npos);
  // Nothing bare is left of any of them.
  CHECK(tree.find("call helper") == std::string::npos);
  CHECK(tree.find("name 'K'") == std::string::npos);
}

} // namespace

int main() {
  aLibraryIsQualified();
  aProgramReachesALibrary();
  noManifestIsNotAMistake();
  aCycleIsRefused();
  if (failures == 0)
    std::cout << "all units tests passed\n";
  return failures == 0 ? 0 : 1;
}
