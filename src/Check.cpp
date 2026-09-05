#include "xag/Check.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace xag {

const char *name(Type type) {
  switch (type) {
  case Type::I64:     return "i64";
  case Type::Str:     return "str";
  case Type::Bool:    return "bool";
  case Type::Nothing: return "nothing";
  case Type::Unknown: return "unknown";
  }
  return "unknown";
}

namespace {

Type typeNamed(std::string_view word) {
  if (word == "i64") return Type::I64;
  if (word == "str") return Type::Str;
  if (word == "bool") return Type::Bool;
  if (word == "nothing") return Type::Nothing;
  return Type::Unknown;
}

bool looksLikeWholeNumber(std::string_view text) {
  if (text.empty())
    return false;
  unsigned i = text[0] == '-' ? 1 : 0;
  if (i == text.size())
    return false;
  for (; i < text.size(); ++i)
    if (text[i] < '0' || text[i] > '9')
      return false;
  return true;
}

struct Symbol {
  Type type = Type::Unknown;
  bool changeable = false;
  Span span;
};

struct Signature {
  std::vector<Type> params;
  Type result = Type::Nothing;
  bool variadic = false;
  Span span;
};

class Checker {
public:
  Checker(const Source &source, const Program &program)
      : source_(source), program_(program) {}

  CheckResult run() {
    // Every signature is read before any body is, so two functions may call
    // each other and a constant may be used above where it stands.
    scopes_.emplace_back();
    collect();
    for (const Item &item : program_.items)
      body(item);
    return std::move(result_);
  }

private:
  const Source &source_;
  const Program &program_;
  CheckResult result_;
  std::vector<std::unordered_map<std::string, Symbol>> scopes_;
  std::unordered_map<std::string, Signature> functions_;
  Type giving_ = Type::Nothing;
  bool inFunction_ = false;
  unsigned loopDepth_ = 0;

  void complain(Span span, std::string code, std::string message,
                std::vector<std::string> rules, std::vector<std::string> tips = {},
                std::string label = "here") {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             std::move(label), std::move(rules),
                                             std::move(tips), {}});
  }

  // ---- names

  void declare(const std::string &text, Symbol symbol) {
    auto &scope = scopes_.back();
    auto found = scope.find(text);
    if (found != scope.end()) {
      complain(symbol.span, "E0502", "`'" + text + "'` is already a name here.",
               {"a name means one thing for as long as it stands"},
               {"an inner block may take the name again; the same block may not."});
      return;
    }
    scope.emplace(text, symbol);
  }

  const Symbol *lookup(const std::string &text) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      auto found = scope->find(text);
      if (found != scope->end())
        return &found->second;
    }
    return nullptr;
  }

  Type typeOfChain(const Chain &chain) {
    if (chain.segments.empty())
      return Type::Unknown;
    const ChainSegment &last = chain.type();
    if (last.isName) {
      complain(last.span, "E0503", "a chain ends in a type, and a loan is not one.",
               {"the segment nearest the name is the type"});
      return Type::Unknown;
    }
    const Type type = typeNamed(last.text);
    if (type == Type::Unknown)
      complain(last.span, "E0503", "`" + last.text + "` is not a type.",
               {"the types are `i64`, `str`, `bool` and `nothing`"});
    return type;
  }

  // `mut` on what a name owns, `refmut` on what it borrows. `ref` lends without
  // letting go of that, and a bare chain changes nothing at all.
  static bool changeable(const Chain &chain) {
    for (const ChainSegment &seg : chain.segments)
      if (!seg.isName && (seg.text == "mut" || seg.text == "refmut"))
        return true;
    return false;
  }

  static std::string joined(const std::vector<std::string> &path) {
    std::string out;
    for (const std::string &part : path)
      out += (out.empty() ? "" : ".") + part;
    return out;
  }

  // ---- gathering what stands at the top of the file

  void collect() {
    functions_["print.stdout"] = Signature{{}, Type::Nothing, true, Span{}};
    functions_["count"] = Signature{{Type::Str}, Type::I64, false, Span{}};

    for (const Item &item : program_.items) {
      if (item.kind == ItemKind::Const) {
        declare(item.name, Symbol{typeOfChain(item.chain), false, item.nameSpan});
      } else if (item.kind == ItemKind::Function) {
        Signature signature;
        signature.result = typeOfChain(item.chain);
        signature.span = item.nameSpan;
        for (const Param &param : item.params)
          signature.params.push_back(typeOfChain(param.chain));
        if (functions_.count(item.name))
          complain(item.nameSpan, "E0502", "`" + item.name + "` is already a function.",
                   {"a word names one function for the whole file"});
        else
          functions_[item.name] = std::move(signature);
      }
    }
  }

  // ---- expressions

  Type expr(const Expr &e, Type expected) {
    switch (e.kind) {
    case ExprKind::Name: {
      const Symbol *symbol = lookup(e.text);
      if (!symbol) {
        complain(e.span, "E0501", "`'" + e.text + "'` is not declared.",
                 {"a name means something only after a declaration says what it means"});
        return Type::Unknown;
      }
      return symbol->type;
    }

    case ExprKind::Written:
      if (expected == Type::Unknown) {
        complain(e.span, "E0507", "nothing here says what this written value is.",
                 {"a written value takes its type from the chain, from the parameter "
                  "it is passed to, or from itself"},
                 {"`*1000*` is a number under `i64` and four characters under `str`, "
                  "so a list with no chain and no declared parameters — a print — "
                  "leaves the value to say it."});
        return Type::Unknown;
      }
      if (expected == Type::I64 && !looksLikeWholeNumber(e.text))
        complain(e.span, "E0509", "`*" + e.text + "*` is not a whole number.",
                 {"a written value has to be one of the things its type holds"});
      if (expected == Type::Bool && e.text != "true" && e.text != "false")
        complain(e.span, "E0509", "`*" + e.text + "*` is not `true` or `false`.",
                 {"a written value has to be one of the things its type holds"});
      return expected;

    case ExprKind::Escape:
      return Type::Str;

    case ExprKind::Typed: {
      const Type stated = typeNamed(e.text);
      if (stated == Type::Unknown) {
        complain(e.span, "E0503", "`" + e.text + "` is not a type.",
                 {"the types are `i64`, `str`, `bool` and `nothing`"});
        return Type::Unknown;
      }
      if (!e.children.empty())
        expr(*e.children[0], stated);
      return stated;
    }

    case ExprKind::Borrow:
      // What a transfer means is the ownership pass's business; the type of the
      // thing transferred is the type of what it names.
      return e.children.empty() ? Type::Unknown : expr(*e.children[0], expected);

    case ExprKind::Group:
      return e.children.empty() ? Type::Unknown : expr(*e.children[0], expected);

    case ExprKind::Unary: {
      const Type inner = e.children.empty() ? Type::Unknown : expr(*e.children[0], Type::Bool);
      if (inner != Type::Unknown && inner != Type::Bool)
        complain(e.span, "E0506", "`not` asks about a `bool`, and this is a `" +
                                      std::string(name(inner)) + "`.",
                 {"nothing converts on its own"});
      return Type::Bool;
    }

    case ExprKind::Binary:
      return binary(e);

    case ExprKind::Call:
      return call(e);
    }
    return Type::Unknown;
  }

  Type binary(const Expr &e) {
    const std::string &op = e.text;
    const bool comparing = op == "<" || op == ">" || op == "<==" || op == ">==" ||
                           op == "==" || op == "!==";
    const bool logical = op == "and" || op == "or";
    const Type want = logical ? Type::Bool : (comparing ? Type::Unknown : Type::I64);

    const Type left = expr(*e.children[0], comparing ? Type::Unknown : want);
    const Type right = expr(*e.children[1], comparing ? left : want);

    if (comparing) {
      if (left != Type::Unknown && right != Type::Unknown && left != right)
        complain(e.span, "E0506",
                 "a `" + std::string(name(left)) + "` and a `" + std::string(name(right)) +
                     "` are not compared.",
                 {"two values are compared when they are the same kind of thing"},
                 {"nothing converts on its own, here or anywhere."});
      return Type::Bool;
    }

    for (const auto &[side, type] : {std::pair{"left", left}, std::pair{"right", right}}) {
      (void)side;
      if (type != Type::Unknown && type != want)
        complain(e.span, "E0506",
                 "`" + op + "` works on `" + std::string(name(want)) + "`, and this is a `" +
                     std::string(name(type)) + "`.",
                 {"nothing converts on its own"});
    }
    return want;
  }

  Type call(const Expr &e) {
    const std::string path = joined(e.path);
    auto found = functions_.find(path);
    if (found == functions_.end()) {
      complain(e.span, "E0504", "`" + path + "` is not a function.",
               {"a word followed by `[` is a call, and a call needs something to call"},
               {"a variable is a name and wears marks; a bare word is a function."});
      for (const Value &value : e.args.values)
        for (const ExprPtr &item : value.items)
          expr(*item, Type::Unknown);
      return Type::Unknown;
    }

    const Signature &signature = found->second;
    if (signature.variadic) {
      // A print states no parameter types, so each value must say what it is.
      for (const Value &value : e.args.values)
        for (const ExprPtr &item : value.items)
          expr(*item, Type::Unknown);
      return signature.result;
    }

    if (e.args.values.size() != signature.params.size()) {
      complain(e.span, "E0505",
               "`" + path + "` is given " + std::to_string(e.args.values.size()) +
                   " and wants " + std::to_string(signature.params.size()) + ".",
               {"a call gives a function what its parameters ask for"});
    }
    for (unsigned i = 0; i < e.args.values.size(); ++i) {
      const Type want = i < signature.params.size() ? signature.params[i] : Type::Unknown;
      const Type got = value(e.args.values[i], want);
      if (want != Type::Unknown && got != Type::Unknown && got != want)
        complain(e.args.values[i].span, "E0506",
                 "this is a `" + std::string(name(got)) + "` and `" + path + "` wants a `" +
                     std::string(name(want)) + "`.",
                 {"nothing converts on its own"});
    }
    return signature.result;
  }

  // A value is one item, or several joined. Joining builds text, so joined items
  // are text — except in a print, which writes them one after another and builds
  // nothing.
  Type value(const Value &v, Type expected) {
    if (v.items.empty())
      return Type::Unknown;
    if (v.items.size() == 1)
      return expr(*v.items[0], expected);

    for (const ExprPtr &item : v.items) {
      const Type got = expr(*item, Type::Str);
      if (got != Type::Unknown && got != Type::Str)
        complain(item->span, "E0506",
                 "this is a `" + std::string(name(got)) + "`, and text is made of text.",
                 {"pieces side by side join, and nothing converts on its own"},
                 {"a print shows any type because showing is not joining — it writes "
                  "one piece after another and builds nothing."});
    }
    return Type::Str;
  }

  Type onlyValue(const ValueList &list, Type expected) {
    if (list.values.empty())
      return Type::Unknown;
    return value(list.values[0], expected);
  }

  // ---- statements

  void block(const Block &b) {
    scopes_.emplace_back();
    for (const StmtPtr &s : b.stmts)
      statement(*s);
    scopes_.pop_back();
  }

  void statement(const Stmt &s) {
    switch (s.kind) {
    case StmtKind::Declare: {
      const Type type = typeOfChain(s.chain);
      onlyValueChecked(s.value, type, s.span);
      declare(s.name, Symbol{type, changeable(s.chain), s.nameSpan});
      break;
    }

    case StmtKind::Set: {
      const Symbol *symbol = lookup(s.name);
      if (!symbol) {
        complain(s.nameSpan, "E0501", "`'" + s.name + "'` is not declared.",
                 {"a name means something only after a declaration says what it means"});
        onlyValue(s.value, Type::Unknown);
        break;
      }
      if (!symbol->changeable)
        complain(s.nameSpan, "E0508", "`'" + s.name + "'` does not change.",
                 {"a name holds what it was given unless its chain said `mut`"},
                 {"a bare chain is the safest chain, and not changing is the safest "
                  "thing a name can do."});
      for (const ExprPtr &index : s.index)
        expr(*index, Type::I64);
      onlyValueChecked(s.value, symbol->type, s.span);
      break;
    }

    case StmtKind::If:
      for (const Branch &branch : s.branches) {
        if (branch.condition) {
          const Type type = expr(*branch.condition, Type::Bool);
          if (type != Type::Unknown && type != Type::Bool)
            complain(branch.condition->span, "E0506",
                     "an `if` asks a `bool`, and this is a `" + std::string(name(type)) + "`.",
                     {"nothing converts on its own"});
        }
        block(branch.body);
      }
      break;

    case StmtKind::LoopRange: {
      const Type type = typeOfChain(s.chain);
      if (s.value.values.size() != 2)
        complain(s.value.span, "E0505", "a counted loop runs between two values.",
                 {"`[first, last]` says where a count starts and stops"});
      for (const Value &v : s.value.values)
        value(v, type);
      scopes_.emplace_back();
      declare(s.name, Symbol{type, false, s.nameSpan});
      ++loopDepth_;
      for (const StmtPtr &inner : s.body.stmts)
        statement(*inner);
      --loopDepth_;
      scopes_.pop_back();
      break;
    }

    case StmtKind::LoopWhile: {
      if (s.condition) {
        const Type type = expr(*s.condition, Type::Bool);
        if (type != Type::Unknown && type != Type::Bool)
          complain(s.condition->span, "E0506",
                   "a `loop.while` asks a `bool`, and this is a `" +
                       std::string(name(type)) + "`.",
                   {"nothing converts on its own"});
      }
      ++loopDepth_;
      block(s.body);
      --loopDepth_;
      break;
    }

    case StmtKind::Break:
      if (loopDepth_ == 0)
        complain(s.span, "E0510", "there is no loop here to break out of.",
                 {"`break` stops the loop it stands in"});
      break;

    case StmtKind::Give:
      if (!inFunction_) {
        complain(s.span, "E0511", "there is nothing here to give an answer to.",
                 {"`give` answers the function it stands in, and `START` answers nobody"});
        onlyValue(s.value, Type::Unknown);
        break;
      }
      if (giving_ == Type::Nothing) {
        complain(s.span, "E0511", "this function answers `nothing`.",
                 {"a chain says what a function answers with, and `nothing` is a real "
                  "answer rather than an omission"});
        onlyValue(s.value, Type::Unknown);
        break;
      }
      onlyValueChecked(s.value, giving_, s.span);
      break;

    case StmtKind::Call:
      if (s.call)
        expr(*s.call, Type::Unknown);
      break;
    }
  }

  void onlyValueChecked(const ValueList &list, Type want, Span where) {
    if (list.values.empty())
      return;
    if (list.values.size() > 1)
      complain(list.span, "E0505", "one name takes one value.",
               {"a comma separates values, and there is one name here"});
    const Type got = value(list.values[0], want);
    if (want != Type::Unknown && got != Type::Unknown && got != want)
      complain(list.values[0].span.begin ? list.values[0].span : where, "E0506",
               "this is a `" + std::string(name(got)) + "` and a `" + std::string(name(want)) +
                   "` was wanted.",
               {"nothing converts on its own"});
  }

  // ---- items

  void body(const Item &item) {
    switch (item.kind) {
    case ItemKind::Const:
      onlyValueChecked(item.value, typeOfChain(item.chain), item.span);
      break;

    case ItemKind::Start:
      inFunction_ = false;
      giving_ = Type::Nothing;
      block(item.body);
      break;

    case ItemKind::Function: {
      inFunction_ = true;
      giving_ = typeOfChain(item.chain);
      scopes_.emplace_back();
      for (const Param &param : item.params)
        declare(param.name, Symbol{typeOfChain(param.chain), changeable(param.chain),
                                   param.nameSpan});
      for (const StmtPtr &s : item.body.stmts)
        statement(*s);
      scopes_.pop_back();
      inFunction_ = false;
      break;
    }
    }
    (void)source_;
  }
};

} // namespace

CheckResult check(const Source &source, const Program &program) {
  return Checker(source, program).run();
}

} // namespace xag
