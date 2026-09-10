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

// A value handed over by being copied, which is everything but text and the
// things that hold it.
// What is left once the `or-nothing` is off it.
// A type as the middle layer spells it, which is the one language both the
// checker and this file already speak.
std::string spellOf(Ty type) {
  return type.kind == Type::Unknown ? std::string("?") : name(type);
}

// The same questions, asked of the type rather than of its spelling. The
// spellings survive only where a string is genuinely wanted; everything that
// used to pull one apart asks these.
bool isLoan(const MirType &type) { return type.isLoan(); }
bool holdsMany(const MirType &type) { return type.many && !type.orNothing; }
bool mayBeNothing(const MirType &type) { return type.orNothing; }
MirType withoutLoan(const MirType &type) { return type.lent(); }
MirType within(const MirType &type) { return type.within(); }
MirType elementOf(const MirType &type) { return type.element(); }

// Plain text: not lent, not absent, not several.
bool isText(const MirType &type) {
  return !type.isLoan() && !type.many && !type.orNothing && type.held == Type::Str;
}

// A value handed over by being copied, which is everything but text, the things
// that hold it, and a loan — a loan is a pointer, and copying one would be
// copying the borrow rather than what it borrows.
// A struct's fields are held as the checker's `Ty`; this is the same type as
// the middle layer holds it.
MirType asMirType(Ty type) {
  MirType out;
  // How a field is held. This was dropped on the floor, because until the
  // checker learned what a borrow is there was nothing here to read — so a
  // struct holding `loan.int64` was laid out with a whole number where a
  // pointer goes, and building one failed LLVM's own verifier. Nothing had ever
  // written such a struct: no example, no test, and the generator does not.
  //
  // It also decides whether the field owns what it points at. A borrowed `str`
  // read as an owned one is a free of something somebody else still holds.
  out.lending = type.held == Held::Loan      ? MirType::Lending::Read
                : type.held == Held::LoanMut ? MirType::Lending::Write
                                             : MirType::Lending::None;
  out.orNothing = type.orNothing;
  out.many = type.holds();
  out.held = type.holds() ? type.element : type.kind;
  out.named = type.named;
  return out;
}

bool copiesNamed(const MirType &type) {
  if (type.isLoan() || type.many || type.orNothing)
    return false;
  return type.held == Type::Bool || isNumber(type.held);
}

class Emitter {
public:
  Emitter(const Mir &mir, bool watching = false)
      : mir_(mir), watching_(watching), module_("xag", context_), builder_(context_) {
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
    // One field more: the room it is not using yet. A second type rather than a
    // wider `many`, so that everything holding a fixed one stays the size it is.
    growing_ = llvm::StructType::create(
        context_,
        {builder_.getPtrTy(), builder_.getInt64Ty(), builder_.getInt64Ty()},
        "XagGrowing");
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
  // Whether this is the build the compiler makes to run a program while
  // compiling it. Nothing a reader is ever handed is built with this on.
  const bool watching_ = false;
  llvm::LLVMContext context_;
  llvm::Module module_;
  llvm::IRBuilder<> builder_;
  llvm::StructType *str_ = nullptr;
  llvm::StructType *many_ = nullptr;
  llvm::StructType *growing_ = nullptr;
  std::unordered_map<std::string, llvm::StructType *> shapes_;
  std::unordered_map<std::string, llvm::StructType *> sums_;

  std::unordered_map<std::string, llvm::Function *> functions_;
  std::unordered_map<std::string, llvm::FunctionCallee> runtime_;
  // Where the statement being lowered was written, so a sum that comes round
  // can say. An RValue has no span of its own; the statement holding it does.
  unsigned at_ = 0;

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
  bool ownsAnything(const MirType &type) const {
    // A borrow owns nothing, whatever it borrows. Taking the loan off first and
    // asking about what is underneath says the opposite, and said it about
    // every borrowed field a struct holds — which was harmless while nothing
    // could tell the middle layer that a field was a borrow, and a double free
    // the moment something could.
    if (type.isLoan())
      return false;
    const MirType bare = withoutLoan(type);
    if (copiesNamed(bare) || bare.held == Type::Nothing)
      return false;
    if (mayBeNothing(bare))
      return ownsAnything(within(bare));
    if (holdsMany(bare))
      return true; // the places themselves have an owner, whatever is in them
    if (bare.held == Type::Struct) {
      for (const Field &field : mir_.shapes[bare.named].fields)
        if (ownsAnything(asMirType(field.type)))
          return true;
      return false;
    }
    return bare.held == Type::Str;
  }

  // Letting go of one value, whatever it is made of.
  //
  // This walks: a struct lets go of each of the things it holds, and one of
  // those may be another struct, or a `many`, or something that may hold
  // nothing. Choosing between three runtime calls with a chain of `?:` instead
  // sent a struct held inside a struct to `xag_many_drop`, which was handed
  // something that was never allocated and aborted.
  void letGo(const MirType &type, llvm::Value *at) {
    // Letting go of a borrow is doing nothing: what it points at belongs to
    // whoever lent it, and is still theirs afterwards.
    if (type.isLoan())
      return;
    const MirType bare = withoutLoan(type);
    if (!ownsAnything(bare))
      return;
    if (isText(bare)) {
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
      const MirType element = elementOf(bare);
      const bool grows = bare.grows;
      if (isText(element)) {
        builder_.CreateCall(runtime_[grows ? "xag_growing_drop_str"
                                           : "xag_many_drop_str"],
                            {at});
        return;
      }
      if (ownsAnything(element))
        letGoOfEveryPlace(element, at);
      builder_.CreateCall(runtime_[grows ? "xag_growing_drop" : "xag_many_drop"], {at});
      return;
    }
    if (bare.held == Type::Struct) {
      const Shape &shape = mir_.shapes[bare.named];
      for (unsigned i = 0; i < shape.fields.size(); ++i)
        letGo(asMirType(shape.fields[i].type),
              builder_.CreateStructGEP(typeFor(bare), at, i));
    }
  }

  // Every place of a `many`, one at a time, before the array itself goes. The
  // runtime has a walk of its own for text, which is the common case; anything
  // else is written out here because what one place holds is not something the
  // runtime knows the shape of.
  void letGoOfEveryPlace(const MirType &element, llvm::Value *array) {
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

  // The type as the middle layer took it apart. Everything here asks this
  // rather than reading the spelling and taking it apart again — which is what
  // four of this week's bugs were: a loan taken for the thing it lends.
  const MirType &typing(TypeRef type) const {
    static const MirType nothing;
    return type.index < body_->typed.size() ? body_->typed[type.index] : nothing;
  }

  const MirType &typing(unsigned local) const {
    return typing(body_->locals[local].type);
  }

  llvm::Type *typeFor(const MirType &type) {
    if (type.isLoan())
      return builder_.getPtrTy();
    if (type.orNothing)
      // Whether it is there, and what it is. Two fields, because a `str` that
      // is absent and a `str` that is empty are different things and no bit
      // pattern of one is free to mean the other.
      return llvm::StructType::get(context_,
                                   {builder_.getInt1Ty(), typeFor(type.within())});
    if (type.many)
      return type.grows ? growing_ : many_;
    if (type.held == Type::Bool)
      return builder_.getInt1Ty();
    if (type.held == Type::Str)
      return str_;
    if (type.held == Type::Struct)
      return structFor(type.named);
    if (type.held == Type::OneOf)
      return sumFor(type.named);
    return typeFor(type.held);
  }

  // Which of the things it may be, and room for whichever that is.
  //
  // The room is counted in `i128` rather than in bytes, so that it is aligned
  // for anything a case could hold — a `deci128` wants sixteen bytes of
  // alignment, and a run of bytes gives one.
  llvm::Type *sumFor(unsigned which) {
    const Shape &sum = mir_.sums[which];
    auto found = sums_.find(sum.name);
    if (found != sums_.end())
      return found->second;
    auto *made = llvm::StructType::create(context_, "xag." + sum.name);
    sums_[sum.name] = made;
    uint64_t widest = 1;
    const auto &held = mir_.caseTypes[which];
    for (const MirType &one : held) {
      if (one.held == Type::Nothing)
        continue;
      const uint64_t size = module_.getDataLayout().getTypeAllocSize(typeFor(one));
      widest = size > widest ? size : widest;
    }
    auto *wide = builder_.getInt128Ty();
    const uint64_t each = module_.getDataLayout().getTypeAllocSize(wide);
    made->setBody({builder_.getInt64Ty(),
                   llvm::ArrayType::get(wide, (widest + each - 1) / each)});
    return made;
  }

  llvm::Type *structFor(unsigned which) {
    const Shape &shape = mir_.shapes[which];
    auto found = shapes_.find(shape.name);
    if (found != shapes_.end())
      return found->second;
    // Named struct types, so the IR reads the way the program does.
    auto *made = llvm::StructType::create(context_, "xag." + shape.name);
    shapes_[shape.name] = made;
    std::vector<llvm::Type *> held;
    for (const Field &field : shape.fields)
      held.push_back(typeFor(asMirType(field.type)));
    made->setBody(held);
    return made;
  }

  // One of the types the standard names, as a machine holds it. Everything a
  // chain can put around one — a loan, an absence, a `many`, a struct — is
  // answered by the caller above, so what reaches here is always plain.
  llvm::Type *typeFor(Type named) {
    if (named == Type::Bool)
      return builder_.getInt1Ty();
    if (named == Type::Str)
      return str_;
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
    add("xag_came_round", voidTy, {i32});
    if (auto *stops = llvm::dyn_cast<llvm::Function>(runtime_["xag_stop"].getCallee()))
      stops->addFnAttr(llvm::Attribute::NoReturn);
    add("xag_str_from", voidTy, {ptr, ptr, i64});
    add("xag_str_join", voidTy, {ptr, ptr, i64});
    add("xag_str_count", i64, {ptr});
    add("xag_str_compare", i64, {ptr, ptr});
    add("xag_str_drop", voidTy, {ptr});
    add("xag_str_push", voidTy, {ptr, ptr});
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
    add("xag_would_read", voidTy, {});
    add("xag_would_take_time", voidTy, {});
    add("xag_arguments", voidTy, {ptr});
    add("xag_int_reads", i32, {i32, i32, ptr, i64, ptr});
    add("xag_deci_reads", i32, {i32, ptr, i64, ptr});
    add("xag_bin_reads", i32, {ptr, i64, i32, ptr});
    add("xag_bin128_reads", i32, {ptr, i64, ptr});
    add("xag_many_out_of_range", voidTy, {i64, i64});
    add("xag_many_new", voidTy, {ptr, i64, i64});
    add("xag_growing_new", voidTy, {ptr});
    add("xag_growing_add", voidTy, {ptr, i64, ptr});
    add("xag_growing_drop", voidTy, {ptr});
    add("xag_growing_drop_str", voidTy, {ptr});
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
      params.push_back(typeFor(typing(body.locals[i].type)));
    const MirType &result = typing(body.result);
    llvm::Type *answer =
        result.held == Type::Nothing ? builder_.getVoidTy() : typeFor(result);
    auto *type = llvm::FunctionType::get(answer, params, false);
    llvm::Function *made = llvm::Function::Create(
        type, llvm::Function::InternalLinkage, symbolFor(body.name), module_);

    // What the borrow rules already promise, said in the one language the
    // optimiser reads. These are the facts LLVM can never work out for itself:
    // it sees two pointers and must assume the worst of them, where the region
    // pass has already refused every program in which the worst is possible.
    //
    // A `loanmut` is the only loan of what it points at while it lasts — lending
    // the same thing twice for writing, or for writing and reading at once, is
    // `E0410`. So it cannot alias anything else the function was handed.
    //
    // A `loan` may share with other `loan`s, so it is not `noalias`. Nothing is
    // ever written through one, so it is `readonly`.
    for (unsigned i = 1; i <= body.parameters && i < body.locals.size(); ++i) {
      const MirType &held = typing(body.locals[i].type);
      if (!held.isLoan())
        continue;
      const unsigned at = i - 1;
      if (held.writesThrough())
        made->addParamAttr(at, llvm::Attribute::NoAlias);
      else
        made->addParamAttr(at, llvm::Attribute::ReadOnly);
    }
    functions_[body.name] = made;
  }

  // ---- defining

  void define(const Body &body) {
    body_ = &body;
    llvm::Function *function = functions_[body.name];
    auto *entry = llvm::BasicBlock::Create(context_, "entry", function);
    builder_.SetInsertPoint(entry);

    slots_.assign(body.locals.size(), nullptr);
    for (const Local &local : body.locals)
      slots_[local.id] = builder_.CreateAlloca(typeFor(typing(local.type)), nullptr,
                                               "_" + std::to_string(local.id));
    // Every slot starts empty, so a drop that reaches one never sees rubbish.
    for (const Local &local : body.locals) {
      const MirType &held = typing(local.type);
      if (isText(held))
        builder_.CreateStore(llvm::Constant::getNullValue(str_), slots_[local.id]);
      else if (holdsMany(held) && !isLoan(held))
        builder_.CreateStore(llvm::Constant::getNullValue(many_), slots_[local.id]);
      else if (mayBeNothing(held) && !isLoan(held))
        builder_.CreateStore(llvm::Constant::getNullValue(typeFor(held)),
                             slots_[local.id]);
      else if (held.held == Type::Struct && !isLoan(held))
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
      // A loop told not to be run while compiling. Only the build the compiler
      // makes stops here, and it stops in the same place the interpreter does,
      // so what happened before it still compares between the two.
      if (watching_ && block.noItmt) {
        builder_.CreateCall(runtime_["xag_would_take_time"], {});
        builder_.CreateUnreachable();
        continue;
      }
      for (const Statement &s : block.statements)
        statement(s);
      terminator(block.terminator, function);
    }
  }

  // ---- reading

  llvm::Value *textOf(const std::string &bytes) {
    auto *global = builder_.CreateGlobalString(bytes, "text");
    auto *slot = scratch(str_, "written");
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

  // What the operand holds, following a loan to the thing it points at. `read`
  // answers the loan itself, which is a pointer — and widening a pointer to a
  // number is not something LLVM will even build.
  llvm::Value *behind(const Operand &operand) {
    const MirType &spelled = typing(operand.type);
    if (!isLoan(spelled))
      return read(operand);
    return builder_.CreateLoad(typeFor(withoutLoan(spelled)), read(operand));
  }

  llvm::Value *read(const Operand &operand) {
    const MirType &type = typing(operand.type);
    switch (operand.kind) {
    case OperandKind::Written: {
      if (operand.written == "nothing" && mayBeNothing(type)) {
        auto *shell = typeFor(type);
        auto *none = llvm::Constant::getNullValue(shell);
        return none; // the flag is false, and what it does not hold is not read
      }
      if (type.held == Type::Bool && !type.isLoan())
        return builder_.getInt1(operand.written == "true");
      const Type named = type.lent().held;
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
        // Built at the width the number is, not at whatever is wrapped around
        // it. Asked of the whole type, `or-nothing bin32` handed `ConstantFP`
        // a two-field struct and it answered with a `ppc_fp128` sitting in a
        // slot shaped for a `float`. Every other branch here reads `named`;
        // this one did not.
        return llvm::ConstantFP::get(typeFor(named), read);
      }
      return textOf(unescape(operand.written));
    }
    case OperandKind::Copy:
      return builder_.CreateLoad(typeFor(localType(operand.local)), slots_[operand.local]);
    case OperandKind::Move: {
      auto *taken =
          builder_.CreateLoad(typeFor(localType(operand.local)), slots_[operand.local]);
      // What was taken is gone from where it was, so a stray drop finds nothing.
      if (localType(operand.local).held == Type::Str && !localType(operand.local).isLoan())
        builder_.CreateStore(llvm::Constant::getNullValue(str_), slots_[operand.local]);
      else if (localType(operand.local).many && !localType(operand.local).isLoan())
        builder_.CreateStore(llvm::Constant::getNullValue(many_), slots_[operand.local]);
      return taken;
    }
    }
    return nullptr;
  }

  const MirType &localType(unsigned local) const {
    return typing(body_->locals[local].type);
  }

  // What an operand actually holds — the local's type, since the operand's own
  // spelling is what was asked of it rather than what is there.
  const MirType &operandType(const Operand &operand) const {
    static const MirType written;
    return operand.kind == OperandKind::Written ? written : localType(operand.local);
  }

  // A pointer to text, whether the operand names it, lends it, or wrote it.
  llvm::Value *textPointer(const Operand &operand) {
    const MirType &type = typing(operand.type);
    if (operand.kind != OperandKind::Written) {
      const MirType &held = localType(operand.local);
      if (isText(held))
        return slots_[operand.local];
      if (isLoan(held))
        return builder_.CreateLoad(builder_.getPtrTy(), slots_[operand.local]);
    }
    auto *slot = scratch(str_, "piece");
    if (isText(type) || isLoan(type))
      builder_.CreateStore(read(operand), slot);
    else
      builder_.CreateStore(llvm::Constant::getNullValue(str_), slot);
    return slot;
  }

  // ---- statements

  void statement(const Statement &s) {
    at_ = s.span.begin;
    // Leave where we are, so that a stop can say where. A store rather than a
    // call, and only in the build that is asked to keep track — a reader's
    // program does none of this.
    if (watching_ && s.span.begin != 0)
      builder_.CreateStore(builder_.getInt32(s.span.begin), whereWeAre());
    // One more place at the end. The room it keeps is the last field, so
    // everything that reads a `many`'s places and length reads one of these
    // unchanged — which is why `count`, indexing and letting go needed nothing.
    if (s.kind == StatementKind::Grow) {
      if (s.value.operands.empty())
        return;
      const MirType held = localType(s.place);
      const MirType element = elementOf(held);
      auto *where = isLoan(held)
                        ? builder_.CreateLoad(builder_.getPtrTy(), slots_[s.place])
                        : slots_[s.place];
      auto *one = builder_.CreateAlloca(typeFor(element), nullptr, "one");
      if (isText(element))
        builder_.CreateStore(builder_.CreateLoad(str_, textPointer(s.value.operands[0])),
                             one);
      else
        builder_.CreateStore(read(s.value.operands[0]), one);
      builder_.CreateCall(runtime_["xag_growing_add"],
                          {where, builder_.getInt64(strideOf(element)), one});
      return;
    }
    if (s.kind == StatementKind::Store) {
      const MirType held = localType(s.place);
      const MirType element = elementOf(held);
      auto *place = placePointer(
          isLoan(held) ? builder_.CreateLoad(builder_.getPtrTy(), slots_[s.place])
                       : slots_[s.place],
          builder_.CreateSExtOrTrunc(read(s.at), builder_.getInt64Ty()), element,
          s.value.settled);
      if (isText(element)) {
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
      const MirType held = localType(s.place);
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
      MirType held = localType(s.place).lent();
      llvm::Value *where = localType(s.place).isLoan()
                               ? builder_.CreateLoad(builder_.getPtrTy(),
                                                     slots_[s.place])
                               : slots_[s.place];
      for (unsigned part : s.parts) {
        if (held.held != Type::Struct ||
            part >= mir_.shapes[held.named].fields.size())
          return;
        where = builder_.CreateStructGEP(typeFor(held), where, part);
        held = asMirType(mir_.shapes[held.named].fields[part].type);
      }
      if (!copiesNamed(held) && isText(held))
        builder_.CreateCall(runtime_["xag_str_drop"], {where});
      builder_.CreateStore(value, where);
      return;
    }

    // A value going into something that may hold nothing is that value, held.
    // The absence writes itself; everything else has to be wrapped on the way.
    const MirType &into = localType(s.place);
    if (mayBeNothing(into) && !mayBeNothing(typing(s.value.type))) {
      auto *shell = llvm::UndefValue::get(typeFor(into));
      auto *held = builder_.CreateInsertValue(shell, builder_.getInt1(true), 0);
      builder_.CreateStore(builder_.CreateInsertValue(held, value, 1),
                           slots_[s.place]);
      return;
    }
    // Writing to a name that holds a loan writes through it — whatever it
    // lends. Only text was ever asked here, so writing through a loan of a
    // number stored the number *over the loan* and the next read of it followed
    // a pointer that was a `3`.
    // Writing *through* a loan, not making one. `_3 = loanmut 't'` puts the
    // address of `'t'` into `_3`; `set 't' = […]` through that loan puts a value
    // where `_3` points. Telling them apart is what the value's own type says:
    // a loan going in is a loan being made. Getting this wrong stored through a
    // slot that had nothing in it yet.
    if (localType(s.place).isLoan() && !isLoan(typing(s.value.type)) &&
        s.value.kind != RValueKind::Ref) {
      const MirType lent = localType(s.place).lent();
      auto *through = builder_.CreateLoad(builder_.getPtrTy(), slots_[s.place]);
      // What was there is let go of first: putting a value in does not make
      // room by forgetting the one it replaces.
      if (isText(lent))
        builder_.CreateCall(runtime_["xag_str_drop"], {through});
      builder_.CreateStore(value, through);
      return;
    }
    builder_.CreateStore(value, slots_[s.place]);
  }

  // One value, written out, however deep it goes.
  //
  // A `many` writes every place it holds and a struct every field, one after
  // another with nothing between them — the same as writing those values side
  // by side in the print, because that is what it is. Anything between them is
  // something the program writes.
  //
  // How many places there are is not known until it runs, so a `many` is a real
  // loop here rather than a print per place worked out at build. A struct is
  // written down, so its fields are.
  // `into` is where the characters go: nothing, and they are written out; a
  // `str`, and they are added to the end of it. One walk either way, because
  // `convert-to-str` promises exactly the characters a print would write and
  // two walks are two chances to drift.
  void showAll(llvm::Value *where, const MirType &type, llvm::Value *into = nullptr) {
    if (type.isLoan()) {
      showAll(builder_.CreateLoad(builder_.getPtrTy(), where), type.lent(), into);
      return;
    }
    if (type.many > 0) {
      auto *whole = builder_.CreateLoad(many_, where);
      auto *base = builder_.CreateExtractValue(whole, 0);
      auto *length = builder_.CreateExtractValue(whole, 1);
      const MirType inner = type.element();
      auto *held = typeFor(inner);
      llvm::Function *function = builder_.GetInsertBlock()->getParent();
      auto *asking = llvm::BasicBlock::Create(context_, "showing", function);
      auto *shows = llvm::BasicBlock::Create(context_, "shows", function);
      auto *shown = llvm::BasicBlock::Create(context_, "shown", function);
      auto *counter = scratch(builder_.getInt64Ty(), "showing");
      builder_.CreateStore(builder_.getInt64(0), counter);
      builder_.CreateBr(asking);

      builder_.SetInsertPoint(asking);
      auto *at = builder_.CreateLoad(builder_.getInt64Ty(), counter);
      builder_.CreateCondBr(builder_.CreateICmpULT(at, length), shows, shown);

      builder_.SetInsertPoint(shows);
      showAll(builder_.CreateGEP(held, base, at), inner, into);
      // Read again rather than reused: showing one place may itself have been a
      // loop, and the block this ends in is not the block it began in.
      auto *now = builder_.CreateLoad(builder_.getInt64Ty(), counter);
      builder_.CreateStore(builder_.CreateAdd(now, builder_.getInt64(1)), counter);
      builder_.CreateBr(asking);

      builder_.SetInsertPoint(shown);
      return;
    }
    if (type.held == Type::Struct) {
      if (type.named >= mir_.shapes.size())
        return;
      auto *shell = typeFor(type);
      const Shape &shape = mir_.shapes[type.named];
      for (unsigned i = 0; i < shape.fields.size(); ++i)
        showAll(builder_.CreateStructGEP(shell, where, i),
                asMirType(shape.fields[i].type), into);
      return;
    }
    const Type named = type.held;
    if (named == Type::Str) {
      if (into)
        builder_.CreateCall(runtime_["xag_str_push"], {into, where});
      else
        builder_.CreateCall(runtime_["xag_print"], {where});
      return;
    }
    auto *value = builder_.CreateLoad(typeFor(type), where);
    auto *one = into ? scratch(str_, "one") : nullptr;
    if (isDecimal(named))
      builder_.CreateCall(
          runtime_[into ? "xag_str_of_deci" : "xag_print_deci"],
          into ? llvm::ArrayRef<llvm::Value *>{
                     one, builder_.getInt32(static_cast<int>(widthOf(named))),
                     builder_.CreateZExt(value, builder_.getInt128Ty())}
               : llvm::ArrayRef<llvm::Value *>{
                     builder_.getInt32(static_cast<int>(widthOf(named))),
                     builder_.CreateZExt(value, builder_.getInt128Ty())});
    else if (named == Type::Bin128)
      builder_.CreateCall(runtime_[into ? "xag_str_of_bin128" : "xag_print_bin128"],
                          into ? llvm::ArrayRef<llvm::Value *>{one, value}
                               : llvm::ArrayRef<llvm::Value *>{value});
    else if (isBinary(named))
      builder_.CreateCall(
          runtime_[into ? "xag_str_of_bin" : "xag_print_bin"],
          into ? llvm::ArrayRef<llvm::Value *>{
                     one, builder_.CreateFPExt(value, builder_.getDoubleTy()),
                     builder_.getInt32(widthOf(named))}
               : llvm::ArrayRef<llvm::Value *>{
                     builder_.CreateFPExt(value, builder_.getDoubleTy()),
                     builder_.getInt32(widthOf(named))});
    else if (isWhole(named))
      builder_.CreateCall(
          runtime_[into ? "xag_str_of_int" : "xag_print_int"],
          into ? llvm::ArrayRef<llvm::Value *>{one, widened(value, named),
                                               builder_.getInt32(widthOf(named)),
                                               builder_.getInt32(isSigned(named) ? 1 : 0)}
               : llvm::ArrayRef<llvm::Value *>{widened(value, named),
                                               builder_.getInt32(widthOf(named)),
                                               builder_.getInt32(isSigned(named) ? 1 : 0)});
    else if (named == Type::Bool)
      builder_.CreateCall(
          runtime_[into ? "xag_str_of_bool" : "xag_print_bool"],
          into ? llvm::ArrayRef<llvm::Value *>{
                     one, builder_.CreateZExt(value, builder_.getInt32Ty())}
               : llvm::ArrayRef<llvm::Value *>{
                     builder_.CreateZExt(value, builder_.getInt32Ty())});
    else
      return;
    if (into) {
      builder_.CreateCall(runtime_["xag_str_push"], {into, one});
      builder_.CreateCall(runtime_["xag_str_drop"], {one});
    }
  }

  // Where a value sits, whether the local holds it or a loan of it. Named for
  // the `many` it was written for, and true of anything with an address.
  llvm::Value *manyPointer(const Operand &operand) {
    if (operand.kind == OperandKind::Written)
      return nullptr;
    const MirType &held = localType(operand.local);
    if (held.isLoan())
      return builder_.CreateLoad(builder_.getPtrTy(), slots_[operand.local]);
    return slots_[operand.local];
  }

  // Scratch that lives for one statement, made where every alloca belongs: the
  // top of the function.
  //
  // Written where the statement stands, an `alloca` inside a loop is one the
  // optimiser will not move, and a loop holding one is a loop it stops
  // reasoning about — a walk over a `many` with a `print` in it kept a bounds
  // check the loop's own end had already answered. Nothing here outlives the
  // statement that asks for it, so one at the top, reused every turn, is the
  // same thing said in the place that costs nothing.
  llvm::Value *scratch(llvm::Type *type, const char *name, llvm::Value *count = nullptr) {
    llvm::BasicBlock &entry = builder_.GetInsertBlock()->getParent()->getEntryBlock();
    llvm::IRBuilder<> top(&entry, entry.getFirstInsertionPt());
    return top.CreateAlloca(type, count, name);
  }

  // The address of one place.
  //
  // The rule lives in the runtime, and every engine asks it — but the half of
  // it that says yes is written out here as a compare and a branch, because a
  // call the optimiser cannot see into is a call it cannot remove, and this one
  // sits in the middle of every loop over a `many`.
  //
  // Places are counted from one, so the offset is one less than the place.
  llvm::Value *placePointer(llvm::Value *array, llvm::Value *index,
                            const MirType &element, bool settled = false) {
    auto *whole = builder_.CreateLoad(many_, array);
    auto *base = builder_.CreateExtractValue(whole, 0);
    auto *length = builder_.CreateExtractValue(whole, 1);
    // Already answered where the program was read: the place and the length
    // were both written down, and the place is one this `many` has. Asking a
    // second time costs a compare and a branch in the middle of every loop, and
    // the answer cannot have changed.
    auto *one = llvm::ConstantInt::get(index->getType(), 1);
    if (settled)
      return builder_.CreateGEP(typeFor(element), base,
                                builder_.CreateSub(index, one));
    llvm::Function *function = builder_.GetInsertBlock()->getParent();
    auto *inside = llvm::BasicBlock::Create(context_, "inside", function);
    auto *outside = llvm::BasicBlock::Create(context_, "outside", function);
    // Each end on its own branch rather than both under one `and`, so that each
    // is a lone compare against something the loop around it may already have
    // said, and the optimiser can drop either without the other.
    //
    // The shape a walk over a `many` is written in does not reach here at all:
    // the checker settles that one, and `settled` above writes the access with
    // nothing in front of it.
    auto *within = llvm::BasicBlock::Create(context_, "within", function);
    builder_.CreateCondBr(builder_.CreateICmpSGE(index, one), within, outside);
    // Past that branch the place is one or more, so taking one off it cannot go
    // below zero and `nuw` says so.
    builder_.SetInsertPoint(within);
    auto *at = builder_.CreateSub(index, one, "", true, false);
    builder_.CreateCondBr(builder_.CreateICmpULT(at, length), inside, outside);

    // Out of range does not come back, which is what lets the optimiser lift
    // the compare out of a loop or drop it altogether: a check whose failure
    // carries on is a value in the middle of the body, and a check whose
    // failure stops is control flow.
    builder_.SetInsertPoint(outside);
    builder_.CreateCall(runtime_["xag_many_out_of_range"], {index, length});
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(inside);
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
      const MirType element = elementOf(typing(value.type));
      // One that grows starts empty and is grown into, because it keeps room
      // it is not using yet and only the runtime knows how much to take.
      if (typing(value.type).grows) {
        auto *begun = builder_.CreateAlloca(growing_, nullptr, "growing");
        builder_.CreateCall(runtime_["xag_growing_new"], {begun});
        auto *one = builder_.CreateAlloca(typeFor(element), nullptr, "one");
        for (const Operand &operand : value.operands) {
          if (isText(element))
            builder_.CreateStore(builder_.CreateLoad(str_, textPointer(operand)), one);
          else
            builder_.CreateStore(read(operand), one);
          builder_.CreateCall(runtime_["xag_growing_add"],
                              {begun, builder_.getInt64(strideOf(element)), one});
        }
        return builder_.CreateLoad(growing_, begun);
      }
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
          if (isText(element))
            builder_.CreateStore(
                builder_.CreateLoad(str_, textPointer(value.operands[i])), at);
          else
            builder_.CreateStore(read(value.operands[i]), at);
        }
      }
      return builder_.CreateLoad(many_, made);
    }

    case RValueKind::Fill: {
      const MirType element = elementOf(typing(value.type));
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
      const MirType element = elementOf(typing(value.operands[0].type));
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
      return localType(value.local).isLoan()
                 ? builder_.CreateLoad(builder_.getPtrTy(), slots_[value.local])
                 : slots_[value.local];

    case RValueKind::Unary:
      // Through the loan: `not` on a borrowed `bool` is `not` on the `bool`.
      // Handed the loan itself, this asked LLVM to invert a pointer, and the
      // compiler fell over rather than the program.
      return builder_.CreateNot(behind(value.operands[0]));

    case RValueKind::Binary:
      return binary(value);

    case RValueKind::Join: {
      const unsigned count = static_cast<unsigned>(value.operands.size());
      auto *array = scratch(str_, "pieces", builder_.getInt64(count));
      for (unsigned i = 0; i < count; ++i) {
        auto *at = builder_.CreateGEP(str_, array, builder_.getInt64(i));
        auto *piece = textPointer(value.operands[i]);
        builder_.CreateStore(builder_.CreateLoad(str_, piece), at);
      }
      auto *out = scratch(str_, "joined");
      builder_.CreateCall(runtime_["xag_str_join"],
                          {out, array, builder_.getInt64(count)});
      return builder_.CreateLoad(str_, out);
    }

    case RValueKind::Group: {
      const MirType &whole = typing(value.type);
      auto *shell = typeFor(whole);
      llvm::Value *built = llvm::UndefValue::get(shell);
      const Shape *shape = whole.held == Type::Struct && whole.named < mir_.shapes.size()
                               ? &mir_.shapes[whole.named]
                               : nullptr;
      for (unsigned i = 0; i < value.operands.size(); ++i) {
        llvm::Value *item = read(value.operands[i]);
        // A value going into one of the things a struct holds that may hold
        // nothing is that value, *held* — the same wrapping a name gets, and
        // which this was not doing. A `str` was written straight into a field
        // shaped `{ is it there, what it is }`, and the module came out
        // ill-formed before anything ran.
        //
        // Asked of the type that came back rather than of the type the operand
        // claims: an absence has already written itself as the pair, and a
        // value has not.
        if (shape && item && i < shape->fields.size()) {
          const MirType field = asMirType(shape->fields[i].type);
          if (mayBeNothing(field) && !isLoan(field) &&
              item->getType() != typeFor(field)) {
            auto *inner = llvm::UndefValue::get(typeFor(field));
            auto *held = builder_.CreateInsertValue(inner, builder_.getInt1(true), 0);
            item = builder_.CreateInsertValue(held, item, 1);
          }
        }
        built = builder_.CreateInsertValue(built, item, i);
      }
      return built;
    }

    case RValueKind::Taken: {
      const MirType of = operandType(value.operands[0]);
      const Shape *shape = of.lent().held == Type::Struct
                               ? &mir_.shapes[of.named]
                               : nullptr;
      if (!shape || value.local >= shape->fields.size())
        return nullptr;
      const MirType inner = asMirType(shape->fields[value.local].type);
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
      const MirType of = operandType(value.operands[0]);
      const Shape *shape = of.lent().held == Type::Struct
                               ? &mir_.shapes[of.named]
                               : nullptr;
      if (!shape || value.local >= shape->fields.size())
        return nullptr;
      const MirType inner = asMirType(shape->fields[value.local].type);
      // What copies is read out; what has an owner is lent where it stands.
      auto *where = isLoan(of)
                        ? builder_.CreateLoad(builder_.getPtrTy(),
                                              slots_[value.operands[0].local])
                        : slots_[value.operands[0].local];
      auto *at = builder_.CreateStructGEP(typeFor(withoutLoan(of)), where,
                                          value.local);
      // A field that is itself a borrow holds the pointer, so the pointer is
      // what is read out. Handing back the field's own address instead gave a
      // pointer to a pointer, and printing it printed the address.
      if (inner.isLoan())
        return builder_.CreateLoad(builder_.getPtrTy(), at);
      return copiesNamed(inner) ? builder_.CreateLoad(typeFor(inner), at)
                                : static_cast<llvm::Value *>(at);
    }

    case RValueKind::Holds: {
      // Asking a *borrowed* one whether it holds something reads through the
      // borrow first. A loan is a pointer, and there is no first field to take
      // out of a pointer — which is where this fell over rather than refusing.
      const MirType of = operandType(value.operands[0]);
      llvm::Value *whole = read(value.operands[0]);
      if (!whole)
        return nullptr;
      if (isLoan(of))
        whole = builder_.CreateLoad(typeFor(withoutLoan(of)), whole);
      return builder_.CreateExtractValue(whole, 0);
    }

    case RValueKind::Inside: {
      // Lent where what is inside has an owner, read out where it has not.
      const MirType of = operandType(value.operands[0]);
      // A `one-of` keeps what it holds in the room beside the number saying
      // which case that is, and which case it is decides how the room is read —
      // so the type this answers with is the one to read it as. An
      // `or-nothing`'s inside is what is left when the absence is off it.
      const MirType held =
          withoutLoan(of).held == Type::OneOf ? typing(value.type) : within(of);
      // The same again: what is inside a borrowed one is reached through the
      // borrow, not through the slot holding the borrow.
      auto *where = isLoan(of) ? builder_.CreateLoad(builder_.getPtrTy(),
                                                    slots_[value.operands[0].local])
                               : slots_[value.operands[0].local];
      auto *at = builder_.CreateStructGEP(typeFor(withoutLoan(of)), where, 1);
      return copiesNamed(held) ? builder_.CreateLoad(typeFor(held), at)
                               : static_cast<llvm::Value *>(at);
    }

    case RValueKind::Which: {
      const MirType of = operandType(value.operands[0]);
      auto *where = isLoan(of) ? builder_.CreateLoad(builder_.getPtrTy(),
                                                    slots_[value.operands[0].local])
                               : slots_[value.operands[0].local];
      return builder_.CreateLoad(
          builder_.getInt64Ty(),
          builder_.CreateStructGEP(typeFor(withoutLoan(of)), where, 0));
    }

    case RValueKind::Case: {
      // Built through memory rather than as a value, because what goes in the
      // room is a different type each time and a value has one shape.
      auto *shell = typeFor(typing(value.type));
      auto *made = scratch(shell, "made");
      builder_.CreateStore(llvm::Constant::getNullValue(shell), made);
      builder_.CreateStore(builder_.getInt64(value.local),
                           builder_.CreateStructGEP(shell, made, 0));
      if (!value.operands.empty())
        builder_.CreateStore(read(value.operands[0]),
                             builder_.CreateStructGEP(shell, made, 1));
      return builder_.CreateLoad(shell, made);
    }

    case RValueKind::Call:
      return call(value);
    }
    return nullptr;
  }

  // One place, in bytes. The layout is the machine's, asked of the module's own
  // data layout rather than guessed at.
  uint64_t strideOf(const MirType &element) {
    return module_.getDataLayout().getTypeAllocSize(typeFor(element));
  }

  // The runtime's own place for where the program got to.
  llvm::Value *whereWeAre() {
    return module_.getOrInsertGlobal("xag_where", builder_.getInt32Ty());
  }

  // The same sum, through the intrinsic that answers whether it fitted. The
  // answer is the machine's own wrapped one either way — this changes nothing
  // about what the program computes, only whether anything noticed.
  llvm::Value *watchedArithmetic(const std::string &op, llvm::Value *left,
                                 llvm::Value *right, bool unsignedly) {
    const llvm::Intrinsic::ID which =
        op == "+" ? (unsignedly ? llvm::Intrinsic::uadd_with_overflow
                                : llvm::Intrinsic::sadd_with_overflow)
        : op == "-" ? (unsignedly ? llvm::Intrinsic::usub_with_overflow
                                  : llvm::Intrinsic::ssub_with_overflow)
                    : (unsignedly ? llvm::Intrinsic::umul_with_overflow
                                  : llvm::Intrinsic::smul_with_overflow);
    llvm::Function *asked = llvm::Intrinsic::getOrInsertDeclaration(
        &module_, which, {left->getType()});
    llvm::Value *both = builder_.CreateCall(asked, {left, right});
    llvm::Value *answer = builder_.CreateExtractValue(both, 0);
    llvm::Value *round = builder_.CreateExtractValue(both, 1);

    llvm::Function *function = builder_.GetInsertBlock()->getParent();
    auto *say = llvm::BasicBlock::Create(context_, "cameround", function);
    auto *on = llvm::BasicBlock::Create(context_, "fitted", function);
    builder_.CreateCondBr(round, say, on);
    builder_.SetInsertPoint(say);
    builder_.CreateCall(runtime_["xag_came_round"], {builder_.getInt32(at_)});
    builder_.CreateBr(on);
    builder_.SetInsertPoint(on);
    return answer;
  }

  llvm::Value *binary(const RValue &value) {
    const std::string &op = value.op;
    // What the loan lends, not the loan. Every loan used to be taken for a loan
    // of text, because for a long time every loan was one — so `loanmut int64`
    // came in here and two pointers-to-a-number went to `xag_str_compare`.
    const MirType leftType = withoutLoan(typing(value.operands[0].type));
    const bool onText = isText(leftType);

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

    // Read through the loan: what is being added is the number it lends.
    auto *left = behind(value.operands[0]);
    auto *right = behind(value.operands[1]);

    if (op == "and") return builder_.CreateAnd(left, right);
    if (op == "or") return builder_.CreateOr(left, right);

    const Type given = leftType.held;
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
                                    typeFor(typing(value.type)));
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
      return builder_.CreateFPTrunc(answered, typeFor(typing(value.type)));
    }
    const Type made = typing(value.type).lent().held;
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
    //
    // Unless this is the build the compiler makes in order to run the program
    // while compiling it. That one is asked where a sum came round, so it does
    // the same arithmetic through the intrinsic that answers as well as adds,
    // and says so when the answer did not fit. Nothing a reader runs is built
    // this way.
    if (op == "+" || op == "-" || op == "x") {
      if (!watching_) {
        if (op == "+") return builder_.CreateAdd(left, right);
        if (op == "-") return builder_.CreateSub(left, right);
        return builder_.CreateMul(left, right);
      }
      // From `working`, the type the sum is done in — not from `given`, which
      // is what a comparison reads its two sides by. They coincide for these
      // three, and asking the wrong one would be a disagreement waiting.
      return watchedArithmetic(op, left, right, isWhole(working) && !isSigned(working));
    }

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
    return builder_.CreateTrunc(answered, typeFor(typing(value.type)));
  }

  llvm::Value *call(const RValue &value) {
    if (value.callee == "print.stdout") {
      for (const Operand &operand : value.operands) {
        // What is behind the loan, not the loan: a borrowed number is a number,
        // and asking `loan int64` what type it is answers nothing at all.
        const MirType type = withoutLoan(typing(operand.type));
        const Type named = type.held;
        // Several values rather than one, and how many is not settled here.
        if (type.many > 0 || named == Type::Struct) {
          if (auto *where = manyPointer(operand))
            showAll(where, type);
          continue;
        }
        if (isDecimal(named))
          builder_.CreateCall(
              runtime_["xag_print_deci"],
              {builder_.getInt32(static_cast<int>(widthOf(named))),
               builder_.CreateZExt(behind(operand), builder_.getInt128Ty())});
        else if (named == Type::Bin128)
          builder_.CreateCall(runtime_["xag_print_bin128"], {behind(operand)});
        else if (isBinary(named))
          builder_.CreateCall(
              runtime_["xag_print_bin"],
              {builder_.CreateFPExt(behind(operand), builder_.getDoubleTy()),
               builder_.getInt32(widthOf(named))});
        else if (isWhole(named))
          builder_.CreateCall(runtime_["xag_print_int"],
                              {widened(behind(operand), named),
                               builder_.getInt32(widthOf(named)),
                               builder_.getInt32(isSigned(named) ? 1 : 0)});
        else if (type.held == Type::Bool)
          builder_.CreateCall(
              runtime_["xag_print_bool"],
              {builder_.CreateZExt(behind(operand), builder_.getInt32Ty())});
        else if (auto *text = textPointer(operand))
          builder_.CreateCall(runtime_["xag_print"], {text});
      }
      return nullptr;
    }

    if (value.callee == "read.stdin") {
      // The build the compiler runs while compiling stops here rather than
      // reading: it has been given nothing, and what a program does on nothing
      // is not what it will do. Both engines stop at the same place, so what
      // happened before it still compares.
      if (watching_) {
        builder_.CreateCall(runtime_["xag_would_read"], {});
        builder_.CreateUnreachable();
        builder_.SetInsertPoint(
            llvm::BasicBlock::Create(context_, "unread",
                                     builder_.GetInsertBlock()->getParent()));
      }
      // The line comes back through a pointer and the answer says whether there
      // was one, which is exactly the two fields the type has.
      auto *shell = typeFor(typing(value.type));
      auto *line = builder_.CreateAlloca(str_, nullptr, "line");
      builder_.CreateStore(llvm::Constant::getNullValue(str_), line);
      auto *got = builder_.CreateCall(runtime_["xag_read_line"], {line});
      auto *there = builder_.CreateICmpNE(got, builder_.getInt32(0));
      auto *held = builder_.CreateInsertValue(llvm::UndefValue::get(shell), there, 0);
      return builder_.CreateInsertValue(held, builder_.CreateLoad(str_, line), 1);
    }

    if (value.callee == "arguments") {
      // Stops here while the compiler is the one running it, for the same
      // reason a read does: what it answers is what somebody types after the
      // program's name, and nobody has typed it yet.
      if (watching_) {
        builder_.CreateCall(runtime_["xag_would_read"], {});
        builder_.CreateUnreachable();
        builder_.SetInsertPoint(
            llvm::BasicBlock::Create(context_, "ungiven",
                                     builder_.GetInsertBlock()->getParent()));
      }
      auto *out = builder_.CreateAlloca(many_, nullptr, "given");
      builder_.CreateCall(runtime_["xag_arguments"], {out});
      return builder_.CreateLoad(many_, out);
    }

    if (value.callee == "convert-to-number") {
      const MirType spelled = typing(value.type);
      const MirType wanted = within(spelled);
      const Type named = wanted.held;
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
      auto *out = scratch(str_, "written");
      if (!value.operands.empty()) {
        const MirType whole = withoutLoan(typing(value.operands[0].type));
        // Several values rather than one: the same walk a print takes, with the
        // characters going into this rather than out.
        if (whole.many > 0 || whole.held == Type::Struct) {
          builder_.CreateCall(runtime_["xag_str_from"],
                              {out, llvm::Constant::getNullValue(builder_.getPtrTy()),
                               builder_.getInt64(0)});
          if (auto *where = manyPointer(value.operands[0]))
            showAll(where, whole, out);
          return builder_.CreateLoad(str_, out);
        }
        const Type named = whole.held;
        llvm::Value *held = behind(value.operands[0]);
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
      if (end.answers && typing(body_->result).held != Type::Nothing) {
        builder_.CreateRet(read(end.answer));
      } else if (typing(body_->result).held != Type::Nothing) {
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

NativeResult emitIr(const Mir &mir, bool optimise, Watching watching) {
  NativeResult result;
  Emitter emitter(mir, watching == Watching::Yes);
  if (!emitter.run(result.trouble))
    return result;
  if (optimise)
    optimiseModule(emitter.module());
  llvm::raw_string_ostream out(result.ir);
  emitter.module().print(out, nullptr);
  return result;
}

NativeResult emitObject(const Mir &mir, bool optimise, const std::string &path,
                        Watching watching) {
  NativeResult result;
  Emitter emitter(mir, watching == Watching::Yes);
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
