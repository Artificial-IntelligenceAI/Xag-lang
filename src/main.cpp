#include "xag/Lexer.h"
#include "xag/Check.h"
#include "xag/Own.h"
#include "xag/Parser.h"
#include "xag/Source.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

void usage() {
  std::cout << "xagc — the Xag compiler\n\n"
               "    xagc lex <file>     read it and print the tokens\n"
               "    xagc parse <file>   read it and print the tree\n"
               "    xagc check <file>   read it, check it, and stop\n"
               "    xagc llvm-smoke     prove the LLVM backend is reachable\n"
               "    xagc --help         this\n\n"
               "Nothing else is built yet.\n";
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
  return report(source, owned.diagnostics);
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
  const std::string command = argc > 1 ? argv[1] : "--help";

  if (command == "lex" && argc > 2)
    return lexFile(argv[2]);
  if (command == "parse" && argc > 2)
    return parseFile(argv[2]);
  if (command == "check" && argc > 2)
    return checkFile(argv[2]);
  if (command == "llvm-smoke")
    return llvmSmoke();

  usage();
  return command == "--help" ? 0 : 1;
}
