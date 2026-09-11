#pragma once

#include "xag/Diagnostic.h"
#include "xag/Source.h"

#include <memory>
#include <string>
#include <vector>

namespace xag {

// A unit is a set of files with a name: a library, or the program itself. Both
// have a `Xag-Config.toml`, and what that manifest says is what this reads.
//
// The library's manifest says what it is called, both ways:
//
//     [unit]
//     name = "text"        what an importer writes:  import 'text';
//     called = "t"         what a use site writes:   t.count-of[…]
//
// The program's says only where things are:
//
//     [uses]
//     paths = ["../text", "../net"]
//
// The compiler reads each library's own manifest to learn what to call it. The
// split is by who owns the fact: an author names their library, a program says
// which libraries it reaches for.
struct Unit {
  std::string name;                // `import 'name';`
  std::string called;              // `called.thing[…]`
  std::string directory;           // where its files are
  std::vector<std::string> files;  // every `.xag` in that directory
  std::vector<std::string> uses;   // the `[uses]` paths of its own manifest
  // The manifest, kept so a diagnostic about it can point into it.
  std::shared_ptr<Source> manifest;
  Span nameSpan, calledSpan;
};

struct UnitsResult {
  // The unit the entry file belongs to. A program's `[unit]` is optional — a
  // program that nothing imports needs no name.
  Unit self;
  // Every library reachable from `self`, each once, in an order where nothing
  // comes before what it uses. Refused before this is filled if there is a
  // cycle, because then no such order exists.
  std::vector<Unit> libraries;
  std::vector<Diagnostic> diagnostics;
  // Which manifest each diagnostic points into, alongside `diagnostics`. A
  // report renders a diagnostic against one source, and these are about a
  // file that is not the one being compiled.
  std::vector<std::shared_ptr<Source>> about;
  bool ok() const { return !anyErrors(diagnostics); }
};

// Reads the manifest beside `sourcePath` — or upward from it, so a project
// decides for every file under it — and follows every `[uses]` path from there.
UnitsResult unitsFor(const std::string &sourcePath);

// The unit `import 'name';` reaches, or nothing.
const Unit *unitNamed(const UnitsResult &units, const std::string &name);

} // namespace xag
