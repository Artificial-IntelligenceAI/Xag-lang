#include "xag/Lexer.h"
#include "xag/Loops.h"
#include "xag/Ahead.h"
#include "xag/Check.h"
#include "xag/Typed.h"
#include "xag/Units.h"
#include "xag/Expand.h"
#include "xag/Fold.h"
#include "xag/Fast.h"
#include "xag/Interpret.h"
#include "xag/Native.h"
#include "xag/Mir.h"
#include "xag/Own.h"
#include "xag/Parser.h"
#include "xag/Regions.h"
#include "xag/Source.h"

#include "xag_runtime.h"

#include <climits>
#include <cstdlib>
#include <deque>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void usage() {
  std::cout << "xagc — the Xag compiler\n\n"
               "    xagc lex <file>     read it and print the tokens\n"
               "    xagc parse <file>   read it and print the tree\n"
               "    xagc check <file>   read it, check it, and stop\n"
               "    xagc typed <file>   print the tree with its questions\n"
               "                        answered\n"
               "    xagc mir <file>     check it and print the mid-level IR\n"
               "    xagc run <file>     check it and run it\n"
               "    xagc fast <file>    check it and run it on the fast engine\n"
               "    xagc ir <file>      check it and print the LLVM IR\n"
               "    xagc build <file>   check it and write a program beside it\n"
               "    xagc llvm-smoke     prove the LLVM backend is reachable\n"
               "    xagc --help         this\n\n"
               "    <file> -- a b c              what follows `--` is the\n"
               "                                 program's, not xagc's\n\n"
               "    --decimal=software|hardware  for this run only, over what\n"
               "                                 Xag-Config.toml decided\n\n"
               "    --no-itmt                    (check only) do not run the\n"
               "                                 program while checking it. Faster\n"
               "                                 by far, and it stops looking for\n"
               "                                 what only a run can find.\n\n"
               "    --anyway                     build even where the two ways I\n"
               "                                 have of running a program did\n"
               "                                 not agree about it. What comes\n"
               "                                 out may be wrong, and the\n"
               "                                 disagreement is still reported.\n\n"
               "Nothing else is built yet.\n";
}

// What this project decided, once, in both of the kinds it decides in: what a
// program *answers*, which every engine has to be told about, and what gets
// *delivered*, which only whoever delivers it needs to know.
//
// The file is read where the source is, and then upward, so a project decides
// for every file under it without any of them saying so.
struct Chosen {
  bool wantsHardwareDecimal = false;   // [build]
};

Chosen settingsFor(const std::string &sourcePath) {
  Chosen settings;
  std::string directory = sourcePath;
  const std::size_t slash = directory.find_last_of('/');
  directory = slash == std::string::npos ? std::string(".") : directory.substr(0, slash);

  for (unsigned up = 0; up < 32; ++up) {
    std::ifstream in(directory + "/Xag-Config.toml");
    if (in) {
      std::string line;
      while (std::getline(in, line)) {
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos)
          line = line.substr(0, hash);
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
          continue;
        std::string key = line.substr(0, equals);
        std::string said = line.substr(equals + 1);
        auto trim = [](std::string &t) {
          while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
            t.erase(t.begin());
          while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r'))
            t.pop_back();
          if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
            t = t.substr(1, t.size() - 2);
        };
        trim(key);
        trim(said);
        if (key == "decimal")
          settings.wantsHardwareDecimal = said == "hardware";
      }
      return settings;
    }
    if (directory == "/" || directory == ".")
      break;
    const std::size_t upward = directory.find_last_of('/');
    directory = upward == std::string::npos ? std::string(".") : directory.substr(0, upward);
    if (directory.empty())
      directory = "/";
  }
  return settings;
}

// A setting said on the command line is said about this run only, and wins.
Chosen *asked = nullptr;

Chosen chosenFor(const std::string &path) {
  return asked ? *asked : settingsFor(path);
}


// A program asking for a decimal this build has none of. There is nothing to
// fall back to quietly: the two encode differently, and answering with the one
// that was not asked for is the kind of silence this compiler is built against.
bool decimalIsThere(const std::string &path) {
  if (!chosenFor(path).wantsHardwareDecimal || xag_decimal_is_hardware())
    return true;
  std::cerr << "xagc: this asks for `decimal = \"hardware\"`, and this build has "
               "software decimal.\n"
               "      IBM's decimal floating-point unit is on z/Architecture "
               "(s390x) from z9\n"
               "      and on POWER (ppc64, ppc64le) from POWER6. Nothing else has "
               "one.\n"
               "      Build the runtime with -DXAG_DECIMAL=hardware on a machine "
               "that does,\n"
               "      or ask for `decimal = \"software\"`, which answers "
               "identically everywhere.\n";
  return false;
}

bool readSource(const std::string &path, std::string &text) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "xagc: cannot read " << path << '\n';
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  text = buffer.str();
  return true;
}

int report(const xag::Source &source, const std::vector<xag::Diagnostic> &raw) {
  if (raw.empty())
    return 0;
  // What followed from something else goes underneath it first, so that one
  // mistake is shown as one mistake with everything it broke drawn under it,
  // and the tally at the end counts mistakes rather than consequences.
  const std::vector<xag::Diagnostic> diagnostics = xag::foldFollowOns(raw);
  // Refusals and warnings are shown apart, because they are answers to
  // different questions: one says the code was not built, the other says it was
  // and here is what could not be worked out.
  std::vector<xag::Diagnostic> errors;
  std::vector<xag::Diagnostic> warnings;
  std::vector<xag::Diagnostic> mine;
  for (const xag::Diagnostic &one : diagnostics) {
    if (one.severity == xag::Severity::Mine)
      mine.push_back(one);
    else
      (one.severity == xag::Severity::Error ? errors : warnings).push_back(one);
  }

  // First and alone. Everything else said about a program the compiler cannot
  // agree with itself about is worth nothing until that is fixed, and it is
  // ours to fix.
  if (!mine.empty()) {
    xag::renderMineOpening(std::cerr);
    for (const xag::Diagnostic &one : mine)
      xag::render(source, one, std::cerr);
    xag::renderMineTally(mine.size(), std::cerr);
    return 1;
  }

  if (!warnings.empty()) {
    xag::renderWarningOpening(std::cerr);
    for (const xag::Diagnostic &one : warnings)
      xag::render(source, one, std::cerr);
    xag::renderWarningTally(warnings.size(), std::cerr);
  }
  if (errors.empty())
    return 0;
  if (!warnings.empty())
    std::cerr << '\n';
  xag::renderOpening(std::cerr);
  for (const xag::Diagnostic &one : errors)
    xag::render(source, one, std::cerr);
  xag::renderTally(errors.size(), std::cerr);
  return 1;
}

// Where this very program is, with every symlink followed. Homebrew puts a
// link in `bin` pointing into the cellar, and an unfollowed link would send
// `../lib` somewhere there is no runtime.
std::string ownPath() {
  char resolved[PATH_MAX] = {};
#if defined(__APPLE__)
  char raw[PATH_MAX] = {};
  uint32_t room = sizeof(raw);
  if (_NSGetExecutablePath(raw, &room) != 0)
    return {};
  if (!realpath(raw, resolved))
    return raw;
#else
  const ssize_t length = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
  if (length <= 0)
    return {};
  resolved[length] = '\0';
#endif
  return resolved;
}

std::string directoryOf(const std::string &path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool isThere(const std::string &path) {
  return !path.empty() && access(path.c_str(), R_OK) == 0;
}

// The runtime a built program is linked against, looked for rather than baked
// in. It used to be the absolute path of whatever build directory compiled the
// compiler, so `xagc build` worked on one machine, in one directory, until
// somebody renamed it.
std::string runtimeLibrary() {
  if (const char *said = std::getenv("XAG_RUNTIME"); said && *said)
    return said;
  const std::string beside = directoryOf(ownPath());
  if (!beside.empty()) {
    // Installed: `<prefix>/bin/xagc` and `<prefix>/lib/xag/libxagrt.a`.
    const std::string under = beside + "/../lib/xag/libxagrt.a";
    if (isThere(under))
      return under;
    // A build directory, wherever it has been moved to since.
    const std::string here = beside + "/libxagrt.a";
    if (isThere(here))
      return here;
  }
  // The build directory this compiler was built in, which is right until it
  // is not.
  return XAG_RUNTIME_LIB;
}

// `tree` is filled in with the checker's answers as a tree, once nothing more
// is going to be written out. Built here rather than handed back as a program
// and a `CheckResult`, because both of those live only as long as this call and
// what is keyed by their nodes lives exactly as long as they do.
bool ready(const std::string &path, std::string &text, xag::MirResult &built,
           int &status, xag::Rewriting rewriting = xag::Rewriting::No,
           xag::TypedResult *tree = nullptr);

// Set by `--anyway`: build even where the two ways of running a program did not
// agree about it. Nothing is folded away then, because a rewrite worked out from
// an answer the compiler cannot stand behind is the one thing that must not
// reach anybody.
bool anyway = false;

// Set by `--no-itmt`, and only `xagc check` takes it: do not run the program
// while checking it.
//
// The whole cost of a check is that run — the front end is free, and building
// and starting a program is half a second. Somebody writing code wants the
// spelling and the types back at once, and can ask for the rest when they are
// done. What is given up is exactly the three things that need a program to
// have run: `E0537`, `E0538`, and a bound turning from *may* into *does*.
//
// Not on `build`, because that is shipping something nothing ever ran, and in
// the source the same word needs an `UNSAFE` block around it to say so. A flag
// has no block to be inside.
bool noItmt = false;

int lexFile(const std::string &path) {
  std::string text;
  if (!readSource(path, text))
    return 1;

  const xag::Source source(path, text);
  const xag::LexResult result = xag::lex(source);

  // Tokens after a failed lex describe a file the reader has not written yet.
  for (const xag::Token &token : result.ok() ? result.tokens : std::vector<xag::Token>{}) {
    const xag::Source::Position at = source.positionOf(token.span.begin);
    std::cout << at.line << ':' << at.column << '\t' << xag::describe(token.kind);
    if (!token.text.empty())
      std::cout << '\t' << token.text;
    std::cout << '\n';
  }

  return report(source, result.diagnostics);
}

int parseFile(const std::string &path) {
  std::string text;
  if (!readSource(path, text))
    return 1;

  const xag::Source source(path, text);
  const xag::LexResult lexed = xag::lex(source);
  if (!lexed.ok())
    return report(source, lexed.diagnostics);

  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (parsed.ok())
    xag::print(parsed.program, std::cout);
  return report(source, parsed.diagnostics);
}

// The tree with the questions answered, printed. There is no pipeline of its
// own here: it is the same road up to the checker, and then the same tree the
// passes below the checker are given.
int typedFile(const std::string &path) {
  // The same road every other command takes, so that what this prints is the
  // tree the passes below the checker are actually given — blanks filled in,
  // arms chosen, walks written out.
  std::string text;
  xag::MirResult built;
  int status = 0;
  xag::TypedResult tree;
  if (!ready(path, text, built, status, xag::Rewriting::No, &tree))
    return status;
  std::cout << xag::printed(tree.program);
  return 0;
}

int checkFile(const std::string &path) {
  std::string text;
  xag::MirResult built;
  int status = 0;
  ready(path, text, built, status);
  return status;
}

int runFile(const std::string &path) {
  if (!decimalIsThere(path))
    return 1;
  // Given the program as written, not what the optimiser made of it. The
  // compiler gets the optimised one, so the oracle is comparing the two on
  // every case it runs — and a mistake in the folding shows up as a
  // disagreement rather than as three engines agreeing on the same wrong
  // number.
  // The same road every other engine takes. Running had a pipeline of its own,
  // which had drifted: it never asked the region pass anything, so `xagc run`
  // ran programs that `xagc check` and `xagc build` both refused — and it is
  // the engine that is meant to be believed.
  std::string text;
  xag::MirResult built;
  int status = 0;
  if (!ready(path, text, built, status))
    return status;

  const xag::InterpretResult ran = xag::interpret(built.mir);
  if (!ran.ran) {
    std::cerr << "\nthe program stopped: " << ran.trouble << '\n';
    return 1;
  }
  return 0;
}

int fastFile(const std::string &path) {
  if (!decimalIsThere(path))
    return 1;
  std::string text;
  xag::MirResult built;
  int status = 0;
  if (!ready(path, text, built, status))
    return status;

  const xag::FastResult ran = xag::runFast(built.mir);
  if (!ran.ran) {
    std::cerr << "\nthe program stopped: " << ran.trouble << '\n';
    return 1;
  }
  return 0;
}

// Everything a program has to survive before any engine sees it.
// Building the program and starting it, so that something other than the test
// interpreter has an opinion about what it does.
//
// Everything happens beside the source, in files named after the process, and
// every one of them is removed whatever happens. What the program writes is
// kept; that is the whole point of running it.
xag::Compiled buildAndStart(const xag::Mir &mir) {
  xag::Compiled out;
  const std::string runtime = runtimeLibrary();
  if (!isThere(runtime))
    return out; // nothing to link against, so nothing was asked

  // `mkstemp` rather than a name made up and hoped for: it makes the file as it
  // names it, so nothing else can take the name in between. The empty file it
  // leaves is what the linker writes over, so it is not removed here — doing
  // that deleted the program between linking it and starting it, and the two
  // engines duly disagreed about a program only one of them had.
  
  std::string stem = "/tmp/xag-ahead-XXXXXX";
  const int held = ::mkstemp(stem.data());
  if (held < 0)
    return out;
  ::close(held);
  const std::string object = stem + ".o";
  out.asked = true;

  // Optimised, because that is what the reader will be given. An answer from
  // an unoptimised build would leave the optimiser as the one thing nothing
  // checks — which is the same reason the interpreters get the program as
  // written and the compiler gets what the optimiser made of it.
  const xag::NativeResult emitted =
      xag::emitObject(mir, true, object, xag::Watching::Yes);
  if (!emitted.ok()) {
    out.trouble = emitted.trouble;
    std::remove(object.c_str());
    return out;
  }

  const std::string link =
      "cc \"" + object + "\" \"" + runtime + "\" -o \"" + stem + "\" 2>/dev/null";
  const int linked = std::system(link.c_str());
  std::remove(object.c_str());
  if (linked != 0) {
    out.trouble = "it would not link.";
    return out;
  }

  // Its own output, read back. Whatever it writes to the terminal it would
  // write when the reader ran it, and that is not this moment.
  const std::string said = stem + ".out";
  const std::string complained = stem + ".err";
  const std::string noticed = stem + ".round";
  // Three destinations, because there are three different things being said: what
  // the program answers, what the program complains about, and what the compiler
  // is telling itself. The last two shared standard error until a program could
  // write there itself, and then a `print.stderr` and a came-round marker were
  // one stream with nothing to tell them apart.
  const std::string start = "XAG_NOTES=\"" + noticed + "\" \"" + stem + "\" > \"" +
                            said + "\" 2> \"" + complained + "\"";
  const int status = std::system(start.c_str());
  std::remove(stem.c_str());

  std::ifstream reading(said, std::ios::binary);
  out.said.assign(std::istreambuf_iterator<char>(reading), std::istreambuf_iterator<char>());
  reading.close();
  std::remove(said.c_str());

  std::ifstream grumbling(complained, std::ios::binary);
  out.complained.assign(std::istreambuf_iterator<char>(grumbling),
                        std::istreambuf_iterator<char>());
  grumbling.close();
  std::remove(complained.c_str());

  // Where it said a sum came round, one line each, in the order they happened.
  // Kept apart from what the program wrote, because a program's own output is
  // its answer and this is the compiler talking to itself.
  std::ifstream saying(noticed);
  std::string line;
  while (std::getline(saying, line)) {
    const std::string mark = "xag-came-round ";
    if (line.rfind(mark, 0) != 0)
      continue;
    const unsigned at =
        static_cast<unsigned>(std::strtoul(line.c_str() + mark.size(), nullptr, 10));
    bool already = false;
    for (const xag::Span &had : out.cameRound)
      if (had.begin == at)
        already = true;
    if (!already)
      out.cameRound.push_back(xag::Span{at, at});
  }
  saying.clear();
  saying.seekg(0);
  while (std::getline(saying, line)) {
    const std::string stopped = "the program stopped: ";
    const std::string where = "xag-stopped-at ";
    if (line == "xag-would-read")
      out.wouldRead = true;
    else if (line == "xag-would-take-time")
      out.wouldTakeTime = true;
    else if (line.rfind(stopped, 0) == 0)
      out.why = line.substr(stopped.size());
    else if (line.rfind(where, 0) == 0) {
      const unsigned at =
          static_cast<unsigned>(std::strtoul(line.c_str() + where.size(), nullptr, 10));
      out.stoppedAt = xag::Span{at, at};
    }
  }
  saying.close();
  std::remove(noticed.c_str());

  // Reaching a read is where it was told to stop, so it stopped there having
  // done what was asked. It did not *finish*, though, and saying it did had the
  // two engines disagreeing about every program that reads: one had stopped at
  // the read and the other was said to have run to the end.
  if (out.wouldRead || out.wouldTakeTime)
    return out;

  if (status != 0) {
    // A program that stops is an answer, not a failure to get one — as long as
    // it said why. Everything else about a program that would not run is.
    if (!out.why.empty()) {
      out.stopped = true;
      return out;
    }
    out.trouble = "it stopped, having written " + std::to_string(out.said.size()) +
                  " character(s).";
    return out;
  }
  out.ran = true;
  return out;
}

bool ready(const std::string &path, std::string &text, xag::MirResult &built, int &status,
           xag::Rewriting rewriting, xag::TypedResult *tree) {
  status = 1;
  if (!readSource(path, text))
    return false;
  const xag::Source source(path, text);

  // What this file may reach for: the manifest beside it, and every library the
  // manifest's `[uses]` leads to. Read first, because the parser has to know a
  // library's call name to read `var.t.point 'p'` — and refused here if a
  // manifest is wrong or a loop closes, since nothing after this could mean
  // anything.
  const xag::UnitsResult units = xag::unitsFor(path);
  if (!units.ok()) {
    for (std::size_t i = 0; i < units.diagnostics.size(); ++i)
      report(units.about[i] ? *units.about[i] : source, {units.diagnostics[i]});
    return false;
  }
  std::vector<std::string> prefixes;
  for (const xag::Unit &one : units.libraries)
    prefixes.push_back(one.called);

  // Every pass is reported whether or not it refused. A warning shown only when
  // something else already went wrong is a warning nobody ever reads.
  const xag::LexResult lexed = xag::lex(source);
  if (report(source, lexed.diagnostics) != 0)
    return false;
  // Not const: expansion points calls at the copies they meant, and pruning
  // lifts the arm a `whichever` chose out of the statement holding it. Both
  // write into the tree the checker walked, because what the checker worked out
  // is keyed by those nodes and a copy's nodes are not those.
  xag::ParseResult parsed = xag::parse(source, lexed.tokens, prefixes);
  if (report(source, parsed.diagnostics) != 0)
    return false;
  // Every `import` names a library the manifest reached. Said here rather than
  // in the checker because it is about the manifest, not about the program.
  {
    std::vector<xag::Diagnostic> unknown;
    for (const xag::Item &item : parsed.program.items) {
      if (item.kind != xag::ItemKind::Import || xag::unitNamed(units, item.name))
        continue;
      std::string known;
      for (const xag::Unit &one : units.libraries)
        known += (known.empty() ? "`" : ", `") + one.name + "`";
      unknown.push_back(xag::Diagnostic{
          item.nameSpan, "E0606", "no library is called `" + item.name + "`.", "here",
          {"`import` names a library the manifest's `[uses]` reaches"},
          {known.empty() ? std::string("this program's manifest reaches no libraries at "
                                       "all — `[uses]` with `paths = [\"../text\"]` is "
                                       "how it says which.")
                         : "the manifest reaches " + known + ". A library is called what "
                           "its own manifest says under `[unit]`, not what its directory "
                           "is called."}});
    }
    if (report(source, unknown) != 0)
      return false;
  }

  // Each library's files, read and written into this program under the
  // library's call name — so that from here on there is one program, and
  // nothing below the parser has to know what a unit is.
  //
  // Read with every prefix the program knows rather than only the ones the
  // library's own manifest names. That lets a library reach a prefix it never
  // imported, which is a rule not yet enforced anywhere — a file's `import`
  // lines are not yet checked against what the file actually reaches for, in
  // the program either. Noted in design/units.md.
  //
  // Kept alive alongside the entry, because a diagnostic about a line in a
  // library has to be able to show that line.
  static std::vector<std::pair<std::unique_ptr<xag::Source>, std::string>> librarySources;
  librarySources.clear();
  for (const xag::Unit &library : units.libraries) {
    for (const std::string &file : library.files) {
      std::string held;
      if (!readSource(file, held))
        return false;
      librarySources.emplace_back(std::make_unique<xag::Source>(file, held), file);
      const xag::Source &theirSource = *librarySources.back().first;
      const xag::LexResult theirLexed = xag::lex(theirSource);
      if (report(theirSource, theirLexed.diagnostics) != 0)
        return false;
      xag::ParseResult theirParsed = xag::parse(theirSource, theirLexed.tokens, prefixes);
      if (report(theirSource, theirParsed.diagnostics) != 0)
        return false;
      if (!theirParsed.program.library) {
        report(theirSource,
               {xag::Diagnostic{xag::Span{0, 0}, "E0607",
                                "`" + library.name + "` is used as a library, and this "
                                "file of it is a program.",
                                "here", {"a library's files are `READ_ME`, `LIBRARY`, `ITMT`"},
                                {"a file with `PREP` and `START` is a program, and a "
                                 "program is not something another program imports."}}});
        return false;
      }
      xag::qualify(theirParsed.program, library);
      for (xag::Item &item : theirParsed.program.items) {
        // A library's `ITMT` is its own business — run when the library is
        // built on its own, not when a program that uses it is. Its imports
        // were resolved when its manifest was.
        if (item.kind == xag::ItemKind::Itmt || item.kind == xag::ItemKind::Import)
          continue;
        parsed.program.items.push_back(std::move(item));
      }
    }
  }
  // Read, and read again after every round of writing generics out. Only what
  // refuses is said as it happens; what is only *said* waits until the rounds
  // have settled, because a warning about a name is the same warning every
  // round and printing it each time said one thing five times.
  xag::CheckResult checked = xag::check(source, parsed.program);
  if (xag::anyErrors(checked.diagnostics))
    return report(source, checked.diagnostics), false;

  // A generic is written out once per type it was called with, and every call
  // pointed at the copy it meant. After this there are no blanks anywhere, so
  // everything below reads ordinary functions — which is why none of them had
  // to learn what a blank is.
  //
  // Read a second time, because the copies have never been read: the first pass
  // deliberately left every generic body alone, since whether a thing copies is
  // the very question the blank had not answered. This is the pass that answers
  // it, once per type, and it is where a generic's own mistakes are found.
  //
  // Round by round, because one generic can call another: the inner call is not
  // read until the outer body has been written out at a type, and it is that
  // reading which says what the inner one was asked for. Each round writes what
  // the last one learned, and the rounds stop when no generic is left standing.
  static std::deque<xag::Program> rounds;
  rounds.clear();
  const xag::Program *program = &parsed.program;
  while (true) {
    // A file whose generics fill each other in for sixty-four rounds is not a
    // file anybody wrote; it is expansion failing to settle, which is ours.
    if (rounds.size() >= 64) {
      report(source, {xag::Diagnostic{
                         xag::Span{}, "",
                         "I could not finish writing the generics in this file out.",
                         "here",
                         {"Each one I wrote out asked for another, and it did not stop."},
                         {}, {}, xag::Severity::Mine}});
      return false;
    }
    // Both written on the tree the checker walked, because what it worked out is
    // keyed by those statements. Neither can happen a round earlier: a
    // `whichever` and a `loop.parts` inside a generic are not reached until that
    // generic has been written out at a real type, and only then is there a
    // type to choose by and a struct to walk.
    //
    // The arms nobody chose go first, and they have to go here rather than at
    // the end. An arm that is not part of the program still holds calls, and a
    // call nothing ever repoints keeps the generic it names standing — so a
    // `show` whose `is struct` arm calls `show` kept itself alive round after
    // round, and stopped only at the sixty-fourth.
    xag::Program &working = const_cast<xag::Program &>(*program);
    const unsigned settled = xag::prune(working, checked);
    const unsigned walked = xag::unroll(working, checked);

    xag::Program &next = rounds.emplace_back();
    if (!xag::expand(const_cast<xag::Program &>(*program), checked, next)) {
      rounds.pop_back();
      if (settled == 0 && walked == 0)
        break;
      // Nothing generic left, but an arm was chosen or a struct was walked — so
      // what stands there now has never been read.
      checked = xag::check(source, working);
      if (xag::anyErrors(checked.diagnostics))
        return report(source, checked.diagnostics), false;
      continue;
    }
    program = &next;
    checked = xag::check(source, next);
    if (xag::anyErrors(checked.diagnostics))
      return report(source, checked.diagnostics), false;
  }

  // Now that nothing is going to be written out again, once.
  if (report(source, checked.diagnostics) != 0)
    return false;
  if (tree) {
    *tree = xag::typedTree(source, *program, checked);
    if (report(source, tree->diagnostics) != 0)
      return false;
  }

  // Every `whichever` becomes the arm it chose. After this the word is gone
  // from the tree, so ownership and the middle layer read ordinary blocks and
  // had to learn nothing about it — the same trick generics get, and for the
  // same reason.
  xag::prune(const_cast<xag::Program &>(*program), checked);

  const xag::OwnResult owned = xag::own(source, *program, checked);
  if (report(source, owned.diagnostics) != 0)
    return false;
  built = xag::build(source, *program, checked);
  if (report(source, built.diagnostics) != 0)
    return false;
  xag::elaborate(built.mir);

  // What is already written down is worked out here, once, rather than by every
  // engine on every run — and what is written down and certainly wrong is
  // refused rather than left to stop when it is reached.
  const xag::FoldResult folded = xag::fold(source, built.mir, rewriting);
  if (report(source, folded.diagnostics) != 0)
    return false;

  // How long a loan lasts is a question the graph answers, so it is asked here
  // rather than of the tree.
  const xag::RegionResult held = xag::regions(source, built.mir);
  if (report(source, held.diagnostics) != 0)
    return false;

  // Last, because it runs the program, and a program is only run once it has
  // been read and found sound. A file holding both a mistake and a very long
  // loop has to report the mistake, and it cannot if it is still counting.
  // Asked not to run anything: the bounds stand as the estimates they are, and
  // nothing that needs a run is looked for.
  xag::AheadResult ran =
      noItmt ? xag::AheadResult{checked.aboutSums, false, false}
             : xag::ahead(source, built.mir, checked.aboutSums,
                          checked.intoPlainNames, buildAndStart,
                          xag::HowLong{static_cast<long long>(checked.mostRounds),
                                       checked.longestLoop});
  bool disagreed = false;
  for (xag::Diagnostic &one : ran.diagnostics)
    if (one.severity == xag::Severity::Mine) {
      disagreed = true;
      if (anyway) {
        one.severity = xag::Severity::Warning;
        one.tips = {"you asked for it anyway, so here it is. Everything the two ways "
                    "of running agreed about still holds, and no loop has been "
                    "written away — but the two of them disagreeing about this "
                    "program is a reason to doubt what comes out of it."};
      }
    }
  if (report(source, ran.diagnostics) != 0)
    return false;

  // Last of all, and only for what gets compiled. A loop whose answer is known
  // is written back as that answer, and the interpreters never see it — which
  // is what leaves the oracle a rewrite to disagree with, rather than three
  // engines agreeing on the same folded number.
  if (rewriting == xag::Rewriting::Yes && !disagreed)
    xag::writeInWhatTheLoopsAnswer(built.mir);

  status = 0;
  return true;
}

int irFile(const std::string &path, bool optimise) {
  if (!decimalIsThere(path))
    return 1;
  std::string text;
  xag::MirResult built;
  int status = 0;
  if (!ready(path, text, built, status, xag::Rewriting::Yes))
    return status;
  const xag::NativeResult emitted = xag::emitIr(built.mir, optimise);
  if (!emitted.ok()) {
    std::cerr << emitted.trouble << '\n';
    return 1;
  }
  std::cout << emitted.ir;
  return 0;
}

int buildFile(const std::string &path) {
  if (!decimalIsThere(path))
    return 1;
  std::string text;
  xag::MirResult built;
  int status = 0;
  if (!ready(path, text, built, status, xag::Rewriting::Yes))
    return status;

  const std::string stem = path.substr(0, path.rfind('.'));
  const std::string object = stem + ".o";
  const xag::NativeResult emitted = xag::emitObject(built.mir, true, object);
  if (!emitted.ok()) {
    std::cerr << emitted.trouble << '\n';
    return 1;
  }

  const std::string runtime = runtimeLibrary();
  if (!isThere(runtime)) {
    std::cerr << "xagc: cannot find the runtime to link against.\n"
              << "  looked for: " << runtime << '\n'
              << "  set XAG_RUNTIME to it, or install xagc so that\n"
              << "  `<prefix>/lib/xag/libxagrt.a` sits beside `<prefix>/bin/xagc`.\n";
    std::remove(object.c_str());
    return 1;
  }

  // Every path here can hold a space, so every path here is quoted.
  const std::string command =
      "cc \"" + object + "\" \"" + runtime + "\" -o \"" + stem + "\"";
  if (std::system(command.c_str()) != 0) {
    std::cerr << "xagc: the linker would not put it together\n";
    return 1;
  }
  std::remove(object.c_str());
  std::cout << stem << '\n';
  return 0;
}

int mirFile(const std::string &path) {
  // The same road every other command takes. It had a pipeline of its own,
  // which is how `xagc run` once came to accept programs the others refused —
  // and how `xagc mir` came to refuse a generic that `run`, `fast` and `build`
  // had all just agreed about, because expanding one happens on that road and
  // not on this.
  //
  // Shown as the interpreters are given it: the program as written, refused
  // where it is certainly wrong but not rewritten. What the optimiser makes of
  // it afterwards is `xagc ir`.
  std::string text;
  xag::MirResult built;
  int status = 0;
  if (!ready(path, text, built, status))
    return status;
  xag::print(built.mir, std::cout);
  return 0;
}

int llvmSmoke() {
  llvm::LLVMContext context;
  llvm::Module module("xag.smoke", context);
  llvm::IRBuilder<> builder(context);

  llvm::FunctionType *signature =
      llvm::FunctionType::get(builder.getInt32Ty(), /*isVarArg=*/false);
  llvm::Function *answer = llvm::Function::Create(
      signature, llvm::Function::ExternalLinkage, "xag_answer", module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", answer));
  builder.CreateRet(builder.getInt32(42));

  if (llvm::verifyModule(module, &llvm::errs()))
    return 1;

  module.print(llvm::outs(), nullptr);
  llvm::outs() << "LLVM " << LLVM_VERSION_STRING << " linked and verified\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  // A setting given here is about this run and nothing else, which is what the
  // oracle needs: both values of a knob are separate languages, and it has to
  // be able to ask for either without editing the project's mind.
  Chosen overridden;
  std::vector<char *> args(argv, argv + argc);
  for (unsigned i = 1; i < args.size();) {
    const std::string one = args[i];
    if (one.rfind("--decimal=", 0) == 0) {
      overridden.wantsHardwareDecimal = one.substr(10) == "hardware";
      asked = &overridden;
      args.erase(args.begin() + i);
      continue;
    }
    // A way past the compiler contradicting itself, for somebody who cannot
    // wait for it to be fixed. It is not a way of being told less: everything
    // the two ways of running agreed about is still said, and the disagreement
    // is still reported. What it stops is the refusing.
    if (one == "--anyway") {
      anyway = true;
      args.erase(args.begin() + i);
      continue;
    }
    if (one == "--no-itmt") {
      noItmt = true;
      args.erase(args.begin() + i);
      continue;
    }
    ++i;
  }
  argv = args.data();
  argc = static_cast<int>(args.size());

  // Everything after `--` belongs to the program being run, not to `xagc`.
  for (unsigned i = 1; i < args.size(); ++i) {
    if (std::string(args[i]) != "--")
      continue;
    xag_set_arguments(static_cast<int32_t>(args.size() - i - 1), args.data() + i + 1);
    args.resize(i);
    break;
  }
  argv = args.data();
  argc = static_cast<int>(args.size());

  const std::string command = argc > 1 ? argv[1] : "--help";

  // Only `check` may be told not to run anything. On `build` it would be
  // shipping a program nothing ever ran, and in the source the same word needs
  // an `UNSAFE` block around it to say so — a flag has none.
  if (noItmt && command != "check") {
    std::cerr << "xagc: `--no-itmt` is for `xagc check`, where it is a faster "
                 "answer while you write.\n"
                 "      `xagc "
              << command
              << "` runs the program to find what only running finds, and a\n"
                 "      program that ships is one that was run. Say `no-itmt` on the "
                 "loop\n      you mean, inside `UNSAFE`, and it is said where anybody "
                 "reading can see it.\n";
    return 1;
  }

  if (command == "lex" && argc > 2)
    return lexFile(argv[2]);
  if (command == "parse" && argc > 2)
    return parseFile(argv[2]);
  if (command == "check" && argc > 2)
    return checkFile(argv[2]);
  if (command == "typed" && argc > 2)
    return typedFile(argv[2]);
  if (command == "mir" && argc > 2)
    return mirFile(argv[2]);
  if (command == "run" && argc > 2)
    return runFile(argv[2]);
  if (command == "fast" && argc > 2)
    return fastFile(argv[2]);
  if (command == "ir" && argc > 2)
    return irFile(argv[2], argc > 3 && std::string(argv[3]) == "--raw" ? false : true);
  if (command == "build" && argc > 2)
    return buildFile(argv[2]);
  if (command == "llvm-smoke")
    return llvmSmoke();

  usage();
  return command == "--help" ? 0 : 1;
}
