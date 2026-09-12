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
  CHECK(missing); // `../nowhere.xaglib`
  CHECK(looped);  // loop-a uses loop-b uses loop-a
  // Each diagnostic says which file it is about.
  CHECK(r.about.size() == r.diagnostics.size());
}

// A program of several files: `main` and `files` in the manifest, `main`
// first. A library named directly is its own unit.
void aProgramIsTheFilesItsManifestNames() {
  const xag::UnitsResult r = xag::unitsFor(kUnits + "twofile/other.xag");
  CHECK(r.ok());
  CHECK(r.self.files.size() == 2);
  CHECK(r.self.files.size() == 2 &&
        r.self.files.front().find("main.xag") != std::string::npos &&
        r.self.files.back().find("other.xag") != std::string::npos);
  CHECK(!r.self.library);
  const xag::UnitsResult stray = xag::unitsFor(kUnits + "twofile/stray.xag");
  CHECK(!stray.ok());
  CHECK(!stray.diagnostics.empty() && stray.diagnostics.front().code == "E0618");

  const xag::UnitsResult lib = xag::unitsFor(kUnits + "flooring.xaglib");
  CHECK(lib.ok());
  CHECK(lib.self.library);
  CHECK(lib.self.name == "flooring" && lib.self.called == "fl");
  CHECK(lib.self.settings.floored && !lib.self.settings.asksBoth);
  CHECK(lib.self.files.size() == 1);
}

// A library's names are written under its call name: what it exports as
// `t.name`, which is what a use site writes, and what it keeps as `t$name`,
// which nothing can spell. Every reference inside it follows.
void aLibraryIsQualified() {
  const xag::Source source("lib.xaglib",
                           "READ_ME { }\nLIBRARY 'text' called 't' {\n"
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
  std::vector<xag::Program> files;
  files.push_back(std::move(parsed.program));
  xag::qualify(files, unit);
  std::ostringstream out;
  xag::print(files.front(), out);
  const std::string tree = out.str();
  // `file` is the default, so what says nothing is this file's alone — the
  // first file, so `t$0$`.
  CHECK(tree.find("struct t$0$pair") != std::string::npos);
  CHECK(tree.find("struct t.point") != std::string::npos);
  CHECK(tree.find("'t$0$K'") != std::string::npos);
  CHECK(tree.find("fn fn.int64 t$0$helper") != std::string::npos);
  CHECK(tree.find("fn.export.int64 t.twice") != std::string::npos);
  // The chain that names `point` now names `t.point`, and `pair` `t$0$pair`.
  CHECK(tree.find("loan.t.point") != std::string::npos);
  CHECK(tree.find("var.t$0$pair") != std::string::npos);
  // A call to the helper, and the constant it reads.
  CHECK(tree.find("call t$0$helper") != std::string::npos);
  CHECK(tree.find("name 't$0$K'") != std::string::npos);
  // Nothing bare is left of any of them.
  CHECK(tree.find("call helper") == std::string::npos);
  CHECK(tree.find("name 'K'") == std::string::npos);
}

// A program's files are renamed together under their visibility words. A
// program exports nothing — nothing imports one — so `export` and `program`
// both leave the name as written, and a `file`-visible name is walled off
// with `$` and no call name in front. Every reference inside follows.
void twoFilesKeepTheirOwnNames() {
  auto file = [](const char *text) {
    const xag::Source source("f.xag", text);
    const xag::LexResult lexed = xag::lex(source);
    return xag::parse(source, lexed.tokens).program;
  };
  std::vector<xag::Program> files;
  files.push_back(file("READ_ME { }\nPREP {\n"
                       "  fn.int64 'helper' [int64 'n'] { give ['n']; }\n"
                       "  fn.program.int64 'shared' [int64 'n'] { give [helper['n']]; }\n"
                       "  fn.export.int64 'answer' [int64 'n'] { give [shared['n']]; }\n"
                       "}\nSTART { }\nITMT { }\n"));
  files.push_back(file("READ_ME { }\nPREP {\n"
                       "  fn.int64 'helper' [int64 'n'] { give ['n']; }\n"
                       "  fn.int64 'peek' [int64 'n'] { give [helper['n'] + shared['n'] + "
                       "answer['n'] + secret['n']]; }\n"
                       "}\nSTART { }\nITMT { }\n"));
  xag::Unit unit; // a program: no name, no call name
  xag::qualify(files, unit);
  std::ostringstream a, b;
  xag::print(files[0], a);
  xag::print(files[1], b);
  // Each file's `helper` is its own.
  CHECK(a.str().find("fn fn.int64 $0$helper") != std::string::npos);
  CHECK(b.str().find("fn fn.int64 $1$helper") != std::string::npos);
  CHECK(b.str().find("call $1$helper") != std::string::npos);
  CHECK(b.str().find("call $0$helper") == std::string::npos);
  // `program` and `export` reach across, as written.
  CHECK(b.str().find("call shared") != std::string::npos);
  CHECK(b.str().find("call answer") != std::string::npos);
  // A name the other file kept to itself is left as written, for the checker
  // to say is not there.
  CHECK(b.str().find("call secret") != std::string::npos);
}

} // namespace

// `t.'LIMIT'` — a library's constant from outside. The parser hands it on as
// the one name `t.LIMIT`, which is what the library's exported `'LIMIT'` is
// renamed to, and `importsCover` sees the prefix in it.
void aLibrarysConstantIsAPrefixedName() {
  const std::vector<std::string> prefixes{"t"};
  {
    const xag::Source source("main.xag",
                             "READ_ME { }\nPREP { import 'text'; }\n"
                             "START { print.stdout[t.'LIMIT' str:* * t.'ORIGIN'.x \\n]; }\n"
                             "ITMT { }\n");
    const xag::LexResult lexed = xag::lex(source);
    xag::ParseResult parsed = xag::parse(source, lexed.tokens, prefixes);
    CHECK(lexed.ok() && parsed.ok());
    std::ostringstream out;
    xag::print(parsed.program, out);
    const std::string tree = out.str();
    CHECK(tree.find("name 't.LIMIT'") != std::string::npos);
    CHECK(tree.find("name 't.ORIGIN'") != std::string::npos);
    CHECK(tree.find("field x") != std::string::npos);
  }
  // Without the import, the prefix in the name is a reach the file did not
  // say it makes.
  {
    const xag::Source source("main.xag",
                             "READ_ME { }\nPREP { }\n"
                             "START { print.stdout[t.'LIMIT' \\n]; }\n"
                             "ITMT { }\n");
    const xag::LexResult lexed = xag::lex(source);
    xag::ParseResult parsed = xag::parse(source, lexed.tokens, prefixes);
    CHECK(parsed.ok());
    xag::UnitsResult units;
    xag::Unit text;
    text.name = "text";
    text.called = "t";
    units.libraries.push_back(text);
    const std::vector<xag::Diagnostic> said = xag::importsCover(parsed.program, units);
    CHECK(said.size() == 1);
    CHECK(!said.empty() && said.front().code == "E0608");
  }
}

// `everywhere` and `different = "refused"` in the program's manifest.
void aProgramMayOverrideItsLibrariesSettings() {
  const xag::UnitsResult over = xag::unitsFor(kUnits + "override-everywhere/main.xag");
  CHECK(over.ok());
  CHECK(over.self.everywhere.division && !over.self.everywhere.logic);
  CHECK(over.self.refusesDifferent);
  CHECK(over.libraries.size() == 1 && !over.libraries.front().settings.floored);

  const xag::UnitsResult refused = xag::unitsFor(kUnits + "override-refused/main.xag");
  CHECK(!refused.ok());
  CHECK(!refused.diagnostics.empty() && refused.diagnostics.front().code == "E0620");
  // Pointing into the library, at its name.
  CHECK(!refused.about.empty() && refused.about.front() &&
        refused.about.front()->name().find("flooring.xaglib") != std::string::npos);

  const xag::UnitsResult agreeing = xag::unitsFor(kUnits + "override-agreeing/main.xag");
  CHECK(agreeing.ok());
  CHECK(agreeing.libraries.size() == 1 && agreeing.libraries.front().settings.floored);
}

int main() {
  aProgramMayOverrideItsLibrariesSettings();
  aProgramIsTheFilesItsManifestNames();
  aLibrarysConstantIsAPrefixedName();
  twoFilesKeepTheirOwnNames();
  aLibraryIsQualified();
  aProgramReachesALibrary();
  noManifestIsNotAMistake();
  aCycleIsRefused();
  if (failures == 0)
    std::cout << "all units tests passed\n";
  return failures == 0 ? 0 : 1;
}
