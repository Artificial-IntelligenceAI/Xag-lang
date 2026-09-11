#include "xag/Typed.h"

#include <sstream>

namespace xag {
namespace {

// Building the typed tree is reading, not deciding. Every question this asks was
// answered by the checker and written down against the node that asked it — so
// where this looks something up rather than working it out, that is the point.
class Builder {
public:
  Builder(const Source &source, const Program &program, const CheckResult &checked)
      : source_(source), program_(program), checked_(checked) {}

  TypedResult run() {
    result_.program.shapes = checked_.shapes;
    result_.program.sums = checked_.sums;
    result_.program.library = program_.library;
    for (const Item &item : program_.items) {
      if (item.kind == ItemKind::Struct || item.kind == ItemKind::OneOf ||
          item.kind == ItemKind::Import)
        continue; // a type declares a shape, and an import names a unit; neither walks
      result_.program.items.push_back(item_(item));
    }
    (void)source_;
    return std::move(result_);
  }

private:
  const Source &source_;
  const Program &program_;
  const CheckResult &checked_;
  TypedResult result_;

  Ty typeOf(const Expr &e) const { return checked_.of(&e); }

  TypedPtr make(TypedKind kind, const Expr &e) {
    auto out = std::make_unique<TypedExpr>();
    out->kind = kind;
    out->span = e.span;
    out->type = typeOf(e);
    out->wrote = &e;
    return out;
  }

  // Which struct or `one-of` a word names, if either does.
  const Shape *shapeNamed(const std::string &word, bool &isSum) const {
    for (const Shape &shape : checked_.shapes)
      if (shape.name == word) {
        isSum = false;
        return &shape;
      }
    for (const Shape &sum : checked_.sums)
      if (sum.name == word) {
        isSum = true;
        return &sum;
      }
    return nullptr;
  }

  unsigned fieldNamed(Ty of, const std::string &field) const {
    const Type kind = of.holds() ? of.element : of.kind;
    if (kind != Type::Struct || of.named >= checked_.shapes.size())
      return 0;
    const Shape &shape = checked_.shapes[of.named];
    for (unsigned i = 0; i < shape.fields.size(); ++i)
      if (shape.fields[i].name == field)
        return i;
    return 0;
  }

  std::vector<TypedPtr> items(const Value &value) {
    std::vector<TypedPtr> out;
    for (const ExprPtr &one : value.items)
      out.push_back(expr_(*one));
    return out;
  }

  // `loan` around a value, as if the program had written it. An operator's
  // sides and a shown value are lent to the function that answers for them,
  // and the borrow is made here so that everything below sees an ordinary
  // call with ordinary loans. A side that is already a borrow is left as it is.
  static TypedPtr lent(TypedPtr inner) {
    if (inner->kind == TypedKind::Borrow)
      return inner;
    auto out = std::make_unique<TypedExpr>();
    out->kind = TypedKind::Borrow;
    out->span = inner->span;
    out->text = "loan";
    out->type = inner->type;
    out->type.held = Held::Loan;
    out->wrote = inner->wrote;
    out->children.push_back(std::move(inner));
    return out;
  }

  // A call to the function that answers for a declared type, with what it is
  // given lent.
  static TypedPtr answeredBy(const std::string &name, Ty answers, const Expr &e,
                             std::vector<TypedPtr> sides) {
    auto out = std::make_unique<TypedExpr>();
    out->kind = TypedKind::Call;
    out->span = e.span;
    out->type = answers;
    out->name = name;
    out->wrote = &e;
    for (TypedPtr &side : sides) {
      out->args.emplace_back();
      out->args.back().push_back(lent(std::move(side)));
    }
    return out;
  }

  TypedPtr expr_(const Expr &e) {
    // A value of a type that says how it is written, where it is shown: the
    // type's own `convert-to-str` is called on it, and what is shown is text.
    if (const auto shown = checked_.shownBy.find(&e); shown != checked_.shownBy.end()) {
      std::vector<TypedPtr> one;
      one.push_back(plain_(e));
      return answeredBy(shown->second, Ty{Type::Str}, e, std::move(one));
    }
    return plain_(e);
  }

  TypedPtr plain_(const Expr &e) {
    switch (e.kind) {
    case ExprKind::Group:
      // Brackets that only group are gone: grouping is the shape of the tree.
      return e.children.empty() ? make(TypedKind::Nothing, e) : expr_(*e.children[0]);

    case ExprKind::Name: {
      auto out = make(TypedKind::Name, e);
      out->text = e.text;
      return out;
    }

    case ExprKind::Written: {
      auto out = make(TypedKind::Written, e);
      out->text = e.text;
      return out;
    }

    case ExprKind::Escape: {
      auto out = make(TypedKind::Escape, e);
      out->text = e.text;
      return out;
    }

    case ExprKind::Nothing:
      return make(TypedKind::Nothing, e);

    case ExprKind::Typed: {
      // The same notation says two things and the checker has already said
      // which: a case of a `one-of`, or a written value wearing its own type.
      const auto made = checked_.cases.find(&e);
      if (made != checked_.cases.end()) {
        auto out = make(TypedKind::Case, e);
        out->which = made->second;
        out->sum = out->type.named;
        out->text = e.text;
        if (!e.children.empty())
          out->children.push_back(expr_(*e.children[0]));
        return out;
      }
      if (e.children.empty())
        return make(TypedKind::Nothing, e);
      // `str:*hi*` is a written value that knows what it is, so that is the one
      // node it becomes.
      TypedPtr inner = expr_(*e.children[0]);
      inner->type = typeOf(e);
      inner->span = e.span;
      return inner;
    }

    case ExprKind::Several: {
      auto out = make(TypedKind::Several, e);
      for (const ExprPtr &one : e.children)
        out->children.push_back(expr_(*one));
      return out;
    }

    case ExprKind::Field: {
      auto out = make(TypedKind::Field, e);
      if (!e.children.empty()) {
        out->children.push_back(expr_(*e.children[0]));
        out->which = fieldNamed(typeOf(*e.children[0]), e.text);
      }
      out->text = e.text;
      return out;
    }

    case ExprKind::Index: {
      auto out = make(TypedKind::Element, e);
      // What is reached into is written where there is no plain name for it:
      // `'g'[*1*][*2*]` reaches into what the first reach answered.
      if (e.children.size() > 1) {
        out->children.push_back(expr_(*e.children[1]));
      } else {
        auto named = std::make_unique<TypedExpr>();
        named->kind = TypedKind::Name;
        named->span = e.span;
        named->text = e.text;
        named->wrote = &e;
        out->children.push_back(std::move(named));
      }
      if (!e.children.empty())
        out->children.push_back(expr_(*e.children[0]));
      out->settled = checked_.settled.count(&e) != 0;
      return out;
    }

    case ExprKind::Borrow: {
      auto out = make(TypedKind::Borrow, e);
      out->text = e.text;
      if (!e.children.empty())
        out->children.push_back(expr_(*e.children[0]));
      return out;
    }

    case ExprKind::Unary: {
      auto out = make(TypedKind::Unary, e);
      out->text = e.text;
      if (!e.children.empty())
        out->children.push_back(expr_(*e.children[0]));
      return out;
    }

    case ExprKind::Binary: {
      // `'x' + 'y'` on two of a declared type is a call to the function that
      // answers `+` for it, both sides lent. Below here nobody knows an
      // operator was written, which is why no engine had to learn one.
      if (const auto answers = checked_.operatorCalls.find(&e);
          answers != checked_.operatorCalls.end() && e.children.size() == 2) {
        std::vector<TypedPtr> sides;
        for (const ExprPtr &one : e.children)
          sides.push_back(expr_(*one));
        return answeredBy(answers->second, typeOf(e), e, std::move(sides));
      }
      auto out = make(TypedKind::Binary, e);
      out->text = e.text;
      for (const ExprPtr &one : e.children)
        out->children.push_back(expr_(*one));
      return out;
    }

    case ExprKind::Call: {
      // A word before a bracket is one of three things, and which it is was
      // settled before this: a struct made where it stands, or a call.
      // The whole path: a library's struct made from outside it is
      // `lib.arith[…]`, two words, and the checker found its shape by the
      // joined name. Reading only a one-word path here sent it below as a
      // call to a function no engine had.
      bool isSum = false;
      const std::string word = joined(e.path);
      const Shape *shape = word.empty() ? nullptr : shapeNamed(word, isSum);
      if (shape && !isSum) {
        auto out = make(TypedKind::Made, e);
        out->text = word;
        // Which struct, not which name — the number is what everything below
        // the checker wants, and working the name out again is the thing this
        // layer exists to stop.
        out->which = static_cast<unsigned>(shape - checked_.shapes.data());
        if (!e.args.values.empty())
          out->children = items(e.args.values[0]);
        return out;
      }
      // `convert-to-str['n']` on a type with a `convert-to-str` of its own is
      // that function's call, not the built-in's: the argument was already
      // marked as shown by it, and wrapping the text it answers in the
      // built-in again would be converting text.
      if (e.path.size() == 1 && e.path[0] == "convert-to-str" && e.args.values.size() == 1 &&
          e.args.values[0].items.size() == 1 &&
          checked_.shownBy.count(e.args.values[0].items[0].get()))
        return expr_(*e.args.values[0].items[0]);
      auto out = make(TypedKind::Call, e);
      out->name = joined(e.path);
      for (const Value &value : e.args.values)
        out->args.push_back(items(value));
      return out;
    }
    }
    return make(TypedKind::Nothing, e);
  }

  static std::string joined(const std::vector<std::string> &path) {
    std::string out;
    for (const std::string &one : path)
      out += (out.empty() ? "" : ".") + one;
    return out;
  }

  std::vector<TypedPtr> valueOf(const ValueList &list) {
    std::vector<TypedPtr> out;
    for (const Value &value : list.values)
      for (const ExprPtr &one : value.items)
        out.push_back(expr_(*one));
    return out;
  }

  TypedArm arm(const Branch &branch, Ty subject) {
    TypedArm out;
    out.span = branch.span;
    out.always = !branch.condition && branch.family.empty() && !branch.matchesNothing &&
                 branch.holds.empty();
    if (branch.condition)
      out.condition = expr_(*branch.condition);
    out.binds = branch.holds;
    out.bindsSpan = branch.holdsSpan;
    out.matchesNothing = branch.matchesNothing;
    out.family = branch.family;
    const auto which = checked_.chosenCase.find(&branch);
    if (which != checked_.chosenCase.end()) {
      out.which = which->second;
      out.chosen = true;
      if (subject.kind == Type::OneOf && subject.named < checked_.sums.size() &&
          out.which < checked_.sums[subject.named].fields.size())
        out.bound = checked_.sums[subject.named].fields[out.which].type;
    } else if (!branch.holds.empty()) {
      out.bound = subject.within();
    }
    out.body = block(branch.body);
    return out;
  }

  TypedBlock block(const Block &body) {
    TypedBlock out;
    for (const StmtPtr &one : body.stmts)
      out.stmts.push_back(stmt(*one));
    return out;
  }

  // The lifetime a chain names, which is written like every other name because
  // it is one.
  static std::string loanOf(const Chain &chain) {
    for (const ChainSegment &seg : chain.segments)
      if (seg.isName)
        return seg.text;
    return {};
  }

  static bool chainSays(const Chain &chain, std::string_view word) {
    for (const ChainSegment &seg : chain.segments)
      if (!seg.isName && seg.text == word)
        return true;
    return false;
  }

  TypedStmtPtr stmt(const Stmt &s) {
    auto out = std::make_unique<TypedStmt>();
    out->span = s.span;
    out->name = s.name;
    out->nameSpan = s.nameSpan;
    out->wrote = &s;
    const auto said = checked_.declarations.find(&s);
    if (said != checked_.declarations.end())
      out->type = said->second;
    // Said where the name was declared, which for a `set` is somewhere else
    // entirely — so it comes from the checker rather than from this chain.
    out->wrapping = checked_.mayWrap.count(&s) != 0;

    switch (s.kind) {
    case StmtKind::Declare:
      out->kind = TypedStmtKind::Declare;
      out->changeable = chainSays(s.chain, "mut");
      out->value = valueOf(s.value);
      break;
    case StmtKind::Set:
    case StmtKind::Add: {
      out->kind = s.kind == StmtKind::Set ? TypedStmtKind::Set : TypedStmtKind::Add;
      if (s.index)
        out->index = expr_(*s.index);
      out->fields = s.fields;
      const auto steps = checked_.setPath.find(&s);
      if (steps != checked_.setPath.end())
        out->path = steps->second;
      out->value = valueOf(s.value);
      break;
    }
    case StmtKind::If:
      out->kind = TypedStmtKind::If;
      for (const Branch &branch : s.branches)
        out->arms.push_back(arm(branch, branch.condition ? typeOf(*branch.condition)
                                                         : Ty{}));
      break;
    case StmtKind::LoopRange:
      out->kind = TypedStmtKind::LoopRange;
      out->keepsCounter = chainSays(s.chain, "perm");
      out->noItmt = chainSays(s.chain, "no-itmt");
      if (s.value.values.size() == 2) {
        if (!s.value.values[0].items.empty())
          out->from = expr_(*s.value.values[0].items[0]);
        if (!s.value.values[1].items.empty())
          out->to = expr_(*s.value.values[1].items[0]);
      }
      out->body = block(s.body);
      break;
    case StmtKind::LoopWhile:
      out->kind = TypedStmtKind::LoopWhile;
      out->noItmt = chainSays(s.chain, "no-itmt");
      if (s.condition) {
        out->condition = expr_(*s.condition);
        out->bound = typeOf(*s.condition).within();
      }
      out->binds = s.holds;
      out->bindsSpan = s.holdsSpan;
      out->body = block(s.body);
      break;
    case StmtKind::When: {
      out->kind = TypedStmtKind::When;
      Ty subject;
      if (s.condition) {
        out->condition = expr_(*s.condition);
        subject = typeOf(*s.condition);
      }
      for (const Branch &branch : s.branches)
        out->arms.push_back(arm(branch, subject));
      break;
    }
    case StmtKind::Break:
      out->kind = TypedStmtKind::Break;
      break;
    case StmtKind::Give:
      out->kind = TypedStmtKind::Give;
      out->value = valueOf(s.value);
      break;
    case StmtKind::Unsafe:
      out->kind = TypedStmtKind::Unsafe;
      out->body = block(s.body);
      break;
    case StmtKind::Whichever:
    case StmtKind::LoopParts:
      // Both are gone before this: expansion rewrites them while the tree is
      // still the written one, and what is left here is what they became.
      out->kind = TypedStmtKind::Unsafe;
      out->body = block(s.body);
      break;
    case StmtKind::Call:
      out->kind = TypedStmtKind::Call;
      if (s.call)
        out->call = expr_(*s.call);
      break;
    }
    return out;
  }

  TypedItem item_(const Item &item) {
    TypedItem out;
    out.span = item.span;
    out.settings = item.settings;
    out.nameSpan = item.nameSpan;
    out.name = item.name;
    out.wrote = &item;
    const auto said = checked_.items.find(&item);
    if (said != checked_.items.end())
      out.answers = said->second;
    out.answersSpan = item.chain.span;
    out.loan = loanOf(item.chain);
    out.generic = chainSays(item.chain, "any");
    for (const Param &param : item.params)
      out.generic = out.generic || chainSays(param.chain, "any");
    switch (item.kind) {
    case ItemKind::Function:
      out.kind = TypedItemKind::Function;
      break;
    case ItemKind::Const:
      out.kind = TypedItemKind::Const;
      out.value = valueOf(item.value);
      return out;
    case ItemKind::Itmt:
      out.kind = TypedItemKind::Itmt;
      break;
    default:
      out.kind = TypedItemKind::Start;
      break;
    }
    for (const Param &param : item.params) {
      TypedParam one;
      one.span = param.span;
      one.nameSpan = param.nameSpan;
      one.name = param.name;
      const auto held = checked_.parameters.find(&param);
      if (held != checked_.parameters.end())
        one.type = held->second;
      one.loan = loanOf(param.chain);
      out.params.push_back(std::move(one));
    }
    out.body = block(item.body);
    return out;
  }
};

// ---- printing

class Printer {
public:
  std::string run(const TypedProgram &program) {
    for (const TypedItem &item : program.items)
      one(item);
    return out_.str();
  }

private:
  std::ostringstream out_;

  void pad(unsigned deep) {
    for (unsigned i = 0; i < deep; ++i)
      out_ << "  ";
  }

  // How it is held is part of what a node carries, and a `Ty` spells itself
  // without it — so the word goes on here, where a reader is looking to see
  // whether the answer on the node is the right one.
  static std::string spelled(Ty type) {
    const char *lent = type.held == Held::Loan      ? "loan "
                       : type.held == Held::LoanMut ? "loanmut "
                                                    : "";
    return lent + name(type);
  }

  static const char *named(TypedKind kind) {
    switch (kind) {
    case TypedKind::Name: return "name";
    case TypedKind::Written: return "written";
    case TypedKind::Escape: return "escape";
    case TypedKind::Nothing: return "nothing";
    case TypedKind::Hold: return "hold";
    case TypedKind::Case: return "case";
    case TypedKind::Made: return "made";
    case TypedKind::Several: return "several";
    case TypedKind::Collect: return "collect";
    case TypedKind::Join: return "join";
    case TypedKind::Element: return "element";
    case TypedKind::Field: return "field";
    case TypedKind::Borrow: return "borrow";
    case TypedKind::Call: return "call";
    case TypedKind::Unary: return "unary";
    case TypedKind::Binary: return "binary";
    }
    return "?";
  }

  void expr(const TypedExpr &e, unsigned deep) {
    pad(deep);
    out_ << named(e.kind);
    if (!e.text.empty())
      out_ << ' ' << e.text;
    if (!e.name.empty())
      out_ << ' ' << e.name;
    if (e.kind == TypedKind::Case || e.kind == TypedKind::Field ||
        e.kind == TypedKind::Made)
      out_ << " #" << e.which;
    out_ << " : " << spelled(e.type) << '\n';
    for (const TypedPtr &child : e.children)
      expr(*child, deep + 1);
    for (const std::vector<TypedPtr> &given : e.args)
      for (const TypedPtr &one : given)
        expr(*one, deep + 1);
  }

  void block(const TypedBlock &body, unsigned deep) {
    for (const TypedStmtPtr &one : body.stmts)
      stmt(*one, deep);
  }

  void stmt(const TypedStmt &s, unsigned deep) {
    pad(deep);
    switch (s.kind) {
    case TypedStmtKind::Declare:
      out_ << "declare '" << s.name << "' : " << spelled(s.type) << '\n';
      break;
    case TypedStmtKind::Set:
      out_ << "set '" << s.name << "'";
      // The names as written and which field each one is, side by side, so a
      // reader can see the numbers are the ones the checker worked out.
      for (unsigned at = 0; at < s.fields.size(); ++at) {
        out_ << " ." << s.fields[at];
        if (at < s.path.size())
          out_ << " #" << s.path[at];
      }
      out_ << '\n';
      break;
    case TypedStmtKind::Add: out_ << "add '" << s.name << "'\n"; break;
    case TypedStmtKind::If: out_ << "if\n"; break;
    case TypedStmtKind::LoopRange:
      out_ << "loop.range '" << s.name << "' : " << spelled(s.type) << '\n';
      break;
    case TypedStmtKind::LoopWhile: out_ << "loop.while\n"; break;
    case TypedStmtKind::When: out_ << "when\n"; break;
    case TypedStmtKind::Break: out_ << "break\n"; break;
    case TypedStmtKind::Give: out_ << "give\n"; break;
    case TypedStmtKind::Call: out_ << "call\n"; break;
    case TypedStmtKind::Unsafe: out_ << "unsafe\n"; break;
    }
    if (s.condition)
      expr(*s.condition, deep + 1);
    if (s.from)
      expr(*s.from, deep + 1);
    if (s.to)
      expr(*s.to, deep + 1);
    if (s.index)
      expr(*s.index, deep + 1);
    if (s.call)
      expr(*s.call, deep + 1);
    for (const TypedPtr &one : s.value)
      expr(*one, deep + 1);
    for (const TypedArm &one : s.arms) {
      pad(deep + 1);
      out_ << "arm";
      if (!one.family.empty())
        out_ << ' ' << one.family;
      if (one.chosen)
        out_ << " #" << one.which;
      if (!one.binds.empty())
        out_ << " '" << one.binds << "' : " << spelled(one.bound);
      if (one.matchesNothing)
        out_ << " nothing";
      if (one.always)
        out_ << " else";
      out_ << '\n';
      if (one.condition)
        expr(*one.condition, deep + 2);
      block(one.body, deep + 2);
    }
    block(s.body, deep + 1);
  }

  void one(const TypedItem &item) {
    switch (item.kind) {
    case TypedItemKind::Function:
      out_ << "fn " << item.name << " -> " << spelled(item.answers) << '\n';
      break;
    case TypedItemKind::Const:
      out_ << "const '" << item.name << "' : " << spelled(item.answers) << '\n';
      break;
    case TypedItemKind::Itmt:
      out_ << "ITMT\n";
      break;
    case TypedItemKind::Start:
      out_ << "START\n";
      break;
    }
    for (const TypedParam &param : item.params) {
      pad(1);
      out_ << "param '" << param.name << "' : " << spelled(param.type) << '\n';
    }
    for (const TypedPtr &value : item.value)
      expr(*value, 1);
    block(item.body, 1);
  }
};

} // namespace

TypedResult typedTree(const Source &source, const Program &program,
                      const CheckResult &checked) {
  return Builder(source, program, checked).run();
}

std::string printed(const TypedProgram &program) { return Printer().run(program); }

} // namespace xag
