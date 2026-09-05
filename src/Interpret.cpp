#include "xag/Interpret.h"

#include "xag/Check.h"

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
  enum class Kind { Nothing, Number, Real, Wide, Deci, Text, Loan, Many } kind = Kind::Nothing;
  XagInt number = 0;
  double real = 0;     // a `bin` up to 64 bits, cut to its width
  XagBin128 wide = 0;  // a `bin128` or a `deci`, as its bits
  XagStr text{nullptr, 0, 0};
  bool owns = false;
  unsigned frame = 0; // Loan: which frame the lent slot lives in
  unsigned slot = 0;  //       and which slot it is
  // Many: the places, held the same way text is — one owner frees them, and
  // everything reading them holds a view with no claim.
  std::vector<Value> *places = nullptr;
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
    if (value.kind == Value::Kind::Many && value.owns && value.places) {
      for (Value &held : *value.places)
        endValue(held);
      delete value.places;
      xag_note_given();
    }
    value = Value{};
  }

  // A `many` with a place for every value handed in, owning what sits in them.
  Value collected(std::vector<Value> held) {
    Value made;
    made.kind = Value::Kind::Many;
    made.places = new std::vector<Value>(std::move(held));
    made.owns = true;
    xag_note_taken();
    return made;
  }

  // Which place an index names, asked of the runtime so that every engine
  // answers the same way — including by stopping in the same place.
  bool placeOf(const Value &array, const Value &index, uint64_t &at) {
    const uint64_t length = array.places ? array.places->size() : 0;
    if (length == 0 || (!mir_.settings.wrapsOutOfRange &&
                        (index.number < 0 ||
                         static_cast<uint64_t>(index.number) >= length))) {
      // The runtime says so and stops, which is the whole point of asking it.
      xag_many_place(static_cast<int64_t>(index.number), length,
                     mir_.settings.wrapsOutOfRange ? 1 : 0);
      return false;
    }
    at = xag_many_place(static_cast<int64_t>(index.number), length,
                        mir_.settings.wrapsOutOfRange ? 1 : 0);
    return true;
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
    case Value::Kind::Number: {
      // Only for comparing text, where the number never reaches.
      return std::to_string(static_cast<long long>(at->number));
    }
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
      const Type named = typeNamed(type);
      if (isWhole(named)) {
        value.kind = Value::Kind::Number;
        value.number = readWhole(operand.written, named);
      } else if (isDecimal(named)) {
        value.kind = Value::Kind::Deci;
        XagDeci read = 0;
        xag_deci_reads(widthOf(named), operand.written.data(), operand.written.size(),
                       &read);
        value.wide = read;
      } else if (named == Type::Bin128) {
        value.kind = Value::Kind::Wide;
        XagBin128 read = 0;
        xag_bin128_reads(operand.written.data(), operand.written.size(), &read);
        value.wide = read;
      } else if (isBinary(named)) {
        value.kind = Value::Kind::Real;
        double read = 0;
        xag_bin_reads(operand.written.data(), operand.written.size(), widthOf(named),
                      &read);
        value.real = read;
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

  // A written whole number, cut to what its type holds.
  static XagInt readWhole(const std::string &text, Type type) {
    const bool negative = !text.empty() && text[0] == '-';
    __uint128_t magnitude = 0;
    for (unsigned i = negative ? 1 : 0; i < text.size(); ++i)
      magnitude = magnitude * 10 + static_cast<unsigned>(text[i] - '0');
    const XagInt value =
        negative ? -static_cast<XagInt>(magnitude) : static_cast<XagInt>(magnitude);
    return xag_int_fit(value, widthOf(type), isSigned(type) ? 1 : 0);
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

    case RValueKind::Collect: {
      std::vector<Value> held;
      for (const Operand &operand : value.operands) {
        Value piece = read(operand);
        // What goes into a place belongs to the array now, and a view of
        // somebody else's bytes is not a thing to keep.
        if (piece.kind == Value::Kind::Text && !piece.owns) {
          XagStr copy{nullptr, 0, 0};
          xag_str_from(&copy, piece.text.bytes, piece.text.length);
          piece.text = copy;
          piece.owns = true;
        }
        held.push_back(piece);
      }
      return collected(std::move(held));
    }

    case RValueKind::Fill: {
      Value what = read(value.operands[0]);
      Value howMany = read(value.operands[1]);
      Value *count = behind(howMany);
      Value *one = behind(what);
      const long long places = count ? static_cast<long long>(count->number) : 0;
      std::vector<Value> held;
      for (long long i = 0; i < places && i < 100000000LL; ++i)
        held.push_back(one ? viewOf(*one) : Value{});
      endValue(what);
      endValue(howMany);
      return collected(std::move(held));
    }

    case RValueKind::Element: {
      Value array = read(value.operands[0]);
      Value where = read(value.operands[1]);
      Value *at = behind(array);
      Value *index = behind(where);
      Value answer;
      uint64_t place = 0;
      if (at && index && at->kind == Value::Kind::Many &&
          placeOf(*at, *index, place))
        answer = viewOf((*at->places)[place]);
      endValue(array);
      endValue(where);
      return answer;
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

    if (a && b && a->kind == Value::Kind::Deci) {
      const Type given = typeNamed(typeOf(value.operands[0].type));
      const uint32_t width = widthOf(given);
      const XagDeci x = a->wide, y = b->wide;
      if (op == "+" || op == "-" || op == "x" || op == "/" || op == "mod" ||
          op == "^") {
        answer.kind = Value::Kind::Deci;
        answer.wide = op == "+"     ? xag_deci_add(width, x, y)
                      : op == "-"   ? xag_deci_sub(width, x, y)
                      : op == "x"   ? xag_deci_mul(width, x, y)
                      : op == "/"   ? xag_deci_div(width, x, y)
                      : op == "mod" ? xag_deci_mod(width, x, y)
                                    : xag_deci_pow(width, x, y);
      } else {
        const int32_t order = xag_deci_compare(width, x, y);
        const bool ordered = order != -3;
        if (op == "==") answer.number = ordered && order == 0;
        else if (op == "!==") answer.number = !ordered || order != 0;
        else if (op == "<") answer.number = ordered && order < 0;
        else if (op == ">") answer.number = ordered && order > 0;
        else if (op == "<==") answer.number = ordered && order <= 0;
        else if (op == ">==") answer.number = ordered && order >= 0;
        else trouble_ = "`" + op + "` was asked of a `deci`";
      }
    } else if (a && b && a->kind == Value::Kind::Wide) {
      const XagBin128 x = a->wide, y = b->wide;
      if (op == "+" || op == "-" || op == "x" || op == "/" || op == "mod" ||
          op == "^") {
        answer.kind = Value::Kind::Wide;
        answer.wide = op == "+"     ? xag_bin128_add(x, y)
                      : op == "-"   ? xag_bin128_sub(x, y)
                      : op == "x"   ? xag_bin128_mul(x, y)
                      : op == "/"   ? xag_bin128_div(x, y)
                      : op == "mod" ? xag_bin128_mod(x, y)
                                    : xag_bin128_pow(x, y);
      } else {
        // -3 says the two cannot be ordered, which only `!==` answers true to.
        const int32_t order = xag_bin128_compare(x, y);
        const bool ordered = order != -3;
        if (op == "==") answer.number = ordered && order == 0;
        else if (op == "!==") answer.number = !ordered || order != 0;
        else if (op == "<") answer.number = ordered && order < 0;
        else if (op == ">") answer.number = ordered && order > 0;
        else if (op == "<==") answer.number = ordered && order <= 0;
        else if (op == ">==") answer.number = ordered && order >= 0;
        else trouble_ = "`" + op + "` was asked of a `bin128`";
      }
    } else if (a && b && a->kind == Value::Kind::Real) {
      // IEEE all the way down: dividing by zero is infinity, and a
      // not-a-number compares equal to nothing at all, itself included.
      const Type made = typeNamed(typeOf(value.type));
      const unsigned width = widthOf(isBinary(made) ? made
                                                    : typeNamed(typeOf(value.operands[0].type)));
      const double x = a->real, y = b->real;
      if (op == "+" || op == "-" || op == "x" || op == "/" || op == "mod" || op == "^") {
        answer.kind = Value::Kind::Real;
        if (op == "+") answer.real = xag_bin_fit(x + y, width);
        else if (op == "-") answer.real = xag_bin_fit(x - y, width);
        else if (op == "x") answer.real = xag_bin_fit(x * y, width);
        else if (op == "/") answer.real = xag_bin_fit(x / y, width);
        else if (op == "mod") answer.real = xag_bin_mod(x, y, width);
        else answer.real = xag_bin_pow(x, y, width);
      } else if (op == "==") answer.number = x == y;
      else if (op == "!==") answer.number = x != y;
      else if (op == "<") answer.number = x < y;
      else if (op == ">") answer.number = x > y;
      else if (op == "<==") answer.number = x <= y;
      else if (op == ">==") answer.number = x >= y;
      else trouble_ = "`" + op + "` was asked of a `bin`";
    } else if (a && b && a->kind == Value::Kind::Text) {
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
      const XagInt x = a->number, y = b->number;
      // A comparison answers a `bool`, so what it was *given* decides how the
      // two sides are read; everything else answers with its own type.
      const Type given = typeNamed(typeOf(value.operands[0].type));
      const Type made = typeNamed(typeOf(value.type));
      const Type arithmetic = isWhole(made) ? made : given;
      const unsigned width = widthOf(arithmetic);
      const int sign = isSigned(arithmetic) ? 1 : 0;
      const bool unsignedCompare = isWhole(given) && !isSigned(given);
      const __uint128_t ux = static_cast<__uint128_t>(x), uy = static_cast<__uint128_t>(y);

      if (op == "+")
        answer.number = xag_int_fit(static_cast<XagInt>(ux + uy), width, sign);
      else if (op == "-")
        answer.number = xag_int_fit(static_cast<XagInt>(ux - uy), width, sign);
      else if (op == "x")
        answer.number = xag_int_fit(static_cast<XagInt>(ux * uy), width, sign);
      else if (op == "/") answer.number = xag_int_div(x, y, width, sign);
      else if (op == "mod") answer.number = xag_int_mod(x, y, width, sign);
      else if (op == "^") answer.number = xag_int_pow(x, y, width, sign);
      else if (op == "and") answer.number = (x != 0) && (y != 0);
      else if (op == "or") answer.number = (x != 0) || (y != 0);
      else if (op == "==") answer.number = x == y;
      else if (op == "!==") answer.number = x != y;
      else if (op == "<") answer.number = unsignedCompare ? ux < uy : x < y;
      else if (op == ">") answer.number = unsignedCompare ? ux > uy : x > y;
      else if (op == "<==") answer.number = unsignedCompare ? ux <= uy : x <= y;
      else if (op == ">==") answer.number = unsignedCompare ? ux >= uy : x >= y;
      else trouble_ = "`" + op + "` is not an operator this engine knows";
    }

    endValue(left);
    endValue(right);
    return answer;
  }

  // `xs[at] = value` — the place keeps what it is given, and what was there
  // before ends here rather than being left behind.
  void store(const Statement &s) {
    Value where = read(s.at);
    Value produced = evaluate(s.value);
    Value *array = behind(frames_.back().locals[s.place]);
    Value *index = behind(where);
    uint64_t place = 0;
    if (array && index && array->kind == Value::Kind::Many &&
        placeOf(*array, *index, place)) {
      if (produced.kind == Value::Kind::Text && !produced.owns) {
        XagStr copy{nullptr, 0, 0};
        xag_str_from(&copy, produced.text.bytes, produced.text.length);
        produced.text = copy;
        produced.owns = true;
      }
      endValue((*array->places)[place]);
      (*array->places)[place] = std::move(produced);
    } else {
      endValue(produced);
    }
    endValue(where);
  }

  Value callByName(const RValue &value) {
    if (value.callee == "print.stdout") {
      for (const Operand &operand : value.operands) {
        Value piece = read(operand);
        Value *at = behind(piece);
        if (at && at->kind == Value::Kind::Text)
          xag_print(&at->text);
        else if (at && at->kind == Value::Kind::Deci)
          xag_print_deci(widthOf(typeNamed(typeOf(operand.type))), at->wide);
        else if (at && at->kind == Value::Kind::Wide)
          xag_print_bin128(at->wide);
        else if (at && at->kind == Value::Kind::Real)
          xag_print_bin(at->real, widthOf(typeNamed(typeOf(operand.type))));
        else if (at && at->kind == Value::Kind::Number) {
          const Type named = typeNamed(typeOf(operand.type));
          if (isWhole(named))
            xag_print_int(at->number, widthOf(named), isSigned(named) ? 1 : 0);
          else
            xag_print_bool(at->number != 0);
        }
        endValue(piece);
      }
      return Value{};
    }

    if (value.callee == "count") {
      Value piece = value.operands.empty() ? Value{} : read(value.operands[0]);
      Value *at = behind(piece);
      Value answer;
      answer.kind = Value::Kind::Number;
      answer.number = !at ? 0
                      : at->kind == Value::Kind::Text ? xag_str_count(&at->text)
                      : at->kind == Value::Kind::Many
                          ? static_cast<XagInt>(at->places ? at->places->size() : 0)
                          : 0;
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
        if (s.kind == StatementKind::Store) {
          store(s);
          if (!trouble_.empty())
            break;
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
