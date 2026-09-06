#include "xag/Fast.h"

#include "xag/Check.h"

#include "xag_runtime.h"

#include <string>
#include <vector>

namespace xag {
namespace {

// What a slot holds. There is no tag: the code knows what each slot is, because
// the graph knew, and knowing it once is the whole point of compiling first.
//
// The fields are in the order that packs them into 64 bytes, because a slot is
// copied whole every time a value moves and the copy costs what the slot weighs.
struct Slot {
  XagInt whole = 0; // an int, a uint, a bool, a bin128's bits, a deci's bits
  XagStr text{nullptr, 0, 0};
  // A `many`: the places, held the way text is. One owner ends them, and
  // everything reading them holds a view with no claim.
  std::vector<Slot> *places = nullptr;
  double real = 0;   // a bin16, bin32 or bin64
  uint32_t loan = 0; // where in the stack the lent slot is
  bool owns = false; // whether this slot must end what it points at
  bool loaned = false;
  // Whether this is the absence rather than a value. Only a slot whose type may
  // hold nothing is ever asked, so the flag costs the others nothing.
  bool empty = false;
};
static_assert(sizeof(Slot) == 64, "a slot is meant to be exactly one cache line");

enum class Op : uint8_t {
  Halt,
  LoadWhole, LoadReal, LoadWide, LoadText, LoadNothing,
  CopySlot, MoveSlot, MakeLoan, StoreThrough, Drop, DropIf,
  // A copy of a plain number, which is the number and nothing else: no claim,
  // no loan, no absence to carry across with it.
  CopyWhole, CopyReal,
  IntAdd, IntSub, IntMul, IntDiv, IntMod, IntPow,
  IntLt, IntGt, IntLe, IntGe, IntEq, IntNe,
  // The same, with the right-hand side a written number read straight from
  // the pool rather than loaded into a slot first: `b` names the constant.
  IntAddK, IntSubK, IntMulK,
  IntLtK, IntGtK, IntLeK, IntGeK, IntEqK, IntNeK,
  // A comparison and the JumpUnless that reads it, as one step: the truth is
  // still written to `to`, and `jump` is taken when it is false. A loop asks
  // one of these on every turn.
  IntLtJ, IntGtJ, IntLeJ, IntGeJ, IntEqJ, IntNeJ,
  RealAdd, RealSub, RealMul, RealDiv, RealMod, RealPow,
  RealLt, RealGt, RealLe, RealGe, RealEq, RealNe,
  WideAdd, WideSub, WideMul, WideDiv, WideMod, WidePow, WideCompare,
  DeciAdd, DeciSub, DeciMul, DeciDiv, DeciMod, DeciPow, DeciCompare,
  TextCompare, TextJoin, TextCount,
  MakeMany, FillMany, ElementAt, StoreAt,
  LoadNone, HoldsSomething, TakeInside,
  ReadLine, Arguments, NumberOf,
  Not, And, Or,
  Order, // turn a -1/0/1 into a truth, by the test in `aux`
  PushArg, Call, PrintWhole, PrintReal, PrintWide, PrintDeci, PrintText, PrintBool,
  Jump, JumpUnless, Return, ReturnValue,
};

struct Code {
  Op op = Op::Halt;
  uint32_t to = 0;   // where the answer goes, or where to jump
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t aux = 0;  // a width, a signedness, a constant, a comparison
  uint32_t jump = 0; // where a fused comparison goes when it is false
};

struct Constant {
  XagInt whole = 0;
  double real = 0;
  std::string text;
};

struct Routine {
  std::string name;
  unsigned slots = 0;
  unsigned parameters = 0;
  std::vector<Code> code;
  std::vector<Constant> pool;
  bool answers = false;
};

// ---- turning the graph into code

class Builder {
public:
  explicit Builder(const Mir &mir) : mir_(mir) {}

  std::vector<Routine> run() {
    for (const Body &body : mir_.bodies)
      names_.push_back(body.name);
    for (const Body &body : mir_.bodies)
      routines_.push_back(compile(body));
    return std::move(routines_);
  }

private:
  const Mir &mir_;
  std::vector<Routine> routines_;
  std::vector<std::string> names_;
  const Body *body_ = nullptr;
  Routine *out_ = nullptr;

  const std::string &spelled(TypeRef type) const {
    static const std::string unknown = "?";
    return type.index < body_->types.size() ? body_->types[type.index] : unknown;
  }

  static bool isLoan(const std::string &type) {
    return type.rfind("ref ", 0) == 0 || type.rfind("refmut ", 0) == 0;
  }

  static std::string behind(const std::string &type) {
    if (type.rfind("refmut ", 0) == 0)
      return type.substr(7);
    if (type.rfind("ref ", 0) == 0)
      return type.substr(4);
    return type;
  }

  unsigned findRoutine(const std::string &name) const {
    for (unsigned i = 0; i < names_.size(); ++i)
      if (names_[i] == name)
        return i;
    return static_cast<unsigned>(names_.size());
  }

  void emit(Code code) { out_->code.push_back(code); }

  uint32_t hold(Constant value) {
    out_->pool.push_back(std::move(value));
    return static_cast<uint32_t>(out_->pool.size() - 1);
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

  // A written value, made ready before anything runs.
  uint32_t constantFor(const Operand &operand, Op &how) {
    const std::string type = behind(spelled(operand.type));
    const Type named = typeNamed(type);
    Constant value;
    if (type == "bool") {
      value.whole = operand.written == "true";
      how = Op::LoadWhole;
    } else if (isWhole(named)) {
      const bool negative = !operand.written.empty() && operand.written[0] == '-';
      __uint128_t magnitude = 0;
      for (unsigned i = negative ? 1 : 0; i < operand.written.size(); ++i)
        magnitude = magnitude * 10 + static_cast<unsigned>(operand.written[i] - '0');
      value.whole = xag_int_fit(negative ? -static_cast<XagInt>(magnitude)
                                         : static_cast<XagInt>(magnitude),
                                widthOf(named), isSigned(named) ? 1 : 0);
      how = Op::LoadWhole;
    } else if (named == Type::Bin128) {
      XagBin128 bits = 0;
      xag_bin128_reads(operand.written.data(), operand.written.size(), &bits);
      value.whole = static_cast<XagInt>(bits);
      how = Op::LoadWide;
    } else if (isDecimal(named)) {
      XagDeci bits = 0;
      xag_deci_reads(widthOf(named), operand.written.data(), operand.written.size(),
                     &bits);
      value.whole = static_cast<XagInt>(bits);
      how = Op::LoadWide;
    } else if (isBinary(named)) {
      double read = 0;
      xag_bin_reads(operand.written.data(), operand.written.size(), widthOf(named),
                    &read);
      value.real = read;
      how = Op::LoadReal;
    } else if (operand.written == "nothing" &&
               spelled(operand.type).rfind("or-nothing ", 0) == 0) {
      how = Op::LoadNone;
    } else {
      value.text = unescape(operand.written);
      how = Op::LoadText;
    }
    return hold(std::move(value));
  }

  // Every operand ends up in a slot, so the code that follows never has to ask
  // where it came from. `borrowed` says the slot is one of the body's own and
  // will still be there afterwards; a slot made here holds the only copy of
  // whatever is in it, and taking a view of that is taking a view of something
  // about to end.
  uint32_t into(const Operand &operand, unsigned &scratch, bool &borrowed) {
    borrowed = false;
    if (operand.kind == OperandKind::Written) {
      Op how = Op::LoadWhole;
      const uint32_t which = constantFor(operand, how);
      const uint32_t slot = scratch++;
      emit(Code{how, slot, 0, 0, which});
      return slot;
    }
    if (operand.kind == OperandKind::Move) {
      const uint32_t slot = scratch++;
      emit(Code{Op::MoveSlot, slot, operand.local, 0, 0});
      return slot;
    }
    borrowed = true;
    return operand.local;
  }

  uint32_t into(const Operand &operand, unsigned &scratch) {
    bool borrowed = false;
    return into(operand, scratch, borrowed);
  }

  Routine compile(const Body &body) {
    body_ = &body;
    Routine routine;
    out_ = &routine;
    routine.name = body.name;
    routine.parameters = body.parameters;
    routine.answers = spelled(body.result) != "nothing";

    // Slots: one per local, then room above for the working ones.
    unsigned scratch = static_cast<unsigned>(body.locals.size());
    unsigned most = scratch;

    std::vector<unsigned> starts(body.blocks.size(), 0);
    struct Patch {
      unsigned at;    // which instruction
      unsigned block; // where it should go
      bool fused;     // whether the target belongs in `jump` rather than `to`
    };
    std::vector<Patch> patches;

    for (size_t which = 0; which < body.blocks.size(); ++which) {
      const BasicBlock &block = body.blocks[which];
      starts[block.id] = static_cast<unsigned>(routine.code.size());
      scratch = static_cast<unsigned>(body.locals.size());
      // A jump to the block laid out next lands where the code would have gone
      // anyway, so it is not emitted. A loop's test block is followed by its
      // body, which makes this the jump taken on every turn.
      const bool nextIs = which + 1 < body.blocks.size();
      auto follows = [&](unsigned target) {
        return nextIs && body.blocks[which + 1].id == target;
      };

      // Where the code of the block's last statement begins, so that the
      // terminator can tell whether the instruction before it is that
      // statement's, and fuse with it if it is.
      unsigned lastStart = static_cast<unsigned>(routine.code.size());
      for (const Statement &s : block.statements) {
        lastStart = static_cast<unsigned>(routine.code.size());
        if (s.kind == StatementKind::Drop) {
          emit(s.conditional ? Code{Op::DropIf, s.place, s.flag, 0, 0}
                             : Code{Op::Drop, s.place, 0, 0, 0});
          continue;
        }
        if (s.kind == StatementKind::Store) {
          const uint32_t at = into(s.at, scratch);
          const uint32_t what = s.value.operands.empty()
                                    ? 0
                                    : into(s.value.operands[0], scratch);
          emit(Code{Op::StoreAt, s.place, at, what, 0});
          most = most > scratch ? most : scratch;
          continue;
        }
        statement(s, scratch);
        most = most > scratch ? most : scratch;
      }

      const Terminator &end = block.terminator;
      if (end.kind == TerminatorKind::Goto) {
        const unsigned target = end.targets.empty() ? 0 : end.targets[0];
        if (!follows(target)) {
          patches.push_back({static_cast<unsigned>(routine.code.size()), target, false});
          emit(Code{Op::Jump, 0, 0, 0, 0});
        }
      } else if (end.kind == TerminatorKind::Switch) {
        const unsigned taken = end.targets.empty() ? 0 : end.targets[0];
        const unsigned otherwise = end.targets.size() > 1 ? end.targets.back() : taken;
        // When the block's last statement compared two whole numbers into the
        // very truth the switch reads, the compare does the switch's work too.
        const Op fused = !block.statements.empty() && routine.code.size() > lastStart &&
                                 end.condition.kind == OperandKind::Copy &&
                                 routine.code.back().to == end.condition.local
                             ? fusedWithJump(routine.code.back().op)
                             : Op::Halt;
        if (fused != Op::Halt) {
          routine.code.back().op = fused;
          patches.push_back({static_cast<unsigned>(routine.code.size() - 1), otherwise, true});
        } else {
          const uint32_t asked = into(end.condition, scratch);
          patches.push_back({static_cast<unsigned>(routine.code.size()), otherwise, false});
          emit(Code{Op::JumpUnless, 0, asked, 0, 0});
        }
        if (!follows(taken)) {
          patches.push_back({static_cast<unsigned>(routine.code.size()), taken, false});
          emit(Code{Op::Jump, 0, 0, 0, 0});
        }
      } else {
        emit(end.answers ? Code{Op::ReturnValue, 0, end.answer.local, 0, 0}
                         : Code{Op::Return, 0, 0, 0, 0});
      }
      most = most > scratch ? most : scratch;
    }
    // Every block ends in a jump or a return, so this is only ever reached by a
    // body with no blocks at all; it is here so the machine never has to ask
    // whether it has run off the end.
    emit(Code{Op::Halt, 0, 0, 0, 0});

    for (const Patch &patch : patches) {
      const unsigned start = starts[patch.block < starts.size() ? patch.block : 0];
      (patch.fused ? routine.code[patch.at].jump : routine.code[patch.at].to) = start;
    }
    routine.slots = most;
    return routine;
  }

  // The comparison that also jumps when false, or Halt for anything else.
  static Op fusedWithJump(Op op) {
    switch (op) {
    case Op::IntLt: return Op::IntLtJ;
    case Op::IntGt: return Op::IntGtJ;
    case Op::IntLe: return Op::IntLeJ;
    case Op::IntGe: return Op::IntGeJ;
    case Op::IntEq: return Op::IntEqJ;
    case Op::IntNe: return Op::IntNeJ;
    default: return Op::Halt;
    }
  }

  void statement(const Statement &s, unsigned &scratch) {
    const RValue &value = s.value;
    const std::string kept = spelled(body_->locals[s.place].type);
    const bool through = isLoan(kept) && !isLoan(spelled(value.type)) &&
                         value.kind != RValueKind::Ref;

    switch (value.kind) {
    case RValueKind::Collect: {
      for (const Operand &operand : value.operands) {
        const uint32_t from = into(operand, scratch);
        emit(Code{Op::PushArg, 0, from, 0, 0});
      }
      emit(Code{Op::MakeMany, s.place, 0,
                static_cast<uint32_t>(value.operands.size()), 0});
      return;
    }

    case RValueKind::Fill: {
      const uint32_t what = into(value.operands[0], scratch);
      const uint32_t places = into(value.operands[1], scratch);
      emit(Code{Op::FillMany, s.place, what, places, 0});
      return;
    }

    case RValueKind::Holds: {
      const uint32_t of = into(value.operands[0], scratch);
      emit(Code{Op::HoldsSomething, s.place, of, 0, 0});
      return;
    }

    case RValueKind::Inside: {
      const uint32_t of = into(value.operands[0], scratch);
      emit(Code{Op::TakeInside, s.place, of, 0, 0});
      return;
    }

    case RValueKind::Element: {
      const uint32_t of = into(value.operands[0], scratch);
      const uint32_t at = into(value.operands[1], scratch);
      emit(Code{Op::ElementAt, s.place, of, at, 0});
      return;
    }

    case RValueKind::Use: {
      if (value.operands.empty())
        return;
      bool borrowed = false;
      const uint32_t from = into(value.operands[0], scratch, borrowed);
      if (through) {
        emit(Code{Op::StoreThrough, s.place, from, 0, 0});
      } else if (borrowed) {
        // A loan copied into a loan travels whole; anything else is the value
        // behind it, read where it stands. A plain number read into a place
        // of the same plain type is only the number.
        const Type plain = typeNamed(kept);
        const bool number = isWhole(plain) || plain == Type::Bool ||
                            isBinary(plain) || isDecimal(plain);
        if (number && behind(spelled(value.operands[0].type)) == kept)
          emit(Code{isBinary(plain) && plain != Type::Bin128 ? Op::CopyReal : Op::CopyWhole,
                    s.place, from, 0, 0});
        else
          emit(Code{Op::CopySlot, s.place, from, 0, isLoan(kept) ? 1u : 0u});
      } else {
        // Made here, so it is handed over rather than looked at.
        emit(Code{Op::MoveSlot, s.place, from, 0, 0});
      }
      return;
    }
    case RValueKind::Ref:
      emit(Code{Op::MakeLoan, s.place, value.local, 0, 0});
      return;
    case RValueKind::Unary: {
      const uint32_t from = into(value.operands[0], scratch);
      emit(Code{Op::Not, s.place, from, 0, 0});
      return;
    }
    case RValueKind::Binary:
      binary(s, scratch, through);
      return;
    case RValueKind::Join: {
      for (const Operand &piece : value.operands) {
        const uint32_t from = into(piece, scratch);
        emit(Code{Op::PushArg, 0, from, 0, 0});
      }
      emit(Code{Op::TextJoin, s.place, 0,
                static_cast<uint32_t>(value.operands.size()), 0});
      return;
    }
    case RValueKind::Call:
      call(s, scratch, through);
      return;
    }
  }

  void binary(const Statement &s, unsigned &scratch, bool through) {
    const RValue &value = s.value;
    const std::string left = behind(spelled(value.operands[0].type));
    const std::string made = behind(spelled(value.type));
    const Type given = typeNamed(left);
    const Type answered = typeNamed(made);
    const Type working = isNumber(answered) ? answered : given;

    const std::string &op = value.op;
    const unsigned test = op == "<" ? 0 : op == ">" ? 1 : op == "<==" ? 2
                          : op == ">==" ? 3 : op == "==" ? 4 : 5;
    const bool comparing = op == "<" || op == ">" || op == "<==" || op == ">==" ||
                           op == "==" || op == "!==";
    const bool wholes = isWhole(given) || given == Type::Bool;
    const bool logical = op == "and" || op == "or";
    const bool textual = left == "str" || isLoan(spelled(value.operands[0].type));

    // A whole number written on the right is read from the pool by the step
    // that uses it, rather than loaded into a slot by a step of its own.
    const uint32_t x = into(value.operands[0], scratch);
    uint32_t constant = 0;
    bool fromPool = false;
    if (wholes && !logical && !textual &&
        value.operands[1].kind == OperandKind::Written &&
        (comparing || op == "+" || op == "-" || op == "x")) {
      Op how = Op::LoadWhole;
      constant = constantFor(value.operands[1], how);
      fromPool = how == Op::LoadWhole;
    }
    const uint32_t y = fromPool ? constant : into(value.operands[1], scratch);
    const uint32_t place = through ? scratch++ : s.place;

    auto compare = [&](Op family, unsigned test) {
      const uint32_t order = scratch++;
      emit(Code{family, order, x, y, widthOf(given)});
      emit(Code{Op::Order, place, order, 0, test});
    };

    if (logical) {
      emit(Code{op == "and" ? Op::And : Op::Or, place, x, y, 0});
    } else if (textual) {
      if (comparing)
        compare(Op::TextCompare, test);
    } else if (wholes) {
      const uint32_t aux = (widthOf(working) << 1) | (isSigned(working) ? 1 : 0);
      if (comparing) {
        static const Op kOps[] = {Op::IntLt, Op::IntGt, Op::IntLe,
                                  Op::IntGe, Op::IntEq, Op::IntNe};
        static const Op kPooled[] = {Op::IntLtK, Op::IntGtK, Op::IntLeK,
                                     Op::IntGeK, Op::IntEqK, Op::IntNeK};
        emit(Code{fromPool ? kPooled[test] : kOps[test], place, x, y,
                  (isWhole(given) && !isSigned(given)) ? 0u : 1u});
      } else if (fromPool) {
        const Op which = op == "+" ? Op::IntAddK : op == "-" ? Op::IntSubK : Op::IntMulK;
        emit(Code{which, place, x, y, aux});
      } else {
        const Op which = op == "+"     ? Op::IntAdd
                         : op == "-"   ? Op::IntSub
                         : op == "x"   ? Op::IntMul
                         : op == "/"   ? Op::IntDiv
                         : op == "mod" ? Op::IntMod
                                       : Op::IntPow;
        emit(Code{which, place, x, y, aux});
      }
    } else if (answered == Type::Bin128 || given == Type::Bin128) {
      if (comparing) {
        compare(Op::WideCompare, test);
      } else {
        const Op which = op == "+"     ? Op::WideAdd
                         : op == "-"   ? Op::WideSub
                         : op == "x"   ? Op::WideMul
                         : op == "/"   ? Op::WideDiv
                         : op == "mod" ? Op::WideMod
                                       : Op::WidePow;
        emit(Code{which, place, x, y, 0});
      }
    } else if (isDecimal(answered) || isDecimal(given)) {
      const uint32_t width = widthOf(isDecimal(working) ? working : given);
      if (comparing) {
        const uint32_t order = scratch++;
        emit(Code{Op::DeciCompare, order, x, y, width});
        emit(Code{Op::Order, place, order, 0, test});
      } else {
        const Op which = op == "+"     ? Op::DeciAdd
                         : op == "-"   ? Op::DeciSub
                         : op == "x"   ? Op::DeciMul
                         : op == "/"   ? Op::DeciDiv
                         : op == "mod" ? Op::DeciMod
                                       : Op::DeciPow;
        emit(Code{which, place, x, y, width});
      }
    } else {
      const uint32_t width = widthOf(isBinary(working) ? working : given);
      if (comparing) {
        static const Op kOps[] = {Op::RealLt, Op::RealGt, Op::RealLe,
                                  Op::RealGe, Op::RealEq, Op::RealNe};
        emit(Code{kOps[test], place, x, y, width});
      } else {
        const Op which = op == "+"     ? Op::RealAdd
                         : op == "-"   ? Op::RealSub
                         : op == "x"   ? Op::RealMul
                         : op == "/"   ? Op::RealDiv
                         : op == "mod" ? Op::RealMod
                                       : Op::RealPow;
        emit(Code{which, place, x, y, width});
      }
    }
    if (through)
      emit(Code{Op::StoreThrough, s.place, place, 0, 0});
  }

  void call(const Statement &s, unsigned &scratch, bool through) {
    const RValue &value = s.value;
    if (value.callee == "print.stdout") {
      for (const Operand &operand : value.operands) {
        const std::string type = behind(spelled(operand.type));
        const Type named = typeNamed(type);
        const uint32_t from = into(operand, scratch);
        Op how = Op::PrintText;
        if (type == "bool")
          how = Op::PrintBool;
        else if (isWhole(named))
          how = Op::PrintWhole;
        else if (named == Type::Bin128)
          how = Op::PrintWide;
        else if (isDecimal(named))
          how = Op::PrintDeci;
        else if (isBinary(named))
          how = Op::PrintReal;
        emit(Code{how, 0, from, 0,
                  (widthOf(named) << 1) | (isSigned(named) ? 1u : 0u)});
      }
      emit(Code{Op::LoadNothing, s.place, 0, 0, 0});
      return;
    }
    if (value.callee == "read.stdin") {
      emit(Code{Op::ReadLine, s.place, 0, 0, 0});
      return;
    }
    if (value.callee == "arguments") {
      emit(Code{Op::Arguments, s.place, 0, 0, 0});
      return;
    }
    if (value.callee == "number") {
      const uint32_t from = value.operands.empty() ? 0 : into(value.operands[0], scratch);
      const Type wanted = typeNamed(behind(spelled(value.type)).rfind("or-nothing ", 0) == 0
                                        ? behind(spelled(value.type)).substr(11)
                                        : behind(spelled(value.type)));
      // Width, family and signedness in one word, so the machine reads no
      // strings: `deci` and `bin` and whole numbers are told apart here once.
      const uint32_t family = isDecimal(wanted) ? 2u : isBinary(wanted) ? 1u : 0u;
      emit(Code{Op::NumberOf, s.place, from, 0,
                (widthOf(wanted) << 3) | (family << 1) | (isSigned(wanted) ? 1u : 0u)});
      return;
    }
    if (value.callee == "count") {
      const uint32_t from = value.operands.empty() ? 0 : into(value.operands[0], scratch);
      emit(Code{Op::TextCount, s.place, from, 0, 0});
      return;
    }
    for (const Operand &operand : value.operands) {
      const uint32_t from = into(operand, scratch);
      emit(Code{Op::PushArg, 0, from, 0, 0});
    }
    const uint32_t place = through ? scratch++ : s.place;
    emit(Code{Op::Call, place, findRoutine(value.callee),
              static_cast<uint32_t>(value.operands.size()), 0});
    if (through)
      emit(Code{Op::StoreThrough, s.place, place, 0, 0});
  }
};

// ---- and now the running

class Machine {
public:
  Machine(std::vector<Routine> routines, Settings settings)
      : routines_(std::move(routines)), settings_(settings) {
    stack_.resize(kStack);
  }

  FastResult run() {
    unsigned start = routines_.size();
    for (unsigned i = 0; i < routines_.size(); ++i)
      if (routines_[i].name == "START")
        start = i;
    if (start == routines_.size())
      return FastResult{false, "there is no START to run"};

    Slot answer;
    call(start, 0, answer, 0);
    end(answer);
    if (trouble_.empty() && !xag_balance_is_clear())
      trouble_ = "the program ended still holding " +
                 std::to_string(xag_live_allocations()) + " thing(s)";
    return FastResult{trouble_.empty(), trouble_};
  }

private:
  static constexpr unsigned kStack = 1u << 18;
  static constexpr uint64_t kBudget = 200u * 1000u * 1000u;

  std::vector<Routine> routines_;
  std::vector<Slot> stack_;
  // Where in the stack the arguments pushed so far are. One list for the whole
  // machine rather than one per frame: a call takes its arguments off the top
  // before it pushes any of its own, so the frames never see each other's.
  std::vector<uint32_t> arguments_;
  std::vector<XagStr> pieces_; // the texts a join is putting side by side
  std::string trouble_;
  Settings settings_;
  uint64_t steps_ = 0;
  unsigned depth_ = 0;

  // A sum cut down to its width, wrapping as a machine would: the bits above
  // the width are shifted off the top and the top one that remains is shifted
  // back down as the sign, or zeros are, for a uint. This is what xag_int_fit
  // answers, done here because the call to ask it cost more than the add. The
  // widths it does not cover are still its to answer.
  static XagInt narrow(XagInt value, uint32_t aux) {
    const unsigned width = aux >> 1;
    if (width == 128)
      return value;
    if (width == 0)
      return xag_int_fit(value, width, aux & 1);
    const unsigned drop = 128 - width;
    const __uint128_t raised = static_cast<__uint128_t>(value) << drop;
    return (aux & 1) ? static_cast<XagInt>(raised) >> drop
                     : static_cast<XagInt>(raised >> drop);
  }

  // Following a loan to the slot it names.
  Slot &behind(Slot &slot) {
    Slot *at = &slot;
    for (unsigned hops = 0; at->loaned && hops < 64; ++hops) {
      if (at->loan >= stack_.size())
        break;
      at = &stack_[at->loan];
    }
    return *at;
  }

  // Nearly every slot holds a number, and a number has nothing to end: nothing
  // owned, nothing lent, nothing absent. Such a slot is left as it is, stale
  // number and all, because whatever is written into it next is the only thing
  // anyone will read from it.
  void end(Slot &slot) {
    if (!slot.owns && !slot.empty && !slot.loaned && !slot.places)
      return;
    finish(slot);
  }

  [[gnu::noinline]] void finish(Slot &slot) {
    if (slot.empty) {
      slot = Slot{};
      return;
    }
    if (slot.owns && slot.places) {
      for (Slot &held : *slot.places)
        end(held);
      delete slot.places;
      xag_note_given();
      slot = Slot{};
      return;
    }
    if (slot.owns)
      xag_str_drop(&slot.text);
    slot = Slot{};
  }

  // The steps that do more than a few loads and stores are functions of their
  // own, kept out of the loop that runs everything else. That loop is one
  // function, and the compiler allots its registers to the whole of it at
  // once: every vector grown or list walked inside it is working state that
  // competes with the code pointer and the frame for a register, and once
  // those spill to the stack every step pays to fetch them back.

  [[gnu::noinline]] void makeMany(Slot &to, unsigned count) {
    auto *held = new std::vector<Slot>();
    held->reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      const uint32_t from = arguments_[arguments_.size() - count + i];
      held->push_back(stack_[from]);
      // What went in belongs to the array now, and the slot it came from
      // must not end it a second time.
      if (stack_[from].owns)
        stack_[from] = Slot{};
    }
    arguments_.resize(arguments_.size() - count);
    end(to);
    to = Slot{};
    to.places = held;
    to.owns = true;
    xag_note_taken();
  }

  [[gnu::noinline]] void fillMany(Slot &to, const Slot &one_of, XagInt places) {
    auto *held = new std::vector<Slot>();
    for (XagInt i = 0; i < places && i < 100000000; ++i) {
      Slot copy = one_of;
      copy.owns = false;
      held->push_back(copy);
    }
    end(to);
    to = Slot{};
    to.places = held;
    to.owns = true;
    xag_note_taken();
  }

  [[gnu::noinline]] void elementAt(Slot &to, const Slot &of, XagInt index) {
    const uint64_t length = of.places ? of.places->size() : 0;
    const uint64_t at = xag_many_place(static_cast<int64_t>(index), length,
                                       settings_.wrapsOutOfRange ? 1 : 0);
    Slot seen = (*of.places)[at];
    seen.owns = false; // a view, and no claim on what it sees
    end(to);
    to = seen;
  }

  [[gnu::noinline]] void storeAt(Slot &of, XagInt index, Slot &given) {
    const uint64_t length = of.places ? of.places->size() : 0;
    const uint64_t at = xag_many_place(static_cast<int64_t>(index), length,
                                       settings_.wrapsOutOfRange ? 1 : 0);
    Slot kept = given;
    if (given.owns)
      given = Slot{};
    end((*of.places)[at]);
    (*of.places)[at] = kept;
  }

  [[gnu::noinline]] void joinText(Slot &to, unsigned count) {
    // The pieces are gathered into a list kept from one join to the next, so
    // that joining does not take and give back a list's worth of memory every
    // time on top of the text's.
    pieces_.clear();
    for (unsigned i = 0; i < count; ++i)
      pieces_.push_back(behind(stack_[arguments_[arguments_.size() - count + i]]).text);
    arguments_.resize(arguments_.size() - count);
    XagStr joined{nullptr, 0, 0};
    xag_str_join(&joined, pieces_.data(), pieces_.size());
    end(to);
    to.text = joined;
    to.owns = true;
  }

  // Runs a routine with the last `given` pushed arguments, which it takes off
  // the list whether or not it can run.
  void call(unsigned which, uint32_t base, Slot &answer, unsigned given) {
    const uint32_t *pushed = arguments_.data() + (arguments_.size() - given);
    if (which >= routines_.size()) {
      arguments_.resize(arguments_.size() - given);
      return;
    }
    if (++depth_ > 400 || base + routines_[which].slots + 8 >= kStack) {
      trouble_ = "a function called itself further than this engine will follow";
      --depth_;
      arguments_.resize(arguments_.size() - given);
      return;
    }
    const Routine &routine = routines_[which];
    for (unsigned i = 0; i < routine.slots; ++i)
      stack_[base + i] = Slot{};
    for (unsigned i = 0; i < given && i + 1 < routine.slots; ++i)
      stack_[base + i + 1] = stack_[pushed[i]];
    // What was handed over is gone from where it was.
    for (unsigned i = 0; i < given; ++i)
      if (stack_[pushed[i]].owns)
        stack_[pushed[i]] = Slot{};
    arguments_.resize(arguments_.size() - given);

    const Code *code = routine.code.data();
    const Constant *pool = routine.pool.data();
    unsigned at = 0;
    // The stack never grows, so the frame is where it was found. The step count
    // lives in a register while the loop runs and is put back whenever another
    // routine needs to see it, because a member written on every step is a
    // store on every step.
    Slot *const slots = stack_.data() + base;
    uint64_t steps = steps_;

    // Nothing here can go wrong except a call, which is asked on its way back;
    // and every routine ends in a Halt, so there is no end to run off.
    for (;;) {
      if (++steps > kBudget) {
        trouble_ = "the program ran longer than this engine will wait";
        break;
      }
      const Code &one = code[at];
      auto read = [&](uint32_t i) -> Slot & { return behind(slots[i]); };
      Slot &to = slots[one.to];

      switch (one.op) {
      case Op::LoadWhole:
        end(to);
        to.whole = routine.pool[one.aux].whole;
        break;
      case Op::LoadReal:
        end(to);
        to.real = routine.pool[one.aux].real;
        break;
      case Op::LoadWide:
        end(to);
        to.whole = routine.pool[one.aux].whole;
        break;
      case Op::LoadText: {
        // A view of the pool, held for as long as the machine is, rather than
        // a copy taken and given back on every pass through a loop. Nothing
        // writes into text in place, so a view reads the same as a copy; and
        // wherever this travels, it travels as a view, which nothing ends.
        const std::string &written = pool[one.aux].text;
        end(to);
        to.text = XagStr{written.empty() ? nullptr : const_cast<char *>(written.data()),
                         written.size(), 0};
        to.owns = false;
        break;
      }
      case Op::LoadNothing:
        end(to);
        break;
      case Op::CopySlot: {
        if (one.aux) { // a loan, travelling whole
          Slot keep = slots[one.a];
          end(to);
          to = keep;
          to.owns = false;
        } else {
          Slot &from = read(one.a);
          Slot keep = from;
          keep.owns = false;
          keep.loaned = false;
          end(to);
          to = keep;
        }
        break;
      }
      case Op::CopyWhole:
        end(to);
        to.whole = read(one.a).whole;
        break;
      case Op::CopyReal:
        end(to);
        to.real = read(one.a).real;
        break;
      case Op::MoveSlot: {
        Slot keep = slots[one.a];
        slots[one.a] = Slot{};
        end(to);
        to = keep;
        break;
      }
      case Op::MakeLoan:
        end(to);
        to.loaned = true;
        to.loan = base + one.a;
        break;
      case Op::StoreThrough: {
        Slot &target = behind(slots[one.to]);
        Slot keep = slots[one.a];
        slots[one.a] = Slot{};
        if (&target != &slots[one.to])
          end(target);
        target = keep;
        break;
      }
      case Op::Drop:
        end(slots[one.to]);
        break;
      case Op::DropIf:
        if (slots[one.a].whole != 0)
          end(slots[one.to]);
        break;

      case Op::IntAdd:
        to.whole = narrow(static_cast<XagInt>(static_cast<__uint128_t>(read(one.a).whole) +
                                              static_cast<__uint128_t>(read(one.b).whole)),
                          one.aux);
        break;
      case Op::IntSub:
        to.whole = narrow(static_cast<XagInt>(static_cast<__uint128_t>(read(one.a).whole) -
                                              static_cast<__uint128_t>(read(one.b).whole)),
                          one.aux);
        break;
      case Op::IntMul:
        to.whole = narrow(static_cast<XagInt>(static_cast<__uint128_t>(read(one.a).whole) *
                                              static_cast<__uint128_t>(read(one.b).whole)),
                          one.aux);
        break;
      case Op::IntDiv:
        to.whole = xag_int_div(read(one.a).whole, read(one.b).whole, one.aux >> 1, one.aux & 1);
        break;
      case Op::IntMod:
        to.whole = xag_int_mod(read(one.a).whole, read(one.b).whole, one.aux >> 1, one.aux & 1);
        break;
      case Op::IntPow:
        to.whole = xag_int_pow(read(one.a).whole, read(one.b).whole, one.aux >> 1, one.aux & 1);
        break;
      case Op::IntLt:
        to.whole = one.aux ? read(one.a).whole < read(one.b).whole
                           : static_cast<__uint128_t>(read(one.a).whole) <
                                 static_cast<__uint128_t>(read(one.b).whole);
        break;
      case Op::IntGt:
        to.whole = one.aux ? read(one.a).whole > read(one.b).whole
                           : static_cast<__uint128_t>(read(one.a).whole) >
                                 static_cast<__uint128_t>(read(one.b).whole);
        break;
      case Op::IntLe:
        to.whole = one.aux ? read(one.a).whole <= read(one.b).whole
                           : static_cast<__uint128_t>(read(one.a).whole) <=
                                 static_cast<__uint128_t>(read(one.b).whole);
        break;
      case Op::IntGe:
        to.whole = one.aux ? read(one.a).whole >= read(one.b).whole
                           : static_cast<__uint128_t>(read(one.a).whole) >=
                                 static_cast<__uint128_t>(read(one.b).whole);
        break;
      case Op::IntEq: to.whole = read(one.a).whole == read(one.b).whole; break;
      case Op::IntNe: to.whole = read(one.a).whole != read(one.b).whole; break;

      case Op::IntAddK:
        to.whole = narrow(static_cast<XagInt>(static_cast<__uint128_t>(read(one.a).whole) +
                                              static_cast<__uint128_t>(pool[one.b].whole)),
                          one.aux);
        break;
      case Op::IntSubK:
        to.whole = narrow(static_cast<XagInt>(static_cast<__uint128_t>(read(one.a).whole) -
                                              static_cast<__uint128_t>(pool[one.b].whole)),
                          one.aux);
        break;
      case Op::IntMulK:
        to.whole = narrow(static_cast<XagInt>(static_cast<__uint128_t>(read(one.a).whole) *
                                              static_cast<__uint128_t>(pool[one.b].whole)),
                          one.aux);
        break;
      case Op::IntLtK:
        to.whole = one.aux ? read(one.a).whole < pool[one.b].whole
                           : static_cast<__uint128_t>(read(one.a).whole) <
                                 static_cast<__uint128_t>(pool[one.b].whole);
        break;
      case Op::IntGtK:
        to.whole = one.aux ? read(one.a).whole > pool[one.b].whole
                           : static_cast<__uint128_t>(read(one.a).whole) >
                                 static_cast<__uint128_t>(pool[one.b].whole);
        break;
      case Op::IntLeK:
        to.whole = one.aux ? read(one.a).whole <= pool[one.b].whole
                           : static_cast<__uint128_t>(read(one.a).whole) <=
                                 static_cast<__uint128_t>(pool[one.b].whole);
        break;
      case Op::IntGeK:
        to.whole = one.aux ? read(one.a).whole >= pool[one.b].whole
                           : static_cast<__uint128_t>(read(one.a).whole) >=
                                 static_cast<__uint128_t>(pool[one.b].whole);
        break;
      case Op::IntEqK: to.whole = read(one.a).whole == pool[one.b].whole; break;
      case Op::IntNeK: to.whole = read(one.a).whole != pool[one.b].whole; break;

      // The truth is written as the plain comparison writes it, and then the
      // jump is taken or not, as JumpUnless would have.
      case Op::IntLtJ:
        to.whole = one.aux ? read(one.a).whole < read(one.b).whole
                           : static_cast<__uint128_t>(read(one.a).whole) <
                                 static_cast<__uint128_t>(read(one.b).whole);
        if (to.whole == 0) { at = one.jump; continue; }
        break;
      case Op::IntGtJ:
        to.whole = one.aux ? read(one.a).whole > read(one.b).whole
                           : static_cast<__uint128_t>(read(one.a).whole) >
                                 static_cast<__uint128_t>(read(one.b).whole);
        if (to.whole == 0) { at = one.jump; continue; }
        break;
      case Op::IntLeJ:
        to.whole = one.aux ? read(one.a).whole <= read(one.b).whole
                           : static_cast<__uint128_t>(read(one.a).whole) <=
                                 static_cast<__uint128_t>(read(one.b).whole);
        if (to.whole == 0) { at = one.jump; continue; }
        break;
      case Op::IntGeJ:
        to.whole = one.aux ? read(one.a).whole >= read(one.b).whole
                           : static_cast<__uint128_t>(read(one.a).whole) >=
                                 static_cast<__uint128_t>(read(one.b).whole);
        if (to.whole == 0) { at = one.jump; continue; }
        break;
      case Op::IntEqJ:
        to.whole = read(one.a).whole == read(one.b).whole;
        if (to.whole == 0) { at = one.jump; continue; }
        break;
      case Op::IntNeJ:
        to.whole = read(one.a).whole != read(one.b).whole;
        if (to.whole == 0) { at = one.jump; continue; }
        break;

      case Op::RealAdd: to.real = xag_bin_fit(read(one.a).real + read(one.b).real, one.aux); break;
      case Op::RealSub: to.real = xag_bin_fit(read(one.a).real - read(one.b).real, one.aux); break;
      case Op::RealMul: to.real = xag_bin_fit(read(one.a).real * read(one.b).real, one.aux); break;
      case Op::RealDiv: to.real = xag_bin_fit(read(one.a).real / read(one.b).real, one.aux); break;
      case Op::RealMod: to.real = xag_bin_mod(read(one.a).real, read(one.b).real, one.aux); break;
      case Op::RealPow: to.real = xag_bin_pow(read(one.a).real, read(one.b).real, one.aux); break;
      case Op::RealLt: to.whole = read(one.a).real < read(one.b).real; break;
      case Op::RealGt: to.whole = read(one.a).real > read(one.b).real; break;
      case Op::RealLe: to.whole = read(one.a).real <= read(one.b).real; break;
      case Op::RealGe: to.whole = read(one.a).real >= read(one.b).real; break;
      case Op::RealEq: to.whole = read(one.a).real == read(one.b).real; break;
      case Op::RealNe: to.whole = read(one.a).real != read(one.b).real; break;

      case Op::WideAdd: to.whole = xag_bin128_add(read(one.a).whole, read(one.b).whole); break;
      case Op::WideSub: to.whole = xag_bin128_sub(read(one.a).whole, read(one.b).whole); break;
      case Op::WideMul: to.whole = xag_bin128_mul(read(one.a).whole, read(one.b).whole); break;
      case Op::WideDiv: to.whole = xag_bin128_div(read(one.a).whole, read(one.b).whole); break;
      case Op::WideMod: to.whole = xag_bin128_mod(read(one.a).whole, read(one.b).whole); break;
      case Op::WidePow: to.whole = xag_bin128_pow(read(one.a).whole, read(one.b).whole); break;
      case Op::WideCompare:
        to.whole = xag_bin128_compare(read(one.a).whole, read(one.b).whole);
        break;

      case Op::DeciAdd: to.whole = xag_deci_add(one.aux, read(one.a).whole, read(one.b).whole); break;
      case Op::DeciSub: to.whole = xag_deci_sub(one.aux, read(one.a).whole, read(one.b).whole); break;
      case Op::DeciMul: to.whole = xag_deci_mul(one.aux, read(one.a).whole, read(one.b).whole); break;
      case Op::DeciDiv: to.whole = xag_deci_div(one.aux, read(one.a).whole, read(one.b).whole); break;
      case Op::DeciMod: to.whole = xag_deci_mod(one.aux, read(one.a).whole, read(one.b).whole); break;
      case Op::DeciPow: to.whole = xag_deci_pow(one.aux, read(one.a).whole, read(one.b).whole); break;
      case Op::DeciCompare:
        to.whole = xag_deci_compare(one.aux, read(one.a).whole, read(one.b).whole);
        break;

      case Op::TextCompare:
        to.whole = xag_str_compare(&read(one.a).text, &read(one.b).text);
        break;
      case Op::ReadLine: {
        XagStr line{nullptr, 0, 0};
        const int got = xag_read_line(&line);
        end(to);
        to = Slot{};
        if (got) {
          to.text = line;
          to.owns = true;
        } else {
          to.empty = true; // nothing left, which is not an empty line
        }
        break;
      }

      case Op::Arguments: {
        XagMany given{nullptr, 0};
        xag_arguments(&given);
        auto *held = new std::vector<Slot>();
        const XagStr *from = static_cast<const XagStr *>(given.places);
        for (uint64_t i = 0; i < given.length; ++i) {
          Slot one;
          one.text = from[i];
          one.owns = true; // taken out of the runtime's array, which goes below
          held->push_back(one);
        }
        xag_many_drop(&given);
        end(to);
        to = Slot{};
        to.places = held;
        to.owns = true;
        xag_note_taken();
        break;
      }

      case Op::NumberOf: {
        Slot &text = read(one.a);
        const uint32_t width = one.aux >> 3;
        const uint32_t family = (one.aux >> 1) & 0x3;
        const int32_t isSignedOne = one.aux & 1;
        const char *bytes = text.text.bytes ? text.text.bytes : "";
        Slot answer;
        answer.empty = true;
        if (family == 0) {
          XagInt got = 0;
          if (xag_int_reads(width, isSignedOne, bytes, text.text.length, &got)) {
            answer = Slot{};
            answer.whole = got;
          }
        } else if (family == 2) {
          XagDeci got = 0;
          if (xag_deci_reads(width, bytes, text.text.length, &got)) {
            answer = Slot{};
            answer.whole = static_cast<XagInt>(got);
          }
        } else if (width == 128) {
          XagBin128 got = 0;
          if (xag_bin128_reads(bytes, text.text.length, &got)) {
            answer = Slot{};
            answer.whole = static_cast<XagInt>(got);
          }
        } else {
          double got = 0;
          if (xag_bin_reads(bytes, text.text.length, width, &got)) {
            answer = Slot{};
            answer.real = got;
          }
        }
        end(to);
        to = answer;
        break;
      }

      case Op::LoadNone:
        end(to);
        to.empty = true;
        break;
      case Op::HoldsSomething:
        to.whole = read(one.a).empty ? 0 : 1;
        break;
      case Op::TakeInside: {
        // Lent, not taken.
        Slot seen = read(one.a);
        seen.owns = false;
        end(to);
        to = seen;
        break;
      }

      case Op::TextCount: {
        Slot &of = read(one.a);
        to.whole = of.places ? static_cast<XagInt>(of.places->size())
                             : xag_str_count(&of.text);
        break;
      }

      case Op::MakeMany: makeMany(to, one.b); break;
      case Op::FillMany: fillMany(to, read(one.a), read(one.b).whole); break;
      case Op::ElementAt: elementAt(to, read(one.a), read(one.b).whole); break;
      case Op::StoreAt: storeAt(read(one.to), read(one.a).whole, slots[one.b]); break;
      case Op::TextJoin: joinText(to, one.b); break;

      case Op::Not: to.whole = read(one.a).whole == 0; break;
      case Op::And: to.whole = read(one.a).whole != 0 && read(one.b).whole != 0; break;
      case Op::Or: to.whole = read(one.a).whole != 0 || read(one.b).whole != 0; break;
      case Op::Order: {
        const XagInt order = read(one.a).whole;
        const bool ordered = order != -3;
        switch (one.aux) {
        case 0: to.whole = ordered && order < 0; break;
        case 1: to.whole = ordered && order > 0; break;
        case 2: to.whole = ordered && order <= 0; break;
        case 3: to.whole = ordered && order >= 0; break;
        case 4: to.whole = ordered && order == 0; break;
        default: to.whole = !ordered || order != 0; break;
        }
        break;
      }

      case Op::PushArg: arguments_.push_back(base + one.a); break;
      case Op::Call: {
        // The answer lands where it is wanted rather than passing through a
        // slot of its own. What was there is ended first: it cannot be one of
        // the arguments, since the graph gives every call's answer a place of
        // its own to fill.
        end(to);
        to = Slot{};
        steps_ = steps;
        call(one.a, base + routine.slots, to, one.b);
        steps = steps_;
        if (!trouble_.empty())
          goto finished;
        break;
      }
      case Op::PrintWhole: xag_print_int(read(one.a).whole, one.aux >> 1, one.aux & 1); break;
      case Op::PrintReal: xag_print_bin(read(one.a).real, one.aux >> 1); break;
      case Op::PrintWide: xag_print_bin128(read(one.a).whole); break;
      case Op::PrintDeci: xag_print_deci(one.aux >> 1, read(one.a).whole); break;
      case Op::PrintText: xag_print(&read(one.a).text); break;
      case Op::PrintBool: xag_print_bool(read(one.a).whole != 0); break;

      case Op::Jump: at = one.to; continue;
      case Op::JumpUnless:
        if (read(one.a).whole == 0) {
          at = one.to;
          continue;
        }
        break;
      case Op::ReturnValue:
        // The answer's slot is below this frame, so it can be written before
        // the one it came from is emptied.
        answer = slots[one.a];
        slots[one.a] = Slot{};
        goto finished;
      case Op::Return:
      case Op::Halt:
        goto finished;
      }
      ++at;
    }
  finished:
    steps_ = steps;

    for (unsigned i = 0; i < routine.slots; ++i)
      end(stack_[base + i]);
    --depth_;
  }
};

} // namespace

FastResult runFast(const Mir &mir) {
  Builder builder(mir);
  return Machine(builder.run(), mir.settings).run();
}

} // namespace xag
