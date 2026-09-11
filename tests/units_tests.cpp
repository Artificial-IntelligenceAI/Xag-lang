#include "xag/Units.h"

#include <iostream>
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

} // namespace

int main() {
  aProgramReachesALibrary();
  noManifestIsNotAMistake();
  aCycleIsRefused();
  if (failures == 0)
    std::cout << "all units tests passed\n";
  return failures == 0 ? 0 : 1;
}
