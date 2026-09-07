#include "xag/Lexer.h"
#include "xag/Check.h"
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
               "    xagc mir <file>     check it and print the mid-level IR\n"
               "    xagc run <file>     check it and run it\n"
               "    xagc fast <file>    check it and run it on the fast engine\n"
               "    xagc ir <file>      check it and print the LLVM IR\n"
               "    xagc build <file>   check it and write a program beside it\n"
               "    xagc llvm-smoke     prove the LLVM backend is reachable\n"
               "    xagc --help         this\n\n"
               "    <file> -- a b c              what follows `--` is the\n"
               "                                 program's, not xagc's\n\n"
               "    --out-of-range=stops|wraps   for this run only, over what\n"
               "    --decimal=software|hardware  Xag-Config.toml decided\n\n"
               "Nothing else is built yet.\n";
}

// What this project decided, once, in both of the kinds it decides in: what a
// program *answers*, which every engine has to be told about, and what gets
// *delivered*, which only whoever delivers it needs to know.
//
// The file is read where the source is, and then upward, so a project decides
// for every file under it without any of them saying so.
struct Chosen {
  xag::Settings answers;               // [defaults]
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
        if (key == "out-of-range")
          settings.answers.wrapsOutOfRange = said == "wraps";
        else if (key == "decimal")
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

xag::Settings settingsUsed(const std::string &path) { return chosenFor(path).answers; }

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

int report(const xag::Source &source, const std::vector<xag::Diagnostic> &diagnostics) {
  if (diagnostics.empty())
    return 0;
  // Refusals and warnings are shown apart, because they are answers to
  // different questions: one says the code was not built, the other says it was
  // and here is what could not be worked out.
  std::vector<xag::Diagnostic> errors;
  std::vector<xag::Diagnostic> warnings;
  for (const xag::Diagnostic &one : diagnostics)
    (one.severity == xag::Severity::Error ? errors : warnings).push_back(one);

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

bool ready(const std::string &path, std::string &text, xag::MirResult &built,
           int &status, xag::Rewriting rewriting = xag::Rewriting::No);

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
bool ready(const std::string &path, std::string &text, xag::MirResult &built, int &status,
           xag::Rewriting rewriting) {
  status = 1;
  if (!readSource(path, text))
    return false;
  const xag::Source source(path, text);
  // Every pass is reported whether or not it refused. A warning shown only when
  // something else already went wrong is a warning nobody ever reads.
  const xag::LexResult lexed = xag::lex(source);
  if (report(source, lexed.diagnostics) != 0)
    return false;
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (report(source, parsed.diagnostics) != 0)
    return false;
  const xag::CheckResult checked = xag::check(source, parsed.program);
  if (report(source, checked.diagnostics) != 0)
    return false;
  const xag::OwnResult owned = xag::own(source, parsed.program);
  if (report(source, owned.diagnostics) != 0)
    return false;
  built = xag::build(source, parsed.program, checked, settingsUsed(path));
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
  std::string text;
  if (!readSource(path, text))
    return 1;

  const xag::Source source(path, text);
  const xag::LexResult lexed = xag::lex(source);
  if (!lexed.ok())
    return report(source, lexed.diagnostics);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (!parsed.ok())
    return report(source, parsed.diagnostics);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  if (!checked.ok())
    return report(source, checked.diagnostics);
  const xag::OwnResult owned = xag::own(source, parsed.program);
  if (!owned.ok())
    return report(source, owned.diagnostics);

  xag::MirResult built = xag::build(source, parsed.program, checked, settingsUsed(path));
  xag::elaborate(built.mir);
  if (!built.ok())
    return report(source, built.diagnostics);
  // Shown as the interpreters are given it — the program as written, refused
  // where it is certainly wrong but not rewritten. What the optimiser makes of
  // it afterwards is the compiler's business, and `xagc ir` shows that.
  const xag::FoldResult folded = xag::fold(source, built.mir, xag::Rewriting::No);
  if (!folded.ok())
    return report(source, folded.diagnostics);
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
    if (one.rfind("--out-of-range=", 0) == 0) {
      overridden.answers.wrapsOutOfRange = one.substr(15) == "wraps";
      asked = &overridden;
      args.erase(args.begin() + i);
      continue;
    }
    if (one.rfind("--decimal=", 0) == 0) {
      overridden.wantsHardwareDecimal = one.substr(10) == "hardware";
      asked = &overridden;
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

  if (command == "lex" && argc > 2)
    return lexFile(argv[2]);
  if (command == "parse" && argc > 2)
    return parseFile(argv[2]);
  if (command == "check" && argc > 2)
    return checkFile(argv[2]);
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
