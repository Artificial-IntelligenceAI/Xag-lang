#include "xag/Interpret.h"

#include "xag_runtime.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace xag {
namespace {

// What a local holds while a program runs.
//
// A `str` here is either owned — this slot must end it — or a view of somebody
// else's bytes, which reading produces and which must never be freed. That is
// the same distinction the IR draws between `move` and `copy`, kept honest at
// run time rather than assumed.
struct Value {
  enum class Kind { Nothing, Number, Text, Loan } kind = Kind::Nothing;
  int64_t number = 0;
  XagStr text{nullptr, 0, 0};
  bool owns = false;
  unsigned frame = 0; // Loan: which frame the lent slot lives in
  unsigned slot = 0;  //       and which slot it is
};

struct Frame {
  const Body *body = nullptr;
  std::vector<Value> locals;
};

class Machine {
public:
  explicit Machine(const Mir &mir) : mir_(mir) {}

  InterpretResult run() {
    const Body *start = find("START");
    if (!start)
      return InterpretResult{false, "there is no START to run"};
    Value answer;
    call(*start, {}, answer);
    endValue(answer);
    if (trouble_.empty() && !xag_balance_is_clear())
      trouble_ = "the program ended still holding " +
                 std::to_string(xag_live_allocations()) + " thing(s)";
    return InterpretResult{trouble_.empty(), trouble_};
  }

private:
  const Mir &mir_;
  std::vector<Frame> frames_;
  std::string trouble_;
  uint64_t steps_ = 0;

  static constexpr uint64_t kBudget = 50u * 1000u * 1000u;

  const Body *find(const std::string &name) const {
    for (const Body &body : mir_.bodies)
      if (body.name == name)
        return &body;
    return nullptr;
  }

  // ---- values

  // Following a loan to the slot it names. A loan of a loan is still one slot.
  Value *behind(Value &value) {
    Value *at = &value;
    // A loan chain is short in any program that means anything. Counting the
    // hops turns a loan that somehow points at itself into something this
    // engine says out loud, rather than something it does forever.
    for (unsigned hops = 0; at->kind == Value::Kind::Loan; ++hops) {
      if (hops > 64) {
        trouble_ = "a loan led back to itself";
        return nullptr;
      }
      if (at->frame >= frames_.size() || at->slot >= frames_[at->frame].locals.size())
        return nullptr;
      at = &frames_[at->frame].locals[at->slot];
    }
    return at;
  }

  // A view is what reading gives: the same bytes, and no claim on them.
  static Value viewOf(const Value &value) {
    Value seen = value;
    seen.owns = false;
    return seen;
  }

  void endValue(Value &value) {
    if (value.kind == Value::Kind::Text && value.owns)
      xag_str_drop(&value.text);
    value = Value{};
  }

  void put(unsigned slot, Value value) {
    Frame &frame = frames_.back();
    Value *target = &frame.locals[slot];
    // Writing a *value* to a name that holds a loan writes through it, which is
    // what being lent for writing means. Writing a *loan* to it replaces the
    // loan — the second time round a loop, `_2 = ref _1` would otherwise write
    // the new loan through the old one and into `_1`, leaving `_1` lent to
    // itself and anything following it going round forever.
    if (target->kind == Value::Kind::Loan && value.kind != Value::Kind::Loan)
      if (Value *lent = behind(*target))
        target = lent;
    endValue(*target);
    *target = std::move(value);
  }

  bool truthOf(const Value &value) { return value.number != 0; }

  std::string spelled(Value value) {
    Value *at = behind(value);
    if (!at)
      return "";
    switch (at->kind) {
    case Value::Kind::Number: return std::to_string(at->number);
    case Value::Kind::Text:   return std::string(at->text.bytes ? at->text.bytes : "",
                                                 at->text.length);
    default:                  return "";
    }
  }

  // ---- reading operands

  Value read(const Operand &operand) {
    switch (operand.kind) {
    case OperandKind::Written: {
      const std::string &type = typeOf(operand.type);
      Value value;
      if (type == "i64") {
        value.kind = Value::Kind::Number;
        value.number = std::strtoll(operand.written.c_str(), nullptr, 10);
      } else if (type == "bool") {
        value.kind = Value::Kind::Number;
        value.number = operand.written == "true" ? 1 : 0;
      } else {
        value.kind = Value::Kind::Text;
        std::string written = unescape(operand.written);
        xag_str_from(&value.text, written.data(), written.size());
        value.owns = true;
      }
      return value;
    }
    case OperandKind::Copy:
      return viewOf(frames_.back().locals[operand.local]);
    case OperandKind::Move: {
      Value taken = frames_.back().locals[operand.local];
      frames_.back().locals[operand.local] = Value{};
      return taken;
    }
    }
    return Value{};
  }

  // Escapes stand outside a written value, so the only ones that reach here are
  // the items that are escapes themselves.
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

  const std::string &typeOf(TypeRef type) const {
    static const std::string unknown = "?";
    const Body &body = *frames_.back().body;
    return type.index < body.types.size() ? body.types[type.index] : unknown;
  }

  // ---- doing

  Value evaluate(const RValue &value) {
    switch (value.kind) {
    case RValueKind::Use:
      return value.operands.empty() ? Value{} : read(value.operands[0]);

    case RValueKind::Ref: {
      Value loan;
      loan.kind = Value::Kind::Loan;
      loan.frame = static_cast<unsigned>(frames_.size() - 1);
      loan.slot = value.local;
      return loan;
    }

    case RValueKind::Unary: {
      Value inner = read(value.operands[0]);
      Value answer;
      answer.kind = Value::Kind::Number;
      answer.number = truthOf(*behind(inner)) ? 0 : 1;
      endValue(inner);
      return answer;
    }

    case RValueKind::Binary:
      return binary(value);

    case RValueKind::Join: {
      std::vector<XagStr> pieces;
      std::vector<Value> read_;
      std::vector<std::string> spellings;
      // The views below point into these strings, so the vector must not move.
      spellings.reserve(value.operands.size());
      for (const Operand &operand : value.operands) {
        Value piece = read(operand);
        Value *at = behind(piece);
        if (at && at->kind == Value::Kind::Text)
          pieces.push_back(at->text);
        else {
          spellings.push_back(spelled(piece));
          pieces.push_back(XagStr{const_cast<char *>(spellings.back().data()),
                                  spellings.back().size(), spellings.back().size()});
        }
        read_.push_back(piece);
      }
      Value joined;
      joined.kind = Value::Kind::Text;
      xag_str_join(&joined.text, pieces.data(), pieces.size());
      joined.owns = true;
      for (Value &piece : read_)
        endValue(piece);
      return joined;
    }

    case RValueKind::Call:
      return callByName(value);
    }
    return Value{};
  }

  Value binary(const RValue &value) {
    Value left = read(value.operands[0]);
    Value right = read(value.operands[1]);
    Value *a = behind(left);
    Value *b = behind(right);

    Value answer;
    answer.kind = Value::Kind::Number;
    const std::string &op = value.op;

    if (a && b && a->kind == Value::Kind::Text) {
      // One implementation of how text orders, called by every engine.
      const int64_t seen = xag_str_compare(&a->text, &b->text);
      if (op == "==") answer.number = seen == 0;
      else if (op == "!==") answer.number = seen != 0;
      else if (op == "<") answer.number = seen < 0;
      else if (op == ">") answer.number = seen > 0;
      else if (op == "<==") answer.number = seen <= 0;
      else if (op == ">==") answer.number = seen >= 0;
      else trouble_ = "`" + op + "` was asked of text";
    } else if (a && b) {
      const int64_t x = a->number, y = b->number;
      if (op == "+") answer.number = xag_i64_add(x, y);
      else if (op == "-") answer.number = xag_i64_sub(x, y);
      else if (op == "x") answer.number = xag_i64_mul(x, y);
      else if (op == "/") answer.number = xag_i64_div(x, y);
      else if (op == "mod") answer.number = xag_i64_mod(x, y);
      else if (op == "^") answer.number = xag_i64_pow(x, y);
      else if (op == "and") answer.number = (x != 0) && (y != 0);
      else if (op == "or") answer.number = (x != 0) || (y != 0);
      else if (op == "==") answer.number = x == y;
      else if (op == "!==") answer.number = x != y;
      else if (op == "<") answer.number = x < y;
      else if (op == ">") answer.number = x > y;
      else if (op == "<==") answer.number = x <= y;
      else if (op == ">==") answer.number = x >= y;
      else trouble_ = "`" + op + "` is not an operator this engine knows";
    }

    endValue(left);
    endValue(right);
    return answer;
  }

  Value callByName(const RValue &value) {
    if (value.callee == "print.stdout") {
      for (const Operand &operand : value.operands) {
        Value piece = read(operand);
        Value *at = behind(piece);
        if (at && at->kind == Value::Kind::Text)
          xag_print(&at->text);
        else if (at && at->kind == Value::Kind::Number)
          xag_print_i64(at->number);
        endValue(piece);
      }
      return Value{};
    }

    if (value.callee == "count") {
      Value piece = value.operands.empty() ? Value{} : read(value.operands[0]);
      Value *at = behind(piece);
      Value answer;
      answer.kind = Value::Kind::Number;
      answer.number = at && at->kind == Value::Kind::Text ? xag_str_count(&at->text) : 0;
      endValue(piece);
      return answer;
    }

    const Body *body = find(value.callee);
    if (!body) {
      trouble_ = "`" + value.callee + "` is not a function this engine knows";
      return Value{};
    }
    std::vector<Value> arguments;
    for (const Operand &operand : value.operands)
      arguments.push_back(read(operand));
    Value answer;
    call(*body, std::move(arguments), answer);
    return answer;
  }

  // ---- one body

  void call(const Body &body, std::vector<Value> arguments, Value &answer) {
    if (frames_.size() > 512) {
      trouble_ = "a function called itself further than this engine will follow";
      return;
    }
    Frame frame;
    frame.body = &body;
    frame.locals.assign(body.locals.size(), Value{});
    for (unsigned i = 0; i < arguments.size() && i + 1 < frame.locals.size(); ++i)
      frame.locals[i + 1] = std::move(arguments[i]);
    frames_.push_back(std::move(frame));

    unsigned at = 0;
    while (at < body.blocks.size() && trouble_.empty()) {
      const BasicBlock &block = body.blocks[at];
      for (const Statement &s : block.statements) {
        if (++steps_ > kBudget) {
          trouble_ = "the program ran longer than this engine will wait";
          break;
        }
        if (s.kind == StatementKind::Drop) {
          if (s.conditional && !truthOf(frames_.back().locals[s.flag]))
            continue;
          endValue(frames_.back().locals[s.place]);
          continue;
        }
        Value produced = evaluate(s.value);
        if (!trouble_.empty())
          break;
        put(s.place, std::move(produced));
      }
      if (!trouble_.empty())
        break;

      const Terminator &end = block.terminator;
      if (end.kind == TerminatorKind::Goto) {
        at = end.targets.empty() ? static_cast<unsigned>(body.blocks.size()) : end.targets[0];
        continue;
      }
      if (end.kind == TerminatorKind::Switch) {
        Value condition = read(end.condition);
        const std::string seen =
            behind(condition) && behind(condition)->kind == Value::Kind::Number
                ? (truthOf(*behind(condition)) ? "true" : "false")
                : spelled(condition);
        endValue(condition);
        unsigned chosen = end.targets.empty() ? 0 : end.targets.back();
        for (unsigned i = 0; i < end.values.size() && i < end.targets.size(); ++i)
          if (end.values[i] == seen)
            chosen = end.targets[i];
        at = chosen;
        continue;
      }
      // Return
      if (end.answers)
        answer = read(end.answer);
      break;
    }

    // Whatever the frame still holds ends with it.
    for (Value &value : frames_.back().locals)
      endValue(value);
    frames_.pop_back();
  }
};

} // namespace

InterpretResult interpret(const Mir &mir) { return Machine(mir).run(); }

} // namespace xag
