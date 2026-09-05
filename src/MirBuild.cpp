#include "xag/Mir.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace xag {
namespace {

// A type as the middle layer holds it: spelled, the way it is written apart
// from the dots. `many int64` and `ref many int64` are read back by prefix, the
// way `ref str` already was.
std::string spell(Ty type) {
  return type.kind == Type::Unknown ? "?" : name(type);
}

// A number is handed over by being copied, however wide it is: there is nothing
// in one to give back.
bool copies(Ty type) { return isNumber(type) || type == Type::Bool; }

// Text and a `many` hold something that has to be given back. `nothing` is not
// a value that copies, but it is not one that owns either.
bool owns(Ty type) { return type.kind == Type::Str || type.holds(); }

// A loan is not a thing to end: it goes back to whoever lent it.
bool isLoanType(const std::string &spelled) {
  return spelled.rfind("ref ", 0) == 0 || spelled.rfind("refmut ", 0) == 0;
}

class Builder {
public:
  Builder(const Program &program, const CheckResult &checked)
      : program_(program), checked_(checked) {}

  MirResult run() {
    // A constant is a body that answers with its value. Naming one is a call,
    // which needs no concept the IR did not already have — and lets a constant
    // be written as an expression rather than only as a literal.
    for (const Item &item : program_.items) {
      if (item.kind == ItemKind::Const)
        consts_[item.name] = chainType(item.chain);
      // The checker's answer for a call is `str` whether the function hands
      // text over or only lends it, so the spelling has to come from the
      // signature. Getting this wrong once made initialising a loan look like
      // writing through one.
      else if (item.kind == ItemKind::Function)
        answers_[item.name] = chainType(item.chain);
    }

    for (const Item &item : program_.items) {
      if (item.kind == ItemKind::Const) {
        body_ = Body{};
        scopes_.clear();
        loops_.clear();
        names_.clear();
        names_.emplace_back();
        body_.name = constBody(item.name);
        const std::string spelled = chainType(item.chain);
        body_.result = typeRef(spelled);
        addLocal("", body_.result, copiesNamed(spelled));
        current_ = addBlock();
        assignInto(0, item.value, item.span);
        finish(Terminator{TerminatorKind::Return, item.span, {}, {}, {}, true,
                          Operand{copiesNamed(spelled) ? OperandKind::Copy
                                                       : OperandKind::Move,
                                  0, {}, body_.result}});
        result_.mir.bodies.push_back(std::move(body_));
        continue;
      }
      body_ = Body{};
      body_.name = item.kind == ItemKind::Start ? "START" : item.name;
      scopes_.clear();
      loops_.clear();
      names_.clear();

      const Ty result =
          item.kind == ItemKind::Function ? lookupItem(item) : Ty{Type::Nothing};
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
  std::unordered_map<std::string, std::string> consts_;
  std::unordered_map<std::string, std::string> answers_;

  static std::string constBody(const std::string &name) { return "const '" + name + "'"; }

  // A name nothing declared may still be a constant, which is a call.
  unsigned callConst(const Expr &e, const std::string &spelled) {
    const unsigned into = temporary(typeRef(spelled), copiesNamed(spelled));
    emit(Statement{StatementKind::Assign, e.span, into, {},
                   RValue{RValueKind::Call, {}, constBody(e.text), 0, {},
                          typeRef(spelled)}});
    return into;
  }

  struct Loop {
    unsigned again = 0; // where a pass restarts
    unsigned after = 0; // where `break` goes
  };
  std::vector<Loop> loops_;

  // ---- small pieces

  Ty lookupItem(const Item &item) const {
    auto found = checked_.items.find(&item);
    return found == checked_.items.end() ? Ty{Type::Nothing} : found->second;
  }

  Ty declaredType(const Stmt &s) const {
    auto found = checked_.declarations.find(&s);
    return found == checked_.declarations.end() ? Ty{} : found->second;
  }

  static bool copiesNamed(const std::string &type) {
    return type == "bool" || (type != "str" && type != "nothing" &&
                              typeNamed(type) != Type::Unknown);
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
    if (chain.segments.empty())
      return mode + "?";
    const std::size_t n = chain.segments.size();
    if (n >= 2 && !chain.segments[n - 2].isName && chain.segments[n - 2].text == "many")
      return mode + "many " + chain.type().text;
    return mode + chain.type().text;
  }

  // What is left of a spelled type once its loan word is off, and what one of
  // its places holds when it is a `many`.
  static std::string withoutLoan(const std::string &spelled) {
    if (spelled.rfind("ref ", 0) == 0)
      return spelled.substr(4);
    if (spelled.rfind("refmut ", 0) == 0)
      return spelled.substr(7);
    return spelled;
  }
  static std::string elementOf(const std::string &spelled) {
    const std::string bare = withoutLoan(spelled);
    return bare.rfind("many ", 0) == 0 ? bare.substr(5) : std::string("?");
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

  // A temporary that owns something is owned by the scope it was made in, and
  // ends there like anything else. If it is moved out first, elaboration sees
  // that and takes the drop away again.
  unsigned owningTemporary(TypeRef type) {
    const unsigned id = addLocal("", type, false);
    if (!scopes_.empty())
      scopes_.back().push_back(id);
    return id;
  }

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
      emit(Statement{StatementKind::Drop, Span{}, *local, {}, RValue{}});
  }

  // ---- expressions

  Operand operandOf(const Expr &e) {
    switch (e.kind) {
    case ExprKind::Name: {
      // Naming something reads it. Taking it is spelled `move`, and arrives as
      // its own node — so joining and printing leave what they read alone.
      const unsigned *local = findName(e.text);
      if (!local) {
        auto constant = consts_.find(e.text);
        if (constant == consts_.end())
          return Operand{OperandKind::Written, 0, e.text, typeRef("?")};
        const unsigned into = callConst(e, constant->second);
        const Local &answered = body_.locals[into];
        return Operand{answered.copies ? OperandKind::Copy : OperandKind::Move, into, {},
                       answered.type};
      }
      const Local &slot = body_.locals[*local];
      return Operand{OperandKind::Copy, *local, {}, slot.type};
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
    const Ty type = checked_.of(&e);
    switch (e.kind) {
    case ExprKind::Name: {
      if (const unsigned *local = findName(e.text))
        return *local;
      auto constant = consts_.find(e.text);
      if (constant != consts_.end())
        return callConst(e, constant->second);
      [[fallthrough]];
    }
    case ExprKind::Written:
    case ExprKind::Escape: {
      // Text written into a temporary is text that temporary owns.
      const unsigned into = owns(type) ? owningTemporary(typeRef(spell(type)))
                                       : temporary(typeRef(spell(type)), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into, {},
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
      emit(Statement{StatementKind::Assign, e.span, into, {},
                     RValue{RValueKind::Ref, e.text, {}, of, {}, typeRef(name)}});
      return into;
    }

    case ExprKind::Unary: {
      const unsigned into = temporary(typeRef(spell(type)), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into, {},
                     RValue{RValueKind::Unary, e.text, {}, 0,
                            {operandOf(*e.children[0])}, typeRef(spell(type))}});
      return into;
    }

    case ExprKind::Binary: {
      Operand left = operandOf(*e.children[0]);
      Operand right = operandOf(*e.children[1]);
      const unsigned into = temporary(typeRef(spell(type)), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into, {},
                     RValue{RValueKind::Binary, e.text, {}, 0,
                            {std::move(left), std::move(right)}, typeRef(spell(type))}});
      return into;
    }

    case ExprKind::Index: {
      // Reading a place gives back what sits in it. When that is something with
      // an owner, what comes back is a loan into the array rather than a copy —
      // there is one of it, and it stays where it is.
      const unsigned *of = findName(e.text);
      if (!of)
        return temporary(typeRef("?"), true);
      const std::string held = elementOf(body_.types[body_.locals[*of].type.index]);
      const bool copiesElement = copiesNamed(held);
      const std::string spelled = copiesElement ? held : "ref " + held;
      const unsigned into = temporary(typeRef(spelled), copiesElement);
      std::vector<Operand> parts;
      parts.push_back(Operand{OperandKind::Copy, *of, {}, body_.locals[*of].type});
      parts.push_back(e.children.empty()
                          ? Operand{OperandKind::Written, 0, "0", typeRef("int64")}
                          : operandOf(*e.children[0]));
      emit(Statement{StatementKind::Assign, e.span, into, {},
                     RValue{RValueKind::Element, {}, {}, 0, std::move(parts),
                            typeRef(spelled)}});
      return into;
    }

    case ExprKind::Call: {
      std::string callee;
      for (const std::string &part : e.path)
        callee += (callee.empty() ? "" : ".") + part;
      if (callee == "fill") {
        const std::string spelled = spell(type);
        std::vector<Operand> parts;
        for (const Value &value : e.args.values)
          parts.push_back(valueOperand(value));
        parts.resize(2);
        const unsigned into = owningTemporary(typeRef(spelled));
        emit(Statement{StatementKind::Assign, e.span, into, {},
                       RValue{RValueKind::Fill, {}, {}, 0, std::move(parts),
                              typeRef(spelled)}});
        return into;
      }
      std::vector<Operand> arguments;
      if (callee == "print.stdout") {
        // Showing is not joining: a print writes one piece after another and
        // builds nothing, so its pieces stay pieces and are never welded into
        // a value first. And it reads them, so they stay where they were.
        for (const Value &value : e.args.values)
          for (const ExprPtr &item : value.items) {
            Operand piece = operandOf(*item);
            if (piece.kind == OperandKind::Move)
              piece.kind = OperandKind::Copy;
            arguments.push_back(std::move(piece));
          }
      } else {
        for (const Value &value : e.args.values)
          arguments.push_back(valueOperand(value));
      }
      auto answered = answers_.find(callee);
      const std::string spelled =
          answered == answers_.end() ? spell(type) : answered->second;
      const unsigned into = (owns(type) && !isLoanType(spelled))
                                ? owningTemporary(typeRef(spelled))
                                : temporary(typeRef(spelled), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into, {},
                     RValue{RValueKind::Call, {}, callee, 0, std::move(arguments),
                            typeRef(spelled)}});
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
    const unsigned into = owningTemporary(typeRef("str"));
    emit(Statement{StatementKind::Assign, value.span, into, {},
                   RValue{RValueKind::Join, {}, {}, 0, std::move(pieces), typeRef("str")}});
    return Operand{OperandKind::Move, into, {}, typeRef("str")};
  }

  void assignInto(unsigned place, const ValueList &list, Span span) {
    const std::string spelled = body_.types[body_.locals[place].type.index];
    if (withoutLoan(spelled).rfind("many ", 0) == 0) {
      collectInto(place, list, span, spelled);
      return;
    }
    if (list.values.empty())
      return;
    Operand operand = valueOperand(list.values[0]);
    const TypeRef type = operand.type;
    emit(Statement{StatementKind::Assign, span, place, {},
                   RValue{RValueKind::Use, {}, {}, 0, {std::move(operand)}, type}});
  }

  // Items side by side under a `many` stay several. A lone item that is already
  // the whole array is the whole array — which is what the checker settled, so
  // nothing here has to settle it again.
  void collectInto(unsigned place, const ValueList &list, Span span,
                   std::string spelled) {
    std::vector<Operand> parts;
    if (!list.values.empty()) {
      const Value &v = list.values[0];
      if (v.items.size() == 1 && checked_.of(v.items[0].get()).holds()) {
        Operand operand = operandOf(*v.items[0]);
        const TypeRef type = operand.type;
        emit(Statement{StatementKind::Assign, span, place, {},
                       RValue{RValueKind::Use, {}, {}, 0, {std::move(operand)}, type}});
        return;
      }
      for (const ExprPtr &item : v.items)
        parts.push_back(operandOf(*item));
    }
    emit(Statement{StatementKind::Assign, span, place, {},
                   RValue{RValueKind::Collect, {}, {}, 0, std::move(parts),
                          typeRef(spelled)}});
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
      if (s.index) {
        const std::string held =
            elementOf(body_.types[body_.locals[*local].type.index]);
        Operand at = operandOf(*s.index);
        Operand value = s.value.values.empty()
                            ? Operand{OperandKind::Written, 0, "", typeRef(held)}
                            : valueOperand(s.value.values[0]);
        emit(Statement{StatementKind::Store, s.span, *local, std::move(at),
                       RValue{RValueKind::Use, {}, {}, 0, {std::move(value)},
                              typeRef(held)}});
        break;
      }
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
      const Ty type = declaredType(s);
      const unsigned counter = addLocal(s.name, typeRef(spell(type)), copies(type));
      // `perm` keeps the counter, so the name is put where the loop is rather
      // than inside it.
      bool keeps = false;
      for (const ChainSegment &seg : s.chain.segments)
        if (!seg.isName && seg.text == "perm")
          keeps = true;
      if (keeps)
        names_.back()[s.name] = counter;
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
      emit(Statement{StatementKind::Assign, s.span, more, {},
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
      if (!keeps)
        names_.back()[s.name] = counter;
      for (const StmtPtr &inner : s.body.stmts)
        statement(*inner);
      closeScope();
      loops_.pop_back();
      emit(Statement{StatementKind::Assign, s.span, counter, {},
                     RValue{RValueKind::Binary, "+", {}, 0,
                            {Operand{OperandKind::Copy, counter, {}, body_.locals[counter].type},
                             // The step is a number of the counter's own type,
                             // whatever size the counter was written with.
                             Operand{OperandKind::Written, 0, "1",
                                     body_.locals[counter].type}},
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
        // `give` is the word: it needs no `move` written, but it takes all the
        // same, so the answer leaves rather than being read in place.
        if (answer.kind == OperandKind::Copy && answer.local < body_.locals.size() &&
            !body_.locals[answer.local].copies)
          answer.kind = OperandKind::Move;
        emit(Statement{StatementKind::Assign, s.span, 0, {},
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
    emit(Statement{StatementKind::Assign, span, place, {},
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

MirResult build(const Source &source, const Program &program,
                const CheckResult &checked, Settings settings) {
  (void)source; // spans in the IR already carry everything a diagnostic needs
  MirResult result = Builder(program, checked).run();
  result.mir.settings = settings;
  return result;
}

} // namespace xag
