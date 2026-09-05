// Toolchain smoke test.
//
// This is not the compiler. It exists to prove one thing before any language
// work begins: that we can link against LLVM, build a module in memory, and
// have LLVM's own verifier accept it. When the real front end starts emitting
// IR, this file goes away.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

int main() {
  llvm::LLVMContext context;
  llvm::Module module("safetybolt.smoke", context);
  llvm::IRBuilder<> builder(context);

  llvm::Type *i32 = builder.getInt32Ty();

  // fn answer() -> i32 { 42 }
  llvm::FunctionType *signature = llvm::FunctionType::get(i32, /*isVarArg=*/false);
  llvm::Function *answer = llvm::Function::Create(
      signature, llvm::Function::ExternalLinkage, "sb_answer", module);

  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", answer));
  builder.CreateRet(builder.getInt32(42));

  // verifyModule returns true when the module is broken.
  if (llvm::verifyModule(module, &llvm::errs())) {
    llvm::errs() << "smoke test: module failed verification\n";
    return 1;
  }

  module.print(llvm::outs(), nullptr);
  llvm::outs() << "smoke test: LLVM " << LLVM_VERSION_STRING << " linked and verified\n";
  return 0;
}
