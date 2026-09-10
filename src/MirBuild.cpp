#include "xag/Mir.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace xag {
namespace {

// A type as the middle layer holds it: spelled, the way it is written apart
// from the dots. `many int64` and `loan many int64` are read back by prefix, the
// way `loan str` already was.
std::string spell(Ty type) {
  return type.kind == Type::Unknown ? "?" : name(type);
}

// How a thing is held, as the middle layer writes it. Only asked where it is
// not already known from the chain — a parameter's loan is written where the
// parameter is, and saying it twice made `loan loan int64`.
const char *lentAs(Ty type) {
  return type.held == Held::Loan      ? "loan "
         : type.held == Held::LoanMut ? "loanmut "
                                      : "";
}

// A number is handed over by being copied, however wide it is: there is nothing
// in one to give back.
bool copies(Ty type) { return isNumber(type) || type == Type::Bool; }

// Text and a `many` hold something that has to be given back. `nothing` is not
// a value that copies, but it is not one that owns either.
bool owns(Ty type) { return type.kind == Type::Str || type.holds(); }

// A loan is not a thing to end: it goes back to whoever lent it.
// Several of something, however it is spelled. `many-growing` is a second type
// and stands where `many` does, so everything that asks whether a thing is
// several has to ask about both — and asking about only one of them left an
// empty `many-growing` with nothing built for it at all.
bool holdsSeveral(const std::string &spelled) {
  return spelled.rfind("many ", 0) == 0 || spelled.rfind("many-growing ", 0) == 0;
}

bool isLoanType(const std::string &spelled) {
  return spelled.rfind("loan ", 0) == 0 || spelled.rfind("loanmut ", 0) == 0;
}

bool opensWith(std::string_view spelled, std::string_view word) {
  return spelled.rfind(word, 0) == 0;
}

MirType takeApart(std::string_view spelled, const Shapes &shapes,
                  const Shapes &sums = {}) {
  MirType out;
  if (opensWith(spelled, "loanmut ")) {
    out.lending = MirType::Lending::Write;
    spelled.remove_prefix(std::string_view("loanmut ").size());
  } else if (opensWith(spelled, "loan ")) {
    out.lending = MirType::Lending::Read;
    spelled.remove_prefix(std::string_view("loan ").size());
  }
  if (spelled.rfind("or-nothing ", 0) == 0) {
    out.orNothing = true;
    spelled.remove_prefix(std::string_view("or-nothing ").size());
  }
  while (spelled.rfind("many ", 0) == 0 || spelled.rfind("many-growing ", 0) == 0) {
    if (spelled.rfind("many-growing ", 0) == 0) {
      out.grows = true;
      spelled.remove_prefix(std::string_view("many-growing ").size());
    } else {
      spelled.remove_prefix(std::string_view("many ").size());
    }
    ++out.many;
  }
  out.held = typeNamed(spelled);
  if (out.held == Type::Unknown)
    for (unsigned which = 0; which < shapes.size(); ++which)
      if (shapes[which].name == spelled) {
        out.held = Type::Struct;
        out.named = which;
        break;
      }
  if (out.held == Type::Unknown)
    for (unsigned which = 0; which < sums.size(); ++which)
      if (sums[which].name == spelled) {
        out.held = Type::OneOf;
        out.named = which;
        break;
      }
  return out;
}

// Every struct's fields, said the way the middle layer says types. A `Shape`
// holds what the checker worked out, and nothing after the checker can read
// that — showing a struct walks its fields and has to know what each one is.
// `walk` is the list being described; `shapes` and `sums` are what a name in it
// may resolve against. Handing the same list as both — which the `one-of` side
// of this did — looks up a case's type in a table that does not hold it, and a
// case holding a struct came back as a type of no size at all.
std::vector<std::vector<MirType>> fieldsOfEveryShape(const Shapes &walk,
                                                     const Shapes &shapes,
                                                     const Shapes &sums) {
  std::vector<std::vector<MirType>> out;
  out.reserve(walk.size());
  for (const Shape &shape : walk) {
    std::vector<MirType> fields;
    fields.reserve(shape.fields.size());
    for (const Field &field : shape.fields)
      fields.push_back(takeApart(spell(field.type), shapes, sums));
    out.push_back(std::move(fields));
  }
  return out;
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
      // A struct declares a shape, not something to run. Laying one out as a
      // body made a callable named after the type, whose parameters were let go
      // at the end though nobody had ever handed them over.
      if (item.kind == ItemKind::Struct || item.kind == ItemKind::OneOf)
        continue;

      // A generic is not lowered with the blank still in it. There is no code to
      // write for `any`: how wide it is, whether it copies, and what an
      // instruction on it means are all the thing the blank has not said. What
      // came out was a module LLVM would not have — a `sext` of a `str`, and a
      // branch on something that was not a truth — and ITMT caught it rather
      // than letting it through, which is what ITMT is for.
      //
      // A generic reaches here once per type it is called with, blank filled.
      if (item.kind == ItemKind::Function && hasBlank(item))
        continue;

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
        if (!copiesNamed(type) && type.rfind("loan", 0) != 0)
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
    emit(Statement{StatementKind::Assign, e.span, into, {}, {},
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

  // Which struct a spelled type names, and what it is made of.
  const Shape *shapeOf(const std::string &spelled) const {
    for (const Shape &shape : checked_.shapes)
      if (shape.name == spelled)
        return &shape;
    return nullptr;
  }

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

  // A parameter's type is written on its chain, loan and all: `loan str`.
  // What is left of a spelled type once `or-nothing` is off it.
  static std::string within(const std::string &spelled) {
    return opensWith(spelled, "or-nothing ")
               ? spelled.substr(std::string_view("or-nothing ").size())
               : spelled;
  }

  static std::string chainType(const Chain &chain) {
    std::string mode;
    for (const ChainSegment &seg : chain.segments) {
      if (seg.isName)
        continue;
      if (seg.text == "loan" || seg.text == "loanmut")
        mode = seg.text + " ";
    }
    if (chain.segments.empty())
      return mode + "?";
    const std::size_t n = chain.segments.size();
    std::size_t at = n - 1;
    std::string built = chain.type().text;
    while (at > 0 && !chain.segments[at - 1].isName &&
           (chain.segments[at - 1].text == "many" ||
            chain.segments[at - 1].text == "many-growing")) {
      built = chain.segments[at - 1].text + " " + built;
      --at;
    }
    if (at > 0 && !chain.segments[at - 1].isName &&
        chain.segments[at - 1].text == "or-nothing")
      built = "or-nothing " + built;
    return mode + built;
  }

  // What is left of a spelled type once its loan word is off, and what one of
  // its places holds when it is a `many`.
  // Taking a word off the front, counted from the word rather than by hand.
  // Renaming `loan` to `loan` left the hand-written 4 behind, and a type came
  // back with a space on the front and meant nothing at all.
  static std::string withoutLoan(const std::string &spelled) {
    if (opensWith(spelled, "loanmut "))
      return spelled.substr(std::string_view("loanmut ").size());
    if (opensWith(spelled, "loan "))
      return spelled.substr(std::string_view("loan ").size());
    return spelled;
  }
  static std::string elementOf(const std::string &spelled) {
    const std::string bare = withoutLoan(spelled);
    if (opensWith(bare, "many-growing "))
      return bare.substr(std::string_view("many-growing ").size());
    return opensWith(bare, "many ") ? bare.substr(std::string_view("many ").size())
                                    : std::string("?");
  }

  TypeRef typeRef(const std::string &name) {
    for (unsigned i = 0; i < body_.types.size(); ++i)
      if (body_.types[i] == name)
        return TypeRef{i};
    body_.types.push_back(name);
    // Taken apart here, once, where the type is made. Everything downstream
    // reads the pieces rather than the spelling.
    body_.typed.push_back(takeApart(name));
    return TypeRef{static_cast<unsigned>(body_.types.size() - 1)};
  }

  MirType takeApart(std::string_view spelled) const {
    return xag::takeApart(spelled, checked_.shapes, checked_.sums);
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
      emit(Statement{StatementKind::Drop, Span{}, *local, {}, {}, RValue{}});
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
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Use, {}, {}, 0, {operandOf(e)}, typeRef(spell(type))}});
      return into;
    }

    case ExprKind::Typed: {
      // A case of a `one-of`, if the checker read it as one: which case, and
      // what goes in it. Otherwise the word named a type, and a written value
      // wearing its type is the value.
      const auto made = checked_.cases.find(&e);
      if (made != checked_.cases.end()) {
        const auto known = checked_.expressions.find(&e);
        const std::string spelled =
            spell(known == checked_.expressions.end() ? Ty{} : known->second);
        std::vector<Operand> parts;
        if (!e.children.empty())
          parts.push_back(operandOf(*e.children[0]));
        const unsigned into = owningTemporary(typeRef(spelled));
        emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                       RValue{RValueKind::Case, {}, {}, made->second,
                              std::move(parts), typeRef(spelled)}});
        return into;
      }
      return e.children.empty() ? temporary(typeRef("?"), true) : lower(*e.children[0]);
    }

    case ExprKind::Group:
      return e.children.empty() ? temporary(typeRef("?"), true) : lower(*e.children[0]);

    case ExprKind::Borrow: {
      if (e.children.empty())
        return temporary(typeRef("?"), true);
      // Taking one of the things a struct holds hands over the value itself,
      // and leaves that one holding nothing — so the drop at the end of the
      // scope finds it already gone and there is no flag to keep.
      if (e.text == "move" && e.children[0]->kind == ExprKind::Field) {
        const Expr &field = *e.children[0];
        const unsigned of = lower(*field.children[0]);
        const std::string held =
            withoutLoan(body_.types[body_.locals[of].type.index]);
        const Shape *shape = shapeOf(held);
        unsigned which = 0;
        std::string inner = "?";
        if (shape)
          for (unsigned i = 0; i < shape->fields.size(); ++i)
            if (shape->fields[i].name == field.text) {
              which = i;
              inner = spell(shape->fields[i].type);
            }
        const unsigned into = owningTemporary(typeRef(inner));
        emit(Statement{StatementKind::Assign, field.span, into, {}, {},
                       RValue{RValueKind::Taken, field.text, {}, which,
                              {Operand{OperandKind::Copy, of, {},
                                       body_.locals[of].type}},
                              typeRef(inner)}});
        return into;
      }
      if (e.text == "move")
        return lower(*e.children[0]);
      const unsigned of = lower(*e.children[0]);
      const std::string name = e.text + " " + body_.types[body_.locals[of].type.index];
      const unsigned into = temporary(typeRef(name), true);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Ref, e.text, {}, of, {}, typeRef(name)}});
      return into;
    }

    case ExprKind::Unary: {
      const unsigned into = temporary(typeRef(spell(type)), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Unary, e.text, {}, 0,
                            {operandOf(*e.children[0])}, typeRef(spell(type))}});
      return into;
    }

    case ExprKind::Binary: {
      Operand left = operandOf(*e.children[0]);
      Operand right = operandOf(*e.children[1]);
      const unsigned into = temporary(typeRef(spell(type)), copies(type));
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Binary, e.text, {}, 0,
                            {std::move(left), std::move(right)}, typeRef(spell(type))}});
      return into;
    }

    case ExprKind::Index: {
      // Reading a place gives back what sits in it. When that is something with
      // an owner, what comes back is a loan into the array rather than a copy —
      // there is one of it, and it stays where it is.
      // What is being reached into: a name, or something already reached into.
      // `'g'[*0*][*1*]` is the second, and the first reach is what it reaches
      // into — lowered here so that the two read the same way from here on.
      unsigned of = 0;
      if (e.children.size() > 1) {
        of = lower(*e.children[1]);
      } else {
        const unsigned *named = findName(e.text);
        if (!named)
          return temporary(typeRef("?"), true);
        of = *named;
      }
      const std::string held = elementOf(body_.types[body_.locals[of].type.index]);
      const bool copiesElement = copiesNamed(held);
      const std::string spelled = copiesElement ? held : "loan " + held;
      const unsigned into = temporary(typeRef(spelled), copiesElement);
      std::vector<Operand> parts;
      parts.push_back(Operand{OperandKind::Copy, of, {}, body_.locals[of].type});
      parts.push_back(e.children.empty()
                          ? Operand{OperandKind::Written, 0, "0", typeRef("int64")}
                          : operandOf(*e.children[0]));
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Element, {}, {}, 0, std::move(parts),
                            typeRef(spelled),
                            // Already answered where the program was read.
                            checked_.settled.count(&e) != 0}});
      return into;
    }

    case ExprKind::Field: {
      // Which of the things it holds, worked out where it is written — so the
      // middle layer carries a number rather than a name.
      if (e.children.empty())
        return temporary(typeRef("?"), true);
      const unsigned of = lower(*e.children[0]);
      const std::string held = withoutLoan(body_.types[body_.locals[of].type.index]);
      const Shape *shape = shapeOf(held);
      unsigned which = 0;
      std::string inner = "?";
      std::string lent;
      if (shape)
        for (unsigned i = 0; i < shape->fields.size(); ++i)
          if (shape->fields[i].name == e.text) {
            which = i;
            inner = spell(shape->fields[i].type);
            lent = lentAs(shape->fields[i].type);
          }
      // What copies is read out; what has an owner is lent where it stands, the
      // same as an element of a `many`. A field that is already a borrow is
      // read out as the borrow it is: lending it again would be a pointer to a
      // pointer, and reading it as the thing itself carried an address in a
      // slot typed as a whole number — `'h'.x` printed one.
      const bool copiesIt = lent.empty() && copiesNamed(inner);
      const std::string as = !lent.empty() ? lent + inner
                             : copiesIt    ? inner
                                           : "loan " + inner;
      const unsigned into = temporary(typeRef(as), copiesIt);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Part, e.text, {}, which,
                            {Operand{OperandKind::Copy, of, {}, body_.locals[of].type}},
                            typeRef(as)}});
      return into;
    }

    case ExprKind::Several: {
      // Brackets where an item goes: a `many` made where it stands. What it
      // holds is one level in from what it is going into.
      const std::string spelled = spell(type);
      const std::string holds = elementOf(spelled);
      (void)holds;
      std::vector<Operand> parts;
      // Each item is lowered where it stands. One that is itself several ends
      // up here again, one level in, which is the whole of how a `many` of a
      // `many` is built.
      for (const ExprPtr &child : e.children)
        if (child)
          parts.push_back(operandOf(*child));
      const unsigned into = owningTemporary(typeRef(spelled));
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Collect, {}, {}, 0, std::move(parts),
                            typeRef(spelled)}});
      return into;
    }

    case ExprKind::Nothing: {
      // An absence is a value like any other, written down where it stands.
      const std::string spelled = spell(type);
      const unsigned into = owningTemporary(typeRef(spelled));
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Use, {}, {}, 0,
                            {Operand{OperandKind::Written, 0, "nothing",
                                     typeRef(spelled)}},
                            typeRef(spelled)}});
      return into;
    }

    case ExprKind::Call: {
      std::string callee;
      for (const std::string &part : e.path)
        callee += (callee.empty() ? "" : ".") + part;
      // A struct named where an item goes makes one there, into a place of its
      // own that it is then handed over from.
      if (const Shape *shape = shapeOf(callee)) {
        (void)shape;
        const std::string spelled = callee;
        std::vector<Operand> parts;
        if (!e.args.values.empty())
          for (const ExprPtr &one : e.args.values[0].items)
            parts.push_back(operandOf(*one));
        const unsigned into = owningTemporary(typeRef(spelled));
        emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                       RValue{RValueKind::Group, {}, {}, 0, std::move(parts),
                              typeRef(spelled)}});
        return into;
      }
      if (callee == "fill") {
        const std::string spelled = spell(type);
        std::vector<Operand> parts;
        for (const Value &value : e.args.values)
          parts.push_back(valueOperand(value));
        parts.resize(2);
        const unsigned into = owningTemporary(typeRef(spelled));
        emit(Statement{StatementKind::Assign, e.span, into, {}, {},
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
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
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
    emit(Statement{StatementKind::Assign, value.span, into, {}, {},
                   RValue{RValueKind::Join, {}, {}, 0, std::move(pieces), typeRef("str")}});
    return Operand{OperandKind::Move, into, {}, typeRef("str")};
  }

  // `taking` is for `give`, which needs no `move` written but takes all the
  // same, so what it answers with leaves rather than being read in place.
  void assignInto(unsigned place, const ValueList &list, Span span,
                  bool taking = false) {
    const std::string spelled = body_.types[body_.locals[place].type.index];
    // Past the `or-nothing` as well as the loan: a name that may hold nothing
    // may hold a `many`, and the items still belong in its places rather than
    // joined into one. Stopping at the loan sent `or-nothing.many.int64` down
    // the joining path and made three numbers into the text "123".
    const std::string held = within(withoutLoan(spelled));
    if (const Shape *shape = shapeOf(held)) {
      // The struct's own spelling, not the name's: a name that may hold nothing
      // is filled with the thing and then wrapped, and saying `or-nothing tag`
      // here had the group built as though the absence were one of its fields.
      groupInto(place, list, span, held, *shape);
      return;
    }
    if (holdsSeveral(held)) {
      collectInto(place, list, span, held);
      return;
    }
    if (list.values.empty())
      return;
    Operand operand = valueOperand(list.values[0]);
    if (taking && operand.kind == OperandKind::Copy &&
        operand.local < body_.locals.size() && !body_.locals[operand.local].copies)
      operand.kind = OperandKind::Move;
    const TypeRef type = operand.type;
    emit(Statement{StatementKind::Assign, span, place, {}, {},
                   RValue{RValueKind::Use, {}, {}, 0, {std::move(operand)}, type}});
  }

  // One item for each of the things a struct holds, in the order it holds them.
  void groupInto(unsigned place, const ValueList &list, Span span,
                 const std::string &spelled, const Shape &shape) {
    (void)shape;
    if (list.values.empty())
      return;
    const Value &v = list.values[0];
    if (v.items.size() == 1 && checked_.of(v.items[0].get()).isStruct()) {
      Operand whole = operandOf(*v.items[0]);
      const TypeRef type = whole.type;
      emit(Statement{StatementKind::Assign, span, place, {}, {},
                     RValue{RValueKind::Use, {}, {}, 0, {std::move(whole)}, type}});
      return;
    }
    std::vector<Operand> parts;
    for (const ExprPtr &one : v.items)
      parts.push_back(operandOf(*one));
    emit(Statement{StatementKind::Assign, span, place, {}, {},
                   RValue{RValueKind::Group, {}, {}, 0, std::move(parts),
                          typeRef(spelled)}});
  }

  // Items side by side under a `many` stay several. A lone item that is already
  // the whole array is the whole array — which is what the checker settled, so
  // nothing here has to settle it again.
  void collectInto(unsigned place, const ValueList &list, Span span,
                   std::string spelled) {
    std::vector<Operand> parts;
    if (!list.values.empty()) {
      const Value &v = list.values[0];
      // A lone item that is already the whole array is the whole array. Asking
      // only whether it is *a* `many` was right while a `many` held one level:
      // one row of a `many` of a `many` is a `many` too, so `[[*ab*]]` put the
      // row itself where the array goes, and letting go of it walked one `str`
      // as though it were an array of them.
      if (v.items.size() == 1 && checked_.of(v.items[0].get()).holds() &&
          spell(checked_.of(v.items[0].get())) == spelled) {
        Operand operand = operandOf(*v.items[0]);
        const TypeRef type = operand.type;
        emit(Statement{StatementKind::Assign, span, place, {}, {},
                       RValue{RValueKind::Use, {}, {}, 0, {std::move(operand)}, type}});
        return;
      }
      for (const ExprPtr &item : v.items)
        parts.push_back(operandOf(*item));
    }
    emit(Statement{StatementKind::Assign, span, place, {}, {},
                   RValue{RValueKind::Collect, {}, {}, 0, std::move(parts),
                          typeRef(spelled)}});
  }

  // ---- statements

  // A condition that lends what it holds: the test is whether there is anything
  // there, and the name is bound to it inside the arm. Written once, because
  // `if` and `loop.while` ask it the same way.
  Operand testing(const Expr &condition, const std::string &holds,
                  unsigned &carried, std::string &held) {
    if (holds.empty())
      return operandOf(condition);
    carried = lower(condition);
    held = body_.types[body_.locals[carried].type.index];
    const unsigned answer = temporary(typeRef("bool"), true);
    emit(Statement{StatementKind::Assign, condition.span, answer, {}, {},
                   RValue{RValueKind::Holds, {}, {}, 0,
                          {Operand{OperandKind::Copy, carried, {},
                                   body_.locals[carried].type}},
                          typeRef("bool")}});
    return Operand{OperandKind::Copy, answer, {}, typeRef("bool")};
  }

  // Inside the arm, the name stands for what was there. It is lent rather than
  // taken, so nothing is dropped through it.
  const Shape *sumOf(const std::string &spelled) const {
    for (const Shape &sum : checked_.sums)
      if (sum.name == spelled)
        return &sum;
    return nullptr;
  }

  // One target per case, chosen by which case the value is in. The same
  // terminator two shapes use, with as many arms as the type names.
  void whenOverASum(const Stmt &s, unsigned subject, const Shape &sum, unsigned after) {
    const unsigned tag = temporary(typeRef("int64"), true);
    emit(Statement{StatementKind::Assign, s.condition->span, tag, {}, {},
                   RValue{RValueKind::Which, {}, {}, 0,
                          {Operand{OperandKind::Copy, subject, {},
                                   body_.locals[subject].type}},
                          typeRef("int64")}});
    // A block per case, and a test in front of each but the last. The switch
    // this ends in carries a truth, which is the one shape every engine already
    // branches on — a switch with a target per case would have been a second
    // shape for each of them to learn, for a choice a chain of asks makes just
    // as well.
    //
    // The last case needs no test: the checker insists every case is written
    // and each of them once, so what is left when the others are ruled out is
    // that one.
    std::vector<unsigned> targets;
    for (unsigned i = 0; i < sum.fields.size(); ++i)
      targets.push_back(addBlock());
    for (unsigned i = 0; i + 1 < sum.fields.size(); ++i) {
      const unsigned matches = temporary(typeRef("bool"), true);
      emit(Statement{StatementKind::Assign, s.span, matches, {}, {},
                     RValue{RValueKind::Binary, "==", {}, 0,
                            {Operand{OperandKind::Copy, tag, {}, typeRef("int64")},
                             Operand{OperandKind::Written, 0, std::to_string(i),
                                     typeRef("int64")}},
                            typeRef("bool")}});
      const unsigned next = i + 2 < sum.fields.size() ? addBlock() : targets.back();
      finish(Terminator{TerminatorKind::Switch, s.span,
                        Operand{OperandKind::Copy, matches, {}, typeRef("bool")},
                        {"true"}, {targets[i], next}, false, {}});
      current_ = next;
    }

    for (const Branch &arm : s.branches) {
      const auto which = checked_.chosenCase.find(&arm);
      if (which == checked_.chosenCase.end() || which->second >= targets.size())
        continue;
      current_ = targets[which->second];
      openScope();
      if (!arm.holds.empty()) {
        const std::string inner = spell(sum.fields[which->second].type);
        const bool copies = copiesNamed(inner);
        const std::string as = copies ? inner : "loan " + inner;
        const unsigned into = addLocal(arm.holds, typeRef(as), copies);
        emit(Statement{StatementKind::Assign, arm.holdsSpan, into, {}, {},
                       RValue{RValueKind::Inside, {}, {}, 0,
                              {Operand{OperandKind::Copy, subject, {},
                                       body_.locals[subject].type}},
                              typeRef(as)}});
        names_.back()[arm.holds] = into;
      }
      for (const StmtPtr &inner : arm.body.stmts)
        statement(*inner);
      closeScope();
      finish(Terminator{TerminatorKind::Goto, arm.span, {}, {}, {after}, false, {}});
    }
    current_ = after;
  }

  void bindHeld(const std::string &holds, unsigned carried, const std::string &spelled,
                Span where) {
    if (holds.empty())
      return;
    // What is inside, once the borrow is off it. The subject may be a borrow
    // already — reaching into one of the things a struct holds lends it where
    // it stands — and asking what is inside a `loan or-nothing int8` without
    // taking the loan off first gave `loan loan or-nothing int8`, a pointer to
    // a pointer that no engine could read.
    const std::string inner = within(withoutLoan(spelled));
    const bool copies = copiesNamed(inner);
    const std::string as = copies ? inner : "loan " + inner;
    const unsigned into = addLocal(holds, typeRef(as), copies);
    emit(Statement{StatementKind::Assign, where, into, {}, {},
                   RValue{RValueKind::Inside, {}, {}, 0,
                          {Operand{OperandKind::Copy, carried, {},
                                   body_.locals[carried].type}},
                          typeRef(as)}});
    names_.back()[holds] = into;
  }

  // A loop whose chain said `no-itmt`, remembered on the block everything jumps
  // back to — which is where a run has to stop.
  // Whether a blank is still written anywhere in what this declares.
  static bool hasBlank(const Item &item) {
    const auto blankIn = [](const Chain &chain) {
      for (const ChainSegment &seg : chain.segments)
        if (!seg.isName && seg.text == "any")
          return true;
      return false;
    };
    if (blankIn(item.chain))
      return true;
    for (const Param &param : item.params)
      if (blankIn(param.chain))
        return true;
    return false;
  }

  void markIfToldNotToRun(const Chain &chain, unsigned header) {
    for (const ChainSegment &seg : chain.segments)
      if (!seg.isName && seg.text == "no-itmt")
        body_.blocks[header].noItmt = true;
  }

  void statement(const Stmt &s) {
    switch (s.kind) {
    case StmtKind::Declare: {
      const std::string spelled = chainType(s.chain);
      const unsigned local = addLocal(s.name, typeRef(spelled), copiesNamed(spelled));
      assignInto(local, s.value, s.span);
      names_.back()[s.name] = local;
      if (!body_.locals[local].copies && spelled.rfind("loan", 0) != 0)
        scopes_.back().push_back(local);
      break;
    }

    case StmtKind::Add: {
      const unsigned *local = findName(s.name);
      if (!local)
        break;
      const std::string held =
          elementOf(body_.types[body_.locals[*local].type.index]);
      // What goes in a new place is read the same way as what goes in an
      // existing one, which is the same way as what goes into a name.
      const std::string inside = within(withoutLoan(held));
      Operand what;
      if (holdsSeveral(inside) || shapeOf(inside)) {
        const unsigned into = owningTemporary(typeRef(held));
        assignInto(into, s.value, s.span);
        what = Operand{OperandKind::Move, into, {}, typeRef(held)};
      } else {
        what = s.value.values.empty()
                   ? Operand{OperandKind::Written, 0, "", typeRef(held)}
                   : valueOperand(s.value.values[0]);
      }
      emit(Statement{StatementKind::Grow, s.span, *local, {}, {},
                     RValue{RValueKind::Use, {}, {}, 0, {std::move(what)},
                            typeRef(held)}});
      break;
    }

    case StmtKind::Set: {
      const unsigned *local = findName(s.name);
      if (!local)
        break;
      // Which of the things it holds is written. The path is worked out here,
      // where the fields are known, so nothing further down reads a name.
      if (!s.fields.empty()) {
        std::vector<unsigned> parts;
        std::vector<std::string> along; // what each step is, in the order taken
        std::string held = withoutLoan(body_.types[body_.locals[*local].type.index]);
        std::string lent; // how the last step is held, which the type word omits
        for (const std::string &field : s.fields) {
          const Shape *shape = shapeOf(held);
          if (!shape)
            break;
          for (unsigned i = 0; i < shape->fields.size(); ++i)
            if (shape->fields[i].name == field) {
              parts.push_back(i);
              held = spell(shape->fields[i].type);
              lent = lentAs(shape->fields[i].type);
              // Every step is reached by lending it where it stands; the last
              // one is lent the way the struct says it holds it.
              along.push_back(lent.empty() ? "loan " + held : lent + held);
              break;
            }
        }

        // A field that is a borrow holds the pointer, so writing to it means
        // writing through it — putting the value into the field's own slot
        // would put a number where an address goes, which is what happened:
        // the compiler took it, the struct was quietly wrecked, and every
        // engine agreed that the thing being written to had not changed.
        //
        // Read out and written through, which is the shape a borrowed name
        // already takes and both backends already know.
        if (!parts.empty() && parts.size() == along.size() && !lent.empty()) {
          unsigned at = *local;
          for (unsigned step = 0; step < parts.size(); ++step) {
            const unsigned into = temporary(typeRef(along[step]), false);
            emit(Statement{StatementKind::Assign, s.span, into, {}, {},
                           RValue{RValueKind::Part, s.fields[step], {}, parts[step],
                                  {Operand{OperandKind::Copy, at, {},
                                           body_.locals[at].type}},
                                  typeRef(along[step])}});
            at = into;
          }
          Operand through = s.value.values.empty()
                                ? Operand{OperandKind::Written, 0, "", typeRef(held)}
                                : valueOperand(s.value.values[0]);
          const TypeRef what = through.type;
          emit(Statement{StatementKind::Assign, s.span, at, {}, {},
                         RValue{RValueKind::Use, {}, {}, 0, {std::move(through)}, what}});
          break;
        }

        Operand what = s.value.values.empty()
                           ? Operand{OperandKind::Written, 0, "", typeRef(held)}
                           : valueOperand(s.value.values[0]);
        const TypeRef type = what.type;
        emit(Statement{StatementKind::Assign, s.span, *local, std::move(parts), {},
                       RValue{RValueKind::Use, {}, {}, 0, {std::move(what)}, type}});
        break;
      }

      if (s.index) {
        const std::string held =
            elementOf(body_.types[body_.locals[*local].type.index]);
        Operand at = operandOf(*s.index);
        Operand value;
        // What goes in a place may itself be several values, or a group of
        // them, and building one of those needs the same reading that building
        // a name does. Read as a lone value instead, `set 'w'[*1*] = [*x* *y*]`
        // joined two pieces of text and put the joined one where a `many str`
        // goes.
        const std::string inside = within(withoutLoan(held));
        if (holdsSeveral(inside) || shapeOf(inside)) {
          const unsigned into = owningTemporary(typeRef(held));
          assignInto(into, s.value, s.span);
          value = Operand{OperandKind::Move, into, {}, typeRef(held)};
        } else {
          value = s.value.values.empty()
                      ? Operand{OperandKind::Written, 0, "", typeRef(held)}
                      : valueOperand(s.value.values[0]);
        }
        emit(Statement{StatementKind::Store, s.span, *local, {}, std::move(at),
                       RValue{RValueKind::Use, {}, {}, 0, {std::move(value)},
                              typeRef(held)}});
        break;
      }
      assignInto(*local, s.value, s.span);
      break;
    }

    case StmtKind::When: {
      // Straight onto the switch terminator, which carries a value per target
      // rather than a true/false pair and was written general from the start
      // for exactly this. Two shapes today; a decision tree uses it unchanged.
      const unsigned after = addBlock();
      if (!s.condition) {
        finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {after}, false, {}});
        current_ = after;
        break;
      }

      const unsigned subject = lower(*s.condition);
      const std::string carried = body_.types[body_.locals[subject].type.index];
      if (const Shape *sum = sumOf(withoutLoan(carried))) {
        whenOverASum(s, subject, *sum, after);
        break;
      }
      const unsigned answer = temporary(typeRef("bool"), true);
      emit(Statement{StatementKind::Assign, s.condition->span, answer, {}, {},
                     RValue{RValueKind::Holds, {}, {}, 0,
                            {Operand{OperandKind::Copy, subject, {},
                                     body_.locals[subject].type}},
                            typeRef("bool")}});

      const unsigned something = addBlock();
      const unsigned none = addBlock();
      finish(Terminator{TerminatorKind::Switch, s.span,
                        Operand{OperandKind::Copy, answer, {}, typeRef("bool")},
                        {"true"}, {something, none}, false, {}});

      for (const Branch &arm : s.branches) {
        current_ = arm.matchesNothing ? none : something;
        openScope();
        if (!arm.matchesNothing)
          bindHeld(arm.holds, subject, carried, arm.holdsSpan);
        for (const StmtPtr &inner : arm.body.stmts)
          statement(*inner);
        closeScope();
        finish(Terminator{TerminatorKind::Goto, arm.span, {}, {}, {after}, false, {}});
      }
      current_ = after;
      break;
    }

    // Nothing of its own to lower: what it grants is asked for by name, and the
    // asking is carried on the loop that asked.
    case StmtKind::Unsafe:
      for (const StmtPtr &inner : s.body.stmts)
        statement(*inner);
      break;

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
        unsigned carried = 0;
        std::string held;
        const Operand condition =
            testing(*branch.condition, branch.holds, carried, held);
        const unsigned taken = addBlock();
        const unsigned otherwise = addBlock();
        finish(Terminator{TerminatorKind::Switch, branch.span, condition,
                          {"true"}, {taken, otherwise}, false, {}});
        current_ = taken;
        openScope();
        bindHeld(branch.holds, carried, held, branch.holdsSpan);
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

      // Four blocks, not three: the counter is tested *before* it is stepped,
      // so the step only ever happens below the end.
      //
      // Stepping first and testing after, a counter whose end was the largest
      // number its type holds came round and the test passed again, and the
      // loop never finished. `E0531` refused an end *written down* as the
      // largest and could see nothing else, so an end worked out while the
      // program runs walked straight past it. Now there is nothing to refuse:
      // the step cannot come round because it never happens at the end, and
      // `E0531` is gone.
      //
      // The way back still goes through the header. Sending it straight to the
      // body left the header outside the loop, and the header is where the
      // counter is read — so the counter looked like something the loop leaves
      // behind, and a loop with real work in it was replaced by its effect on
      // the counter alone. The header's question is answered twice per turn on
      // paper; LLVM asks it once.
      const unsigned header = addBlock();
      const unsigned inside = addBlock();
      const unsigned step = addBlock();
      const unsigned after = addBlock();
      markIfToldNotToRun(s.chain, header);
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});

      current_ = header;
      const unsigned more = temporary(typeRef("bool"), true);
      emit(Statement{StatementKind::Assign, s.span, more, {}, {},
                     RValue{RValueKind::Binary, "<==", {}, 0,
                            {Operand{OperandKind::Copy, counter, {}, body_.locals[counter].type},
                             Operand{OperandKind::Copy, last, {}, body_.locals[last].type}},
                            typeRef("bool")}});
      finish(Terminator{TerminatorKind::Switch, s.span,
                        Operand{OperandKind::Copy, more, {}, typeRef("bool")},
                        {"true"}, {inside, after}, false, {}});

      current_ = inside;
      loops_.push_back(Loop{step, after});
      openScope();
      if (!keeps)
        names_.back()[s.name] = counter;
      for (const StmtPtr &inner : s.body.stmts)
        statement(*inner);
      closeScope();
      loops_.pop_back();
      // The header has already said the counter is at or before the end, so
      // being at it means this was the last turn.
      const unsigned done = temporary(typeRef("bool"), true);
      emit(Statement{StatementKind::Assign, s.span, done, {}, {},
                     RValue{RValueKind::Binary, ">==", {}, 0,
                            {Operand{OperandKind::Copy, counter, {}, body_.locals[counter].type},
                             Operand{OperandKind::Copy, last, {}, body_.locals[last].type}},
                            typeRef("bool")}});
      finish(Terminator{TerminatorKind::Switch, s.span,
                        Operand{OperandKind::Copy, done, {}, typeRef("bool")},
                        {"true"}, {after, step}, false, {}});

      current_ = step;
      emit(Statement{StatementKind::Assign, s.span, counter, {}, {},
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
      markIfToldNotToRun(s.chain, header);
      body_.blocks[header].mayNotFinish = true;
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});

      current_ = header;
      unsigned carried = 0;
      std::string held;
      const Operand condition =
          s.condition ? testing(*s.condition, s.holds, carried, held)
                      : Operand{OperandKind::Written, 0, "true", typeRef("bool")};
      finish(Terminator{TerminatorKind::Switch, s.span, condition, {"true"},
                        {inside, after}, false, {}});

      current_ = inside;
      loops_.push_back(Loop{header, after});
      openScope();
      bindHeld(s.holds, carried, held, s.holdsSpan);
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
        // The same road a declaration takes, so that answering with a struct
        // fills it and answering with a `many` collects it. Reading the items
        // straight off as one operand joined them instead: a function answering
        // a struct built its two numbers into text and handed that back.
        assignInto(0, s.value, s.span, true);
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
    emit(Statement{StatementKind::Assign, span, place, {}, {},
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
                const CheckResult &checked) {
  (void)source; // spans in the IR already carry everything a diagnostic needs
  MirResult result = Builder(program, checked).run();
  result.mir.shapes = checked.shapes;
  result.mir.sums = checked.sums;
  result.mir.fieldTypes =
      fieldsOfEveryShape(checked.shapes, checked.shapes, checked.sums);
  result.mir.caseTypes = fieldsOfEveryShape(checked.sums, checked.shapes, checked.sums);
  return result;
}

} // namespace xag
