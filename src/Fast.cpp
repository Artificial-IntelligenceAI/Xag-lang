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
  // A comparison and the switch that reads it, as one step: the truth is
  // still written to `to`, then `jump` is taken when it is false and what
  // `aux` holds above the signedness when it is true. A loop asks one of
  // these on every turn.
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
  // One argument to the step before it — a Call, a MakeMany or a TextJoin —
  // which reads as many of these as it was told to and steps over them. An
  // Argument is data laid out as code, never a step of its own.
  Argument,
  Call, PrintWhole, PrintReal, PrintWide, PrintDeci, PrintText, PrintBool,
  Jump, JumpUnless, Return, ReturnValue,
  // A struct is a fixed run of places, held exactly as a `many` is; only the
  // type tells them apart, and the type was settled before anything ran. What
  // it has that a `many` does not is that a field is known where it is
  // written, so one can be lent, taken or written on its own.
  MakeGroup, // `b` fields, each an Argument after it (`b` set when it is text)
  ViewPart,  // `to` = a view of field `b` of the struct in `a`, lent where it stands
  TakePart,  // `to` = field `b` of the struct in `a`, which is left holding nothing
  StorePart, // field path of `b` Arguments into `to`, given what is in `a`
  TextOf,    // `to` = the number in `a` as text, exactly as print would write it
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
  // How often each local is read and written across the whole body, so that a
  // temporary made by one step and read by the next can be seen for what it is.
  std::vector<unsigned> reads_;
  std::vector<unsigned> writes_;
  unsigned blockStart_ = 0; // where the code of the block being compiled began

  void countUses(const Body &body) {
    reads_.assign(body.locals.size(), 0);
    writes_.assign(body.locals.size(), 0);
    auto read = [&](const Operand &operand) {
      if (operand.kind != OperandKind::Written && operand.local < reads_.size())
        ++reads_[operand.local];
    };
    for (const BasicBlock &block : body.blocks) {
      for (const Statement &s : block.statements) {
        if (s.kind == StatementKind::Assign && s.parts.empty()) {
          if (s.place < writes_.size())
            ++writes_[s.place];
        } else if (s.place < reads_.size()) {
          ++reads_[s.place]; // a drop ends it, a store or a field write reaches into it
        }
        if (s.kind == StatementKind::Store)
          read(s.at);
        if (s.conditional && s.flag < reads_.size())
          ++reads_[s.flag];
        if (s.value.kind == RValueKind::Ref && s.value.local < reads_.size())
          ++reads_[s.value.local];
        for (const Operand &operand : s.value.operands)
          read(operand);
      }
      const Terminator &end = block.terminator;
      if (end.kind == TerminatorKind::Switch)
        read(end.condition);
      if (end.kind == TerminatorKind::Return && end.answers)
        read(end.answer);
    }
  }

  // The steps that write a number and nothing else into `to`, and so can be
  // pointed at any slot of that number's type.
  static bool producesNumber(Op op) {
    switch (op) {
    case Op::LoadWhole: case Op::LoadReal: case Op::LoadWide:
    case Op::CopyWhole: case Op::CopyReal:
    case Op::IntAdd: case Op::IntSub: case Op::IntMul: case Op::IntDiv: case Op::IntMod:
    case Op::IntPow: case Op::IntLt: case Op::IntGt: case Op::IntLe: case Op::IntGe:
    case Op::IntEq: case Op::IntNe: case Op::IntAddK: case Op::IntSubK: case Op::IntMulK:
    case Op::IntLtK: case Op::IntGtK: case Op::IntLeK: case Op::IntGeK: case Op::IntEqK:
    case Op::IntNeK: case Op::RealAdd: case Op::RealSub: case Op::RealMul: case Op::RealDiv:
    case Op::RealMod: case Op::RealPow: case Op::RealLt: case Op::RealGt: case Op::RealLe:
    case Op::RealGe: case Op::RealEq: case Op::RealNe: case Op::WideAdd: case Op::WideSub:
    case Op::WideMul: case Op::WideDiv: case Op::WideMod: case Op::WidePow:
    case Op::WideCompare: case Op::DeciAdd: case Op::DeciSub: case Op::DeciMul:
    case Op::DeciDiv: case Op::DeciMod: case Op::DeciPow: case Op::DeciCompare:
    case Op::TextCompare: case Op::TextCount: case Op::HoldsSomething:
    case Op::Not: case Op::And: case Op::Or: case Op::Order:
      return true;
    default:
      return false;
    }
  }

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

  // Every operand into a slot, in the order written, before the step that
  // takes them all.
  std::vector<uint32_t> gather(const std::vector<Operand> &operands, unsigned &scratch) {
    std::vector<uint32_t> froms;
    froms.reserve(operands.size());
    for (const Operand &operand : operands)
      froms.push_back(into(operand, scratch));
    return froms;
  }

  // The slots a step takes, laid out after it as Arguments for it to read.
  void arguments(const std::vector<uint32_t> &froms) {
    for (const uint32_t from : froms)
      emit(Code{Op::Argument, 0, from, 0, 0});
  }

  Routine compile(const Body &body) {
    body_ = &body;
    Routine routine;
    out_ = &routine;
    routine.name = body.name;
    routine.parameters = body.parameters;
    routine.answers = spelled(body.result) != "nothing";
    countUses(body);

    // Slots: one per local, then room above for the working ones.
    unsigned scratch = static_cast<unsigned>(body.locals.size());
    unsigned most = scratch;

    std::vector<unsigned> starts(body.blocks.size(), 0);
    // A fused comparison carries both its targets: where to go when false, in
    // `jump`, and when true, in the high bits of `aux` above the signedness.
    enum class Field { To, Jump, Then };
    struct Patch {
      unsigned at;    // which instruction
      unsigned block; // where it should go
      Field field;    // and which field of it says so
    };
    std::vector<Patch> patches;

    for (size_t which = 0; which < body.blocks.size(); ++which) {
      const BasicBlock &block = body.blocks[which];
      starts[block.id] = static_cast<unsigned>(routine.code.size());
      blockStart_ = starts[block.id];
      scratch = static_cast<unsigned>(body.locals.size());
      // A jump to the block laid out next lands where the code would have gone
      // anyway, so it is not emitted. A loop's test block is followed by its
      // body, which makes this the jump taken on every turn.
      const bool nextIs = which + 1 < body.blocks.size();
      auto follows = [&](unsigned target) {
        return nextIs && body.blocks[which + 1].id == target;
      };

      for (const Statement &s : block.statements) {
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
        if (!s.parts.empty()) {
          partStore(s, scratch);
          most = most > scratch ? most : scratch;
          continue;
        }
        statement(s, scratch);
        most = most > scratch ? most : scratch;
      }

      const Terminator &end = block.terminator;
      if (end.kind == TerminatorKind::Goto) {
        const unsigned target = end.targets.empty() ? 0 : end.targets[0];
        // A block already laid out that is nothing but one fused comparison —
        // a loop's test — is not jumped to but repeated here, targets and all,
        // since running the copy is running the block. The loop's body then
        // ends in the test rather than in a jump to it.
        unsigned test = static_cast<unsigned>(routine.code.size());
        for (size_t before = 0; before < which; ++before) {
          if (body.blocks[before].id != target)
            continue;
          const unsigned from = starts[target];
          const unsigned until = starts[body.blocks[before + 1].id];
          if (until == from + 1 && jumps(routine.code[from].op))
            test = from;
          break;
        }
        if (test < routine.code.size()) {
          const unsigned copy = static_cast<unsigned>(routine.code.size());
          const size_t known = patches.size();
          for (size_t i = 0; i < known; ++i)
            if (patches[i].at == test)
              patches.push_back({copy, patches[i].block, patches[i].field});
          emit(routine.code[test]);
        } else if (!follows(target)) {
          patches.push_back({static_cast<unsigned>(routine.code.size()), target, Field::To});
          emit(Code{Op::Jump, 0, 0, 0, 0});
        }
      } else if (end.kind == TerminatorKind::Switch) {
        const unsigned taken = end.targets.empty() ? 0 : end.targets[0];
        const unsigned otherwise = end.targets.size() > 1 ? end.targets.back() : taken;
        // When the block's last step compared two whole numbers into the very
        // truth the switch reads, the compare does the switch's work too. The
        // step has to be this block's: another block's would be a compare on
        // the way in, and there may be more than one way in.
        const Op fused = routine.code.size() > blockStart_ &&
                                 end.condition.kind == OperandKind::Copy &&
                                 routine.code.back().to == end.condition.local
                             ? fusedWithJump(routine.code.back().op)
                             : Op::Halt;
        if (fused != Op::Halt) {
          routine.code.back().op = fused;
          const unsigned at = static_cast<unsigned>(routine.code.size() - 1);
          patches.push_back({at, otherwise, Field::Jump});
          patches.push_back({at, taken, Field::Then});
        } else {
          const uint32_t asked = into(end.condition, scratch);
          patches.push_back({static_cast<unsigned>(routine.code.size()), otherwise, Field::To});
          emit(Code{Op::JumpUnless, 0, asked, 0, 0});
          if (!follows(taken)) {
            patches.push_back({static_cast<unsigned>(routine.code.size()), taken, Field::To});
            emit(Code{Op::Jump, 0, 0, 0, 0});
          }
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
      Code &code = routine.code[patch.at];
      switch (patch.field) {
      case Field::To: code.to = start; break;
      case Field::Jump: code.jump = start; break;
      case Field::Then: code.aux = (code.aux & 1u) | (start << 1); break;
      }
    }
    routine.slots = most;
    return routine;
  }

  // Whether this is a comparison that carries its own jumps.
  static bool jumps(Op op) {
    switch (op) {
    case Op::IntLtJ: case Op::IntGtJ: case Op::IntLeJ:
    case Op::IntGeJ: case Op::IntEqJ: case Op::IntNeJ:
      return true;
    default:
      return false;
    }
  }

  // The comparison that also jumps, or Halt for anything else.
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
      const std::vector<uint32_t> froms = gather(value.operands, scratch);
      emit(Code{Op::MakeMany, s.place, 0, static_cast<uint32_t>(froms.size()), 0});
      arguments(froms);
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

    case RValueKind::Group: {
      const std::vector<uint32_t> froms = gather(value.operands, scratch);
      const uint32_t place = through ? scratch++ : s.place;
      emit(Code{Op::MakeGroup, place, 0, static_cast<uint32_t>(froms.size()), 0});
      // Text going in must be the struct's own, so a field that is text is
      // marked for the machine to copy if what arrives is only a view.
      for (size_t i = 0; i < froms.size(); ++i)
        emit(Code{Op::Argument, 0, froms[i],
                  behind(spelled(value.operands[i].type)) == "str" ? 1u : 0u, 0});
      if (through)
        emit(Code{Op::StoreThrough, s.place, place, 0, 0});
      return;
    }

    case RValueKind::Part:
    case RValueKind::Taken: {
      const uint32_t of = into(value.operands[0], scratch);
      const uint32_t place = through ? scratch++ : s.place;
      emit(Code{value.kind == RValueKind::Part ? Op::ViewPart : Op::TakePart, place, of,
                value.local, 0});
      if (through)
        emit(Code{Op::StoreThrough, s.place, place, 0, 0});
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
        const bool same = number && behind(spelled(value.operands[0].type)) == kept;
        // A temporary that the step just before made, that is read here and
        // nowhere else, and that nothing else writes, need not exist at all:
        // that step writes here instead. The step has to be this block's, so
        // that no other way into the block arrives with the copy undone.
        const bool once = from < reads_.size() && reads_[from] == 1 && writes_[from] == 1 &&
                          body_->locals[from].name.empty();
        if (same && once && out_->code.size() > blockStart_ &&
            out_->code.back().to == from && producesNumber(out_->code.back().op)) {
          out_->code.back().to = s.place;
        } else if (same) {
          emit(Code{isBinary(plain) && plain != Type::Bin128 ? Op::CopyReal : Op::CopyWhole,
                    s.place, from, 0, 0});
        } else {
          emit(Code{Op::CopySlot, s.place, from, 0, isLoan(kept) ? 1u : 0u});
        }
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
      const std::vector<uint32_t> froms = gather(value.operands, scratch);
      emit(Code{Op::TextJoin, s.place, 0, static_cast<uint32_t>(froms.size()), 0});
      arguments(froms);
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

  // `place.f.g = value`: the value into a slot, then the path of fields laid
  // out after the step as Arguments, for it to follow down to the one place
  // being written. The graph only ever writes a field from one operand.
  void partStore(const Statement &s, unsigned &scratch) {
    const Operand &what = s.value.operands.empty() ? Operand{} : s.value.operands[0];
    const uint32_t from = into(what, scratch);
    const bool textual = behind(spelled(what.type)) == "str";
    emit(Code{Op::StorePart, s.place, from, static_cast<uint32_t>(s.parts.size()),
              textual ? 1u : 0u});
    for (const unsigned part : s.parts)
      emit(Code{Op::Argument, 0, part, 0, 0});
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
    if (value.callee == "convert-to-str") {
      // The operand's own type says which family writes it, exactly as it does
      // for print, and the same one word carries that to the machine.
      const uint32_t from = value.operands.empty() ? 0 : into(value.operands[0], scratch);
      const std::string type =
          value.operands.empty() ? std::string() : behind(spelled(value.operands[0].type));
      const Type given = typeNamed(type);
      const uint32_t family = type == "bool" ? 3u : isDecimal(given) ? 2u : isBinary(given) ? 1u : 0u;
      emit(Code{Op::TextOf, s.place, from, 0,
                (widthOf(given) << 3) | (family << 1) | (isSigned(given) ? 1u : 0u)});
      return;
    }
    if (value.callee == "convert-to-number") {
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
    const std::vector<uint32_t> froms = gather(value.operands, scratch);
    const uint32_t place = through ? scratch++ : s.place;
    emit(Code{Op::Call, place, findRoutine(value.callee),
              static_cast<uint32_t>(froms.size()), 0});
    arguments(froms);
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
    call(start, 0, answer, nullptr, nullptr, 0);
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

  [[gnu::noinline]] void makeMany(Slot &to, Slot *slots, const Code *given, unsigned count) {
    auto *held = new std::vector<Slot>();
    held->reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      Slot &from = slots[given[i].a];
      held->push_back(from);
      // What went in belongs to the array now, and the slot it came from
      // must not end it a second time.
      if (from.owns)
        from = Slot{};
    }
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

  [[gnu::noinline]] void joinText(Slot &to, Slot *slots, const Code *given, unsigned count) {
    // The pieces are gathered into a list kept from one join to the next, so
    // that joining does not take and give back a list's worth of memory every
    // time on top of the text's.
    pieces_.clear();
    for (unsigned i = 0; i < count; ++i)
      pieces_.push_back(behind(slots[given[i].a]).text);
    XagStr joined{nullptr, 0, 0};
    xag_str_join(&joined, pieces_.data(), pieces_.size());
    end(to);
    to.text = joined;
    to.owns = true;
  }

  [[gnu::noinline]] void readLine(Slot &to) {
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
  }

  [[gnu::noinline]] void takeArguments(Slot &to) {
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
  }

  [[gnu::noinline]] void numberOf(Slot &to, const Slot &text, uint32_t aux) {
    const uint32_t width = aux >> 3;
    const uint32_t family = (aux >> 1) & 0x3;
    const int32_t isSignedOne = aux & 1;
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
  }

  // Text that a struct is to hold must be the struct's own, so that it lives
  // exactly as long as the struct and can be lent from where it stands. What
  // arrives as only a view — a written text is one — is copied.
  static void own(Slot &piece) {
    if (piece.owns)
      return;
    XagStr copy{nullptr, 0, 0};
    xag_str_from(&copy, piece.text.bytes, piece.text.length);
    piece.text = copy;
    piece.owns = true;
  }

  [[gnu::noinline]] void makeGroup(Slot &to, Slot *slots, const Code *given, unsigned count) {
    auto *held = new std::vector<Slot>();
    held->reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      Slot &from = slots[given[i].a];
      Slot piece = from;
      if (from.owns)
        from = Slot{}; // handed over, so the slot it came from must not end it
      else if (given[i].b)
        own(piece);
      held->push_back(piece);
    }
    end(to);
    to = Slot{};
    to.places = held;
    to.owns = true;
    xag_note_taken();
  }

  // These three follow a loan to the struct themselves, so that the loop's
  // step is nothing but the call.
  [[gnu::noinline]] void viewPart(Slot &to, Slot &holder, uint32_t field) {
    const Slot &of = behind(holder);
    Slot seen;
    if (of.places && field < of.places->size()) {
      seen = (*of.places)[field];
      seen.owns = false; // lent where it stands, and no claim on it
    }
    end(to);
    to = seen;
  }

  [[gnu::noinline]] void takePart(Slot &to, Slot &holder, uint32_t field) {
    Slot &of = behind(holder);
    Slot taken;
    if (of.places && field < of.places->size()) {
      taken = (*of.places)[field];
      (*of.places)[field] = Slot{}; // gone from here, so the later drop finds nothing
    }
    end(to);
    to = taken;
  }

  // Down the path to the one place being written, leaving everything beside
  // it exactly as it was. Written through a loan when the place holds one, as
  // a whole value written to a loan is.
  [[gnu::noinline]] void storePart(Slot &holder, const Code *path, unsigned depth, Slot &given,
                                   bool textual) {
    Slot kept = given;
    if (given.owns)
      given = Slot{};
    Slot *target = &behind(holder);
    for (unsigned i = 0; i < depth; ++i) {
      Slot &at = behind(*target);
      if (!at.places || path[i].a >= at.places->size()) {
        end(kept);
        return;
      }
      target = &(*at.places)[path[i].a];
    }
    if (textual)
      own(kept);
    end(*target);
    *target = kept;
  }

  // A number as text, by the same runtime functions print writes through, so
  // that what this answers and what print writes cannot drift apart. What is
  // made here is the slot's own, and is let go of like any other text.
  [[gnu::noinline]] void textOf(Slot &to, const Slot &of, uint32_t aux) {
    const uint32_t width = aux >> 3;
    const uint32_t family = (aux >> 1) & 0x3;
    XagStr made{nullptr, 0, 0};
    if (family == 3)
      xag_str_of_bool(&made, of.whole != 0);
    else if (family == 2)
      xag_str_of_deci(&made, width, static_cast<XagDeci>(of.whole));
    else if (family == 1 && width == 128)
      xag_str_of_bin128(&made, static_cast<XagBin128>(of.whole));
    else if (family == 1)
      xag_str_of_bin(&made, of.real, width);
    else
      xag_str_of_int(&made, of.whole, width, aux & 1);
    end(to);
    to = Slot{};
    to.text = made;
    to.owns = true;
  }

  // Runs a routine on `given` arguments, each naming a slot in the caller's
  // frame at `from`.
  void call(unsigned which, uint32_t base, Slot &answer, Slot *from, const Code *given,
            unsigned count) {
    if (which >= routines_.size())
      return;
    if (++depth_ > 400 || base + routines_[which].slots + 8 >= kStack) {
      trouble_ = "a function called itself further than this engine will follow";
      --depth_;
      return;
    }
    const Routine &routine = routines_[which];
    for (unsigned i = 0; i < routine.slots; ++i)
      stack_[base + i] = Slot{};
    for (unsigned i = 0; i < count && i + 1 < routine.slots; ++i)
      stack_[base + i + 1] = from[given[i].a];
    // What was handed over is gone from where it was.
    for (unsigned i = 0; i < count; ++i)
      if (from[given[i].a].owns)
        from[given[i].a] = Slot{};

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
      if (++steps > kBudget) [[unlikely]] {
        trouble_ = "the program ran longer than this engine will wait";
        break;
      }
      const Code &one = code[at];
      auto read = [&](uint32_t i) -> Slot & { return behind(slots[i]); };
      Slot &to = slots[one.to];

      switch (one.op) {
      case Op::LoadWhole:
        end(to);
        to.whole = pool[one.aux].whole;
        break;
      case Op::LoadReal:
        end(to);
        to.real = pool[one.aux].real;
        break;
      case Op::LoadWide:
        end(to);
        to.whole = pool[one.aux].whole;
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

      // The truth is written as the plain comparison writes it, and then one
      // of the two ways on is taken: `aux` above its lowest bit when true,
      // `jump` when false.
      case Op::IntLtJ:
        to.whole = (one.aux & 1) ? read(one.a).whole < read(one.b).whole
                                 : static_cast<__uint128_t>(read(one.a).whole) <
                                       static_cast<__uint128_t>(read(one.b).whole);
        at = to.whole ? one.aux >> 1 : one.jump;
        continue;
      case Op::IntGtJ:
        to.whole = (one.aux & 1) ? read(one.a).whole > read(one.b).whole
                                 : static_cast<__uint128_t>(read(one.a).whole) >
                                       static_cast<__uint128_t>(read(one.b).whole);
        at = to.whole ? one.aux >> 1 : one.jump;
        continue;
      case Op::IntLeJ:
        to.whole = (one.aux & 1) ? read(one.a).whole <= read(one.b).whole
                                 : static_cast<__uint128_t>(read(one.a).whole) <=
                                       static_cast<__uint128_t>(read(one.b).whole);
        at = to.whole ? one.aux >> 1 : one.jump;
        continue;
      case Op::IntGeJ:
        to.whole = (one.aux & 1) ? read(one.a).whole >= read(one.b).whole
                                 : static_cast<__uint128_t>(read(one.a).whole) >=
                                       static_cast<__uint128_t>(read(one.b).whole);
        at = to.whole ? one.aux >> 1 : one.jump;
        continue;
      case Op::IntEqJ:
        to.whole = read(one.a).whole == read(one.b).whole;
        at = to.whole ? one.aux >> 1 : one.jump;
        continue;
      case Op::IntNeJ:
        to.whole = read(one.a).whole != read(one.b).whole;
        at = to.whole ? one.aux >> 1 : one.jump;
        continue;

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
      [[unlikely]] case Op::ReadLine: readLine(to); break;
      [[unlikely]] case Op::Arguments: takeArguments(to); break;
      [[unlikely]] case Op::NumberOf: numberOf(to, read(one.a), one.aux); break;
      [[unlikely]] case Op::TextOf: textOf(to, read(one.a), one.aux); break;

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

      // The steps that call out are marked as the rare ones, so that the
      // compiler keeps the loop's own state in registers across the common
      // ones and does its saving and restoring here instead. Without the
      // mark it weighs every step alike, and adding a few more of these once
      // moved the frame pointer to a register that had to be fetched back
      // from the stack on every step.
      [[unlikely]] case Op::MakeMany:
        makeMany(to, slots, code + at + 1, one.b);
        at += one.b;
        break;
      [[unlikely]] case Op::MakeGroup:
        makeGroup(to, slots, code + at + 1, one.b);
        at += one.b;
        break;
      [[unlikely]] case Op::ViewPart: viewPart(to, slots[one.a], one.b); break;
      [[unlikely]] case Op::TakePart: takePart(to, slots[one.a], one.b); break;
      [[unlikely]] case Op::StorePart:
        storePart(to, code + at + 1, one.b, slots[one.a], one.aux != 0);
        at += one.b;
        break;
      [[unlikely]] case Op::FillMany: fillMany(to, read(one.a), read(one.b).whole); break;
      case Op::ElementAt: elementAt(to, read(one.a), read(one.b).whole); break;
      case Op::StoreAt: storeAt(read(one.to), read(one.a).whole, slots[one.b]); break;
      [[unlikely]] case Op::TextJoin:
        joinText(to, slots, code + at + 1, one.b);
        at += one.b;
        break;

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

      case Op::Argument:
        // Never a step of its own: the step before it read it and stepped over
        // it. Only a routine that began with one could arrive here.
        break;
      case Op::Call: {
        // The answer lands where it is wanted rather than passing through a
        // slot of its own. What was there is ended first: it cannot be one of
        // the arguments, since the graph gives every call's answer a place of
        // its own to fill.
        end(to);
        to = Slot{};
        steps_ = steps;
        call(one.a, base + routine.slots, to, slots, code + at + 1, one.b);
        steps = steps_;
        if (!trouble_.empty())
          goto finished;
        at += one.b;
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
