#include "xag/Fold.h"

#include "xag/Check.h"
#include "xag_runtime.h"

#include <string>
#include <unordered_map>

namespace xag {
namespace {

std::string spelledOut(XagInt value) {
  if (value == 0)
    return "0";
  const bool negative = value < 0;
  __uint128_t magnitude =
      negative ? -static_cast<__uint128_t>(value) : static_cast<__uint128_t>(value);
  std::string digits;
  while (magnitude > 0) {
    digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(magnitude % 10)));
    magnitude /= 10;
  }
  return negative ? "-" + digits : digits;
}

bool looksWhole(const std::string &text) {
  if (text.empty() || text == "-")
    return false;
  for (unsigned i = text[0] == '-' ? 1 : 0; i < text.size(); ++i)
    if (text[i] < '0' || text[i] > '9')
      return false;
  return true;
}

// Read the way the test interpreter reads one, so that what is folded here is
// what would have been computed there.
XagInt wholeFrom(const std::string &text, Type type) {
  const bool negative = !text.empty() && text[0] == '-';
  __uint128_t magnitude = 0;
  for (unsigned i = negative ? 1 : 0; i < text.size(); ++i)
    magnitude = magnitude * 10 + static_cast<unsigned>(text[i] - '0');
  const XagInt value =
      negative ? -static_cast<XagInt>(magnitude) : static_cast<XagInt>(magnitude);
  return xag_int_fit(value, widthOf(type), isSigned(type) ? 1 : 0);
}

class Folder {
public:
  Folder(const Source &source, Mir &mir) : source_(source), mir_(mir) {}

  FoldResult run() {
    for (Body &body : mir_.bodies) {
      body_ = &body;
      // Folding one step at a time reaches one step: `*8* x *7*` becomes `*56*`
      // and the `+ *1*` beside it still sees a name. Carrying what is known
      // forward and going round again is what makes it reach the end.
      for (unsigned again = 0; again < kRounds; ++again) {
        lengths_.clear();
        countAssignments();
        const std::size_t before = result_.diagnostics.size();
        bool moved = false;
        for (BasicBlock &block : body.blocks) {
          // Only within one block. Statements in a block run in the order they
          // are written, and blocks do not: a value known at the end of one is
          // not known at the top of the next when an edge comes back to it.
          // Carrying it across anyway froze a loop's counter at the number it
          // started on, and the loop then never ended.
          known_.clear();
          for (Statement &s : block.statements)
            moved = statement(s) || moved;
        }
        if (!moved || result_.diagnostics.size() != before)
          break;
      }
    }
    return std::move(result_);
  }

  // Enough to walk a chain of written arithmetic to its end. A number that
  // needed more rounds than this was not written down in the first place.
  static constexpr unsigned kRounds = 8;

private:
  const Source &source_;
  Mir &mir_;
  Body *body_ = nullptr;
  // How many places a `many` has, where the program said so where it was made.
  std::unordered_map<unsigned, std::int64_t> lengths_;
  // What a local is known to hold, when it is written once and that once is a
  // value written down. Anything assigned twice is not known: which of them
  // reached here is what the graph is for, and this pass does not walk it.
  std::unordered_map<unsigned, std::string> known_;
  std::unordered_map<unsigned, unsigned> assignments_;
  FoldResult result_;

  void complain(Span span, std::string code, std::string message,
                std::vector<std::string> rules, std::vector<std::string> tips) {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             "here", std::move(rules), std::move(tips),
                                             {}, Severity::Error});
  }

  const std::string &nameOf(TypeRef type) const {
    static const std::string unknown = "?";
    return type.index < body_->types.size() ? body_->types[type.index] : unknown;
  }

  static bool written(const Operand &operand) {
    return operand.kind == OperandKind::Written;
  }

  void countAssignments() {
    assignments_.clear();
    for (const BasicBlock &block : body_->blocks)
      for (const Statement &s : block.statements)
        if (s.kind == StatementKind::Assign)
          ++assignments_[s.place];
  }

  // Only what copies, and only whole numbers: putting a written `str` where a
  // name was read would hand the same text over twice.
  bool worthKnowing(unsigned local) const {
    if (local >= body_->locals.size())
      return false;
    const TypeRef type = body_->locals[local].type;
    const std::string &spelled =
        type.index < body_->types.size() ? body_->types[type.index] : std::string();
    const Type named = typeNamed(spelled);
    return isWhole(named);
  }

  bool statement(Statement &s) {
    // Writing a place of a `many`, or letting a value go, says nothing good
    // about what the name holds afterwards.
    if (s.kind != StatementKind::Assign) {
      known_.erase(s.place);
      return false;
    }
    RValue &value = s.value;
    bool moved = false;

    // What is known stands in for the name that held it.
    for (Operand &operand : value.operands)
      if (operand.kind == OperandKind::Copy) {
        const auto found = known_.find(operand.local);
        if (found != known_.end()) {
          operand = Operand{OperandKind::Written, 0, found->second, operand.type};
          moved = true;
        }
      }

    // How many places a `many` has, taken where it was made rather than worked
    // out later: the items are right there. A length outlives its block because
    // a `many` is a fixed length once made — but only when nothing else is ever
    // assigned to that name.
    if (value.kind == RValueKind::Collect && assignments_[s.place] == 1)
      lengths_[s.place] = static_cast<std::int64_t>(value.operands.size());

    if (value.kind == RValueKind::Element)
      element(s);
    if (value.kind == RValueKind::Binary)
      moved = binary(s) || moved;

    // What this place holds now, or nothing known about it any more.
    if (value.kind == RValueKind::Use && !value.operands.empty() &&
        written(value.operands[0]) && worthKnowing(s.place))
      known_[s.place] = value.operands[0].written;
    else
      known_.erase(s.place);
    return moved;
  }

  // A place asked for by a number, of a `many` whose length was written down.
  void element(Statement &s) {
    const RValue &value = s.value;
    if (value.operands.size() != 2 || !written(value.operands[1]))
      return;
    const auto found = lengths_.find(value.operands[0].local);
    if (found == lengths_.end() || !looksWhole(value.operands[1].written))
      return;
    const XagInt at = wholeFrom(value.operands[1].written, Type::Int64);
    if (at >= 0 && at < found->second)
      return;
    complain(s.span, "E0532",
             "place " + spelledOut(at) + " was asked for, and this `many` has " +
                 std::to_string(found->second) + ".",
             {"a `many` holds the number of places it was made with"},
             {"both the place and the length are written down, so this stops every "
              "time it is reached rather than only sometimes."});
  }

  bool binary(Statement &s) {
    RValue &value = s.value;
    if (value.operands.size() != 2 || !written(value.operands[0]) ||
        !written(value.operands[1]))
      return false;
    const Type given = typeNamed(nameOf(value.operands[0].type));
    const Type made = typeNamed(nameOf(value.type));
    if (!isWhole(given) || !looksWhole(value.operands[0].written) ||
        !looksWhole(value.operands[1].written))
      return false;

    const Type arithmetic = isWhole(made) ? made : given;
    const unsigned width = widthOf(arithmetic);
    const int sign = isSigned(arithmetic) ? 1 : 0;
    const XagInt x = wholeFrom(value.operands[0].written, arithmetic);
    const XagInt y = wholeFrom(value.operands[1].written, arithmetic);
    const std::string &op = value.op;

    // Dividing by a written zero stops every time it is reached. The compiler
    // has both numbers in front of it, so it is certain rather than suspected.
    if ((op == "/" || op == "mod") && y == 0) {
      complain(s.span, "E0533",
               op == "/" ? "this divides by zero." : "this takes a remainder against zero.",
               {"a number divided by zero has no answer to give"},
               {"both numbers are written down, so this stops every time it is "
                "reached rather than only sometimes."});
      return false;
    }
    if (op == "^" && sign == 1 && y < 0)
      return false; // the runtime stops on this; leave it to say so

    const auto ux = static_cast<__uint128_t>(x);
    const auto uy = static_cast<__uint128_t>(y);
    std::string folded;
    if (op == "+")
      folded = spelledOut(xag_int_fit(static_cast<XagInt>(ux + uy), width, sign));
    else if (op == "-")
      folded = spelledOut(xag_int_fit(static_cast<XagInt>(ux - uy), width, sign));
    else if (op == "x")
      folded = spelledOut(xag_int_fit(static_cast<XagInt>(ux * uy), width, sign));
    else if (op == "/")
      folded = spelledOut(xag_int_div(x, y, width, sign));
    else if (op == "mod")
      folded = spelledOut(xag_int_mod(x, y, width, sign));
    else if (op == "^")
      folded = spelledOut(xag_int_pow(x, y, width, sign));
    else
      return false; // a comparison answers a `bool`, and is left alone for now

    Operand answer{OperandKind::Written, 0, folded, value.type};
    value = RValue{RValueKind::Use, {}, {}, 0, {std::move(answer)}, value.type};
    return true;
  }
};

} // namespace

FoldResult fold(const Source &source, Mir &mir) { return Folder(source, mir).run(); }

} // namespace xag
