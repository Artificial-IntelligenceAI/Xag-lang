#include "xag/Mir.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace xag {
namespace {

const char *spell(Type type) {
  switch (type) {
  case Type::I64:     return "i64";
  case Type::Str:     return "str";
  case Type::Bool:    return "bool";
  case Type::Nothing: return "nothing";
  case Type::Unknown: return "?";
  }
  return "?";
}

bool copies(Type type) { return type == Type::I64 || type == Type::Bool; }

class Builder {
public:
  Builder(const Program &program, const CheckResult &checked)
      : program_(program), checked_(checked) {}

  MirResult run() {
    for (const Item &item : program_.items) {
      if (item.kind == ItemKind::Const)
        continue;
      body_ = Body{};
      body_.name = item.kind == ItemKind::Start ? "START" : item.name;
      scopes_.clear();
      loops_.clear();
      names_.clear();

      const Type result =
          item.kind == ItemKind::Function ? lookupItem(item) : Type::Nothing;
      body_.result = typeRef(spell(result));
      // Local 0 is the answer.
      addLocal("", body_.result, copies(result));

      openScope();
      for (const Param &param : item.params) {
        const std::string type = chainType(param.chain);
        const unsigned local = addLocal(param.name, typeRef(type), copiesNamed(type));
        names_.back()[param.name] = local;
        ++body_.parameters;
        // A parameter taken by value belongs to the callee, and ends with it.
        if (!copiesNamed(type) && type.rfind("ref", 0) != 0)
          scopes_.back().push_back(local);
      }

      current_ = addBlock();
      for (const StmtPtr &s : item.body.stmts)
        statement(*s);
      closeScope();
      finish(Terminator{TerminatorKind::Return, item.span, {}, {}, {}, false, {}});

      result_.mir.bodies.push_back(std::move(body_));
    }
    return std::move(result_);
  }

private:
  const Program &program_;
  const CheckResult &checked_;
  MirResult result_;

  Body body_;
  unsigned current_ = 0;
  // Locals declared in each open scope, innermost last, dropped in reverse.
  std::vector<std::vector<unsigned>> scopes_;
  std::vector<std::unordered_map<std::string, unsigned>> names_;
  struct Loop {
    unsigned again = 0; // where a pass restarts
    unsigned after = 0; // where `break` goes
  };
  std::vector<Loop> loops_;

  // ---- small pieces

  Type lookupItem(const Item &item) const {
    auto found = checked_.items.find(&item);
    return found == checked_.items.end() ? Type::Nothing : found->second;
  }

  Type declaredType(const Stmt &s) const {
    auto found = checked_.declarations.find(&s);
    return found == checked_.declarations.end() ? Type::Unknown : found->second;
  }

  static bool copiesNamed(const std::string &type) {
    return type == "i64" || type == "bool";
  }

  // A parameter's type is written on its chain, loan and all: `ref str`.
  static std::string chainType(const Chain &chain) {
    std::string mode;
    for (const ChainSegment &seg : chain.segments) {
      if (seg.isName)
        continue;
      if (seg.text == "ref" || seg.text == "refmut")
        mode = seg.text + " ";
    }
    return mode + (chain.segments.empty() ? "?" : chain.type().text);
  }

  TypeRef typeRef(const std::string &name) {
    for (unsigned i = 0; i < body_.types.size(); ++i)
      if (body_.types[i] == name)
        return TypeRef{i};
    body_.types.push_back(name);
    return TypeRef{static_cast<unsigned>(body_.types.size() - 1)};
  }

  unsigned addLocal(const std::string &name, TypeRef type, bool copyable) {
    const unsigned id = static_cast<unsigned>(body_.locals.size());
    body_.locals.push_back(Local{id, type, name, copyable});
    return id;
  }

  unsigned temporary(TypeRef type, bool copyable) { return addLocal("", type, copyable); }

  unsigned addBlock() {
    const unsigned id = static_cast<unsigned>(body_.blocks.size());
    body_.blocks.push_back(BasicBlock{id, {}, Terminator{}});
    return id;
  }

  void emit(Statement s) { body_.blocks[current_].statements.push_back(std::move(s)); }
  void finish(Terminator t) { body_.blocks[current_].terminator = std::move(t); }

  unsigned *findName(const std::string &name) {
    for (auto scope = names_.rbegin(); scope != names_.rend(); ++scope) {
      auto found = scope->find(name);
      if (found != scope->end())
        return &found->second;
    }
    return nullptr;
  }

  // Everything a scope owns ends when the scope does, in the reverse of the
  // order it was taken on.
  void dropScope() {
    if (scopes_.empty())
      return;
    const std::vector<unsigned> &owned = scopes_.back();
    for (auto local = owned.rbegin(); local != owned.rend(); ++local)
      emit(Statement{StatementKind::Drop, Span{}, *local, RValue{}});
  }

  // ---- expressions

  Operand operandOf(const Expr &e) {
    switch (e.kind) {
    case ExprKind::Name: {
      const unsigned *local = findName(e.text);
      if (!local)
        return Operand{OperandKind::Written, 0, e.text, typeRef("?")};
      const Local &slot = body_.locals[*local];
      return Operand{slot.copies ? OperandKind::Copy : OperandKind::Move, *local, {},
                     slot.type};
    }
    case ExprKind::Written:
      return Operand{OperandKind::Written, 0, e.text, typeRef(spell(checked_.of(&e)))};
    case ExprKind::Escape:
      return Operand{OperandKind::Written, 0, "\\" + e.text, typeRef("str")};
    default:
      break;
    }
    const unsigned into = lower(e);
    const Local &slot = body_.locals[into];
    return Operand{slot.copies ? OperandKind::Copy : OperandKind::Move, into, {}, slot.type};
  }

  // Lower an expression into a local and answer which one holds it.
  unsigned lower(const Expr &e) {
    const Type type = checked_.of(&e);
    switch (e.kind) {
    case ExprKind::Name:
      if (const unsigned *local = findName(e.text))
        return *local;
      [[fallthrough]];
    case ExprKind::Written:
    case ExprKind::Escape: {
      const unsigned into = temporary(typeRef(spell(type)), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into,
                     RValue{RValueKind::Use, {}, {}, 0, {operandOf(e)}, typeRef(spell(type))}});
      return into;
    }

    case ExprKind::Typed:
    case ExprKind::Group:
      return e.children.empty() ? temporary(typeRef("?"), true) : lower(*e.children[0]);

    case ExprKind::Borrow: {
      if (e.children.empty())
        return temporary(typeRef("?"), true);
      if (e.text == "move")
        return lower(*e.children[0]);
      const unsigned of = lower(*e.children[0]);
      const std::string name = e.text + " " + body_.types[body_.locals[of].type.index];
      const unsigned into = temporary(typeRef(name), true);
      emit(Statement{StatementKind::Assign, e.span, into,
                     RValue{RValueKind::Ref, e.text, {}, of, {}, typeRef(name)}});
      return into;
    }

    case ExprKind::Unary: {
      const unsigned into = temporary(typeRef(spell(type)), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into,
                     RValue{RValueKind::Unary, e.text, {}, 0,
                            {operandOf(*e.children[0])}, typeRef(spell(type))}});
      return into;
    }

    case ExprKind::Binary: {
      Operand left = operandOf(*e.children[0]);
      Operand right = operandOf(*e.children[1]);
      const unsigned into = temporary(typeRef(spell(type)), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into,
                     RValue{RValueKind::Binary, e.text, {}, 0,
                            {std::move(left), std::move(right)}, typeRef(spell(type))}});
      return into;
    }

    case ExprKind::Call: {
      std::string callee;
      for (const std::string &part : e.path)
        callee += (callee.empty() ? "" : ".") + part;
      std::vector<Operand> arguments;
      for (const Value &value : e.args.values)
        arguments.push_back(valueOperand(value));
      const unsigned into = temporary(typeRef(spell(type)), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into,
                     RValue{RValueKind::Call, {}, callee, 0, std::move(arguments),
                            typeRef(spell(type))}});
      return into;
    }
    }
    return temporary(typeRef("?"), true);
  }

  // One value, which is one item or several joined into a new one.
  Operand valueOperand(const Value &value) {
    if (value.items.empty())
      return Operand{OperandKind::Written, 0, "", typeRef("nothing")};
    if (value.items.size() == 1)
      return operandOf(*value.items[0]);

    std::vector<Operand> pieces;
    for (const ExprPtr &item : value.items)
      pieces.push_back(operandOf(*item));
    const unsigned into = temporary(typeRef("str"), false);
    emit(Statement{StatementKind::Assign, value.span, into,
                   RValue{RValueKind::Join, {}, {}, 0, std::move(pieces), typeRef("str")}});
    return Operand{OperandKind::Move, into, {}, typeRef("str")};
  }

  void assignInto(unsigned place, const ValueList &list, Span span) {
    if (list.values.empty())
      return;
    Operand operand = valueOperand(list.values[0]);
    const TypeRef type = operand.type;
    emit(Statement{StatementKind::Assign, span, place,
                   RValue{RValueKind::Use, {}, {}, 0, {std::move(operand)}, type}});
  }

  // ---- statements

  void statement(const Stmt &s) {
    switch (s.kind) {
    case StmtKind::Declare: {
      const std::string spelled = chainType(s.chain);
      const unsigned local = addLocal(s.name, typeRef(spelled), copiesNamed(spelled));
      assignInto(local, s.value, s.span);
      names_.back()[s.name] = local;
      if (!body_.locals[local].copies && spelled.rfind("ref", 0) != 0)
        scopes_.back().push_back(local);
      break;
    }

    case StmtKind::Set: {
      const unsigned *local = findName(s.name);
      if (!local)
        break;
      for (const ExprPtr &index : s.index)
        (void)lower(*index);
      assignInto(*local, s.value, s.span);
      break;
    }

    case StmtKind::If: {
      const unsigned after = addBlock();
      for (const Branch &branch : s.branches) {
        if (!branch.condition) {
          openScope();
          for (const StmtPtr &inner : branch.body.stmts)
            statement(*inner);
          closeScope();
          finish(Terminator{TerminatorKind::Goto, branch.span, {}, {}, {after}, false, {}});
          current_ = after;
          return;
        }
        const Operand condition = operandOf(*branch.condition);
        const unsigned taken = addBlock();
        const unsigned otherwise = addBlock();
        finish(Terminator{TerminatorKind::Switch, branch.span, condition,
                          {"true"}, {taken, otherwise}, false, {}});
        current_ = taken;
        openScope();
        for (const StmtPtr &inner : branch.body.stmts)
          statement(*inner);
        closeScope();
        finish(Terminator{TerminatorKind::Goto, branch.span, {}, {}, {after}, false, {}});
        current_ = otherwise;
      }
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {after}, false, {}});
      current_ = after;
      break;
    }

    case StmtKind::LoopRange: {
      const Type type = declaredType(s);
      const unsigned counter = addLocal(s.name, typeRef(spell(type)), copies(type));
      const unsigned last = temporary(typeRef(spell(type)), true);
      if (s.value.values.size() == 2) {
        assignOne(counter, s.value.values[0], s.span);
        assignOne(last, s.value.values[1], s.span);
      }

      const unsigned header = addBlock();
      const unsigned inside = addBlock();
      const unsigned after = addBlock();
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});

      current_ = header;
      const unsigned more = temporary(typeRef("bool"), true);
      emit(Statement{StatementKind::Assign, s.span, more,
                     RValue{RValueKind::Binary, "<==", {}, 0,
                            {Operand{OperandKind::Copy, counter, {}, body_.locals[counter].type},
                             Operand{OperandKind::Copy, last, {}, body_.locals[last].type}},
                            typeRef("bool")}});
      finish(Terminator{TerminatorKind::Switch, s.span,
                        Operand{OperandKind::Copy, more, {}, typeRef("bool")},
                        {"true"}, {inside, after}, false, {}});

      current_ = inside;
      loops_.push_back(Loop{header, after});
      openScope();
      names_.back()[s.name] = counter;
      for (const StmtPtr &inner : s.body.stmts)
        statement(*inner);
      closeScope();
      loops_.pop_back();
      emit(Statement{StatementKind::Assign, s.span, counter,
                     RValue{RValueKind::Binary, "+", {}, 0,
                            {Operand{OperandKind::Copy, counter, {}, body_.locals[counter].type},
                             Operand{OperandKind::Written, 0, "1", typeRef("i64")}},
                            body_.locals[counter].type}});
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});
      current_ = after;
      break;
    }

    case StmtKind::LoopWhile: {
      const unsigned header = addBlock();
      const unsigned inside = addBlock();
      const unsigned after = addBlock();
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});

      current_ = header;
      const Operand condition =
          s.condition ? operandOf(*s.condition) : Operand{OperandKind::Written, 0, "true", typeRef("bool")};
      finish(Terminator{TerminatorKind::Switch, s.span, condition, {"true"},
                        {inside, after}, false, {}});

      current_ = inside;
      loops_.push_back(Loop{header, after});
      openScope();
      for (const StmtPtr &inner : s.body.stmts)
        statement(*inner);
      closeScope();
      loops_.pop_back();
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});
      current_ = after;
      break;
    }

    case StmtKind::Break:
      if (!loops_.empty()) {
        finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {loops_.back().after},
                          false, {}});
        current_ = addBlock(); // anything after a break is its own unreached block
      }
      break;

    case StmtKind::Give: {
      if (!s.value.values.empty()) {
        Operand answer = valueOperand(s.value.values[0]);
        emit(Statement{StatementKind::Assign, s.span, 0,
                       RValue{RValueKind::Use, {}, {}, 0, {answer}, answer.type}});
        finish(Terminator{TerminatorKind::Return, s.span, {}, {}, {}, true,
                          Operand{OperandKind::Move, 0, {}, body_.result}});
      } else {
        finish(Terminator{TerminatorKind::Return, s.span, {}, {}, {}, false, {}});
      }
      current_ = addBlock();
      break;
    }

    case StmtKind::Call:
      if (s.call)
        (void)lower(*s.call);
      break;
    }
  }

  void assignOne(unsigned place, const Value &value, Span span) {
    Operand operand = valueOperand(value);
    const TypeRef type = operand.type;
    emit(Statement{StatementKind::Assign, span, place,
                   RValue{RValueKind::Use, {}, {}, 0, {std::move(operand)}, type}});
  }

  void openScope() {
    scopes_.emplace_back();
    names_.emplace_back();
  }

  void closeScope() {
    dropScope();
    scopes_.pop_back();
    names_.pop_back();
  }
};

} // namespace

MirResult build(const Source &source, const Program &program, const CheckResult &checked) {
  (void)source; // spans in the IR already carry everything a diagnostic needs
  return Builder(program, checked).run();
}

} // namespace xag
