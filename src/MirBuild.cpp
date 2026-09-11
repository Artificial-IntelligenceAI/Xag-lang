#include "xag/Mir.h"

#include "xag/Typed.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace xag {
namespace {

// A type as the middle layer holds it. The checker's answer is carried on every
// node of the typed tree, so this is a translation between two structures and
// never a spelling read back: `spell` writes one out for the printer, and
// nothing reads that spelling again.
//
// It used to go the other way. Every type here was written to text and parsed
// back by `takeApart`, forty-eight times over, and a table had to be to hand at
// each of them to say which struct a name meant. Handing the wrong table to one
// of those calls is what made a case holding a `pair` come back with no size at
// all.
MirType typing(Ty type) {
  MirType out;
  out.lending = type.held == Held::LoanMut ? MirType::Lending::Write
                : type.held == Held::Loan  ? MirType::Lending::Read
                                           : MirType::Lending::None;
  out.orNothing = type.orNothing;
  out.many = type.deep;
  out.grows = type.grows;
  out.held = type.holds() ? type.element : type.kind;
  out.named = type.named;
  return out;
}

// How a thing is held, as the middle layer writes it. A `Ty` says it and the
// checker's own spelling does not, so the word goes on the front here.
const char *lentAs(Ty type) {
  return type.held == Held::Loan      ? "loan "
         : type.held == Held::LoanMut ? "loanmut "
                                      : "";
}

std::string spell(Ty type) {
  return type.kind == Type::Unknown ? "?" : lentAs(type) + name(type);
}

// The same type, held the way this says.
Ty lent(Ty type, Held how) {
  type.held = how;
  return type;
}
Ty owned(Ty type) { return lent(type, Held::Owned); }

// A number is handed over by being copied, however wide it is: there is nothing
// in one to give back.
bool copies(Ty type) { return isNumber(type) || type == Type::Bool; }

// The same question asked of a name rather than of a value. A borrow is a
// pointer to whatever it borrows, and one is never copied out of its slot
// however small the thing on the far end is.
bool copiesHeld(Ty type) { return type.held == Held::Owned && copies(type); }

// Text and a `many` hold something that has to be given back. `nothing` is not
// a value that copies, but it is not one that owns either.
bool owns(Ty type) { return type.kind == Type::Str || type.holds(); }

// What one place of a `many` holds. The borrow and the growing are the array's
// own and do not come in with it.
Ty placeOf(Ty array) { return array.holds() ? elementOf(owned(array)) : Ty{}; }

// Every struct's fields, said the way the middle layer says types. A `Shape`
// holds what the checker worked out, and nothing after the checker can read
// that — showing a struct walks its fields and has to know what each one is.
//
// Once, this took the tables a name might resolve against, and handing the same
// one twice looked a case's type up where it was not. There are no names left in
// it now: a field carries the checker's `Ty`, which already says which struct.
std::vector<std::vector<MirType>> fieldsOfEveryShape(const Shapes &walk) {
  std::vector<std::vector<MirType>> out;
  out.reserve(walk.size());
  for (const Shape &shape : walk) {
    std::vector<MirType> fields;
    fields.reserve(shape.fields.size());
    for (const Field &field : shape.fields)
      fields.push_back(typing(field.type));
    out.push_back(std::move(fields));
  }
  return out;
}

class Builder {
public:
  explicit Builder(const TypedProgram &program) : program_(program) {}

  MirResult run() {
    // A constant is a body that answers with its value. Naming one is a call,
    // which needs no concept the IR did not already have — and lets a constant
    // be written as an expression rather than only as a literal.
    //
    // A function's answer comes from its signature rather than from the type of
    // the call, because the two used to differ: the checker said `str` whether
    // the function handed text over or only lent it. They agree now — a `Ty`
    // says how a thing is held — and the signature is still where the answer
    // belongs.
    for (const TypedItem &item : program_.items) {
      if (item.kind == TypedItemKind::Const)
        consts_[item.name] = item.answers;
      else if (item.kind == TypedItemKind::Function)
        answers_[item.name] = item.answers;
    }

    for (const TypedItem &item : program_.items) {
      if (item.kind == TypedItemKind::Const) {
        newBody(constBody(item.name));
        names_.emplace_back();
        const Ty answers = item.answers;
        body_.result = typeRef(answers);
        addLocal("", answers);
        current_ = addBlock();
        assignInto(0, item.value, item.span);
        finish(Terminator{TerminatorKind::Return, item.span, {}, {}, {}, true,
                          Operand{copiesHeld(answers) ? OperandKind::Copy
                                                      : OperandKind::Move,
                                  0, {}, body_.result}});
        result_.mir.bodies.push_back(std::move(body_));
        continue;
      }

      // A generic is not lowered with the blank still in it. There is no code to
      // write for `any`: how wide it is, whether it copies, and what an
      // instruction on it means are all the thing the blank has not said. What
      // came out was a module LLVM would not have — a `sext` of a `str`, and a
      // branch on something that was not a truth — and ITMT caught it rather
      // than letting it through, which is what ITMT is for.
      //
      // A generic reaches here once per type it is called with, blank filled.
      if (item.generic)
        continue;

      newBody(item.kind == TypedItemKind::Start  ? "START"
              : item.kind == TypedItemKind::Itmt ? "ITMT"
                                                 : item.name);

      const Ty result =
          item.kind == TypedItemKind::Function ? item.answers : Ty{Type::Nothing};
      body_.result = typeRef(result);
      // Local 0 is the answer.
      addLocal("", result);

      openScope();
      for (const TypedParam &param : item.params) {
        const unsigned local = addLocal(param.name, param.type);
        names_.back()[param.name] = local;
        ++body_.parameters;
        // A parameter taken by value belongs to the callee, and ends with it.
        if (!copiesHeld(param.type) && param.type.held == Held::Owned)
          scopes_.back().push_back(local);
      }

      current_ = addBlock();
      for (const TypedStmtPtr &s : item.body.stmts)
        statement(*s);
      closeScope();
      finish(Terminator{TerminatorKind::Return, item.span, {}, {}, {}, false, {}});

      result_.mir.bodies.push_back(std::move(body_));
    }
    return std::move(result_);
  }

private:
  const TypedProgram &program_;
  MirResult result_;

  Body body_;
  unsigned current_ = 0;
  // Locals declared in each open scope, innermost last, dropped in reverse.
  std::vector<std::vector<unsigned>> scopes_;
  std::vector<std::unordered_map<std::string, unsigned>> names_;
  // What each local holds, as the checker knows it. The middle layer keeps a
  // spelling for its printer; everything here asks this instead, so a type is
  // never worked out from a name a second time.
  std::vector<Ty> holds_;
  // Whether the statement being lowered puts its answer somewhere that said
  // `wrapping`. A sum is checked while the program runs unless it did.
  bool wrapsHere_ = false;
  std::unordered_map<std::string, Ty> consts_;
  std::unordered_map<std::string, Ty> answers_;

  void newBody(std::string name) {
    body_ = Body{};
    body_.name = std::move(name);
    scopes_.clear();
    loops_.clear();
    names_.clear();
    holds_.clear();
  }

  static std::string constBody(const std::string &name) { return "const '" + name + "'"; }

  // A name nothing declared may still be a constant, which is a call.
  unsigned callConst(const TypedExpr &e, Ty answers) {
    const unsigned into = addLocal("", answers);
    emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                   RValue{RValueKind::Call, {}, constBody(e.text), 0, {},
                          typeRef(answers)}});
    return into;
  }

  // Puts the flag back however the statement left, including out of a `break`.
  struct Wrapping {
    bool was;
    Builder &of;
    ~Wrapping() { of.wrapsHere_ = was; }
  };

  struct Loop {
    unsigned again = 0; // where a pass restarts
    unsigned after = 0; // where `break` goes
  };
  std::vector<Loop> loops_;

  // ---- small pieces

  // Which struct a type is, and what it is made of. A type says which by number,
  // so there is nothing to look up by name and nothing to look it up in.
  const Shape *shapeOf(Ty type) const {
    return type.kind == Type::Struct && type.named < program_.shapes.size()
               ? &program_.shapes[type.named]
               : nullptr;
  }

  const Shape *sumOf(Ty type) const {
    return type.kind == Type::OneOf && type.named < program_.sums.size()
               ? &program_.sums[type.named]
               : nullptr;
  }

  TypeRef typeRef(Ty type) {
    const std::string spelled = spell(type);
    for (unsigned i = 0; i < body_.types.size(); ++i)
      if (body_.types[i] == spelled)
        return TypeRef{i};
    body_.types.push_back(spelled);
    body_.typed.push_back(typing(type));
    return TypeRef{static_cast<unsigned>(body_.types.size() - 1)};
  }

  unsigned addLocal(const std::string &name, Ty type) {
    const unsigned id = static_cast<unsigned>(body_.locals.size());
    body_.locals.push_back(Local{id, typeRef(type), name, copiesHeld(type)});
    holds_.push_back(type);
    return id;
  }

  // A temporary that owns something is owned by the scope it was made in, and
  // ends there like anything else. If it is moved out first, elaboration sees
  // that and takes the drop away again.
  unsigned owningTemporary(Ty type) {
    const unsigned id = addLocal("", type);
    body_.locals[id].copies = false;
    if (!scopes_.empty())
      scopes_.back().push_back(id);
    return id;
  }

  // A local for an expression with nothing in it. Only reached where something
  // was already refused, and never read.
  unsigned nowhere() {
    const unsigned id = addLocal("", Ty{});
    body_.locals[id].copies = true;
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

  // Reading a local as an operand, the way naming one reads it.
  Operand reading(unsigned local) const {
    return Operand{OperandKind::Copy, local, {}, body_.locals[local].type};
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

  Operand operandOf(const TypedExpr &e) {
    switch (e.kind) {
    case TypedKind::Name: {
      // Naming something reads it. Taking it is spelled `move`, and arrives as
      // its own node — so joining and printing leave what they read alone.
      const unsigned *local = findName(e.text);
      if (!local) {
        auto constant = consts_.find(e.text);
        if (constant == consts_.end())
          return Operand{OperandKind::Written, 0, e.text, typeRef(Ty{})};
        const unsigned into = callConst(e, constant->second);
        const Local &answered = body_.locals[into];
        return Operand{answered.copies ? OperandKind::Copy : OperandKind::Move, into, {},
                       answered.type};
      }
      return reading(*local);
    }
    case TypedKind::Written:
      return Operand{OperandKind::Written, 0, e.text, typeRef(owned(e.type))};
    case TypedKind::Escape:
      return Operand{OperandKind::Written, 0, "\\" + e.text, typeRef(Ty{Type::Str})};
    default:
      break;
    }
    const unsigned into = lower(e);
    const Local &slot = body_.locals[into];
    return Operand{slot.copies ? OperandKind::Copy : OperandKind::Move, into, {}, slot.type};
  }

  // Lower an expression into a local and answer which one holds it.
  //
  // How a *value* is held is this pass's to say, not the checker's. The checker
  // carries the borrow along through what is worked out from a borrow, which is
  // right for the question it answers — a borrowed number is a number and a
  // borrow at once — and wrong for a slot: the sum of two borrowed numbers is a
  // number of its own, and a slot typed `loanmut int64` is written *through*.
  // Every place below that really does hold a borrow builds it from what the
  // thing was declared as, which is where a borrow is written down.
  unsigned lower(const TypedExpr &e) {
    const Ty type = owned(e.type);
    switch (e.kind) {
    case TypedKind::Name: {
      if (const unsigned *local = findName(e.text))
        return *local;
      auto constant = consts_.find(e.text);
      if (constant != consts_.end())
        return callConst(e, constant->second);
      [[fallthrough]];
    }
    case TypedKind::Written:
    case TypedKind::Escape: {
      // Text written into a temporary is text that temporary owns.
      const unsigned into = owns(type) ? owningTemporary(type) : addLocal("", type);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Use, {}, {}, 0, {operandOf(e)}, typeRef(type)}});
      return into;
    }

    case TypedKind::Case: {
      // Which case, and what goes in it. Which one it is was settled where the
      // program was read; nothing here tells a case from a written value.
      std::vector<Operand> parts;
      if (!e.children.empty())
        parts.push_back(operandOf(*e.children[0]));
      const unsigned into = owningTemporary(type);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Case, {}, {}, e.which, std::move(parts),
                            typeRef(type)}});
      return into;
    }

    case TypedKind::Hold:
      return e.children.empty() ? nowhere() : lower(*e.children[0]);

    case TypedKind::Borrow: {
      if (e.children.empty())
        return nowhere();
      // Taking one of the things a struct holds hands over the value itself,
      // and leaves that one holding nothing — so the drop at the end of the
      // scope finds it already gone and there is no flag to keep.
      if (e.text == "move" && e.children[0]->kind == TypedKind::Field) {
        const TypedExpr &field = *e.children[0];
        const unsigned of =
            field.children.empty() ? nowhere() : lower(*field.children[0]);
        const Shape *shape = shapeOf(owned(holds_[of]));
        const Ty inner = shape && field.which < shape->fields.size()
                             ? owned(shape->fields[field.which].type)
                             : Ty{};
        const unsigned into = owningTemporary(inner);
        emit(Statement{StatementKind::Assign, field.span, into, {}, {},
                       RValue{RValueKind::Taken, field.text, {}, field.which,
                              {reading(of)}, typeRef(inner)}});
        return into;
      }
      if (e.text == "move")
        return lower(*e.children[0]);
      const unsigned of = lower(*e.children[0]);
      const Ty as = lent(holds_[of], e.text == "loanmut" ? Held::LoanMut : Held::Loan);
      const unsigned into = addLocal("", as);
      body_.locals[into].copies = true;
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Ref, e.text, {}, of, {}, typeRef(as)}});
      return into;
    }

    case TypedKind::Unary: {
      const unsigned into = addLocal("", type);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Unary, e.text, {}, 0,
                            {operandOf(*e.children[0])}, typeRef(type)}});
      return into;
    }

    case TypedKind::Binary: {
      Operand left = operandOf(*e.children[0]);
      Operand right = operandOf(*e.children[1]);
      const unsigned into = addLocal("", type);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Binary, e.text, {}, 0,
                            {std::move(left), std::move(right)}, typeRef(type),
                            false, wrapsHere_}});
      return into;
    }

    case TypedKind::Element: {
      // Reading a place gives back what sits in it. When that is something with
      // an owner, what comes back is a loan into the array rather than a copy —
      // there is one of it, and it stays where it is.
      //
      // What is being reached into is the first child, whether a name was
      // written or another reach was: `'g'[*1*][*2*]` reaches into what the
      // first reach answered, and the two read the same way from here on.
      const unsigned of = lower(*e.children[0]);
      const Ty place = placeOf(holds_[of]);
      const Ty as = copies(place) ? place : lent(place, Held::Loan);
      const unsigned into = addLocal("", as);
      std::vector<Operand> parts;
      parts.push_back(reading(of));
      parts.push_back(e.children.size() > 1
                          ? operandOf(*e.children[1])
                          : Operand{OperandKind::Written, 0, "0", typeRef(Ty{Type::Int64})});
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Element, {}, {}, 0, std::move(parts),
                            typeRef(as),
                            // Already answered where the program was read.
                            e.settled}});
      return into;
    }

    case TypedKind::Field: {
      // Which of the things it holds is a number by now, worked out where the
      // program was read — so nothing here reads a name.
      if (e.children.empty())
        return nowhere();
      const unsigned of = lower(*e.children[0]);
      const Shape *shape = shapeOf(owned(holds_[of]));
      const Ty declared = shape && e.which < shape->fields.size()
                              ? shape->fields[e.which].type
                              : Ty{};
      // What copies is read out; what has an owner is lent where it stands, the
      // same as an element of a `many`. A field that is already a borrow is
      // read out as the borrow it is: lending it again would be a pointer to a
      // pointer, and reading it as the thing itself carried an address in a
      // slot typed as a whole number — `'h'.x` printed one.
      const bool copiesIt = copiesHeld(declared);
      const Ty as = declared.held != Held::Owned || copiesIt
                        ? declared
                        : lent(declared, Held::Loan);
      const unsigned into = addLocal("", as);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Part, e.text, {}, e.which, {reading(of)},
                            typeRef(as)}});
      return into;
    }

    case TypedKind::Several: {
      // Brackets where an item goes: a `many` made where it stands. Each item is
      // lowered where it stands, and one that is itself several ends up here
      // again one level in — which is the whole of how a `many` of a `many` is
      // built.
      std::vector<Operand> parts;
      for (const TypedPtr &child : e.children)
        if (child)
          parts.push_back(operandOf(*child));
      const unsigned into = owningTemporary(type);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Collect, {}, {}, 0, std::move(parts),
                            typeRef(type)}});
      return into;
    }

    case TypedKind::Collect:
    case TypedKind::Join:
      // Neither is written where an expression stands: both are what a value
      // list becomes, and that is read where the value list is.
      return e.children.empty() ? nowhere() : lower(*e.children[0]);

    case TypedKind::Nothing: {
      // An absence is a value like any other, written down where it stands.
      const unsigned into = owningTemporary(type);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Use, {}, {}, 0,
                            {Operand{OperandKind::Written, 0, "nothing", typeRef(type)}},
                            typeRef(type)}});
      return into;
    }

    case TypedKind::Made: {
      // A struct named where an item goes makes one there, into a place of its
      // own that it is then handed over from.
      const Ty made = structNamed(e.which);
      std::vector<Operand> parts;
      for (const TypedPtr &one : e.children)
        parts.push_back(operandOf(*one));
      const unsigned into = owningTemporary(made);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Group, {}, {}, 0, std::move(parts),
                            typeRef(made)}});
      return into;
    }

    case TypedKind::Call: {
      if (e.name == "fill") {
        std::vector<Operand> parts;
        for (const std::vector<TypedPtr> &value : e.args)
          parts.push_back(valueOperand(value, e.span));
        parts.resize(2);
        const unsigned into = owningTemporary(type);
        emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                       RValue{RValueKind::Fill, {}, {}, 0, std::move(parts),
                              typeRef(type)}});
        return into;
      }
      std::vector<Operand> arguments;
      if (e.name == "print.stdout" || e.name == "print.stderr") {
        // Showing is not joining: a print writes one piece after another and
        // builds nothing, so its pieces stay pieces and are never welded into
        // a value first. And it reads them, so they stay where they were.
        for (const std::vector<TypedPtr> &value : e.args)
          for (const TypedPtr &item : value) {
            Operand piece = operandOf(*item);
            if (piece.kind == OperandKind::Move)
              piece.kind = OperandKind::Copy;
            arguments.push_back(std::move(piece));
          }
      } else {
        for (const std::vector<TypedPtr> &value : e.args)
          arguments.push_back(valueOperand(value, e.span));
      }
      auto answered = answers_.find(e.name);
      const Ty answers = answered == answers_.end() ? type : answered->second;
      // A borrow answered back is not a thing to end: it goes back to whoever
      // lent it, and the caller's slot holds only the pointer.
      const bool ownsIt = owns(type) && answers.held == Held::Owned;
      const unsigned into = ownsIt ? owningTemporary(answers) : addLocal("", answers);
      if (!ownsIt)
        body_.locals[into].copies = copies(type);
      emit(Statement{StatementKind::Assign, e.span, into, {}, {},
                     RValue{RValueKind::Call, {}, e.name, 0, std::move(arguments),
                            typeRef(answers)}});
      return into;
    }
    }
    return nowhere();
  }

  // One value, which is one item or several joined into a new one. Where there
  // is no value at all, `empty` is what the place it was going into holds —
  // only reached where something was already refused, and typed so that what
  // comes out is still a middle layer the engines can read.
  Operand valueOperand(const std::vector<TypedPtr> &items, Span span,
                       Ty empty = Ty{Type::Nothing}) {
    if (items.empty())
      return Operand{OperandKind::Written, 0, "", typeRef(empty)};
    if (items.size() == 1)
      return operandOf(*items[0]);

    std::vector<Operand> pieces;
    for (const TypedPtr &item : items)
      pieces.push_back(operandOf(*item));
    const Ty text{Type::Str};
    const unsigned into = owningTemporary(text);
    emit(Statement{StatementKind::Assign, span, into, {}, {},
                   RValue{RValueKind::Join, {}, {}, 0, std::move(pieces), typeRef(text)}});
    return Operand{OperandKind::Move, into, {}, typeRef(text)};
  }

  // `taking` is for `give`, which needs no `move` written but takes all the
  // same, so what it answers with leaves rather than being read in place.
  void assignInto(unsigned place, const std::vector<TypedPtr> &items, Span span,
                  bool taking = false) {
    // Past the `or-nothing` as well as the loan: a name that may hold nothing
    // may hold a `many`, and the items still belong in its places rather than
    // joined into one. Stopping at the loan sent `or-nothing.many.int64` down
    // the joining path and made three numbers into the text "123".
    const Ty inside = owned(holds_[place]).within();
    if (shapeOf(inside)) {
      // The struct's own type, not the name's: a name that may hold nothing is
      // filled with the thing and then wrapped, and saying `or-nothing tag`
      // here had the group built as though the absence were one of its fields.
      groupInto(place, items, span, inside);
      return;
    }
    if (inside.holds()) {
      collectInto(place, items, span, inside);
      return;
    }
    if (items.empty())
      return;
    Operand operand = valueOperand(items, span);
    if (taking && operand.kind == OperandKind::Copy &&
        operand.local < body_.locals.size() && !body_.locals[operand.local].copies)
      operand.kind = OperandKind::Move;
    const TypeRef type = operand.type;
    emit(Statement{StatementKind::Assign, span, place, {}, {},
                   RValue{RValueKind::Use, {}, {}, 0, {std::move(operand)}, type}});
  }

  // One item for each of the things a struct holds, in the order it holds them.
  void groupInto(unsigned place, const std::vector<TypedPtr> &items, Span span,
                 Ty shape) {
    if (items.empty())
      return;
    // A lone item that is already the whole struct is the whole struct. Asking
    // only whether it *is a* struct was a weaker question than the one that had
    // to be answered: a struct holding one struct is filled by one item that is
    // a struct too, and reading that as the whole thing put the inner one where
    // the outer belonged. A level went missing, reaching into it read a field of
    // the wrong struct, and writing through a borrowed field two deep changed
    // nothing at all — with every engine agreeing, because they were all handed
    // the same wrong middle layer.
    if (items.size() == 1 && owned(items[0]->type).within() == shape) {
      Operand whole = operandOf(*items[0]);
      const TypeRef type = whole.type;
      emit(Statement{StatementKind::Assign, span, place, {}, {},
                     RValue{RValueKind::Use, {}, {}, 0, {std::move(whole)}, type}});
      return;
    }
    std::vector<Operand> parts;
    for (const TypedPtr &one : items)
      parts.push_back(operandOf(*one));
    emit(Statement{StatementKind::Assign, span, place, {}, {},
                   RValue{RValueKind::Group, {}, {}, 0, std::move(parts),
                          typeRef(shape)}});
  }

  // Items side by side under a `many` stay several. A lone item that is already
  // the whole array is the whole array — which is what the checker settled, so
  // nothing here has to settle it again.
  void collectInto(unsigned place, const std::vector<TypedPtr> &items, Span span,
                   Ty array) {
    std::vector<Operand> parts;
    // A lone item that is already the whole array is the whole array. Asking
    // only whether it is *a* `many` was right while a `many` held one level:
    // one row of a `many` of a `many` is a `many` too, so `[[*ab*]]` put the
    // row itself where the array goes, and letting go of it walked one `str`
    // as though it were an array of them.
    if (items.size() == 1 && items[0]->type.holds() && owned(items[0]->type) == array) {
      Operand operand = operandOf(*items[0]);
      const TypeRef type = operand.type;
      emit(Statement{StatementKind::Assign, span, place, {}, {},
                     RValue{RValueKind::Use, {}, {}, 0, {std::move(operand)}, type}});
      return;
    }
    for (const TypedPtr &item : items)
      parts.push_back(operandOf(*item));
    emit(Statement{StatementKind::Assign, span, place, {}, {},
                   RValue{RValueKind::Collect, {}, {}, 0, std::move(parts),
                          typeRef(array)}});
  }

  // ---- statements

  // A condition that lends what it holds: the test is whether there is anything
  // there, and the name is bound to it inside the arm. Written once, because
  // `if` and `loop.while` ask it the same way.
  Operand testing(const TypedExpr &condition, const std::string &holds,
                  unsigned &carried, Ty &held) {
    if (holds.empty())
      return operandOf(condition);
    carried = lower(condition);
    held = holds_[carried];
    const Ty truth{Type::Bool};
    const unsigned answer = addLocal("", truth);
    emit(Statement{StatementKind::Assign, condition.span, answer, {}, {},
                   RValue{RValueKind::Holds, {}, {}, 0, {reading(carried)},
                          typeRef(truth)}});
    return reading(answer);
  }

  // One target per case, chosen by which case the value is in. The same
  // terminator two shapes use, with as many arms as the type names.
  void whenOverASum(const TypedStmt &s, unsigned subject, const Shape &sum,
                    unsigned after) {
    const Ty counting{Type::Int64};
    const Ty truth{Type::Bool};
    const unsigned tag = addLocal("", counting);
    emit(Statement{StatementKind::Assign, s.condition->span, tag, {}, {},
                   RValue{RValueKind::Which, {}, {}, 0, {reading(subject)},
                          typeRef(counting)}});
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
      const unsigned matches = addLocal("", truth);
      emit(Statement{StatementKind::Assign, s.span, matches, {}, {},
                     RValue{RValueKind::Binary, "==", {}, 0,
                            {reading(tag),
                             Operand{OperandKind::Written, 0, std::to_string(i),
                                     typeRef(counting)}},
                            typeRef(truth)}});
      const unsigned next = i + 2 < sum.fields.size() ? addBlock() : targets.back();
      finish(Terminator{TerminatorKind::Switch, s.span, reading(matches), {"true"},
                        {targets[i], next}, false, {}});
      current_ = next;
    }

    for (const TypedArm &arm : s.arms) {
      if (!arm.chosen || arm.which >= targets.size())
        continue;
      current_ = targets[arm.which];
      openScope();
      if (!arm.binds.empty()) {
        // What the case holds, held the way a name that stands for it is: read
        // out where it copies, lent where it does not.
        const Ty inner = owned(sum.fields[arm.which].type);
        const Ty as = copies(inner) ? inner : lent(inner, Held::Loan);
        const unsigned into = addLocal(arm.binds, as);
        emit(Statement{StatementKind::Assign, arm.bindsSpan, into, {}, {},
                       RValue{RValueKind::Inside, {}, {}, 0, {reading(subject)},
                              typeRef(as)}});
        names_.back()[arm.binds] = into;
      }
      for (const TypedStmtPtr &inner : arm.body.stmts)
        statement(*inner);
      closeScope();
      finish(Terminator{TerminatorKind::Goto, arm.span, {}, {}, {after}, false, {}});
    }
    current_ = after;
  }

  // Inside the arm, the name stands for what was there. It is lent rather than
  // taken, so nothing is dropped through it.
  void bindHeld(const std::string &holds, unsigned carried, Ty carries, Span where) {
    if (holds.empty())
      return;
    // What is inside, once the borrow is off it. The subject may be a borrow
    // already — reaching into one of the things a struct holds lends it where
    // it stands — and asking what is inside a `loan or-nothing int8` without
    // taking the loan off first gave `loan loan or-nothing int8`, a pointer to
    // a pointer that no engine could read.
    const Ty inner = owned(carries).within();
    const Ty as = copies(inner) ? inner : lent(inner, Held::Loan);
    const unsigned into = addLocal(holds, as);
    emit(Statement{StatementKind::Assign, where, into, {}, {},
                   RValue{RValueKind::Inside, {}, {}, 0, {reading(carried)},
                          typeRef(as)}});
    names_.back()[holds] = into;
  }

  void statement(const TypedStmt &s) {
    // Held across the whole statement, because the word is written where the
    // name is and the sum is somewhere inside what the name is given.
    const bool wrapped = wrapsHere_;
    wrapsHere_ = s.wrapping;
    const Wrapping restore{wrapped, *this};
    switch (s.kind) {
    case TypedStmtKind::Declare: {
      const unsigned local = addLocal(s.name, s.type);
      assignInto(local, s.value, s.span);
      names_.back()[s.name] = local;
      if (!body_.locals[local].copies && s.type.held == Held::Owned)
        scopes_.back().push_back(local);
      break;
    }

    case TypedStmtKind::Add: {
      const unsigned *local = findName(s.name);
      if (!local)
        break;
      const Ty place = placeOf(holds_[*local]);
      // What goes in a new place is read the same way as what goes in an
      // existing one, which is the same way as what goes into a name.
      const Ty inside = place.within();
      Operand what;
      if (inside.holds() || shapeOf(inside)) {
        const unsigned into = owningTemporary(place);
        assignInto(into, s.value, s.span);
        what = Operand{OperandKind::Move, into, {}, typeRef(place)};
      } else {
        what = valueOperand(s.value, s.span, place);
      }
      emit(Statement{StatementKind::Grow, s.span, *local, {}, {},
                     RValue{RValueKind::Use, {}, {}, 0, {std::move(what)},
                            typeRef(place)}});
      break;
    }

    case TypedStmtKind::Set: {
      const unsigned *local = findName(s.name);
      if (!local)
        break;
      // Which of the things it holds is written, carried as numbers so that
      // nothing further down reads a name.
      if (!s.fields.empty()) {
        // Which of them each step is came from the checker, which walked this
        // path to work out what is being written to. Walking it again here by
        // matching names was one more answer to a question already answered.
        std::vector<unsigned> parts;
        std::vector<Ty> along;    // what each step is, in the order taken
        Ty held = owned(holds_[*local]);
        Ty declared;              // the last step as the struct says it holds it
        for (const unsigned which : s.path) {
          const Shape *shape = shapeOf(held);
          if (!shape || which >= shape->fields.size())
            break;
          parts.push_back(which);
          declared = shape->fields[which].type;
          held = owned(declared);
          // Every step is reached by lending it where it stands; the last one
          // is lent the way the struct says it holds it.
          along.push_back(declared.held == Held::Owned ? lent(held, Held::Loan)
                                                       : declared);
        }

        // A field that is a borrow holds the pointer, so writing to it means
        // writing through it — putting the value into the field's own slot
        // would put a number where an address goes, which is what happened:
        // the compiler took it, the struct was quietly wrecked, and every
        // engine agreed that the thing being written to had not changed.
        //
        // Read out and written through, which is the shape a borrowed name
        // already takes and both backends already know.
        if (!parts.empty() && parts.size() == s.fields.size() &&
            declared.held != Held::Owned) {
          unsigned at = *local;
          for (unsigned step = 0; step < parts.size(); ++step) {
            const unsigned into = addLocal("", along[step]);
            emit(Statement{StatementKind::Assign, s.span, into, {}, {},
                           RValue{RValueKind::Part, s.fields[step], {}, parts[step],
                                  {reading(at)}, typeRef(along[step])}});
            at = into;
          }
          Operand through = valueOperand(s.value, s.span, held);
          const TypeRef what = through.type;
          emit(Statement{StatementKind::Assign, s.span, at, {}, {},
                         RValue{RValueKind::Use, {}, {}, 0, {std::move(through)}, what}});
          break;
        }

        Operand what = valueOperand(s.value, s.span, held);
        const TypeRef type = what.type;
        emit(Statement{StatementKind::Assign, s.span, *local, std::move(parts), {},
                       RValue{RValueKind::Use, {}, {}, 0, {std::move(what)}, type}});
        break;
      }

      if (s.index) {
        const Ty place = placeOf(holds_[*local]);
        Operand at = operandOf(*s.index);
        Operand value;
        // What goes in a place may itself be several values, or a group of
        // them, and building one of those needs the same reading that building
        // a name does. Read as a lone value instead, `set 'w'[*1*] = [*x* *y*]`
        // joined two pieces of text and put the joined one where a `many str`
        // goes.
        const Ty inside = place.within();
        if (inside.holds() || shapeOf(inside)) {
          const unsigned into = owningTemporary(place);
          assignInto(into, s.value, s.span);
          value = Operand{OperandKind::Move, into, {}, typeRef(place)};
        } else {
          value = valueOperand(s.value, s.span, place);
        }
        emit(Statement{StatementKind::Store, s.span, *local, {}, std::move(at),
                       RValue{RValueKind::Use, {}, {}, 0, {std::move(value)},
                              typeRef(place)}});
        break;
      }
      assignInto(*local, s.value, s.span);
      break;
    }

    case TypedStmtKind::When: {
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
      const Ty carried = holds_[subject];
      if (const Shape *sum = sumOf(owned(carried))) {
        whenOverASum(s, subject, *sum, after);
        break;
      }
      const Ty truth{Type::Bool};
      const unsigned answer = addLocal("", truth);
      emit(Statement{StatementKind::Assign, s.condition->span, answer, {}, {},
                     RValue{RValueKind::Holds, {}, {}, 0, {reading(subject)},
                            typeRef(truth)}});

      const unsigned something = addBlock();
      const unsigned none = addBlock();
      finish(Terminator{TerminatorKind::Switch, s.span, reading(answer), {"true"},
                        {something, none}, false, {}});

      for (const TypedArm &arm : s.arms) {
        current_ = arm.matchesNothing ? none : something;
        openScope();
        if (!arm.matchesNothing)
          bindHeld(arm.binds, subject, carried, arm.bindsSpan);
        for (const TypedStmtPtr &inner : arm.body.stmts)
          statement(*inner);
        closeScope();
        finish(Terminator{TerminatorKind::Goto, arm.span, {}, {}, {after}, false, {}});
      }
      current_ = after;
      break;
    }

    // Nothing of its own to lower: what it grants is asked for by name, and the
    // asking is carried on the loop that asked.
    case TypedStmtKind::Unsafe:
      for (const TypedStmtPtr &inner : s.body.stmts)
        statement(*inner);
      break;

    case TypedStmtKind::If: {
      const unsigned after = addBlock();
      for (const TypedArm &arm : s.arms) {
        if (!arm.condition) {
          openScope();
          for (const TypedStmtPtr &inner : arm.body.stmts)
            statement(*inner);
          closeScope();
          finish(Terminator{TerminatorKind::Goto, arm.span, {}, {}, {after}, false, {}});
          current_ = after;
          return;
        }
        unsigned carried = 0;
        Ty held;
        const Operand condition = testing(*arm.condition, arm.binds, carried, held);
        const unsigned taken = addBlock();
        const unsigned otherwise = addBlock();
        finish(Terminator{TerminatorKind::Switch, arm.span, condition, {"true"},
                          {taken, otherwise}, false, {}});
        current_ = taken;
        openScope();
        bindHeld(arm.binds, carried, held, arm.bindsSpan);
        for (const TypedStmtPtr &inner : arm.body.stmts)
          statement(*inner);
        closeScope();
        finish(Terminator{TerminatorKind::Goto, arm.span, {}, {}, {after}, false, {}});
        current_ = otherwise;
      }
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {after}, false, {}});
      current_ = after;
      break;
    }

    case TypedStmtKind::LoopRange: {
      const Ty type = s.type;
      const unsigned counter = addLocal(s.name, type);
      // `perm` keeps the counter, so the name is put where the loop is rather
      // than inside it.
      if (s.keepsCounter)
        names_.back()[s.name] = counter;
      const unsigned last = addLocal("", type);
      if (s.from && s.to) {
        assignOne(counter, *s.from, s.span);
        assignOne(last, *s.to, s.span);
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
      if (s.noItmt)
        body_.blocks[header].noItmt = true;
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});

      const Ty truth{Type::Bool};
      current_ = header;
      const unsigned more = addLocal("", truth);
      emit(Statement{StatementKind::Assign, s.span, more, {}, {},
                     RValue{RValueKind::Binary, "<==", {}, 0,
                            {reading(counter), reading(last)}, typeRef(truth)}});
      finish(Terminator{TerminatorKind::Switch, s.span, reading(more), {"true"},
                        {inside, after}, false, {}});

      current_ = inside;
      loops_.push_back(Loop{step, after});
      openScope();
      if (!s.keepsCounter)
        names_.back()[s.name] = counter;
      for (const TypedStmtPtr &inner : s.body.stmts)
        statement(*inner);
      closeScope();
      loops_.pop_back();
      // The header has already said the counter is at or before the end, so
      // being at it means this was the last turn.
      const unsigned done = addLocal("", truth);
      emit(Statement{StatementKind::Assign, s.span, done, {}, {},
                     RValue{RValueKind::Binary, ">==", {}, 0,
                            {reading(counter), reading(last)}, typeRef(truth)}});
      finish(Terminator{TerminatorKind::Switch, s.span, reading(done), {"true"},
                        {after, step}, false, {}});

      current_ = step;
      emit(Statement{StatementKind::Assign, s.span, counter, {}, {},
                     RValue{RValueKind::Binary, "+", {}, 0,
                            {reading(counter),
                             // The step is a number of the counter's own type,
                             // whatever size the counter was written with.
                             Operand{OperandKind::Written, 0, "1",
                                     body_.locals[counter].type}},
                            body_.locals[counter].type}});
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});
      current_ = after;
      break;
    }

    case TypedStmtKind::LoopWhile: {
      const unsigned header = addBlock();
      const unsigned inside = addBlock();
      const unsigned after = addBlock();
      if (s.noItmt)
        body_.blocks[header].noItmt = true;
      body_.blocks[header].mayNotFinish = true;
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});

      current_ = header;
      unsigned carried = 0;
      Ty held;
      const Operand condition =
          s.condition ? testing(*s.condition, s.binds, carried, held)
                      : Operand{OperandKind::Written, 0, "true", typeRef(Ty{Type::Bool})};
      finish(Terminator{TerminatorKind::Switch, s.span, condition, {"true"},
                        {inside, after}, false, {}});

      current_ = inside;
      loops_.push_back(Loop{header, after});
      openScope();
      bindHeld(s.binds, carried, held, s.bindsSpan);
      for (const TypedStmtPtr &inner : s.body.stmts)
        statement(*inner);
      closeScope();
      loops_.pop_back();
      finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {header}, false, {}});
      current_ = after;
      break;
    }

    case TypedStmtKind::Break:
      if (!loops_.empty()) {
        finish(Terminator{TerminatorKind::Goto, s.span, {}, {}, {loops_.back().after},
                          false, {}});
        current_ = addBlock(); // anything after a break is its own unreached block
      }
      break;

    case TypedStmtKind::Give: {
      if (!s.value.empty()) {
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

    case TypedStmtKind::Call:
      if (s.call)
        (void)lower(*s.call);
      break;
    }
  }

  void assignOne(unsigned place, const TypedExpr &e, Span span) {
    Operand operand = operandOf(e);
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

MirResult build(const Source &source, const TypedProgram &program) {
  (void)source; // spans in the IR already carry everything a diagnostic needs
  MirResult result = Builder(program).run();
  result.mir.shapes = program.shapes;
  result.mir.sums = program.sums;
  result.mir.library = program.library;
  result.mir.fieldTypes = fieldsOfEveryShape(program.shapes);
  result.mir.caseTypes = fieldsOfEveryShape(program.sums);
  return result;
}

MirResult build(const Source &source, const Program &program,
                const CheckResult &checked) {
  TypedResult typed = typedTree(source, program, checked);
  MirResult result = build(source, typed.program);
  for (Diagnostic &one : typed.diagnostics)
    result.diagnostics.push_back(std::move(one));
  return result;
}

} // namespace xag
