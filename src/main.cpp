#include "safetybolt/Lexer.h"
#include "safetybolt/Source.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <iostream>
#include <sstream>

namespace {

void usage() {
  std::cout << "sbc — the SafetyBolt compiler\n\n"
               "    sbc lex <file>     read it and print the tokens\n"
               "    sbc llvm-smoke     prove the LLVM backend is reachable\n"
               "    sbc --help         this\n\n"
               "Nothing else is built yet.\n";
}

int lexFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "sbc: cannot read " << path << '\n';
    return 1;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();

  const sb::Source source(path, buffer.str());
  const sb::LexResult result = sb::lex(source);

  // Tokens after a failed lex describe a file the reader has not written yet.
  for (const sb::Token &token : result.ok() ? result.tokens : std::vector<sb::Token>{}) {
    const sb::Source::Position at = source.positionOf(token.span.begin);
    std::cout << at.line << ':' << at.column << '\t' << sb::describe(token.kind);
    if (!token.text.empty())
      std::cout << '\t' << token.text;
    std::cout << '\n';
  }

  if (!result.ok()) {
    sb::renderOpening(std::cerr);
    for (const sb::Diagnostic &diagnostic : result.diagnostics)
      sb::render(source, diagnostic, std::cerr);
    sb::renderTally(result.diagnostics.size(), std::cerr);
  }
  return result.ok() ? 0 : 1;
}

int llvmSmoke() {
  llvm::LLVMContext context;
  llvm::Module module("safetybolt.smoke", context);
  llvm::IRBuilder<> builder(context);

  llvm::FunctionType *signature =
      llvm::FunctionType::get(builder.getInt32Ty(), /*isVarArg=*/false);
  llvm::Function *answer = llvm::Function::Create(
      signature, llvm::Function::ExternalLinkage, "sb_answer", module);
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
  if (command == "llvm-smoke")
    return llvmSmoke();

  usage();
  return command == "--help" ? 0 : 1;
}
