#include "xag/Lexer.h"
#include "xag/Check.h"
#include "xag/Fast.h"
#include "xag/Interpret.h"
#include "xag/Native.h"
#include "xag/Mir.h"
#include "xag/Own.h"
#include "xag/Parser.h"
#include "xag/Regions.h"
#include "xag/Source.h"

#include "xag_runtime.h"

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
  xag::renderOpening(std::cerr);
  for (const xag::Diagnostic &diagnostic : diagnostics)
    xag::render(source, diagnostic, std::cerr);
  xag::renderTally(diagnostics.size(), std::cerr);
  return 1;
}

bool ready(const std::string &path, std::string &text, xag::MirResult &built,
           int &status);

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
  if (!built.ok())
    return report(source, built.diagnostics);
  xag::elaborate(built.mir);

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
bool ready(const std::string &path, std::string &text, xag::MirResult &built, int &status) {
  status = 1;
  if (!readSource(path, text))
    return false;
  const xag::Source source(path, text);
  const xag::LexResult lexed = xag::lex(source);
  if (!lexed.ok()) {
    status = report(source, lexed.diagnostics);
    return false;
  }
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (!parsed.ok()) {
    status = report(source, parsed.diagnostics);
    return false;
  }
  const xag::CheckResult checked = xag::check(source, parsed.program);
  if (!checked.ok()) {
    status = report(source, checked.diagnostics);
    return false;
  }
  const xag::OwnResult owned = xag::own(source, parsed.program);
  if (!owned.ok()) {
    status = report(source, owned.diagnostics);
    return false;
  }
  built = xag::build(source, parsed.program, checked, settingsUsed(path));
  if (!built.ok()) {
    status = report(source, built.diagnostics);
    return false;
  }
  xag::elaborate(built.mir);

  // How long a loan lasts is a question the graph answers, so it is asked here
  // rather than of the tree.
  const xag::RegionResult held = xag::regions(source, built.mir);
  if (!held.ok()) {
    status = report(source, held.diagnostics);
    return false;
  }
  status = 0;
  return true;
}

int irFile(const std::string &path, bool optimise) {
  if (!decimalIsThere(path))
    return 1;
  std::string text;
  xag::MirResult built;
  int status = 0;
  if (!ready(path, text, built, status))
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
  if (!ready(path, text, built, status))
    return status;

  const std::string stem = path.substr(0, path.rfind('.'));
  const std::string object = stem + ".o";
  const xag::NativeResult emitted = xag::emitObject(built.mir, true, object);
  if (!emitted.ok()) {
    std::cerr << emitted.trouble << '\n';
    return 1;
  }

  // Every path here can hold a space, so every path here is quoted.
  const std::string command = "cc \"" + object + "\" \"" XAG_RUNTIME_LIB "\" -o \"" +
                              stem + "\"";
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
  if (built.ok())
    xag::print(built.mir, std::cout);
  return report(source, built.diagnostics);
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
