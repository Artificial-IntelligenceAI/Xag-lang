#include "xag/Mir.h"

#include <ostream>

namespace xag {
namespace {

struct Printer {
  std::ostream &out;
  const Body *body = nullptr;

  std::string typeName(TypeRef type) const {
    return type.index < body->types.size() ? body->types[type.index] : "?";
  }

  std::string local(unsigned id) const {
    if (id < body->locals.size() && !body->locals[id].name.empty())
      return "_" + std::to_string(id) + "'" + body->locals[id].name + "'";
    return "_" + std::to_string(id);
  }

  std::string operand(const Operand &o) const {
    switch (o.kind) {
    case OperandKind::Copy:    return "copy " + local(o.local);
    case OperandKind::Move:    return "move " + local(o.local);
    case OperandKind::Written: return "*" + o.written + "*";
    }
    return "?";
  }

  std::string rvalue(const RValue &v) const {
    std::string text;
    switch (v.kind) {
    case RValueKind::Use:
      return v.operands.empty() ? "()" : operand(v.operands[0]);
    case RValueKind::Binary:
      return operand(v.operands[0]) + " " + v.op + " " + operand(v.operands[1]);
    case RValueKind::Unary:
      return v.op + " " + operand(v.operands[0]);
    case RValueKind::Ref:
      return v.op + " " + local(v.local);
    case RValueKind::Call:
      text = v.callee + "(";
      break;
    case RValueKind::Join:
      text = "join(";
      break;
    case RValueKind::Collect:
      text = "collect(";
      break;
    case RValueKind::Element:
      return operand(v.operands[0]) + "[" + operand(v.operands[1]) + "]";
    case RValueKind::Fill:
      text = "fill(";
      break;
    case RValueKind::Holds:
      return "holds(" + operand(v.operands[0]) + ")";
    case RValueKind::Inside:
      return "inside(" + operand(v.operands[0]) + ")";
    case RValueKind::Part:
      return operand(v.operands[0]) + "." + v.op;
    case RValueKind::Taken:
      return "take " + operand(v.operands[0]) + "." + v.op;
    case RValueKind::Group:
      text = "group(";
      break;
    }
    for (unsigned i = 0; i < v.operands.size(); ++i)
      text += (i ? ", " : "") + operand(v.operands[i]);
    return text + ")";
  }

  void one(const Body &b) {
    body = &b;
    out << "fn " << b.name << " -> " << typeName(b.result) << " {\n";
    for (const Local &slot : b.locals) {
      out << "    let " << local(slot.id) << ": " << typeName(slot.type);
      if (slot.id != 0 && slot.id <= b.parameters)
        out << "   # parameter";
      out << '\n';
    }
    for (const BasicBlock &block : b.blocks) {
      out << "\n  block" << block.id << ":\n";
      for (const Statement &s : block.statements) {
        if (s.kind == StatementKind::Drop)
          out << "    drop " << local(s.place)
              << (s.conditional ? " if " + local(s.flag) : "") << '\n';
        else if (s.kind == StatementKind::Store)
          out << "    " << local(s.place) << "[" << operand(s.at) << "] = "
              << rvalue(s.value) << '\n';
        else {
          out << "    " << local(s.place);
          for (unsigned part : s.parts)
            out << '.' << part;
          out << " = " << rvalue(s.value) << '\n';
        }
      }
      const Terminator &end = block.terminator;
      switch (end.kind) {
      case TerminatorKind::Goto:
        out << "    goto block" << (end.targets.empty() ? 0 : end.targets[0]) << '\n';
        break;
      case TerminatorKind::Switch:
        out << "    switch " << operand(end.condition);
        for (unsigned i = 0; i < end.values.size() && i < end.targets.size(); ++i)
          out << " [" << end.values[i] << " -> block" << end.targets[i] << ']';
        if (end.targets.size() > end.values.size())
          out << " [else -> block" << end.targets.back() << ']';
        out << '\n';
        break;
      case TerminatorKind::Return:
        out << "    return" << (end.answers ? " " + operand(end.answer) : "") << '\n';
        break;
      }
    }
    out << "}\n";
  }
};

} // namespace

void print(const Mir &mir, std::ostream &out) {
  Printer printer{out, nullptr};
  for (unsigned i = 0; i < mir.bodies.size(); ++i) {
    if (i)
      out << '\n';
    printer.one(mir.bodies[i]);
  }
}

} // namespace xag
