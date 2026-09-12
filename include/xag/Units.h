#pragma once

#include "xag/Ast.h"
#include "xag/Diagnostic.h"
#include "xag/Source.h"

#include <memory>
#include <string>
#include <vector>

namespace xag {

// A unit is a program or a library.
//
// A program is the files its manifest lists, and its manifest is where it says
// what it uses and what it decided:
//
//     [unit]
//     main = "main.xag"                  the one file whose START runs
//     files = ["helpers.xag"]            the rest of it
//     [uses]
//     paths = ["../text.xaglib"]         what its files may import
//     [defaults]
//     division = "floored"
//
// A library is one `.xaglib` file and has no manifest. Everything it says about
// itself is on its `LIBRARY` line:
//
//     LIBRARY.floored 'text' called 't' uses [*../net.xaglib*] { … }
//
// The program's manifest lists every library the program reaches, the ones
// its libraries use included; a library reached only through another is
// written into the manifest by the compiler, which says so.
struct Unit {
  std::string name;                // `import 'name';` — empty for a program
  std::string called;              // `called.thing[…]` — empty for a program
  std::string directory;           // where its files are; paths are written against it
  std::vector<std::string> files;  // a program's, `main` first; a library's one file
  std::vector<std::string> uses;   // paths, as written
  Settings settings;               // the manifest's `[defaults]`, or the `LIBRARY` line's
  // A program's manifest may say a setting is its own in every library too —
  // `division = "floored everywhere"` — and that a library whose settings
  // differ from the program's is refused — `different = "refused"`. A library
  // says neither. What `everywhere` says wins before `different` is asked, since
  // an overridden setting cannot differ.
  struct Everywhere {
    bool logic = false, division = false, characters = false, noNumber = false;
  } everywhere;
  bool refusesDifferent = false;
  bool library = false;
  // What a diagnostic about the unit points into: a program's manifest, or
  // the library's own file.
  std::shared_ptr<Source> manifest;
  Span nameSpan, calledSpan;
  std::vector<Span> usesSpans;
};

struct UnitsResult {
  // The unit the entry file belongs to.
  Unit self;
  // Every library reachable from `self`, each once, in an order where nothing
  // comes before what it uses. Refused before this is filled if there is a
  // cycle, because then no such order exists.
  std::vector<Unit> libraries;
  std::vector<Diagnostic> diagnostics;
  // Which source each diagnostic points into, alongside `diagnostics`. A
  // report renders a diagnostic against one source, and these are about a
  // file that is not the one being compiled.
  std::vector<std::shared_ptr<Source>> about;
  // What the compiler did on its own and should say: a library written into
  // the manifest because another library used it.
  std::vector<std::string> notes;
  bool ok() const { return !anyErrors(diagnostics); }
};

// Reads the manifest beside `sourcePath` — or upward from it, so a project
// decides for every file under it — and follows every `[uses]` path from there,
// and every `uses` of every library reached. A `.xaglib` named directly is a
// library built alone: it is its own unit, and its `uses` are followed.
UnitsResult unitsFor(const std::string &sourcePath);

// The unit `import 'name';` reaches, or nothing.
const Unit *unitNamed(const UnitsResult &units, const std::string &name);

// Writes a library's call name onto everything it declares, so that its files
// can be read alongside a program's as one program and nothing collides. All
// of a library's files together, because what a name is renamed to depends on
// who may see it:
//
//   fn.export.int64 'twice'    →  t.twice      what a use site writes
//   fn.program.int64 'shared'  →  t$shared     every file of the library
//   fn.int64 'helper'          →  t$2$helper   this file — the third — alone
//
// `$` is not a word character, so nothing outside can spell the last two. A
// `file`-visible name reached from another file of the same library is left
// as written, and the checker says it is not declared, which is the truth.
// Every reference inside the library is rewritten to match: calls, the type in
// every chain, a written value's type, a constant's name.
//
// This is the same trick generics use — a copy named `twice$int64` that nothing
// can collide with — and it is why nothing below the parser had to learn what a
// unit is.
void qualify(std::vector<Program> &files, const Unit &unit);

// Writes what a unit's manifest decided onto every item of one of its files,
// and which unit and file the item came from. Done to the program's own file
// as much as to a library's, because the program is a unit too; done here
// rather than in the parser because the parser reads one file and a setting is
// about a directory of them. `file` is the file's place in `unit.files`.
void applySettings(Program &file, const Unit &unit, unsigned which = 0);

// Every place a file reaches into a library — `t.twice[…]`, `var.t.point`,
// `t.uer:*…*` — checked against the file's own `import` lines. A file uses what
// it imports and nothing else: the manifest says what the program *could* use,
// and `import` says what *this file* does. Answers a diagnostic per file that
// reaches for a prefix it never imported (E0608).
std::vector<Diagnostic> importsCover(const Program &file, const UnitsResult &units);

} // namespace xag
