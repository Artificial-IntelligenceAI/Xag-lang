#include "xag/Native.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace xag {
namespace {

// A symbol name a linker will accept, out of a body name that may hold quotes
// and spaces — `const 'LIMIT'` is a perfectly good body name and no kind of
// symbol.
std::string symbolFor(const std::string &name) {
  std::string out = "xag_";
  for (char c : name)
    out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
  return out;
}

bool isLoan(const std::string &type) {
  return type.rfind("ref ", 0) == 0 || type.rfind("refmut ", 0) == 0;
}

class Emitter {
public:
  Emitter(const Mir &mir)
      : mir_(mir), module_("xag", context_), builder_(context_) {
    str_ = llvm::StructType::create(context_, {builder_.getPtrTy(), builder_.getInt64Ty(),
                                               builder_.getInt64Ty()},
                                    "XagStr");
    declareRuntime();
  }

  bool run(std::string &trouble) {
    for (const Body &body : mir_.bodies)
      declare(body);
    for (const Body &body : mir_.bodies)
      define(body);
    emitMain();

    std::string reason;
    llvm::raw_string_ostream out(reason);
    if (llvm::verifyModule(module_, &out)) {
      trouble = "the module this compiler built is not well formed:\n" + out.str();
      return false;
    }
    return true;
  }

  llvm::Module &module() { return module_; }
  llvm::LLVMContext &context() { return context_; }

private:
  const Mir &mir_;
  llvm::LLVMContext context_;
  llvm::Module module_;
  llvm::IRBuilder<> builder_;
  llvm::StructType *str_ = nullptr;

  std::unordered_map<std::string, llvm::Function *> functions_;
  std::unordered_map<std::string, llvm::FunctionCallee> runtime_;

  const Body *body_ = nullptr;
  std::vector<llvm::Value *> slots_; // one alloca per local
  std::vector<llvm::BasicBlock *> blocks_;

  // ---- types

  llvm::Type *typeFor(const std::string &spelled) {
    if (spelled == "bool")
      return builder_.getInt1Ty();
    if (spelled == "str")
      return str_;
    if (isLoan(spelled))
      return builder_.getPtrTy();
    const Type named = typeNamed(spelled);
    if (isWhole(named))
      return builder_.getIntNTy(widthOf(named));
    return builder_.getInt8Ty(); // `nothing`, and anything unknown
  }

  // A whole number widened to the carrier every runtime call speaks in.
  llvm::Value *widened(llvm::Value *value, Type named) {
    auto *carrier = builder_.getInt128Ty();
    return isSigned(named) ? builder_.CreateSExt(value, carrier)
                           : builder_.CreateZExt(value, carrier);
  }

  const std::string &nameOf(TypeRef type) const {
    static const std::string unknown = "?";
    return type.index < body_->types.size() ? body_->types[type.index] : unknown;
  }

  void declareRuntime() {
    auto *ptr = builder_.getPtrTy();
    auto *i64 = builder_.getInt64Ty();
    auto *i32 = builder_.getInt32Ty();
    auto *voidTy = builder_.getVoidTy();

    auto add = [&](const char *name, llvm::Type *result,
                   llvm::ArrayRef<llvm::Type *> params) {
      runtime_[name] =
          module_.getOrInsertFunction(name, llvm::FunctionType::get(result, params, false));
    };
    add("xag_str_from", voidTy, {ptr, ptr, i64});
    add("xag_str_join", voidTy, {ptr, ptr, i64});
    add("xag_str_count", i64, {ptr});
    add("xag_str_compare", i64, {ptr, ptr});
    add("xag_str_drop", voidTy, {ptr});
    add("xag_print", voidTy, {ptr});
    add("xag_print_bool", voidTy, {i32});
    auto *i128 = builder_.getInt128Ty();
    add("xag_print_int", voidTy, {i128, i32, i32});
    for (const char *op : {"xag_int_div", "xag_int_mod", "xag_int_pow"})
      add(op, i128, {i128, i128, i32, i32});
    (void)i64;
  }

  // ---- declaring

  void declare(const Body &body) {
    body_ = &body;
    std::vector<llvm::Type *> params;
    for (unsigned i = 1; i <= body.parameters && i < body.locals.size(); ++i)
      params.push_back(typeFor(nameOf(body.locals[i].type)));
    const std::string &result = nameOf(body.result);
    llvm::Type *answer = result == "nothing" ? builder_.getVoidTy() : typeFor(result);
    auto *type = llvm::FunctionType::get(answer, params, false);
    functions_[body.name] = llvm::Function::Create(
        type, llvm::Function::InternalLinkage, symbolFor(body.name), module_);
  }

  // ---- defining

  void define(const Body &body) {
    body_ = &body;
    llvm::Function *function = functions_[body.name];
    auto *entry = llvm::BasicBlock::Create(context_, "entry", function);
    builder_.SetInsertPoint(entry);

    slots_.assign(body.locals.size(), nullptr);
    for (const Local &local : body.locals)
      slots_[local.id] = builder_.CreateAlloca(typeFor(nameOf(local.type)), nullptr,
                                               "_" + std::to_string(local.id));
    // Every slot starts empty, so a drop that reaches one never sees rubbish.
    for (const Local &local : body.locals)
      if (nameOf(local.type) == "str")
        builder_.CreateStore(llvm::Constant::getNullValue(str_), slots_[local.id]);

    unsigned i = 0;
    for (llvm::Argument &argument : function->args()) {
      builder_.CreateStore(&argument, slots_[i + 1]);
      ++i;
    }

    blocks_.clear();
    for (const BasicBlock &block : body.blocks)
      blocks_.push_back(
          llvm::BasicBlock::Create(context_, "block" + std::to_string(block.id), function));
    builder_.CreateBr(blocks_.empty() ? entry : blocks_[0]);

    for (const BasicBlock &block : body.blocks) {
      builder_.SetInsertPoint(blocks_[block.id]);
      for (const Statement &s : block.statements)
        statement(s);
      terminator(block.terminator, function);
    }
  }

  // ---- reading

  llvm::Value *textOf(const std::string &bytes) {
    auto *global = builder_.CreateGlobalString(bytes, "text");
    auto *slot = builder_.CreateAlloca(str_, nullptr, "written");
    builder_.CreateCall(runtime_["xag_str_from"],
                        {slot, global, builder_.getInt64(bytes.size())});
    return builder_.CreateLoad(str_, slot);
  }

  static std::string unescape(const std::string &written) {
    if (written.size() == 2 && written[0] == '\\') {
      switch (written[1]) {
      case 'n': return "\n";
      case 't': return "\t";
      case 'r': return "\r";
      case '\\': return "\\";
      default: break;
      }
    }
    return written;
  }

  llvm::Value *read(const Operand &operand) {
    const std::string &type = nameOf(operand.type);
    switch (operand.kind) {
    case OperandKind::Written: {
      if (type == "bool")
        return builder_.getInt1(operand.written == "true");
      const Type named = typeNamed(type);
      if (isWhole(named)) {
        // Read at the width it was written with. The checker has already said
        // it fits, which is what makes reading it here safe at any width.
        const llvm::APInt bits(widthOf(named), operand.written, 10);
        return llvm::ConstantInt::get(builder_.getIntNTy(widthOf(named)), bits);
      }
      return textOf(unescape(operand.written));
    }
    case OperandKind::Copy:
      return builder_.CreateLoad(typeFor(localType(operand.local)), slots_[operand.local]);
    case OperandKind::Move: {
      auto *taken =
          builder_.CreateLoad(typeFor(localType(operand.local)), slots_[operand.local]);
      // What was taken is gone from where it was, so a stray drop finds nothing.
      if (localType(operand.local) == "str")
        builder_.CreateStore(llvm::Constant::getNullValue(str_), slots_[operand.local]);
      return taken;
    }
    }
    return nullptr;
  }

  const std::string &localType(unsigned local) const {
    return nameOf(body_->locals[local].type);
  }

  // A pointer to text, whether the operand names it, lends it, or wrote it.
  llvm::Value *textPointer(const Operand &operand) {
    const std::string &type = nameOf(operand.type);
    if (operand.kind != OperandKind::Written) {
      const std::string &held = localType(operand.local);
      if (held == "str")
        return slots_[operand.local];
      if (isLoan(held))
        return builder_.CreateLoad(builder_.getPtrTy(), slots_[operand.local]);
    }
    auto *slot = builder_.CreateAlloca(str_, nullptr, "piece");
    if (type == "str" || isLoan(type))
      builder_.CreateStore(read(operand), slot);
    else
      builder_.CreateStore(llvm::Constant::getNullValue(str_), slot);
    return slot;
  }

  // ---- statements

  void statement(const Statement &s) {
    if (s.kind == StatementKind::Drop) {
      if (localType(s.place) != "str")
        return;
      if (!s.conditional) {
        builder_.CreateCall(runtime_["xag_str_drop"], {slots_[s.place]});
        return;
      }
      llvm::Function *function = builder_.GetInsertBlock()->getParent();
      auto *doIt = llvm::BasicBlock::Create(context_, "drop", function);
      auto *after = llvm::BasicBlock::Create(context_, "kept", function);
      auto *flag = builder_.CreateLoad(builder_.getInt1Ty(), slots_[s.flag]);
      builder_.CreateCondBr(flag, doIt, after);
      builder_.SetInsertPoint(doIt);
      builder_.CreateCall(runtime_["xag_str_drop"], {slots_[s.place]});
      builder_.CreateBr(after);
      builder_.SetInsertPoint(after);
      return;
    }

    llvm::Value *value = evaluate(s.value);
    if (!value)
      return;
    // Writing to a name that holds a loan writes through it.
    if (isLoan(localType(s.place)) && nameOf(s.value.type) == "str") {
      auto *through = builder_.CreateLoad(builder_.getPtrTy(), slots_[s.place]);
      builder_.CreateCall(runtime_["xag_str_drop"], {through});
      builder_.CreateStore(value, through);
      return;
    }
    builder_.CreateStore(value, slots_[s.place]);
  }

  llvm::Value *evaluate(const RValue &value) {
    switch (value.kind) {
    case RValueKind::Use:
      return value.operands.empty() ? nullptr : read(value.operands[0]);

    case RValueKind::Ref:
      return slots_[value.local];

    case RValueKind::Unary:
      return builder_.CreateNot(read(value.operands[0]));

    case RValueKind::Binary:
      return binary(value);

    case RValueKind::Join: {
      const unsigned count = static_cast<unsigned>(value.operands.size());
      auto *array = builder_.CreateAlloca(str_, builder_.getInt64(count), "pieces");
      for (unsigned i = 0; i < count; ++i) {
        auto *at = builder_.CreateGEP(str_, array, builder_.getInt64(i));
        auto *piece = textPointer(value.operands[i]);
        builder_.CreateStore(builder_.CreateLoad(str_, piece), at);
      }
      auto *out = builder_.CreateAlloca(str_, nullptr, "joined");
      builder_.CreateCall(runtime_["xag_str_join"],
                          {out, array, builder_.getInt64(count)});
      return builder_.CreateLoad(str_, out);
    }

    case RValueKind::Call:
      return call(value);
    }
    return nullptr;
  }

  llvm::Value *binary(const RValue &value) {
    const std::string &op = value.op;
    const std::string &leftType = nameOf(value.operands[0].type);
    const bool onText = leftType == "str" || isLoan(leftType);

    if (onText) {
      auto *seen = builder_.CreateCall(
          runtime_["xag_str_compare"],
          {textPointer(value.operands[0]), textPointer(value.operands[1])});
      auto *zero = builder_.getInt64(0);
      if (op == "==") return builder_.CreateICmpEQ(seen, zero);
      if (op == "!==") return builder_.CreateICmpNE(seen, zero);
      if (op == "<") return builder_.CreateICmpSLT(seen, zero);
      if (op == ">") return builder_.CreateICmpSGT(seen, zero);
      if (op == "<==") return builder_.CreateICmpSLE(seen, zero);
      if (op == ">==") return builder_.CreateICmpSGE(seen, zero);
      return nullptr;
    }

    auto *left = read(value.operands[0]);
    auto *right = read(value.operands[1]);

    if (op == "and") return builder_.CreateAnd(left, right);
    if (op == "or") return builder_.CreateOr(left, right);

    const Type given = typeNamed(leftType);
    const Type made = typeNamed(nameOf(value.type));
    const Type working = isWhole(made) ? made : given;
    const bool unsignedly = isWhole(given) && !isSigned(given);

    if (op == "==") return builder_.CreateICmpEQ(left, right);
    if (op == "!==") return builder_.CreateICmpNE(left, right);
    if (op == "<")
      return unsignedly ? builder_.CreateICmpULT(left, right)
                        : builder_.CreateICmpSLT(left, right);
    if (op == ">")
      return unsignedly ? builder_.CreateICmpUGT(left, right)
                        : builder_.CreateICmpSGT(left, right);
    if (op == "<==")
      return unsignedly ? builder_.CreateICmpULE(left, right)
                        : builder_.CreateICmpSLE(left, right);
    if (op == ">==")
      return unsignedly ? builder_.CreateICmpUGE(left, right)
                        : builder_.CreateICmpSGE(left, right);

    // Under `overflow = "wrap"` a machine's own add is exactly the answer, so
    // there is nothing to be gained by calling out for it and a great deal to
    // be lost: a call is something the optimiser cannot see through.
    if (op == "+") return builder_.CreateAdd(left, right);
    if (op == "-") return builder_.CreateSub(left, right);
    if (op == "x") return builder_.CreateMul(left, right);

    // What is left is what a choice was made about, and a choice is written in
    // one place: dividing by zero stops, and a negative power has no answer.
    const char *called = op == "/" ? "xag_int_div" : op == "mod" ? "xag_int_mod"
                                                                 : "xag_int_pow";
    if (op != "/" && op != "mod" && op != "^")
      return nullptr;
    auto *answered = builder_.CreateCall(
        runtime_[called],
        {widened(left, working), widened(right, working),
         builder_.getInt32(widthOf(working)),
         builder_.getInt32(isSigned(working) ? 1 : 0)});
    return builder_.CreateTrunc(answered, typeFor(nameOf(value.type)));
  }

  llvm::Value *call(const RValue &value) {
    if (value.callee == "print.stdout") {
      for (const Operand &operand : value.operands) {
        const std::string &type = nameOf(operand.type);
        const Type named = typeNamed(type);
        if (isWhole(named))
          builder_.CreateCall(runtime_["xag_print_int"],
                              {widened(read(operand), named),
                               builder_.getInt32(widthOf(named)),
                               builder_.getInt32(isSigned(named) ? 1 : 0)});
        else if (type == "bool")
          builder_.CreateCall(runtime_["xag_print_bool"],
                              {builder_.CreateZExt(read(operand), builder_.getInt32Ty())});
        else if (auto *text = textPointer(operand))
          builder_.CreateCall(runtime_["xag_print"], {text});
      }
      return nullptr;
    }

    if (value.callee == "count") {
      auto *text = value.operands.empty() ? nullptr : textPointer(value.operands[0]);
      return text ? static_cast<llvm::Value *>(
                        builder_.CreateCall(runtime_["xag_str_count"], {text}))
                  : static_cast<llvm::Value *>(builder_.getInt64(0));
    }

    auto found = functions_.find(value.callee);
    if (found == functions_.end())
      return nullptr;
    std::vector<llvm::Value *> arguments;
    for (const Operand &operand : value.operands)
      arguments.push_back(read(operand));
    auto *answer = builder_.CreateCall(found->second, arguments);
    return answer->getType()->isVoidTy() ? nullptr : answer;
  }

  void terminator(const Terminator &end, llvm::Function *function) {
    (void)function;
    switch (end.kind) {
    case TerminatorKind::Goto:
      builder_.CreateBr(blocks_[end.targets.empty() ? 0 : end.targets[0]]);
      return;
    case TerminatorKind::Switch: {
      auto *condition = read(end.condition);
      const unsigned taken = end.targets.empty() ? 0 : end.targets[0];
      const unsigned otherwise = end.targets.size() > 1 ? end.targets.back() : taken;
      builder_.CreateCondBr(condition, blocks_[taken], blocks_[otherwise]);
      return;
    }
    case TerminatorKind::Return:
      if (end.answers && nameOf(body_->result) != "nothing") {
        builder_.CreateRet(read(end.answer));
      } else if (nameOf(body_->result) != "nothing") {
        // Lowering leaves a block after every `give`, which nothing reaches.
        builder_.CreateUnreachable();
      } else {
        builder_.CreateRetVoid();
      }
      return;
    }
  }

  void emitMain() {
    auto *type = llvm::FunctionType::get(builder_.getInt32Ty(), {}, false);
    auto *main = llvm::Function::Create(type, llvm::Function::ExternalLinkage, "main",
                                        module_);
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", main));
    auto found = functions_.find("START");
    if (found != functions_.end())
      builder_.CreateCall(found->second, {});
    builder_.CreateRet(builder_.getInt32(0));
  }
};

void optimiseModule(llvm::Module &module) {
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager groups;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(groups);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, groups, modules);
  llvm::ModulePassManager passes =
      builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  passes.run(module, modules);
}

} // namespace

NativeResult emitIr(const Mir &mir, bool optimise) {
  NativeResult result;
  Emitter emitter(mir);
  if (!emitter.run(result.trouble))
    return result;
  if (optimise)
    optimiseModule(emitter.module());
  llvm::raw_string_ostream out(result.ir);
  emitter.module().print(out, nullptr);
  return result;
}

NativeResult emitObject(const Mir &mir, bool optimise, const std::string &path) {
  NativeResult result;
  Emitter emitter(mir);
  if (!emitter.run(result.trouble))
    return result;

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();

  const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  std::string reason;
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, reason);
  if (!target) {
    result.trouble = "this machine is not one this compiler can write code for: " + reason;
    return result;
  }
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> machine(
      target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_));
  emitter.module().setDataLayout(machine->createDataLayout());
  emitter.module().setTargetTriple(triple);

  if (optimise)
    optimiseModule(emitter.module());

  std::error_code trouble;
  llvm::raw_fd_ostream file(path, trouble, llvm::sys::fs::OF_None);
  if (trouble) {
    result.trouble = "could not write " + path + ": " + trouble.message();
    return result;
  }
  llvm::legacy::PassManager writer;
  if (machine->addPassesToEmitFile(writer, file, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    result.trouble = "this machine cannot be asked for an object file";
    return result;
  }
  writer.run(emitter.module());
  file.flush();
  return result;
}

} // namespace xag
