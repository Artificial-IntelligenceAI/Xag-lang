#include "xag/Check.h"

#include "xag_runtime.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace xag {

namespace {

struct Named {
  const char *word;
  Type type;
  unsigned width;
};

// Every type there is, with the size it carries. Written once so that naming a
// type, spelling one, and asking how wide one is cannot drift apart.
constexpr Named kTypes[] = {
    {"nothing", Type::Nothing, 0}, {"bool", Type::Bool, 0}, {"str", Type::Str, 0},
    {"int8", Type::Int8, 8},       {"int16", Type::Int16, 16},
    {"int32", Type::Int32, 32},    {"int64", Type::Int64, 64},
    {"int128", Type::Int128, 128}, {"uint8", Type::Uint8, 8},
    {"uint16", Type::Uint16, 16},  {"uint32", Type::Uint32, 32},
    {"uint64", Type::Uint64, 64},  {"uint128", Type::Uint128, 128},
    {"bin16", Type::Bin16, 16},    {"bin32", Type::Bin32, 32},
    {"bin64", Type::Bin64, 64},    {"bin128", Type::Bin128, 128},
    {"deci32", Type::Deci32, 32},  {"deci64", Type::Deci64, 64},
    {"deci128", Type::Deci128, 128},
};

} // namespace

const char *name(Type type) {
  for (const Named &known : kTypes)
    if (known.type == type)
      return known.word;
  return "unknown";
}

// Named without the table to hand, which is every caller outside the checker.
// A struct is spelled by the name it was given, and that is set below.
// It holds the names rather than pointing at them: the checker that worked them
// out is gone by the time the middle layer asks, so pointing at what it held
// was reading freed memory, and a field of a struct came back spelled
// `unknown`.
std::vector<std::string> shapeNames;

const char *shapeName(unsigned which) {
  return which < shapeNames.size() ? shapeNames[which].c_str() : "unknown";
}

std::string name(Ty type) {
  const std::string one =
      type.kind == Type::Struct ? shapeName(type.named) : name(type.kind);
  const std::string inside =
      type.holds()
          ? std::string("many ") +
                (type.element == Type::Struct ? shapeName(type.named)
                                              : name(type.element))
          : one;
  return type.orNothing ? "or-nothing " + inside : inside;
}

Type typeNamed(std::string_view word) {
  for (const Named &known : kTypes)
    if (known.word == word)
      return known.type;
  return Type::Unknown;
}

unsigned widthOf(Type type) {
  for (const Named &known : kTypes)
    if (known.type == type)
      return known.width;
  return 0;
}

bool isSigned(Type type) {
  return type >= Type::Int8 && type <= Type::Int128;
}
bool isWhole(Type type) {
  return type >= Type::Int8 && type <= Type::Uint128;
}
bool isBinary(Type type) {
  return type >= Type::Bin16 && type <= Type::Bin128;
}
bool isDecimal(Type type) {
  return type >= Type::Deci32 && type <= Type::Deci128;
}
bool isNumber(Type type) { return isWhole(type) || isBinary(type) || isDecimal(type); }

namespace {

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

// Whether a written whole number is one the type can hold. A size that is
// always written is a size that can always be checked against.
bool fitsWithin(std::string_view text, Type type) {
  const unsigned width = widthOf(type);
  const bool negative = !text.empty() && text[0] == '-';
  if (negative && !isSigned(type))
    return false;

  __uint128_t magnitude = 0;
  const __uint128_t ceiling = isSigned(type)
                                  ? (static_cast<__uint128_t>(1) << (width - 1))
                                  : ~static_cast<__uint128_t>(0);
  for (unsigned i = negative ? 1 : 0; i < text.size(); ++i) {
    magnitude = magnitude * 10 + static_cast<unsigned>(text[i] - '0');
    if (width < 128 && magnitude > (static_cast<__uint128_t>(1) << width))
      return false;
  }
  if (isSigned(type))
    return negative ? magnitude <= ceiling : magnitude < ceiling;
  if (width == 128)
    return true;
  return magnitude < (static_cast<__uint128_t>(1) << width);
}

struct Symbol {
  Ty type;
  bool changeable = false;
  Span span;
};

struct Signature {
  std::vector<Ty> params;
  Ty result = Type::Nothing;
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
    collectShapes();
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
  std::vector<std::string> names_; // struct names, indexed the way `Ty` names them
  Ty giving_ = Type::Nothing;
  bool inFunction_ = false;
  unsigned loopDepth_ = 0;

  void complain(Span span, std::string code, std::string message,
                std::vector<std::string> rules, std::vector<std::string> tips = {},
                std::string label = "here", std::vector<Note> notes = {}) {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             std::move(label), std::move(rules),
                                             std::move(tips), std::move(notes)});
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

  // A type that is named but has nothing behind it yet is said so plainly,
  // rather than being let through to fail somewhere further down.
  Ty typeOfChain(const Chain &chain) {
    if (chain.segments.empty())
      return Type::Unknown;
    const ChainSegment &last = chain.type();
    if (last.isName) {
      complain(last.span, "E0503", "a chain ends in a type, and a loan is not one.",
               {"the segment nearest the name is the type"});
      return Type::Unknown;
    }
    const Type type = typeNamed(last.text);
    unsigned which = 0;
    bool isShape = false;
    if (type == Type::Unknown) {
      for (unsigned i = 0; i < result_.shapes.size(); ++i)
        if (result_.shapes[i].name == last.text) {
          which = i;
          isShape = true;
          break;
        }
      if (!isShape) {
        complain(last.span, "E0503", "`" + last.text + "` is not a type.",
                 {"a size is always written, and only sizes the standard defines"});
        return Type::Unknown;
      }
    }
    // `many` stands with the type and says the name holds several of it, and
    // `or-nothing` stands outside that and says the whole of it may be missing.
    // The parser has already refused a second level of either.
    const std::size_t n = chain.segments.size();
    std::size_t at = n - 1;
    bool several = false, orNothing = false;
    if (at > 0 && !chain.segments[at - 1].isName &&
        chain.segments[at - 1].text == "many") {
      several = true;
      --at;
    }
    if (at > 0 && !chain.segments[at - 1].isName &&
        chain.segments[at - 1].text == "or-nothing") {
      orNothing = true;
      --at;
    }

    if ((several || orNothing) && type == Type::Nothing) {
      complain(chain.segments[at].span, "E0503",
               several ? "there is no holding several of `nothing`."
                       : "`nothing` is already nothing, and cannot be it twice.",
               {"`nothing` is an answer, and not a value to keep"});
      return Type::Unknown;
    }

    Ty settled = isShape ? (several ? Ty{Type::Many, Type::Struct, false, which}
                                    : structNamed(which))
                         : (several ? many(type) : Ty{type});
    if (orNothing)
      settled = orNothingOf(settled);
    return settled;
  }

  // An expression whose type is its own, whatever was expected of it. It is
  // what lets `give ['xs']` hand an array over while `[*1* *2*]` builds one:
  // the item says which it is, rather than where it sits.
  static bool selfTyped(const Expr &e) {
    switch (e.kind) {
    case ExprKind::Name:
    case ExprKind::Borrow:
    case ExprKind::Call:
    case ExprKind::Index:
      return true;
    case ExprKind::Group:
      return !e.children.empty() && selfTyped(*e.children[0]);
    default:
      return false;
    }
  }

  // `mut` on what a name owns, `refmut` on what it borrows. `ref` lends without
  // letting go of that, and a bare chain changes nothing at all.
  // `perm` asks to keep the counter after the loop, which is what a `break`
  // leaves behind and the only reason to keep one at all.
  static bool keepsCounter(const Chain &chain) {
    for (const ChainSegment &seg : chain.segments)
      if (!seg.isName && seg.text == "perm")
        return true;
    return false;
  }

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

  // Structs are read before anything else, because a function's chain may name
  // one and a field's chain may name another.
  void collectShapes() {
    for (const Item &item : program_.items) {
      if (item.kind != ItemKind::Struct)
        continue;
      if (typeNamed(item.name) != Type::Unknown || item.name == "nothing") {
        complain(item.nameSpan, "E0524",
                 "`" + item.name + "` is already a type.",
                 {"a word names one thing for the whole file"});
        continue;
      }
      for (const Shape &already : result_.shapes)
        if (already.name == item.name) {
          complain(item.nameSpan, "E0502", "`" + item.name + "` is already a struct.",
                   {"a word names one thing for the whole file"});
        }
      result_.shapes.push_back(Shape{item.name, {}, item.nameSpan});
      names_.push_back(item.name);
    }
    shapeNames = names_;

    // The fields come second, so that one struct may name another.
    unsigned at = 0;
    for (const Item &item : program_.items) {
      if (item.kind != ItemKind::Struct)
        continue;
      Shape &shape = result_.shapes[at++];
      for (const Param &field : item.params) {
        for (const Field &already : shape.fields)
          if (already.name == field.name)
            complain(field.nameSpan, "E0502",
                     "`'" + field.name + "'` is already a field of `" + shape.name +
                         "`.",
                     {"a name means one thing for as long as it stands"});
        shape.fields.push_back(Field{field.name, typeOfChain(field.chain),
                                     field.nameSpan});
      }
      if (shape.fields.empty())
        complain(item.nameSpan, "E0525", "`" + shape.name + "` holds nothing.",
                 {"a struct is a group of named things"},
                 {"a group of none is `nothing`, which the language already has."});
    }

    // A struct that holds itself has no size a machine could give it.
    for (unsigned which = 0; which < result_.shapes.size(); ++which)
      if (reaches(which, which, 0))
        complain(result_.shapes[which].span, "E0526",
                 "`" + result_.shapes[which].name + "` holds itself.",
                 {"a struct is as big as the things in it"},
                 {"however many times it were laid out, there would always be one "
                  "more of it inside."});
  }

  // Whether one struct can be reached from another by walking fields.
  bool reaches(unsigned from, unsigned to, unsigned depth) const {
    if (depth > result_.shapes.size())
      return false;
    for (const Field &field : result_.shapes[from].fields) {
      const unsigned next = field.type.named;
      if (field.type.kind != Type::Struct && field.type.element != Type::Struct)
        continue;
      if (next == to || reaches(next, to, depth + 1))
        return true;
    }
    return false;
  }

  void collect() {
    functions_["print.stdout"] = Signature{{}, Type::Nothing, true, Span{}};
    // A line, or nothing left to read. The end of the input is not an empty
    // line: an empty line is something a program may legitimately read, and
    // telling the two apart is what the type is for.
    functions_["read.stdin"] = Signature{{}, orNothingOf(Ty{Type::Str}), false, Span{}};
    functions_["arguments"] = Signature{{}, many(Type::Str), false, Span{}};
    functions_["count"] = Signature{{Type::Str}, Type::Int64, false, Span{}};

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

  Ty expr(const Expr &e, Ty expected) {
    const Ty got = exprKind(e, expected);
    result_.expressions[&e] = got;
    return got;
  }

  Ty exprKind(const Expr &e, Ty expected) {
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
                 {"`*1000*` is a number under `int64` and four characters under `str`, "
                  "so a list with no chain and no declared parameters — a print — "
                  "leaves the value to say it."});
        return Type::Unknown;
      }
      if (isDecimal(expected)) {
        XagDeci read = 0;
        if (!xag_deci_reads(widthOf(expected), e.text.data(), e.text.size(), &read))
          complain(e.span, "E0509",
                   "`*" + e.text + "*` is not a number a `" + std::string(name(expected)) +
                       "` holds.",
                   {"a written value has to be one of the things its type holds"});
      } else if (expected == Type::Bin128) {
        XagBin128 read = 0;
        if (!xag_bin128_reads(e.text.data(), e.text.size(), &read))
          complain(e.span, "E0509",
                   "`*" + e.text + "*` is not a number a `bin128` holds.",
                   {"a written value has to be one of the things its type holds"});
      } else if (isBinary(expected)) {
        double read = 0;
        if (!xag_bin_reads(e.text.data(), e.text.size(), widthOf(expected), &read))
          complain(e.span, "E0509",
                   "`*" + e.text + "*` is not a number a `" + std::string(name(expected)) +
                       "` holds.",
                   {"a written value has to be one of the things its type holds"},
                   {"a number too large for the width would arrive as infinity, which "
                    "is not what was written down."});
      }
      if (isWhole(expected)) {
        if (!looksLikeWholeNumber(e.text))
          complain(e.span, "E0509", "`*" + e.text + "*` is not a whole number.",
                   {"a written value has to be one of the things its type holds"});
        else if (!fitsWithin(e.text, expected.kind))
          complain(e.span, "E0509",
                   "`*" + e.text + "*` does not fit in a `" + name(expected) + "`.",
                   {"a written value has to be one of the things its type holds"},
                   {"the size is written, so what will and will not go in it is "
                    "settled before the program runs."});
      }
      if (expected == Type::Bool && e.text != "true" && e.text != "false")
        complain(e.span, "E0509", "`*" + e.text + "*` is not `true` or `false`.",
                 {"a written value has to be one of the things its type holds"});
      return expected;

    case ExprKind::Escape:
      return Type::Str;

    case ExprKind::Typed: {
      const Ty stated = typeNamed(e.text);
      if (stated == Type::Unknown) {
        complain(e.span, "E0503", "`" + e.text + "` is not a type.",
                 {"a size is always written, and only sizes the standard defines"});
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
      const Ty inner = e.children.empty() ? Ty{} : expr(*e.children[0], Type::Bool);
      if (inner != Type::Unknown && inner != Type::Bool)
        complain(e.span, "E0506", "`not` asks about a `bool`, and this is a `" +
                                      std::string(name(inner)) + "`.",
                 {"nothing converts on its own"});
      return Type::Bool;
    }

    case ExprKind::Binary:
      return binary(e, expected);

    case ExprKind::Nothing:
      // An absence takes its type from what was expected of it, the way a
      // written value does: there is no telling a missing `str` from a missing
      // `int64` by looking at it.
      if (!expected.mayBeNothing()) {
        complain(e.span, "E0518",
                 expected.kind == Type::Unknown
                     ? "nothing here says what this `nothing` would be instead of."
                     : "a `" + name(expected) + "` is always something.",
                 {"a type says whether it may hold nothing"},
                 {"`or-nothing` in the chain is what makes room for this; without it "
                  "there is no absence for the name to be in."});
        return Type::Unknown;
      }
      return expected;

    case ExprKind::Field:
      return field(e);

    case ExprKind::Index:
      return element(e);

    case ExprKind::Call:
      return call(e, expected);
    }
    return Type::Unknown;
  }

  // `'p'.x` — which of the things a struct holds, and what that one is.
  Ty field(const Expr &e) {
    const Ty of = e.children.empty() ? Ty{} : expr(*e.children[0], Ty{});
    if (of == Ty{})
      return Ty{};
    if (!of.isStruct() || of.orNothing) {
      complain(e.span, "E0527",
               "a `" + name(of) + "` has no fields.",
               {"a field is one of the things a struct holds"},
               of.orNothing ? std::vector<std::string>{
                                  "what may hold nothing has to be asked before it "
                                  "can be reached into."}
                            : std::vector<std::string>{});
      return Ty{};
    }
    const Shape &shape = result_.shapes[of.named];
    for (const Field &one : shape.fields)
      if (one.name == e.text)
        return one.type;
    complain(e.span, "E0528",
             "`" + shape.name + "` has no field called `" + e.text + "`.",
             {"a field is one of the things a struct holds"},
             {"what it does hold is written where it was declared."});
    return Ty{};
  }

  // `'xs'[*2*]` — the place a value sits, and the type of what sits there.
  Ty element(const Expr &e) {
    if (!e.children.empty())
      expr(*e.children[0], Type::Int64);
    const Symbol *symbol = lookup(e.text);
    if (!symbol) {
      complain(e.span, "E0501", "`'" + e.text + "'` is not declared.",
               {"a name means something only after a declaration says what it means"});
      return Type::Unknown;
    }
    if (!symbol->type.holds()) {
      if (symbol->type.kind == Type::Unknown)
        return Type::Unknown;
      complain(e.span, "E0514",
               "`'" + e.text + "'` is a `" + name(symbol->type) +
                   "`, and holds one value rather than several.",
               {"an element is one of the values a `many` holds"},
               {"a name holding one value is that value, and there is no first of it."});
      return Type::Unknown;
    }
    if (!e.children.empty()) {
      const Ty where = result_.of(e.children[0].get());
      if (where != Ty{} && where != Ty{Type::Int64})
        complain(e.children[0]->span, "E0506",
                 "an index is an `int64`, and this is a `" + name(where) + "`.",
                 {"nothing converts on its own"},
                 {"`count` answers an `int64`, and two sizes never meet on their own."});
    }
    return Ty{symbol->type.element};
  }

  // What a condition has to be, which depends on whether a name is being lent
  // what it holds. Without `holds` it is a `bool`; with it, a thing that may
  // hold nothing — and asking a `bool` to hold something is its own mistake,
  // because a `bool` is never absent.
  Ty asked(const Expr &condition, const std::string &holds, Span where,
           const char *what) {
    if (holds.empty()) {
      const Ty type = expr(condition, Type::Bool);
      if (type != Ty{} && type != Ty{Type::Bool})
        complain(condition.span, "E0506",
                 std::string(what) + " asks a `bool`, and this is a `" + name(type) + "`.",
                 {"nothing converts on its own"});
      return type;
    }
    const Ty type = expr(condition, Ty{});
    if (type != Ty{} && !type.mayBeNothing())
      complain(where, "E0519",
               "`holds` lends what may not be there, and a `" + name(type) +
                   "` is always something.",
               {"a type says whether it may hold nothing"},
               {"without `or-nothing` there is no absence to ask about, so the arm "
                "would run every time and lend the same value every time."});
    return type;
  }

  Ty binary(const Expr &e, Ty expected) {
    const std::string &op = e.text;
    const bool comparing = op == "<" || op == ">" || op == "<==" || op == ">==" ||
                           op == "==" || op == "!==";
    const bool logical = op == "and" || op == "or";

    // Arithmetic answers with what it was given, so the type wanted here is the
    // type wanted of it — and the right side takes whatever the left turned out
    // to be, which is how a written value in a sum gets a size at all.
    const Ty asked = logical ? Type::Bool
                               : (comparing ? Type::Unknown
                                            : (isNumber(expected) ? expected : Type::Unknown));
    const Ty left = expr(*e.children[0], asked);
    const Ty right =
        expr(*e.children[1], comparing || (!logical && asked == Type::Unknown) ? left : asked);

    if (comparing) {
      if (left != Type::Unknown && right != Type::Unknown && left != right)
        complain(e.span, "E0506",
                 "a `" + std::string(name(left)) + "` and a `" + std::string(name(right)) +
                     "` are not compared.",
                 {"two values are compared when they are the same kind of thing"},
                 {"nothing converts on its own, here or anywhere."});
      return Type::Bool;
    }

    if (logical) {
      for (Ty side : {left, right})
        if (side != Type::Unknown && side != Type::Bool)
          complain(e.span, "E0506",
                   "`" + op + "` asks about a `bool`, and this is a `" +
                       std::string(name(side)) + "`.",
                   {"nothing converts on its own"});
      return Type::Bool;
    }

    const Ty answered = isNumber(left) ? left : right;
    for (Ty side : {left, right}) {
      if (side == Type::Unknown)
        continue;
      if (!isNumber(side))
        complain(e.span, "E0506",
                 "`" + op + "` works on numbers, and this is a `" +
                     std::string(name(side)) + "`.",
                 {"nothing converts on its own"});
      else if (side != answered)
        complain(e.span, "E0506",
                 "a `" + std::string(name(left)) + "` and a `" + std::string(name(right)) +
                     "` are not added, subtracted or multiplied together.",
                 {"two numbers meet when they are the same size and the same kind"},
                 {"a size is always written, so widening one is something a program "
                  "says rather than something that happens to it."});
    }
    return answered == Type::Unknown ? Type::Unknown : answered;
  }

  Ty call(const Expr &e, Ty expected) {
    const std::string path = joined(e.path);

    // `count` asks how many, of a `str` and of a `many` alike: the same
    // question, and the type already says what is being counted.
    if (path == "count" && e.args.values.size() == 1) {
      const Ty got = value(e.args.values[0], Ty{});
      if (got != Ty{} && got.kind != Type::Str && !got.holds())
        complain(e.args.values[0].span, "E0506",
                 "`count` counts a `str` or a `many`, and this is a `" + name(got) + "`.",
                 {"nothing converts on its own"});
      return Type::Int64;
    }

    // A number out of text, which is where text stops being text. Which number
    // is the question the chain beside it has already answered, the same way
    // `fill` knows what it is filling.
    if (path == "number") {
      const Ty wanted = expected.mayBeNothing() ? expected.within() : expected;
      if (!isNumber(wanted)) {
        complain(e.span, "E0523",
                 expected.kind == Type::Unknown
                     ? "nothing here says what number this would be."
                     : "`number` answers a number, and a `" + name(expected) +
                           "` is not one.",
                 {"a size is always written, and only sizes the standard defines"},
                 {"text that is not a number has no number in it, so this answers "
                  "`or-nothing` of whichever number was asked for — and the chain "
                  "beside it is what asks."});
        for (const Value &v : e.args.values)
          (void)value(v, Ty{});
        return Type::Unknown;
      }
      if (e.args.values.size() != 1)
        complain(e.span, "E0505",
                 "`number` is given " + std::to_string(e.args.values.size()) +
                     " and wants 1.",
                 {"a call gives a function what its parameters ask for"});
      if (!e.args.values.empty()) {
        const Ty got = value(e.args.values[0], Ty{Type::Str});
        if (got != Ty{} && got != Ty{Type::Str})
          complain(e.args.values[0].span, "E0506",
                   "`number` reads text, and this is a `" + name(got) + "`.",
                   {"nothing converts on its own"});
      }
      return orNothingOf(wanted);
    }

    // `fill` writes one value into every place, so it needs a value that can be
    // copied — there is no copying a `str`, and nothing to put in each place.
    if (path == "fill") {
      if (!expected.holds()) {
        complain(e.span, "E0507", "nothing here says what `fill` is filling.",
                 {"a written value takes its type from the chain, from the parameter "
                  "it is passed to, or from itself"},
                 {"`fill` answers a `many`, and which `many` is a question the chain "
                  "beside it has already answered everywhere it is allowed to stand."});
        for (const Value &v : e.args.values)
          (void)value(v, Ty{});
        return Type::Unknown;
      }
      const Ty holds{expected.element};
      if (e.args.values.size() != 2)
        complain(e.span, "E0505",
                 "`fill` is given " + std::to_string(e.args.values.size()) +
                     " and wants 2.",
                 {"a call gives a function what its parameters ask for"},
                 {"one value to put everywhere, and how many places to put it in."});
      if (!isNumber(holds) && holds.kind != Type::Bool)
        complain(e.span, "E0515",
                 "`fill` puts the same value in every place, and a `" + name(holds) +
                     "` cannot be in two places.",
                 {"a value that does not copy has one owner"},
                 {"a number or a `bool` is handed over by being copied; text is not, "
                  "so there is nothing to put in the second place."});
      if (!e.args.values.empty())
        (void)value(e.args.values[0], holds);
      if (e.args.values.size() > 1)
        (void)value(e.args.values[1], Type::Int64);
      return expected;
    }

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
      for (const Value &v : e.args.values)
        for (const ExprPtr &item : v.items) {
          const Ty got = expr(*item, Ty{});
          if (got.holds())
            complain(item->span, "E0516",
                     "a `" + name(got) + "` holds several values, and this shows one thing.",
                     {"showing writes one piece after another"},
                     {"what would stand between two of them is a decision nobody has "
                      "made, so nothing here makes it for you."});
        }
      return signature.result;
    }

    if (e.args.values.size() != signature.params.size()) {
      complain(e.span, "E0505",
               "`" + path + "` is given " + std::to_string(e.args.values.size()) +
                   " and wants " + std::to_string(signature.params.size()) + ".",
               {"a call gives a function what its parameters ask for"});
    }
    for (unsigned i = 0; i < e.args.values.size(); ++i) {
      const Ty want = i < signature.params.size() ? signature.params[i] : Ty{};
      const Ty got = value(e.args.values[i], want);
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
  Ty value(const Value &v, Ty expected) {
    // A `str` where a `str`-or-nothing was wanted is that `str`, held. Nothing
    // else it could mean, so nothing is written — the same reason `give` takes
    // no word.
    if (expected.mayBeNothing()) {
      if (v.items.size() == 1) {
        const Expr &only = *v.items[0];
        if (only.kind == ExprKind::Nothing) {
          (void)expr(only, expected);
          return expected;
        }
        // A lone item that already may hold nothing is the whole of it, the
        // same way a lone `many` is the whole array rather than one place.
        if (selfTyped(only)) {
          const Ty got = expr(only, expected);
          if (got == expected || got == Ty{} || got == expected.within())
            return expected;
          complain(only.span, "E0506",
                   "this is a `" + name(got) + "`, and a `" + name(expected) +
                       "` holds a `" + name(expected.within()) + "`.",
                   {"nothing converts on its own"});
          return expected;
        }
      }
      const Ty got = value(v, expected.within());
      if (got == Ty{} || got == expected.within())
        return expected;
      complain(v.span, "E0506",
               "this is a `" + name(got) + "`, and a `" + name(expected) +
                   "` holds a `" + name(expected.within()) + "`.",
               {"nothing converts on its own"});
      return expected;
    }
    if (expected.isStruct())
      return grouped(v, expected);
    if (expected.holds())
      return collected(v, expected);
    if (v.items.empty())
      return Type::Unknown;
    if (v.items.size() == 1)
      return expr(*v.items[0], expected);

    for (const ExprPtr &item : v.items) {
      const Ty got = expr(*item, Type::Str);
      if (got != Type::Unknown && got != Type::Str)
        complain(item->span, "E0506",
                 "this is a `" + std::string(name(got)) + "`, and text is made of text.",
                 {"pieces side by side join, and nothing converts on its own"},
                 {"a print shows any type because showing is not joining — it writes "
                  "one piece after another and builds nothing."});
    }
    return Type::Str;
  }

  // Items side by side under a `many`: kept as several rather than joined into
  // one. Which of the two happens is the type's answer, and the only one it
  // gives — a lone item that is already the whole array is the whole array,
  // because with one level of `many` nothing can be read both ways.
  Ty collected(const Value &v, Ty want) {
    const Ty holds{want.element};
    if (v.items.empty())
      return want;
    if (v.items.size() == 1 && selfTyped(*v.items[0])) {
      const Ty got = expr(*v.items[0], want);
      if (got == want || got == Ty{})
        return want;
      if (got != holds)
        complain(v.items[0]->span, "E0506",
                 "this is a `" + name(got) + "`, and a `" + name(want) + "` holds `" +
                     name(holds) + "`.",
                 {"nothing converts on its own"});
      return want;
    }
    for (const ExprPtr &item : v.items) {
      const Ty got = expr(*item, holds);
      if (got != Ty{} && got != holds)
        complain(item->span, "E0506",
                 "this is a `" + name(got) + "`, and a `" + name(want) + "` holds `" +
                     name(holds) + "`.",
                 {"nothing converts on its own"});
    }
    return want;
  }

  // Items side by side under a struct: one for each of the things it holds, in
  // the order it holds them. The same rule as everywhere — a value is a list of
  // items and the type says what they are — and a lone item that is already the
  // whole struct is the whole struct.
  Ty grouped(const Value &v, Ty want) {
    const Shape &shape = result_.shapes[want.named];
    if (v.items.size() == 1 && selfTyped(*v.items[0])) {
      const Ty got = expr(*v.items[0], want);
      if (got == want || got == Ty{})
        return want;
    } else if (v.items.size() != shape.fields.size()) {
      complain(v.span, "E0529",
               "`" + shape.name + "` holds " + std::to_string(shape.fields.size()) +
                   ", and this is " + std::to_string(v.items.size()) + ".",
               {"a struct is made with one value for each of the things it holds"},
               {"they go in the order the struct was written in, so leaving one out "
                "would silently move every one after it."});
    }
    for (unsigned i = 0; i < v.items.size(); ++i) {
      const Ty wanted = i < shape.fields.size() ? shape.fields[i].type : Ty{};
      const Ty got = expr(*v.items[i], wanted);
      if (wanted != Ty{} && got != Ty{} && got != wanted)
        complain(v.items[i]->span, "E0506",
                 "`'" + shape.fields[i].name + "'` is a `" + name(wanted) +
                     "`, and this is a `" + name(got) + "`.",
                 {"nothing converts on its own"});
    }
    return want;
  }

  Ty onlyValue(const ValueList &list, Ty expected) {
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
      const Ty type = typeOfChain(s.chain);
      result_.declarations[&s] = type;
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
      Ty want = symbol->type;
      // `set 'p'.x = …` — which of the things it holds is written, and what
      // that one is.
      for (unsigned i = 0; i < s.fields.size(); ++i) {
        if (!want.isStruct() || want.orNothing) {
          if (want != Ty{})
            complain(s.fieldSpans[i], "E0527", "a `" + name(want) + "` has no fields.",
                     {"a field is one of the things a struct holds"});
          want = Ty{};
          break;
        }
        const Shape &shape = result_.shapes[want.named];
        bool found = false;
        for (const Field &one : shape.fields)
          if (one.name == s.fields[i]) {
            want = one.type;
            found = true;
            break;
          }
        if (!found) {
          complain(s.fieldSpans[i], "E0528",
                   "`" + shape.name + "` has no field called `" + s.fields[i] + "`.",
                   {"a field is one of the things a struct holds"});
          want = Ty{};
          break;
        }
      }
      if (s.index) {
        const Ty at = expr(*s.index, Type::Int64);
        if (at != Ty{} && at != Ty{Type::Int64})
          complain(s.index->span, "E0506",
                   "an index is an `int64`, and this is a `" + name(at) + "`.",
                   {"nothing converts on its own"},
                   {"`count` answers an `int64`, and two sizes never meet on their own."});
        if (!symbol->type.holds() && symbol->type.kind != Type::Unknown) {
          complain(s.nameSpan, "E0514",
                   "`'" + s.name + "'` is a `" + name(symbol->type) +
                       "`, and holds one value rather than several.",
                   {"an element is one of the values a `many` holds"},
                   {"a name holding one value is that value, and there is no first of it."});
          want = Type::Unknown;
        } else {
          want = Ty{symbol->type.element};
        }
      }
      onlyValueChecked(s.value, want, s.span);
      break;
    }

    case StmtKind::When: {
      // Every case, once each. The compiler insisting on that is the whole
      // reason to write a `when` rather than an `if` — a case nobody wrote is a
      // case nobody thought about, and it would be found by the program running
      // rather than by reading it.
      const Ty subject = s.condition ? expr(*s.condition, Ty{}) : Ty{};
      if (subject != Ty{} && !subject.mayBeNothing()) {
        complain(s.condition->span, "E0520",
                 "a `" + name(subject) + "` is only ever one thing, so there is "
                 "nothing here to choose between.",
                 {"a `when` chooses between the things a value could be"},
                 {"`or-nothing` in the chain is what gives a value a second shape; "
                  "without it there is one case and an `if` says it better."});
      }

      const Branch *held = nullptr;
      const Branch *absent = nullptr;
      for (const Branch &arm : s.branches) {
        const Branch *&already = arm.matchesNothing ? absent : held;
        if (already) {
          complain(arm.holdsSpan, "E0521",
                   arm.matchesNothing
                       ? "this `when` already says what to do with nothing."
                       : "this `when` already says what to do with something.",
                   {"every case a `when` covers is written once"},
                   {}, "again here", {Note{already->holdsSpan, "and here first"}});
        } else {
          already = &arm;
        }

        scopes_.emplace_back();
        if (!arm.matchesNothing && subject.mayBeNothing())
          declare(arm.holds, Symbol{subject.within(), false, arm.holdsSpan});
        for (const StmtPtr &inner : arm.body.stmts)
          statement(*inner);
        scopes_.pop_back();
      }

      if (subject.mayBeNothing() && (!held || !absent))
        complain(s.span, "E0522",
                 std::string("this `when` says nothing about what to do with ") +
                     (held ? "nothing." : "something."),
                 {"a `when` covers every case a value could be"},
                 {held ? "`is nothing` is the case that is missing."
                       : "`is 'name'` is the case that is missing, and the name is "
                         "what it lends you."},
                 "this leaves a case out");
      break;
    }

    case StmtKind::If:
      for (const Branch &branch : s.branches) {
        if (!branch.condition) {
          block(branch.body);
          continue;
        }
        const Ty type = asked(*branch.condition, branch.holds, branch.holdsSpan, "an `if`");
        scopes_.emplace_back();
        if (!branch.holds.empty() && type.mayBeNothing())
          declare(branch.holds, Symbol{type.within(), false, branch.holdsSpan});
        for (const StmtPtr &inner : branch.body.stmts)
          statement(*inner);
        scopes_.pop_back();
      }
      break;

    case StmtKind::LoopRange: {
      const Ty type = typeOfChain(s.chain);
      result_.declarations[&s] = type;
      if (s.value.values.size() != 2)
        complain(s.value.span, "E0505", "a counted loop runs between two values.",
                 {"`[first, last]` says where a count starts and stops"});
      for (const Value &v : s.value.values)
        value(v, type);
      const bool keeps = keepsCounter(s.chain);
      if (keeps)
        declare(s.name, Symbol{type, false, s.nameSpan});
      scopes_.emplace_back();
      if (!keeps)
        declare(s.name, Symbol{type, false, s.nameSpan});
      ++loopDepth_;
      for (const StmtPtr &inner : s.body.stmts)
        statement(*inner);
      --loopDepth_;
      scopes_.pop_back();
      break;
    }

    case StmtKind::LoopWhile: {
      Ty carried;
      if (s.condition)
        carried = asked(*s.condition, s.holds, s.holdsSpan, "a `loop.while`");
      scopes_.emplace_back();
      if (!s.holds.empty() && carried.mayBeNothing())
        declare(s.holds, Symbol{carried.within(), false, s.holdsSpan});
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

  void onlyValueChecked(const ValueList &list, Ty want, Span where) {
    if (list.values.empty()) {
      if (!want.holds() && want.kind != Type::Unknown)
        complain(list.span.begin ? list.span : where, "E0517",
                 "there is no value here, and a `" + name(want) + "` was wanted.",
                 {"a name holds what it was given"},
                 {"a `many` may hold nothing, because holding nothing is a length; "
                  "one value is not a length, and has to be there."});
      return;
    }
    if (list.values.size() > 1)
      complain(list.span, "E0505", "one name takes one value.",
               {"a comma separates values, and there is one name here"});
    const Ty got = value(list.values[0], want);
    if (want != Type::Unknown && got != Type::Unknown && got != want)
      complain(list.values[0].span.begin ? list.values[0].span : where, "E0506",
               "this is a `" + std::string(name(got)) + "` and a `" + std::string(name(want)) +
                   "` was wanted.",
               {"nothing converts on its own"});
  }

  // Whether every way out of here hands back an answer. A loop does not count:
  // it may run no times at all, and then it has answered nothing.
  static bool alwaysGives(const Block &block) {
    for (const StmtPtr &s : block.stmts) {
      if (s->kind == StmtKind::Give)
        return true;
      if (s->kind != StmtKind::If)
        continue;
      bool otherwise = false, everyArm = true;
      for (const Branch &branch : s->branches) {
        if (!branch.hasCondition)
          otherwise = true;
        if (!alwaysGives(branch.body))
          everyArm = false;
      }
      if (otherwise && everyArm)
        return true;
    }
    return false;
  }

  // ---- items

  void body(const Item &item) {
    switch (item.kind) {
    case ItemKind::Struct:
      break; // read before anything else, and it has no body to walk

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
      result_.items[&item] = giving_;
      scopes_.emplace_back();
      for (const Param &param : item.params)
        declare(param.name, Symbol{typeOfChain(param.chain), changeable(param.chain),
                                   param.nameSpan});
      for (const StmtPtr &s : item.body.stmts)
        statement(*s);
      if (giving_ != Type::Nothing && giving_ != Type::Unknown &&
          !alwaysGives(item.body))
        complain(item.nameSpan, "E0513",
                 "`" + item.name + "` answers a `" + std::string(name(giving_)) +
                     "`, and can end without saying what.",
                 {"a function that answers something answers it every way out"},
                 {"`nothing` is a real answer, and a function that means to give "
                  "none says so in its chain."},
                 "this answers something");
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
