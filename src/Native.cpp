#include "xag/Native.h"

#include "xag_runtime.h"

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

bool mayBeNothing(const std::string &type);

std::string withoutLoan(const std::string &type) {
  if (type.rfind("refmut ", 0) == 0)
    return type.substr(7);
  if (type.rfind("ref ", 0) == 0)
    return type.substr(4);
  return type;
}

bool holdsMany(const std::string &type) {
  return withoutLoan(type).rfind("many ", 0) == 0;
}

bool mayBeNothing(const std::string &type) {
  return withoutLoan(type).rfind("or-nothing ", 0) == 0;
}

// A value handed over by being copied, which is everything but text and the
// things that hold it.
bool copiesNamed(const std::string &type) {
  if (type == "bool")
    return true;
  const Type named = typeNamed(type);
  return isNumber(named);
}

// What is left once the `or-nothing` is off it.
// A type as the middle layer spells it, which is the one language both the
// checker and this file already speak.
std::string spellOf(Ty type) {
  return type.kind == Type::Unknown ? std::string("?") : name(type);
}

std::string within(const std::string &type) {
  const std::string bare = withoutLoan(type);
  return bare.rfind("or-nothing ", 0) == 0 ? bare.substr(11) : bare;
}

std::string elementOf(const std::string &type) {
  const std::string bare = withoutLoan(type);
  return bare.rfind("many ", 0) == 0 ? bare.substr(5) : std::string("?");
}

class Emitter {
public:
  Emitter(const Mir &mir)
      : mir_(mir), module_("xag", context_), builder_(context_) {
    // A `many` asks how wide one of its places is while the code is being
    // written, so the layout has to be settled before any of it is — an empty
    // one answers zero, and a buffer of that size is a heap overflow.
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    std::string reason;
    if (const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, reason)) {
      llvm::TargetOptions options;
      std::unique_ptr<llvm::TargetMachine> machine(
          target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_));
      module_.setDataLayout(machine->createDataLayout());
      module_.setTargetTriple(triple);
    }
    str_ = llvm::StructType::create(context_, {builder_.getPtrTy(), builder_.getInt64Ty(),
                                               builder_.getInt64Ty()},
                                    "XagStr");
    many_ = llvm::StructType::create(
        context_, {builder_.getPtrTy(), builder_.getInt64Ty()}, "XagMany");
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
  llvm::StructType *many_ = nullptr;
  std::unordered_map<std::string, llvm::StructType *> shapes_;

  std::unordered_map<std::string, llvm::Function *> functions_;
  std::unordered_map<std::string, llvm::FunctionCallee> runtime_;

  const Body *body_ = nullptr;
  std::vector<llvm::Value *> slots_; // one alloca per local
  std::vector<llvm::BasicBlock *> blocks_;

  // ---- types

  // What each struct is made of, by the name it was given.
  const Shape *shapeOf(const std::string &spelled) const {
    for (const Shape &shape : mir_.shapes)
      if (shape.name == spelled)
        return &shape;
    return nullptr;
  }

  // Whether letting one of these go means doing anything at all. A struct of
  // numbers holds nothing that has an owner, and asking is what keeps a drop of
  // one from being written at all.
  bool ownsAnything(const std::string &spelled) const {
    const std::string bare = withoutLoan(spelled);
    if (copiesNamed(bare) || bare == "nothing")
      return false;
    if (mayBeNothing(bare))
      return ownsAnything(within(bare));
    if (holdsMany(bare))
      return true; // the places themselves have an owner, whatever is in them
    if (const Shape *shape = shapeOf(bare))
      for (const Field &field : shape->fields) {
        if (ownsAnything(spellOf(field.type)))
          return true;
      }
    return shapeOf(bare) ? false : bare == "str";
  }

  // Letting go of one value, whatever it is made of.
  //
  // This walks: a struct lets go of each of the things it holds, and one of
  // those may be another struct, or a `many`, or something that may hold
  // nothing. Choosing between three runtime calls with a chain of `?:` instead
  // sent a struct held inside a struct to `xag_many_drop`, which was handed
  // something that was never allocated and aborted.
  void letGo(const std::string &spelled, llvm::Value *at) {
    const std::string bare = withoutLoan(spelled);
    if (!ownsAnything(bare))
      return;
    if (bare == "str") {
      builder_.CreateCall(runtime_["xag_str_drop"], {at});
      return;
    }
    if (mayBeNothing(bare)) {
      // What is inside goes only when there is something inside. The flag is in
      // the value rather than in a local beside it.
      llvm::Function *function = builder_.GetInsertBlock()->getParent();
      auto *letgo = llvm::BasicBlock::Create(context_, "letgo", function);
      auto *after = llvm::BasicBlock::Create(context_, "kept", function);
      auto *whole = builder_.CreateLoad(typeFor(bare), at);
      builder_.CreateCondBr(builder_.CreateExtractValue(whole, 0), letgo, after);
      builder_.SetInsertPoint(letgo);
      letGo(within(bare), builder_.CreateStructGEP(typeFor(bare), at, 1));
      builder_.CreateBr(after);
      builder_.SetInsertPoint(after);
      return;
    }
    if (holdsMany(bare)) {
      const std::string element = elementOf(bare);
      if (element == "str") {
        builder_.CreateCall(runtime_["xag_many_drop_str"], {at});
        return;
      }
      if (ownsAnything(element))
        letGoOfEveryPlace(element, at);
      builder_.CreateCall(runtime_["xag_many_drop"], {at});
      return;
    }
    if (const Shape *shape = shapeOf(bare))
      for (unsigned i = 0; i < shape->fields.size(); ++i)
        letGo(spellOf(shape->fields[i].type),
              builder_.CreateStructGEP(typeFor(bare), at, i));
  }

  // Every place of a `many`, one at a time, before the array itself goes. The
  // runtime has a walk of its own for text, which is the common case; anything
  // else is written out here because what one place holds is not something the
  // runtime knows the shape of.
  void letGoOfEveryPlace(const std::string &element, llvm::Value *array) {
    auto *whole = builder_.CreateLoad(many_, array);
    auto *base = builder_.CreateExtractValue(whole, 0);
    auto *length = builder_.CreateExtractValue(whole, 1);
    llvm::Function *function = builder_.GetInsertBlock()->getParent();
    auto *test = llvm::BasicBlock::Create(context_, "eachplace", function);
    auto *body = llvm::BasicBlock::Create(context_, "letgoplace", function);
    auto *done = llvm::BasicBlock::Create(context_, "placesgone", function);
    auto *counter = builder_.CreateAlloca(builder_.getInt64Ty(), nullptr, "place");
    builder_.CreateStore(builder_.getInt64(0), counter);
    builder_.CreateBr(test);

    builder_.SetInsertPoint(test);
    auto *i = builder_.CreateLoad(builder_.getInt64Ty(), counter);
    builder_.CreateCondBr(builder_.CreateICmpULT(i, length), body, done);

    builder_.SetInsertPoint(body);
    letGo(element, builder_.CreateGEP(typeFor(element), base, i));
    builder_.CreateStore(builder_.CreateAdd(i, builder_.getInt64(1)), counter);
    builder_.CreateBr(test);

    builder_.SetInsertPoint(done);
  }

  llvm::Type *typeFor(const std::string &spelled) {
    if (spelled == "bool")
      return builder_.getInt1Ty();
    if (spelled == "str")
      return str_;
    if (isLoan(spelled))
      return builder_.getPtrTy();
    if (const Shape *shape = shapeOf(withoutLoan(spelled))) {
      // Named struct types, so the IR reads the way the program does.
      auto found = shapes_.find(shape->name);
      if (found != shapes_.end())
        return found->second;
      auto *made = llvm::StructType::create(context_, "xag." + shape->name);
      shapes_[shape->name] = made;
      std::vector<llvm::Type *> held;
      for (const Field &field : shape->fields)
        held.push_back(typeFor(spellOf(field.type)));
      made->setBody(held);
      return made;
    }
    if (mayBeNothing(spelled))
      // Whether it is there, and what it is. Two fields, because a `str` that
      // is absent and a `str` that is empty are different things and no bit
      // pattern of one is free to mean the other.
      return llvm::StructType::get(context_,
                                   {builder_.getInt1Ty(), typeFor(within(spelled))});
    if (holdsMany(spelled))
      return many_;
    const Type named = typeNamed(spelled);
    if (isWhole(named))
      return builder_.getIntNTy(widthOf(named));
    if (isBinary(named))
      return widthOf(named) == 16    ? builder_.getHalfTy()
             : widthOf(named) == 32  ? builder_.getFloatTy()
             : widthOf(named) == 64  ? builder_.getDoubleTy()
                                     : builder_.getInt128Ty(); // bin128, as its bits
    if (isDecimal(named))
      return builder_.getIntNTy(widthOf(named)); // a `deci`, as its bits
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
    add("xag_str_of_bool", voidTy, {ptr, i32});
    add("xag_str_of_int", voidTy, {ptr, builder_.getInt128Ty(), i32, i32});
    add("xag_str_of_bin", voidTy, {ptr, builder_.getDoubleTy(), i32});
    add("xag_str_of_bin128", voidTy, {ptr, builder_.getInt128Ty()});
    add("xag_str_of_deci", voidTy, {ptr, i32, builder_.getInt128Ty()});
    add("xag_stop", voidTy, {ptr});
    if (auto *stops = llvm::dyn_cast<llvm::Function>(runtime_["xag_stop"].getCallee()))
      stops->addFnAttr(llvm::Attribute::NoReturn);
    add("xag_str_from", voidTy, {ptr, ptr, i64});
    add("xag_str_join", voidTy, {ptr, ptr, i64});
    add("xag_str_count", i64, {ptr});
    add("xag_str_compare", i64, {ptr, ptr});
    add("xag_str_drop", voidTy, {ptr});
    add("xag_print", voidTy, {ptr});
    add("xag_print_bool", voidTy, {i32});
    auto *i128 = builder_.getInt128Ty();
    add("xag_print_int", voidTy, {i128, i32, i32});
    auto *f64 = builder_.getDoubleTy();
    add("xag_print_bin", voidTy, {f64, i32});
    add("xag_bin_mod", f64, {f64, f64, i32});
    add("xag_bin_pow", f64, {f64, f64, i32});
    add("xag_print_bin128", voidTy, {i128});
    add("xag_bin128_compare", i32, {i128, i128});
    for (const char *op : {"xag_bin128_add", "xag_bin128_sub", "xag_bin128_mul",
                           "xag_bin128_div"})
      add(op, i128, {i128, i128});
    add("xag_print_deci", voidTy, {i32, i128});
    add("xag_deci_compare", i32, {i32, i128, i128});
    for (const char *op : {"xag_deci_add", "xag_deci_sub", "xag_deci_mul",
                           "xag_deci_div", "xag_deci_mod", "xag_deci_pow"})
      add(op, i128, {i32, i128, i128});
    for (const char *op : {"xag_bin128_mod", "xag_bin128_pow"})
      add(op, i128, {i128, i128});
    for (const char *op : {"xag_int_div", "xag_int_mod", "xag_int_pow"})
      add(op, i128, {i128, i128, i32, i32});
    add("xag_many_place", i64, {i64, i64, i32});
    add("xag_set_arguments", voidTy, {i32, ptr});
    add("xag_read_line", i32, {ptr});
    add("xag_arguments", voidTy, {ptr});
    add("xag_int_reads", i32, {i32, i32, ptr, i64, ptr});
    add("xag_deci_reads", i32, {i32, ptr, i64, ptr});
    add("xag_bin_reads", i32, {ptr, i64, i32, ptr});
    add("xag_bin128_reads", i32, {ptr, i64, ptr});
    add("xag_many_out_of_range", voidTy, {i64, i64});
    add("xag_many_new", voidTy, {ptr, i64, i64});
    add("xag_many_drop", voidTy, {ptr});
    add("xag_many_drop_str", voidTy, {ptr});
    add("xag_many_fill", voidTy, {ptr, i64, ptr});
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
    for (const Local &local : body.locals) {
      const std::string &held = nameOf(local.type);
      if (held == "str")
        builder_.CreateStore(llvm::Constant::getNullValue(str_), slots_[local.id]);
      else if (holdsMany(held) && !isLoan(held))
        builder_.CreateStore(llvm::Constant::getNullValue(many_), slots_[local.id]);
      else if (mayBeNothing(held) && !isLoan(held))
        builder_.CreateStore(llvm::Constant::getNullValue(typeFor(held)),
                             slots_[local.id]);
      else if (shapeOf(held) && !isLoan(held))
        builder_.CreateStore(llvm::Constant::getNullValue(typeFor(held)),
                             slots_[local.id]);
    }

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
      if (operand.written == "nothing" && mayBeNothing(type)) {
        auto *shell = typeFor(type);
        auto *none = llvm::Constant::getNullValue(shell);
        return none; // the flag is false, and what it does not hold is not read
      }
      if (type == "bool")
        return builder_.getInt1(operand.written == "true");
      const Type named = typeNamed(type);
      if (isWhole(named)) {
        // Read at the width it was written with. The checker has already said
        // it fits, which is what makes reading it here safe at any width.
        const llvm::APInt bits(widthOf(named), operand.written, 10);
        return llvm::ConstantInt::get(builder_.getIntNTy(widthOf(named)), bits);
      }
      if (isDecimal(named)) {
        XagDeci read = 0;
        xag_deci_reads(widthOf(named), operand.written.data(), operand.written.size(),
                       &read);
        llvm::APInt bits(128, {static_cast<uint64_t>(read),
                               static_cast<uint64_t>(read >> 64)});
        return llvm::ConstantInt::get(builder_.getIntNTy(widthOf(named)),
                                      bits.trunc(widthOf(named)));
      }
      if (named == Type::Bin128) {
        XagBin128 read = 0;
        xag_bin128_reads(operand.written.data(), operand.written.size(), &read);
        llvm::APInt bits(128, {static_cast<uint64_t>(read),
                               static_cast<uint64_t>(read >> 64)});
        return llvm::ConstantInt::get(builder_.getInt128Ty(), bits);
      }
      if (isBinary(named)) {
        double read = 0;
        xag_bin_reads(operand.written.data(), operand.written.size(), widthOf(named),
                      &read);
        return llvm::ConstantFP::get(typeFor(type), read);
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
      else if (holdsMany(localType(operand.local)) && !isLoan(localType(operand.local)))
        builder_.CreateStore(llvm::Constant::getNullValue(many_), slots_[operand.local]);
      return taken;
    }
    }
    return nullptr;
  }

  const std::string &localType(unsigned local) const {
    return nameOf(body_->locals[local].type);
  }

  // What an operand actually holds — the local's type, since the operand's own
  // spelling is what was asked of it rather than what is there.
  const std::string &operandType(const Operand &operand) const {
    static const std::string written = "?";
    return operand.kind == OperandKind::Written ? written : localType(operand.local);
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
    if (s.kind == StatementKind::Store) {
      const std::string held = localType(s.place);
      const std::string element = elementOf(held);
      auto *place = placePointer(
          isLoan(held) ? builder_.CreateLoad(builder_.getPtrTy(), slots_[s.place])
                       : slots_[s.place],
          builder_.CreateSExtOrTrunc(read(s.at), builder_.getInt64Ty()), element,
          s.value.settled);
      if (element == "str") {
        // What was in the place ends here: a `many` holds a value everywhere,
        // and putting one in does not make room by forgetting the other.
        builder_.CreateCall(runtime_["xag_str_drop"], {place});
        builder_.CreateStore(
            builder_.CreateLoad(str_, textPointer(s.value.operands[0])), place);
        return;
      }
      builder_.CreateStore(read(s.value.operands[0]), place);
      return;
    }

    if (s.kind == StatementKind::Drop) {
      const std::string held = localType(s.place);
      if (!ownsAnything(held))
        return;
      if (!s.conditional) {
        letGo(held, slots_[s.place]);
        return;
      }
      // A value that only some ways through the program put anything in is let
      // go behind the flag that says whether they did.
      llvm::Function *function = builder_.GetInsertBlock()->getParent();
      auto *doIt = llvm::BasicBlock::Create(context_, "drop", function);
      auto *after = llvm::BasicBlock::Create(context_, "kept", function);
      auto *flag = builder_.CreateLoad(builder_.getInt1Ty(), slots_[s.flag]);
      builder_.CreateCondBr(flag, doIt, after);
      builder_.SetInsertPoint(doIt);
      letGo(held, slots_[s.place]);
      builder_.CreateBr(after);
      builder_.SetInsertPoint(after);
      return;
    }

    llvm::Value *value = evaluate(s.value);
    if (!value)
      return;

    // Down to the one thing being written, which leaves everything beside it
    // exactly where it was — the whole point of a place having parts.
    if (!s.parts.empty()) {
      std::string held = withoutLoan(localType(s.place));
      llvm::Value *where = isLoan(localType(s.place))
                               ? builder_.CreateLoad(builder_.getPtrTy(),
                                                     slots_[s.place])
                               : slots_[s.place];
      for (unsigned part : s.parts) {
        const Shape *shape = shapeOf(held);
        if (!shape || part >= shape->fields.size())
          return;
        where = builder_.CreateStructGEP(typeFor(held), where, part);
        held = spellOf(shape->fields[part].type);
      }
      if (!copiesNamed(held) && held == "str")
        builder_.CreateCall(runtime_["xag_str_drop"], {where});
      builder_.CreateStore(value, where);
      return;
    }

    // A value going into something that may hold nothing is that value, held.
    // The absence writes itself; everything else has to be wrapped on the way.
    const std::string &into = localType(s.place);
    if (mayBeNothing(into) && !mayBeNothing(nameOf(s.value.type))) {
      auto *shell = llvm::UndefValue::get(typeFor(into));
      auto *held = builder_.CreateInsertValue(shell, builder_.getInt1(true), 0);
      builder_.CreateStore(builder_.CreateInsertValue(held, value, 1),
                           slots_[s.place]);
      return;
    }
    // Writing to a name that holds a loan writes through it.
    if (isLoan(localType(s.place)) && nameOf(s.value.type) == "str") {
      auto *through = builder_.CreateLoad(builder_.getPtrTy(), slots_[s.place]);
      builder_.CreateCall(runtime_["xag_str_drop"], {through});
      builder_.CreateStore(value, through);
      return;
    }
    builder_.CreateStore(value, slots_[s.place]);
  }

  // Where a `many` sits, whether the local holds one or a loan of one.
  llvm::Value *manyPointer(const Operand &operand) {
    if (operand.kind == OperandKind::Written)
      return nullptr;
    const std::string &held = localType(operand.local);
    if (isLoan(held))
      return builder_.CreateLoad(builder_.getPtrTy(), slots_[operand.local]);
    return slots_[operand.local];
  }

  // The address of one place.
  //
  // The rule lives in the runtime, and every engine asks it — but the half of
  // it that says yes is written out here as a compare and a branch, because a
  // call the optimiser cannot see into is a call it cannot remove, and this one
  // sits in the middle of every loop over a `many`. An unsigned compare covers
  // a negative index and an empty array at once: both are outside.
  llvm::Value *placePointer(llvm::Value *array, llvm::Value *index,
                            const std::string &element, bool settled = false) {
    auto *whole = builder_.CreateLoad(many_, array);
    auto *base = builder_.CreateExtractValue(whole, 0);
    auto *length = builder_.CreateExtractValue(whole, 1);
    // Already answered where the program was read: the place and the length
    // were both written down, and the place is one this `many` has. Asking a
    // second time costs a compare and a branch in the middle of every loop, and
    // the answer cannot have changed.
    if (settled)
      return builder_.CreateGEP(typeFor(element), base, index);
    llvm::Function *function = builder_.GetInsertBlock()->getParent();
    llvm::BasicBlock *asked = builder_.GetInsertBlock();
    auto *inside = llvm::BasicBlock::Create(context_, "inside", function);
    auto *outside = llvm::BasicBlock::Create(context_, "outside", function);
    builder_.CreateCondBr(builder_.CreateICmpULT(index, length), inside, outside);

    builder_.SetInsertPoint(outside);
    llvm::Value *wrapped = nullptr;
    if (mir_.settings.wrapsOutOfRange) {
      wrapped = builder_.CreateCall(runtime_["xag_many_place"],
                                    {index, length, builder_.getInt32(1)});
      builder_.CreateBr(inside);
    } else {
      builder_.CreateCall(runtime_["xag_many_out_of_range"], {index, length});
      builder_.CreateUnreachable();
    }
    llvm::BasicBlock *wentAround = builder_.GetInsertBlock();

    builder_.SetInsertPoint(inside);
    llvm::Value *at = index;
    if (wrapped) {
      auto *both = builder_.CreatePHI(builder_.getInt64Ty(), 2);
      both->addIncoming(index, asked);
      both->addIncoming(wrapped, wentAround);
      at = both;
    }
    return builder_.CreateGEP(typeFor(element), base, at);
  }

  // An index arrives as whatever width it was written at; the runtime asks in
  // int64, which is what `count` answers with anyway.
  llvm::Value *asIndex(const Operand &operand) {
    auto *value = read(operand);
    return builder_.CreateSExtOrTrunc(value, builder_.getInt64Ty());
  }

  llvm::Value *evaluate(const RValue &value) {
    switch (value.kind) {
    case RValueKind::Collect: {
      const std::string element = elementOf(nameOf(value.type));
      auto *made = builder_.CreateAlloca(many_, nullptr, "collected");
      const unsigned count = static_cast<unsigned>(value.operands.size());
      builder_.CreateCall(
          runtime_["xag_many_new"],
          {made, builder_.getInt64(count), builder_.getInt64(strideOf(element))});
      if (count) {
        auto *whole = builder_.CreateLoad(many_, made);
        auto *base = builder_.CreateExtractValue(whole, 0);
        for (unsigned i = 0; i < count; ++i) {
          auto *at = builder_.CreateGEP(typeFor(element), base, builder_.getInt64(i));
          if (element == "str")
            builder_.CreateStore(
                builder_.CreateLoad(str_, textPointer(value.operands[i])), at);
          else
            builder_.CreateStore(read(value.operands[i]), at);
        }
      }
      return builder_.CreateLoad(many_, made);
    }

    case RValueKind::Fill: {
      const std::string element = elementOf(nameOf(value.type));
      auto *made = builder_.CreateAlloca(many_, nullptr, "filled");
      auto *howMany = asIndex(value.operands[1]);
      builder_.CreateCall(runtime_["xag_many_new"],
                          {made, howMany, builder_.getInt64(strideOf(element))});
      auto *one = builder_.CreateAlloca(typeFor(element), nullptr, "one");
      builder_.CreateStore(read(value.operands[0]), one);
      builder_.CreateCall(runtime_["xag_many_fill"],
                          {made, builder_.getInt64(strideOf(element)), one});
      return builder_.CreateLoad(many_, made);
    }

    case RValueKind::Element: {
      const std::string element = elementOf(nameOf(value.operands[0].type));
      auto *place = placePointer(manyPointer(value.operands[0]),
                                 asIndex(value.operands[1]), element, value.settled);
      // What copies is read out; what does not is lent where it stands. Asking
      // only whether it was text was right while text was the only thing a
      // place could own — a struct in a `many` was loaded into a slot that held
      // a pointer, and reading one of its fields found nothing.
      return copiesNamed(element)
                 ? builder_.CreateLoad(typeFor(element), place)
                 : place;
    }

    case RValueKind::Use:
      return value.operands.empty() ? nullptr : read(value.operands[0]);

    case RValueKind::Ref:
      // Lending something that is itself a loan passes the loan along; it does
      // not make a loan of the pointer. Taking the address here handed the
      // callee somewhere to find an `XagStr` rather than the `XagStr`, and
      // every borrow passed through two functions read rubbish. Both
      // interpreters follow a chain of loans, which is why only this engine
      // was wrong and no vote between the other two would have said so.
      return isLoan(localType(value.local))
                 ? builder_.CreateLoad(builder_.getPtrTy(), slots_[value.local])
                 : slots_[value.local];

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

    case RValueKind::Group: {
      auto *shell = typeFor(nameOf(value.type));
      llvm::Value *built = llvm::UndefValue::get(shell);
      for (unsigned i = 0; i < value.operands.size(); ++i)
        built = builder_.CreateInsertValue(built, read(value.operands[i]), i);
      return built;
    }

    case RValueKind::Taken: {
      const std::string of = operandType(value.operands[0]);
      const Shape *shape = shapeOf(withoutLoan(of));
      if (!shape || value.local >= shape->fields.size())
        return nullptr;
      const std::string inner = spellOf(shape->fields[value.local].type);
      auto *where = isLoan(of) ? builder_.CreateLoad(builder_.getPtrTy(),
                                                     slots_[value.operands[0].local])
                               : slots_[value.operands[0].local];
      auto *at = builder_.CreateStructGEP(typeFor(withoutLoan(of)), where,
                                          value.local);
      auto *got = builder_.CreateLoad(typeFor(inner), at);
      // What is left behind holds nothing, so the drop at the end of the scope
      // has nothing to let go of.
      builder_.CreateStore(llvm::Constant::getNullValue(typeFor(inner)), at);
      return got;
    }

    case RValueKind::Part: {
      const std::string of = operandType(value.operands[0]);
      const Shape *shape = shapeOf(withoutLoan(of));
      if (!shape || value.local >= shape->fields.size())
        return nullptr;
      const std::string inner = spellOf(shape->fields[value.local].type);
      // What copies is read out; what has an owner is lent where it stands.
      auto *where = isLoan(of)
                        ? builder_.CreateLoad(builder_.getPtrTy(),
                                              slots_[value.operands[0].local])
                        : slots_[value.operands[0].local];
      auto *at = builder_.CreateStructGEP(typeFor(withoutLoan(of)), where,
                                          value.local);
      return copiesNamed(inner) ? builder_.CreateLoad(typeFor(inner), at)
                                : static_cast<llvm::Value *>(at);
    }

    case RValueKind::Holds: {
      auto *whole = read(value.operands[0]);
      return builder_.CreateExtractValue(whole, 0);
    }

    case RValueKind::Inside: {
      // Lent where what is inside has an owner, read out where it has not.
      const std::string held = within(operandType(value.operands[0]));
      auto *where = slots_[value.operands[0].local];
      auto *at = builder_.CreateStructGEP(typeFor(operandType(value.operands[0])),
                                          where, 1);
      return copiesNamed(held) ? builder_.CreateLoad(typeFor(held), at)
                               : static_cast<llvm::Value *>(at);
    }

    case RValueKind::Call:
      return call(value);
    }
    return nullptr;
  }

  // One place, in bytes. The layout is the machine's, asked of the module's own
  // data layout rather than guessed at.
  uint64_t strideOf(const std::string &element) {
    return module_.getDataLayout().getTypeAllocSize(typeFor(element));
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
    if (isDecimal(given)) {
      auto *carrier = builder_.getInt128Ty();
      auto *say = builder_.getInt32(static_cast<int>(widthOf(given)));
      auto *x = builder_.CreateZExt(left, carrier);
      auto *y = builder_.CreateZExt(right, carrier);
      if (op == "+" || op == "-" || op == "x" || op == "/" || op == "mod" ||
          op == "^") {
        const char *called = op == "+"     ? "xag_deci_add"
                             : op == "-"   ? "xag_deci_sub"
                             : op == "x"   ? "xag_deci_mul"
                             : op == "/"   ? "xag_deci_div"
                             : op == "mod" ? "xag_deci_mod"
                                           : "xag_deci_pow";
        return builder_.CreateTrunc(builder_.CreateCall(runtime_[called], {say, x, y}),
                                    typeFor(nameOf(value.type)));
      }
      auto *order = builder_.CreateCall(runtime_["xag_deci_compare"], {say, x, y});
      auto *unordered = builder_.CreateICmpEQ(order, builder_.getInt32(-3));
      auto *zero = builder_.getInt32(0);
      llvm::Value *asked = nullptr;
      if (op == "==") asked = builder_.CreateICmpEQ(order, zero);
      else if (op == "!==") asked = builder_.CreateICmpNE(order, zero);
      else if (op == "<") asked = builder_.CreateICmpSLT(order, zero);
      else if (op == ">") asked = builder_.CreateICmpSGT(order, zero);
      else if (op == "<==") asked = builder_.CreateICmpSLE(order, zero);
      else if (op == ">==") asked = builder_.CreateICmpSGE(order, zero);
      if (!asked)
        return nullptr;
      return op == "!==" ? builder_.CreateOr(asked, unordered)
                         : builder_.CreateAnd(asked, builder_.CreateNot(unordered));
    }
    if (given == Type::Bin128) {
      if (op == "+" || op == "-" || op == "x" || op == "/" || op == "mod" ||
          op == "^") {
        const char *called = op == "+"     ? "xag_bin128_add"
                             : op == "-"   ? "xag_bin128_sub"
                             : op == "x"   ? "xag_bin128_mul"
                             : op == "/"   ? "xag_bin128_div"
                             : op == "mod" ? "xag_bin128_mod"
                                           : "xag_bin128_pow";
        return builder_.CreateCall(runtime_[called], {left, right});
      }
      auto *order = builder_.CreateCall(runtime_["xag_bin128_compare"], {left, right});
      auto *unordered = builder_.CreateICmpEQ(order, builder_.getInt32(-3));
      auto *zero = builder_.getInt32(0);
      llvm::Value *asked = nullptr;
      if (op == "==") asked = builder_.CreateICmpEQ(order, zero);
      else if (op == "!==") asked = builder_.CreateICmpNE(order, zero);
      else if (op == "<") asked = builder_.CreateICmpSLT(order, zero);
      else if (op == ">") asked = builder_.CreateICmpSGT(order, zero);
      else if (op == "<==") asked = builder_.CreateICmpSLE(order, zero);
      else if (op == ">==") asked = builder_.CreateICmpSGE(order, zero);
      if (!asked)
        return nullptr;
      // Nothing but `!==` is true of two that cannot be ordered.
      return op == "!==" ? builder_.CreateOr(asked, unordered)
                         : builder_.CreateAnd(asked, builder_.CreateNot(unordered));
    }
    if (isBinary(given)) {
      if (op == "+") return builder_.CreateFAdd(left, right);
      if (op == "-") return builder_.CreateFSub(left, right);
      if (op == "x") return builder_.CreateFMul(left, right);
      if (op == "/") return builder_.CreateFDiv(left, right);
      // An ordered comparison answers false when either side is not a number,
      // which is what IEEE says and what the interpreter does.
      if (op == "==") return builder_.CreateFCmpOEQ(left, right);
      if (op == "!==") return builder_.CreateFCmpUNE(left, right);
      if (op == "<") return builder_.CreateFCmpOLT(left, right);
      if (op == ">") return builder_.CreateFCmpOGT(left, right);
      if (op == "<==") return builder_.CreateFCmpOLE(left, right);
      if (op == ">==") return builder_.CreateFCmpOGE(left, right);
      auto *wide = builder_.getDoubleTy();
      auto *answered = builder_.CreateCall(
          runtime_[op == "mod" ? "xag_bin_mod" : "xag_bin_pow"],
          {builder_.CreateFPExt(left, wide), builder_.CreateFPExt(right, wide),
           builder_.getInt32(widthOf(given))});
      return builder_.CreateFPTrunc(answered, typeFor(nameOf(value.type)));
    }
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

    // Dividing has a question in front of it rather than inside it: what to do
    // about zero, and the one signed pair a machine has no answer for. Both are
    // compares against written numbers, so both fold away wherever the divisor
    // is written down — which is where dividing usually is. Calling out for it
    // instead cost about twenty times a machine's own instruction, because the
    // optimiser cannot see through a call and so never turns `mod *7*` into the
    // multiply and shift it is.
    if (op == "/" || op == "mod") {
      auto *none = llvm::ConstantInt::get(right->getType(), 0);
      llvm::Function *function = builder_.GetInsertBlock()->getParent();
      auto *stops = llvm::BasicBlock::Create(context_, "byzero", function);
      auto *goes = llvm::BasicBlock::Create(context_, "divides", function);
      builder_.CreateCondBr(builder_.CreateICmpEQ(right, none), stops, goes);

      builder_.SetInsertPoint(stops);
      builder_.CreateCall(
          runtime_["xag_stop"],
          {builder_.CreateGlobalString(op == "/"
                                           ? "a number was divided by zero"
                                           : "a remainder was taken against zero",
                                       "why")});
      builder_.CreateUnreachable();

      builder_.SetInsertPoint(goes);
      if (!isSigned(working))
        return op == "/" ? builder_.CreateUDiv(left, right)
                         : builder_.CreateURem(left, right);

      // The least number over -1 is the one quotient that does not fit, and it
      // wraps like every other; the remainder beside it is none. Dividing by
      // one instead and choosing afterwards keeps that pair off the
      // instruction, which has no answer for it at all.
      auto *one = llvm::ConstantInt::get(right->getType(), 1);
      auto *wraps = builder_.CreateICmpEQ(
          right, llvm::ConstantInt::getSigned(right->getType(), -1));
      auto *by = builder_.CreateSelect(wraps, one, right);
      auto *usual = op == "/" ? builder_.CreateSDiv(left, by)
                              : builder_.CreateSRem(left, by);
      auto *instead = op == "/" ? builder_.CreateSub(none, left)
                                : llvm::ConstantInt::get(left->getType(), 0);
      return builder_.CreateSelect(wraps, instead, usual);
    }

    // Raising to a power is a loop rather than an instruction, so it stays
    // where it is written once: a negative power has no whole answer.
    if (op != "^")
      return nullptr;
    auto *answered = builder_.CreateCall(
        runtime_["xag_int_pow"],
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
        if (isDecimal(named))
          builder_.CreateCall(
              runtime_["xag_print_deci"],
              {builder_.getInt32(static_cast<int>(widthOf(named))),
               builder_.CreateZExt(read(operand), builder_.getInt128Ty())});
        else if (named == Type::Bin128)
          builder_.CreateCall(runtime_["xag_print_bin128"], {read(operand)});
        else if (isBinary(named))
          builder_.CreateCall(
              runtime_["xag_print_bin"],
              {builder_.CreateFPExt(read(operand), builder_.getDoubleTy()),
               builder_.getInt32(widthOf(named))});
        else if (isWhole(named))
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

    if (value.callee == "read.stdin") {
      // The line comes back through a pointer and the answer says whether there
      // was one, which is exactly the two fields the type has.
      auto *shell = typeFor(nameOf(value.type));
      auto *line = builder_.CreateAlloca(str_, nullptr, "line");
      builder_.CreateStore(llvm::Constant::getNullValue(str_), line);
      auto *got = builder_.CreateCall(runtime_["xag_read_line"], {line});
      auto *there = builder_.CreateICmpNE(got, builder_.getInt32(0));
      auto *held = builder_.CreateInsertValue(llvm::UndefValue::get(shell), there, 0);
      return builder_.CreateInsertValue(held, builder_.CreateLoad(str_, line), 1);
    }

    if (value.callee == "arguments") {
      auto *out = builder_.CreateAlloca(many_, nullptr, "given");
      builder_.CreateCall(runtime_["xag_arguments"], {out});
      return builder_.CreateLoad(many_, out);
    }

    if (value.callee == "convert-to-number") {
      const std::string spelled = nameOf(value.type);
      const std::string wanted = within(spelled);
      const Type named = typeNamed(wanted);
      auto *shell = typeFor(spelled);
      auto *text = value.operands.empty() ? nullptr : textPointer(value.operands[0]);
      if (!text)
        return llvm::Constant::getNullValue(shell);

      // The bytes and how many, which is what every reader in the runtime asks
      // for; where the number goes differs by family and nothing else does.
      auto *bytes = builder_.CreateLoad(builder_.getPtrTy(), text);
      auto *length = builder_.CreateLoad(
          builder_.getInt64Ty(), builder_.CreateStructGEP(str_, text, 1));
      auto *room = builder_.CreateAlloca(typeFor(wanted), nullptr, "read");
      builder_.CreateStore(llvm::Constant::getNullValue(typeFor(wanted)), room);

      llvm::Value *got = nullptr;
      const auto width = builder_.getInt32(static_cast<int>(widthOf(named)));
      if (isWhole(named)) {
        auto *wide = builder_.CreateAlloca(builder_.getInt128Ty(), nullptr, "whole");
        got = builder_.CreateCall(
            runtime_["xag_int_reads"],
            {width, builder_.getInt32(isSigned(named) ? 1 : 0), bytes, length, wide});
        builder_.CreateStore(
            builder_.CreateTrunc(builder_.CreateLoad(builder_.getInt128Ty(), wide),
                                 typeFor(wanted)),
            room);
      } else if (isDecimal(named)) {
        auto *wide = builder_.CreateAlloca(builder_.getInt128Ty(), nullptr, "deci");
        got = builder_.CreateCall(runtime_["xag_deci_reads"],
                                  {width, bytes, length, wide});
        builder_.CreateStore(
            builder_.CreateTrunc(builder_.CreateLoad(builder_.getInt128Ty(), wide),
                                 typeFor(wanted)),
            room);
      } else if (named == Type::Bin128) {
        got = builder_.CreateCall(runtime_["xag_bin128_reads"], {bytes, length, room});
      } else {
        auto *wide = builder_.CreateAlloca(builder_.getDoubleTy(), nullptr, "bin");
        got = builder_.CreateCall(runtime_["xag_bin_reads"],
                                  {bytes, length, width, wide});
        builder_.CreateStore(
            builder_.CreateFPCast(builder_.CreateLoad(builder_.getDoubleTy(), wide),
                                  typeFor(wanted)),
            room);
      }

      auto *there = builder_.CreateICmpNE(got, builder_.getInt32(0));
      auto *held = builder_.CreateInsertValue(llvm::UndefValue::get(shell), there, 0);
      return builder_.CreateInsertValue(held,
                                        builder_.CreateLoad(typeFor(wanted), room), 1);
    }

    // Written out by the very code that prints, so a number on the screen and
    // the same number in a `str` can never come out differently.
    if (value.callee == "convert-to-str") {
      auto *out = builder_.CreateAlloca(str_, nullptr, "written");
      if (!value.operands.empty()) {
        const std::string given = nameOf(value.operands[0].type);
        const Type named = typeNamed(given);
        llvm::Value *held = read(value.operands[0]);
        if (named == Type::Bool)
          builder_.CreateCall(runtime_["xag_str_of_bool"],
                              {out, builder_.CreateZExt(held, builder_.getInt32Ty())});
        else if (isDecimal(named))
          builder_.CreateCall(
              runtime_["xag_str_of_deci"],
              {out, builder_.getInt32(static_cast<int>(widthOf(named))),
               builder_.CreateZExt(held, builder_.getInt128Ty())});
        else if (named == Type::Bin128)
          builder_.CreateCall(runtime_["xag_str_of_bin128"], {out, held});
        else if (isBinary(named))
          builder_.CreateCall(
              runtime_["xag_str_of_bin"],
              {out, builder_.CreateFPExt(held, builder_.getDoubleTy()),
               builder_.getInt32(static_cast<int>(widthOf(named)))});
        else
          builder_.CreateCall(
              runtime_["xag_str_of_int"],
              {out, widened(held, named),
               builder_.getInt32(static_cast<int>(widthOf(named))),
               builder_.getInt32(isSigned(named) ? 1 : 0)});
      }
      return builder_.CreateLoad(str_, out);
    }

    if (value.callee == "count") {
      if (!value.operands.empty() && holdsMany(operandType(value.operands[0]))) {
        auto *whole = builder_.CreateLoad(many_, manyPointer(value.operands[0]));
        return builder_.CreateExtractValue(whole, 1);
      }
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
    // `main` takes what it was given so the program can ask for it. The name it
    // was run under is skipped: it is not something anybody passed.
    auto *type = llvm::FunctionType::get(
        builder_.getInt32Ty(), {builder_.getInt32Ty(), builder_.getPtrTy()}, false);
    auto *main = llvm::Function::Create(type, llvm::Function::ExternalLinkage, "main",
                                        module_);
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", main));
    auto *count = main->getArg(0);
    auto *values = main->getArg(1);
    auto *without = builder_.CreateSub(count, builder_.getInt32(1));
    auto *past = builder_.CreateGEP(builder_.getPtrTy(), values, builder_.getInt64(1));
    builder_.CreateCall(runtime_["xag_set_arguments"], {without, past});

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
