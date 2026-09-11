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
    {"any", Type::Blank, 0},
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
std::vector<std::string> sumNames;

const char *shapeName(unsigned which) {
  return which < shapeNames.size() ? shapeNames[which].c_str() : "unknown";
}

const char *sumName(unsigned which) {
  return which < sumNames.size() ? sumNames[which].c_str() : "unknown";
}

// Which table `named` points into: a struct's or a `one-of`'s. Only one of them
// can be, so one number says which.
const char *namedAs(Type kind, unsigned which) {
  return kind == Type::OneOf ? sumName(which) : shapeName(which);
}

std::string name(Ty type) {
  const std::string one = type.kind == Type::Struct || type.kind == Type::OneOf
                              ? namedAs(type.kind, type.named)
                              : name(type.kind);
  std::string inside =
      type.holds() ? (type.element == Type::Struct || type.element == Type::OneOf
                          ? namedAs(type.element, type.named)
                          : name(type.element))
                   : one;
  // One `many` per level, so a `many` of a `many` says so rather than reading
  // as either one of them.
  for (unsigned at = 0; at < type.deep; ++at)
    inside = (type.grows && at + 1 == type.deep ? "many-growing " : "many ") + inside;
  return type.orNothing ? "or-nothing " + inside : inside;
}

namespace {

struct FamilyWord {
  const char *word;
  Family family;
  const char *asks; // how a sentence says it
  Axis axis;
};

// The words a blank may be narrowed with — the same list `is` asks with, read in
// the other direction.
//
// `loan` and `loanmut` belong here and are missing, because nothing here could
// answer them: a `Ty` does not know whether it is a borrow. To this pass a
// `loan.int64` field is an `int64`, and how a thing is held is `Own.cpp`'s to
// say. `is loan` will want the same answer, so it is one piece of work rather
// than two, and it waits for `whichever`.
constexpr FamilyWord kFamilies[] = {
    {"number", Family::Number, "a number", Axis::What},
    {"int", Family::Int, "an `int`", Axis::What},
    {"uint", Family::Uint, "a `uint`", Axis::What},
    {"bin", Family::Bin, "a `bin`", Axis::What},
    {"deci", Family::Deci, "a `deci`", Axis::What},
    {"str", Family::Str, "a `str`", Axis::What},
    {"bool", Family::Bool, "a `bool`", Axis::What},
    {"many", Family::Many, "a `many`", Axis::What},
    {"or-nothing", Family::OrNothing, "something that may hold nothing", Axis::What},
    {"struct", Family::Struct, "a struct", Axis::What},
    {"owned", Family::Owned, "something held outright", Axis::How},
    {"loan", Family::Loan, "a borrow", Axis::How},
    {"loanmut", Family::LoanMut, "a borrow that may be written through", Axis::How},
};

} // namespace

Family familyNamed(std::string_view word) {
  for (const FamilyWord &known : kFamilies)
    if (word == known.word)
      return known.family;
  return Family::Anything;
}

bool namesFamily(std::string_view word) {
  for (const FamilyWord &known : kFamilies)
    if (word == known.word)
      return true;
  return false;
}

Axis axisOf(Family family) {
  for (const FamilyWord &known : kFamilies)
    if (known.family == family)
      return known.axis;
  return Axis::What;
}

const char *asksFor(Family family) {
  for (const FamilyWord &known : kFamilies)
    if (known.family == family)
      return known.asks;
  return "anything";
}

bool overlaps(Family a, Family b) {
  if (a == b)
    return true;
  const auto underNumber = [](Family f) {
    return f == Family::Int || f == Family::Uint || f == Family::Bin ||
           f == Family::Deci;
  };
  // Across the two questions nothing is said about overlap here: an arm from
  // each is refused where it is written, because a type word and a borrow word
  // overlap for every borrowed value and there is no level to pick between them.
  if (axisOf(a) != axisOf(b))
    return false;
  return (a == Family::Number && underNumber(b)) ||
         (b == Family::Number && underNumber(a));
}

bool inFamily(Ty type, Family family) {
  switch (family) {
  case Family::Anything:
    return true;
  case Family::Number:
    return isNumber(type);
  case Family::Int:
    return isSigned(type);
  case Family::Uint:
    return isWhole(type) && !isSigned(type);
  case Family::Bin:
    return isBinary(type);
  case Family::Deci:
    return isDecimal(type);
  case Family::Str:
    return !type.holds() && !type.orNothing && type.kind == Type::Str;
  case Family::Bool:
    return !type.holds() && !type.orNothing && type.kind == Type::Bool;
  case Family::Many:
    return type.holds() && !type.orNothing;
  case Family::OrNothing:
    return type.orNothing;
  case Family::Struct:
    return type.isStruct() && !type.orNothing;
  case Family::Owned:
    return type.held == Held::Owned;
  case Family::Loan:
    return type.held == Held::Loan;
  case Family::LoanMut:
    return type.held == Held::LoanMut;
  }
  return false;
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
  // Said `wrapping` where it was declared: a sum that does not fit is meant to
  // come round, so nothing is said about it.
  bool wraps = false;
  // What it was given, when that was a number written down and nothing has been
  // set into it since. It is what lets a loop be counted forward from a
  // starting point rather than guessed at.
  bool knownStart = false;
  __int128 start = 0;
  // Whether anything ever changes it: a `set`, a place or a field written, or
  // being lent out for writing. A `mut` that none of those happen to is a word
  // that was not needed — the chain says what is unusual, and nothing unusual
  // happened.
  bool everChanged = false;
  std::string name;
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
  // How many `UNSAFE` blocks this statement sits inside.
  unsigned insideUnsafe_ = 0;
  std::vector<std::unordered_map<std::string, Symbol>> scopes_;
  std::unordered_map<std::string, Signature> functions_;
  std::vector<std::string> names_;     // struct names, indexed the way `Ty` names them
  std::vector<std::string> sumsNamed_; // and the same for `one-of` names
  Ty giving_ = Type::Nothing;
  bool inFunction_ = false;
  unsigned loopDepth_ = 0;

  // The bounds' own two, kept apart so a run can overturn them. Everything
  // about them is the same; only where they are filed differs.
  void aboutSums(Span span, std::string code, std::string message,
                 std::vector<std::string> rules, std::vector<std::string> tips,
                 Severity severity) {
    result_.aboutSums.push_back(Diagnostic{span, std::move(code), std::move(message),
                                           "here", std::move(rules), std::move(tips),
                                           {}, severity});
  }

  void complain(Span span, std::string code, std::string message,
                std::vector<std::string> rules, std::vector<std::string> tips = {},
                std::string label = "here", std::vector<Note> notes = {}) {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             std::move(label), std::move(rules),
                                             std::move(tips), std::move(notes)});
  }

  // A refusal that happened only because of an earlier one. It is shown
  // underneath the mistake it came from rather than counted as a mistake of its
  // own, so a reader with one typo is told about one typo and can see, in the
  // same place, everything the typo broke.
  void because(Span from, Span span, std::string code, std::string message,
               std::vector<std::string> rules, std::vector<std::string> tips = {}) {
    Diagnostic one{span,          std::move(code),  std::move(message), "here",
                   std::move(rules), std::move(tips), {}};
    one.follows = from;
    result_.diagnostics.push_back(std::move(one));
  }

  // A check that could not be made, because what it would have looked at could
  // not be worked out — something it was built from was already refused. Said
  // rather than passed over: a reader who is told nothing cannot see how far one
  // mistake reached, and a check quietly not made is worse than one that fails.
  //
  // Placed before the check it stands in for rather than replacing it. A traced
  // type is an unknown one, so the guard below it — `got != Ty{}`, or the same
  // written the other way round — is already false and the two never both speak.
  // Every name a block changes, without reading the block for anything else.
  // Used where a body is carried into the program without being read here: what
  // it does is not known yet, but that it writes to a name is plain to see.
  void changedSomewhereIn(const Block &block) {
    for (const StmtPtr &s : block.stmts) {
      if (!s)
        continue;
      if (s->kind == StmtKind::Set || s->kind == StmtKind::LoopRange)
        if (Symbol *held = lookupToChange(s->name)) {
          held->knownStart = false;
          held->everChanged = true;
        }
      changedLentIn(s->index.get());
      changedLentIn(s->value);
      changedLentIn(s->condition.get());
      changedLentIn(s->call.get());
      for (const Branch &branch : s->branches) {
        changedLentIn(branch.condition.get());
        changedSomewhereIn(branch.body);
      }
      changedSomewhereIn(s->body);
    }
  }

  void changedLentIn(const ValueList &list) {
    for (const Value &value : list.values)
      for (const ExprPtr &item : value.items)
        changedLentIn(item.get());
  }

  void changedLentIn(const Expr *e) {
    if (!e)
      return;
    if (e->kind == ExprKind::Borrow && e->text == "loanmut" && !e->children.empty() &&
        e->children[0] && e->children[0]->kind == ExprKind::Name)
      if (Symbol *held = lookupToChange(e->children[0]->text))
        held->everChanged = true;
    for (const ExprPtr &child : e->children)
      changedLentIn(child.get());
    changedLentIn(e->args);
  }

  void couldNotCheck(Ty got, Span where, std::string what, std::string rule) {
    if (got.tracedBack())
      because(got.from, where, "E0506", std::move(what), {std::move(rule)});
  }

  // Said rather than refused: what the compiler could not settle either way.
  void warn(Span span, std::string code, std::string message,
            std::vector<std::string> rules, std::vector<std::string> tips = {}) {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             "here", std::move(rules), std::move(tips),
                                             {}, Severity::Warning});
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
    // `any.number` is one type region: the blank, and the word saying what it
    // will take. The word stands nearest the name the way `many` stands nearest
    // the type, so the type itself is one further back.
    std::size_t typeAt = chain.segments.size() - 1;
    Family asks = Family::Anything;
    if (typeAt > 0 && !chain.segments[typeAt].isName &&
        !chain.segments[typeAt - 1].isName &&
        chain.segments[typeAt - 1].text == "any" &&
        namesFamily(chain.segments[typeAt].text)) {
      asks = familyNamed(chain.segments[typeAt].text);
      --typeAt;
    }
    const ChainSegment &last = chain.segments[typeAt];
    if (last.isName) {
      complain(last.span, "E0503", "a chain ends in a type, and a loan is not one.",
               {"the segment nearest the name is the type"});
      return unknownFrom(last.span);
    }
    const Type type = typeNamed(last.text);
    unsigned which = 0;
    bool isShape = false;
    bool isSum = false;
    if (type == Type::Unknown) {
      for (unsigned i = 0; i < result_.shapes.size(); ++i)
        if (result_.shapes[i].name == last.text) {
          which = i;
          isShape = true;
          break;
        }
      if (!isShape)
        for (unsigned i = 0; i < result_.sums.size(); ++i)
          if (result_.sums[i].name == last.text) {
            which = i;
            isSum = true;
            break;
          }
      if (!isShape && !isSum) {
        complain(last.span, "E0503", "`" + last.text + "` is not a type.",
                 {"a size is always written, and only sizes the standard defines"});
        return unknownFrom(last.span);
      }
    }
    // `many` stands with the type and says the name holds several of it, and
    // `or-nothing` stands outside that and says the whole of it may be missing.
    // The parser has already refused a second level of either.
    std::size_t at = typeAt;
    unsigned several = 0;
    bool orNothing = false;
    bool grows = false;
    // However many were written. A second `many` is a `many` of a `many`, and
    // there is no limit written down because there is no place a limit would
    // come from.
    //
    // `many-growing` stands where `many` does and counts as one of them, so
    // everything that reads several reads one of these — what differs is that
    // it may become more of them, and that is one word further out.
    while (at > 0 && !chain.segments[at - 1].isName &&
           (chain.segments[at - 1].text == "many" ||
            chain.segments[at - 1].text == "many-growing")) {
      grows = grows || chain.segments[at - 1].text == "many-growing";
      ++several;
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
      return unknownFrom(chain.segments[at].span);
    }

    Ty settled = isShape ? (several ? Ty{Type::Many, Type::Struct, false, which}
                                    : structNamed(which))
                 : isSum ? (several ? Ty{Type::Many, Type::OneOf, false, which}
                                    : sumTyped(which))
                         : (several ? many(type) : Ty{type});
    settled.deep = several;
    settled.grows = grows;
    if (orNothing)
      settled = orNothingOf(settled);
    // What the blank will take rides along with it, and is asked at the call
    // that fills it in. `any.number` on its own and `many.any.number` both come
    // through here, and there is one blank either way.
    settled.asks = asks;
    // How it is held is written in the chain, and until now only `Own.cpp` read
    // it. The type carries it so that a program can ask.
    // Only what stands before the type. The word after it is what the blank
    // asks for, not how this is held — `any.loan` asks for a borrow and is not
    // one, and reading it as one made the asking always answer yes.
    for (std::size_t at = 0; at < typeAt; ++at) {
      const ChainSegment &seg = chain.segments[at];
      if (seg.isName)
        continue;
      if (seg.text == "loan")
        settled.held = Held::Loan;
      else if (seg.text == "loanmut")
        settled.held = Held::LoanMut;
    }
    return settled;
  }

  // Which `one-of` a case name belongs to, and which case it is. The expected
  // type says when it is a `one-of`; where nothing expected one, a name used by
  // exactly one `one-of` in the file says it by itself, and one used by more
  // than one cannot.
  bool caseNamed(const std::string &word, Ty expected, unsigned &which,
                 unsigned &at) const {
    if (expected.kind == Type::OneOf && expected.named < result_.sums.size()) {
      const Shape &sum = result_.sums[expected.named];
      for (unsigned i = 0; i < sum.fields.size(); ++i)
        if (sum.fields[i].name == word) {
          which = expected.named;
          at = i;
          return true;
        }
      return false;
    }
    unsigned found = 0;
    for (unsigned s = 0; s < result_.sums.size(); ++s)
      for (unsigned i = 0; i < result_.sums[s].fields.size(); ++i)
        if (result_.sums[s].fields[i].name == word) {
          ++found;
          which = s;
          at = i;
        }
    return found == 1;
  }

  // `text:'s'` and, where the case holds nothing, `gave-up` on its own.
  Ty madeCase(const Expr &e, Ty expected) {
    unsigned which = 0;
    unsigned at = 0;
    if (!caseNamed(e.text, expected, which, at)) {
      if (expected.kind != Type::OneOf && knownCase(e.text))
        complain(e.span, "E0503",
                 "`" + e.text + "` is a case of more than one `one-of`, and nothing "
                 "here says which.",
                 {"a value says which of the things it could be it is"},
                 {"what it is going into says which `one-of` this is, and here "
                  "nothing does."});
      else if (expected.kind == Type::OneOf)
        complain(e.span, "E0503",
                 "`" + e.text + "` is not a case of `" + name(expected) + "`.",
                 {"a value says which of the things it could be it is"},
                 {"what `" + name(expected) + "` may be is written where it is "
                  "declared."});
      else if (e.children.empty())
        // A bare word that names nothing is the mistake it always was.
        complain(e.span, "E0107", "a word on its own is not a value.",
                 {"a name is a value, and a word followed by `[` is a call"},
                 {"words name functions, types and chain segments; a variable is a "
                  "name, and names wear marks."});
      else
        complain(e.span, "E0503", "`" + e.text + "` is not a type.",
                 {"a size is always written, and only sizes the standard defines"});
      return unknownFrom(e.span);
    }
    const Field &one = result_.sums[which].fields[at];
    const bool empty = one.type == Ty{Type::Nothing};
    if (empty && !e.children.empty()) {
      complain(e.span, "E0528",
               "`" + e.text + "` is a case that holds nothing, and this gives it "
               "something.",
               {"a case holds what its type says it holds"},
               {"`" + e.text + "` on its own is the whole of it."});
    } else if (!empty && e.children.empty()) {
      complain(e.span, "E0528",
               "`" + e.text + "` holds a `" + name(one.type) + "`, and nothing is "
               "given to it.",
               {"a case holds what its type says it holds"},
               {"`" + e.text + ":" + "…` is how the value goes in."});
    } else if (!empty) {
      expr(*e.children[0], one.type);
    }
    Ty made = sumTyped(which);
    made.from = e.span;
    result_.cases[&e] = at;
    return made;
  }

  bool knownCase(const std::string &word) const {
    for (const Shape &sum : result_.sums)
      for (const Field &one : sum.fields)
        if (one.name == word)
          return true;
    return false;
  }

  // Every case of a `one-of`, once each. The same rule `or-nothing` has had
  // since there were two cases to cover, counted over as many as the type names.
  void whenOverASum(const Stmt &s, Ty subject) {
    const Shape &sum = result_.sums[subject.named];
    std::vector<const Branch *> seen(sum.fields.size(), nullptr);
    for (const Branch &arm : s.branches) {
      unsigned at = sum.fields.size();
      for (unsigned i = 0; i < sum.fields.size(); ++i)
        if (sum.fields[i].name == arm.family)
          at = i;
      if (at == sum.fields.size()) {
        complain(arm.familySpan.begin == arm.familySpan.end ? arm.holdsSpan
                                                            : arm.familySpan,
                 "E0503",
                 arm.family.empty()
                     ? "an `is` over a `" + sum.name + "` says which case it is."
                     : "`" + arm.family + "` is not a case of `" + sum.name + "`.",
                 {"every case a `when` covers is written out"},
                 {"what `" + sum.name + "` may be is written where it is declared."});
        continue;
      }
      if (seen[at])
        complain(arm.familySpan, "E0521",
                 "this `when` already says what to do with `" + arm.family + "`.",
                 {"every case a `when` covers is written once"}, {}, "again here",
                 {Note{seen[at]->familySpan, "and here first"}});
      else
        seen[at] = &arm;

      const Field &one = sum.fields[at];
      const bool empty = one.type == Ty{Type::Nothing};
      if (empty && !arm.holds.empty())
        complain(arm.holdsSpan, "E0528",
                 "`" + arm.family + "` holds nothing, and this asks for a name to "
                 "lend it to.",
                 {"a case holds what its type says it holds"},
                 {"`is " + arm.family + "` on its own is the whole of it."});
      else if (!empty && arm.holds.empty())
        complain(arm.familySpan, "E0528",
                 "`" + arm.family + "` holds a `" + name(one.type) + "`, and nothing "
                 "here is lent it.",
                 {"a case holds what its type says it holds"},
                 {"`is " + arm.family + " 'name'` is how it comes out."});

      scopes_.emplace_back();
      if (!empty && !arm.holds.empty()) {
        // Lent for the arm, the way `holds` lends: what held it goes on holding
        // it, and taking it away is refused where taking is refused.
        Ty held = one.type;
        held.held = Held::Loan;
        declare(arm.holds, Symbol{held, false, arm.holdsSpan});
      }
      for (const StmtPtr &inner : arm.body.stmts)
        statement(*inner);
      scopes_.pop_back();
      result_.chosenCase[&arm] = at;
    }
    for (unsigned i = 0; i < sum.fields.size(); ++i)
      if (!seen[i]) {
        complain(s.span, "E0522",
                 "this `when` says nothing about what to do with `" +
                     sum.fields[i].name + "`.",
                 {"a `when` covers every case a value could be"},
                 {"`is " + sum.fields[i].name +
                  (sum.fields[i].type == Ty{Type::Nothing} ? "" : " 'name'") +
                  "` is the case that is missing."},
                 "this leaves a case out");
        break;
      }
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

  // `mut` on what a name owns, `loanmut` on what it borrows. `loan` lends without
  // letting go of that, and a bare chain changes nothing at all.
  // `perm` asks to keep the counter after the loop, which is what a `break`
  // leaves behind and the only reason to keep one at all.
  static bool keepsCounter(const Chain &chain) {
    for (const ChainSegment &seg : chain.segments)
      if (!seg.isName && seg.text == "perm")
        return true;
    return false;
  }

  // The most and least a whole type holds.
  static __int128 mostOf(Type type) {
    const unsigned width = widthOf(type);
    if (isSigned(type))
      return width >= 128 ? ~(static_cast<__int128>(1) << 127)
                          : (static_cast<__int128>(1) << (width - 1)) - 1;
    return width >= 128 ? -1 : (static_cast<__int128>(1) << width) - 1;
  }

  static __int128 leastOf(Type type) {
    if (!isSigned(type))
      return 0;
    const unsigned width = widthOf(type);
    return width >= 128 ? (static_cast<__int128>(1) << 127)
                        : -(static_cast<__int128>(1) << (width - 1));
  }

  // Multiplying without leaving the room the answer is worked out in. A count
  // this large is not one anybody wrote down, so it is read as unknowable
  // rather than as a number.
  static bool timesWithin(__int128 a, __int128 b, __int128 &out) {
    if (a == 0 || b == 0) {
      out = 0;
      return true;
    }
    const __int128 ceiling = ~(static_cast<__int128>(1) << 127);
    if (a > ceiling / (b < 0 ? -b : b) || a < -(ceiling / (b < 0 ? -b : b)))
      return false;
    out = a * b;
    return true;
  }

  // Brackets say what order to read in and nothing about the value, so they are
  // stepped through rather than stopped at.
  static const Expr &inside(const Expr &e) {
    const Expr *at = &e;
    while (at->kind == ExprKind::Group && at->children.size() == 1)
      at = at->children[0].get();
    return *at;
  }

  // What a name is added to by, once round the loop: a number written down, or
  // the counter itself, whose largest step the loop's own bounds already say.
  bool stepOf(const Stmt &set, const std::string &counter, __int128 most,
              __int128 &step, bool &plus) const {
    if (set.value.values.size() != 1 || set.value.values[0].items.size() != 1)
      return false;
    const Expr &only = inside(*set.value.values[0].items[0]);
    if (only.kind != ExprKind::Binary || only.children.size() != 2)
      return false;
    if (only.text != "+" && only.text != "-")
      return false;
    const Expr &left = inside(*only.children[0]);
    const Expr &right = inside(*only.children[1]);
    if (left.kind != ExprKind::Name || left.text != set.name)
      return false;
    plus = only.text == "+";
    if (right.kind == ExprKind::Written && looksLikeWholeNumber(right.text)) {
      step = wholeValue(right.text);
      return true;
    }
    // The counter never goes past where the loop stops, so its largest step is
    // the largest number the loop counts to — and the same times a written
    // number is that many times as far.
    if (right.kind == ExprKind::Name && right.text == counter) {
      step = most < 0 ? -most : most;
      return true;
    }
    // A remainder never reaches what it was taken against, whatever it was taken
    // from — so this one is bounded without knowing the left side at all.
    if (right.kind == ExprKind::Binary && right.text == "mod" &&
        right.children.size() == 2) {
      const Expr &by = inside(*right.children[1]);
      if (by.kind == ExprKind::Written && looksLikeWholeNumber(by.text)) {
        const __int128 against = wholeValue(by.text);
        if (against != 0) {
          step = (against < 0 ? -against : against) - 1;
          return true;
        }
      }
    }
    // The counter divided by a written number gets no further than the largest
    // it counts to, divided by the same.
    if (right.kind == ExprKind::Binary && right.text == "/" &&
        right.children.size() == 2) {
      const Expr &over = inside(*right.children[0]);
      const Expr &by = inside(*right.children[1]);
      if (over.kind == ExprKind::Name && over.text == counter &&
          by.kind == ExprKind::Written && looksLikeWholeNumber(by.text)) {
        const __int128 against = wholeValue(by.text);
        if (against != 0) {
          step = (most < 0 ? -most : most) / (against < 0 ? -against : against);
          return true;
        }
      }
    }
    if (right.kind == ExprKind::Binary && right.text == "x" &&
        right.children.size() == 2) {
      const Expr &one = inside(*right.children[0]);
      const Expr &other = inside(*right.children[1]);
      const Expr *named = one.kind == ExprKind::Name ? &one : &other;
      const Expr *number = named == &one ? &other : &one;
      if (named->kind == ExprKind::Name && named->text == counter &&
          number->kind == ExprKind::Written && looksLikeWholeNumber(number->text)) {
        const __int128 by = wholeValue(number->text);
        return timesWithin(most < 0 ? -most : most, by < 0 ? -by : by, step);
      }
    }
    return false;
  }

  // The name a loop counts the places of: `[*1*, count['xs']]`, however the
  // counting is bracketed and whether it borrows or not.
  //
  // This read `[*0*, (count['xs'] - *1*)]` until places were counted from one,
  // and went on reading it after — so it recognised a shape nobody writes any
  // more, nothing was settled, and every walk over a `many` carried a check its
  // own end had already answered. The shape a walk is written in is the shape
  // this has to know.
  static std::string countedOver(const Stmt &s) {
    if (s.value.values.size() != 2)
      return {};
    __int128 from = 0;
    if (!wholeItemOf1(s.value.values[0], from) || from != 1)
      return {};
    if (s.value.values[1].items.size() != 1)
      return {};
    const Expr &counting = inside(*s.value.values[1].items[0]);
    if (counting.kind != ExprKind::Call || counting.path.size() != 1 ||
        counting.path[0] != "count" || counting.args.values.size() != 1 ||
        counting.args.values[0].items.size() != 1)
      return {};
    const Expr &of = inside(*counting.args.values[0].items[0]);
    if (of.kind == ExprKind::Name)
      return of.text;
    // `count[loan 'xs']` counts the same places `'xs'` has.
    if (of.kind == ExprKind::Borrow && of.children.size() == 1 &&
        of.children[0]->kind == ExprKind::Name)
      return of.children[0]->text;
    return {};
  }

  // Whether anything in here is set into that name. A `many` never changes
  // length, but a name can be given a different one.
  static bool setsInto(const Block &body, const std::string &name) {
    for (const StmtPtr &s : body.stmts) {
      if (s->kind == StmtKind::Set && s->name == name && !s->index)
        return true;
      if (setsInto(s->body, name))
        return true;
      for (const Branch &branch : s->branches)
        if (setsInto(branch.body, name))
          return true;
    }
    return false;
  }

  // A loop counting the places a `many` has, reaching into it with its own
  // counter, reaches a place it has every time round. A `many` is a fixed
  // length once made, so `count[…]` does not move under it — which is what
  // makes this answerable here rather than only guessable.
  void reachingInto(const Stmt &s) {
    const std::string over = countedOver(s);
    if (over.empty() || setsInto(s.body, over))
      return;
    markReaches(s.body, over, s.name);
  }

  void markReaches(const Block &body, const std::string &over,
                   const std::string &counter) {
    for (const StmtPtr &one : body.stmts) {
      for (const Value &v : one->value.values)
        for (const ExprPtr &item : v.items)
          markReaches(*item, over, counter);
      if (one->index)
        markReaches(*one->index, over, counter);
      if (one->condition)
        markReaches(*one->condition, over, counter);
      if (one->call)
        markReaches(*one->call, over, counter);
      markReaches(one->body, over, counter);
      for (const Branch &branch : one->branches) {
        if (branch.condition)
          markReaches(*branch.condition, over, counter);
        markReaches(branch.body, over, counter);
      }
    }
  }

  void markReaches(const Expr &e, const std::string &over,
                   const std::string &counter) {
    if (e.kind == ExprKind::Index && e.text == over && e.children.size() == 1) {
      const Expr &at = inside(*e.children[0]);
      if (at.kind == ExprKind::Name && at.text == counter)
        result_.settled.insert(&e);
    }
    for (const ExprPtr &child : e.children)
      markReaches(*child, over, counter);
    for (const Value &v : e.args.values)
      for (const ExprPtr &item : v.items)
        markReaches(*item, over, counter);
  }

  // A counted loop with both ends written down runs a known number of times, so
  // what it adds up is a number rather than a guess.
  void countingUp(const Stmt &s, Ty counted) {
    if (!isWhole(counted) || s.value.values.size() != 2)
      return;
    __int128 first = 0;
    __int128 last = 0;
    if (!wholeItemOf1(s.value.values[0], first) || !wholeItemOf1(s.value.values[1], last))
      return;
    if (last < first)
      return; // it runs no times, and adds nothing up
    const __int128 trips = last - first + 1;
    // A loop already told not to be run is not one to warn anybody about the
    // length of: the answer would be to write the word that is already there.
    bool toldNot = false;
    for (const ChainSegment &seg : s.chain.segments)
      if (!seg.isName && seg.text == "no-itmt")
        toldNot = true;
    if (!toldNot && trips > result_.mostRounds) {
      result_.mostRounds = trips;
      result_.longestLoop = s.span;
    }
    const __int128 widest = (first < 0 ? -first : first) > (last < 0 ? -last : last)
                                ? (first < 0 ? -first : first)
                                : (last < 0 ? -last : last);

    for (const StmtPtr &inner : s.body.stmts) {
      if (inner->kind != StmtKind::Set || !inner->fields.empty() || inner->index)
        continue;
      const Symbol *held = lookup(inner->name);
      if (!held || held->wraps || !isWhole(held->type))
        continue;

      __int128 step = 0;
      bool plus = true;
      const bool known = stepOf(*inner, s.name, widest, step, plus);
      __int128 total = 0;
      if (!known || !held->knownStart || !timesWithin(trips, step, total)) {
        // Something is being added up here that cannot be followed. Saying
        // nothing would let it come round in silence; refusing would turn away
        // a program that is very likely fine.
        if (known || held->knownStart)
          aboutSums(inner->span, "W0001",
                    "`'" + inner->name + "'` is added to here, and I cannot work out "
                    "how far it gets.",
                    {"a sum that does not fit comes round, and that is rarely what was "
                     "wanted"},
                    {"`wrapping` on the declaration says it is meant to, and then "
                     "nothing is said about it."},
                    Severity::Warning);
        continue;
      }
      const __int128 reach = plus ? held->start + total : held->start - total;
      // Said rather than refused. This is worked out from the loop's ends and
      // says *at most*, and at most turns away programs that are fine: a
      // hundred rounds adding `'i' / *50*` is bounded at 200 and reaches 52.
      // Running the loop is what makes it certain, and `ahead` says so then.
      if (reach > mostOf(held->type.kind) || reach < leastOf(held->type.kind))
        aboutSums(inner->span, "E0534",
                  "`'" + inner->name + "'` may reach past what a `" +
                      std::string(name(held->type)) + "` holds.",
                  {"a sum that does not fit comes round, and that is rarely what was "
                   "wanted"},
                  {"this is worked out from the loop's ends rather than by running it, "
                   "so it says how far this could get and not how far it does; "
                   "`wrapping` says coming round is meant."},
                  Severity::Warning);
    }
  }

  static bool wholeItemOf1(const Value &v, __int128 &out) {
    if (v.items.size() != 1)
      return false;
    const Expr &only = *v.items[0];
    if (only.kind != ExprKind::Written || !looksLikeWholeNumber(only.text))
      return false;
    out = wholeValue(only.text);
    return true;
  }

  // A value that is one whole number written down, and nothing else.
  static bool wholeItemOf(const ValueList &list, __int128 &out) {
    if (list.values.size() != 1 || list.values[0].items.size() != 1)
      return false;
    const Expr &only = *list.values[0].items[0];
    if (only.kind != ExprKind::Written || !looksLikeWholeNumber(only.text))
      return false;
    out = wholeValue(only.text);
    return true;
  }

  static __int128 wholeValue(std::string_view text) {
    const bool negative = !text.empty() && text[0] == '-';
    __int128 magnitude = 0;
    for (unsigned i = negative ? 1 : 0; i < text.size(); ++i)
      magnitude = magnitude * 10 + (text[i] - '0');
    return negative ? -magnitude : magnitude;
  }

  Symbol *lookupToChange(const std::string &name) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      auto found = scope->find(name);
      if (found != scope->end())
        return &found->second;
    }
    return nullptr;
  }

  // `no-itmt` asks for something and `UNSAFE` grants it; neither alone does
  // anything. Asking outside is refused rather than quietly obeyed, because the
  // whole use of the word being in capitals is that grepping for it finds every
  // place a check was turned off — and a `no-itmt` that worked without one
  // would be a check turned off where nothing says so.
  void askedOutsideUnsafe(const Chain &chain) {
    if (insideUnsafe_ > 0)
      return;
    for (const ChainSegment &seg : chain.segments)
      if (!seg.isName && seg.text == "no-itmt")
        complain(seg.span, "E0212",
                 "`no-itmt` asks for something only `UNSAFE` gives.",
                 {"what is unsafe is asked for by name, inside a block that says so"},
                 {"`UNSAFE` is in capitals so that looking for it finds every place a "
                  "check was turned off; one that worked without it would be a check "
                  "turned off where nothing says so."});
  }

  // One of the things a struct holds, by the name it was given.
  const Field *fieldNamed(Ty of, const std::string &called) const {
    if (!of.isStruct() || of.named >= result_.shapes.size())
      return nullptr;
    for (const Field &one : result_.shapes[of.named].fields)
      if (one.name == called)
        return &one;
    return nullptr;
  }

  static bool wrapsChain(const Chain &chain) {
    for (const ChainSegment &seg : chain.segments)
      if (!seg.isName && seg.text == "wrapping")
        return true;
    return false;
  }

  static bool changeable(const Chain &chain) {
    for (const ChainSegment &seg : chain.segments)
      if (!seg.isName && (seg.text == "mut" || seg.text == "loanmut"))
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
      const bool isSum = item.kind == ItemKind::OneOf;
      if (item.kind != ItemKind::Struct && !isSum)
        continue;
      if (typeNamed(item.name) != Type::Unknown || item.name == "nothing") {
        complain(item.nameSpan, "E0524",
                 "`" + item.name + "` is already a type.",
                 {"a word names one thing for the whole file"});
        continue;
      }
      bool taken = false;
      for (const Shape &already : result_.shapes)
        if (already.name == item.name)
          taken = true;
      for (const Shape &already : result_.sums)
        if (already.name == item.name)
          taken = true;
      if (taken)
        complain(item.nameSpan, "E0502", "`" + item.name + "` is already a type.",
                 {"a word names one thing for the whole file"});
      if (isSum) {
        result_.sums.push_back(Shape{item.name, {}, item.nameSpan});
        sumsNamed_.push_back(item.name);
      } else {
        result_.shapes.push_back(Shape{item.name, {}, item.nameSpan});
        names_.push_back(item.name);
      }
    }
    shapeNames = names_;
    sumNames = sumsNamed_;

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
                                     field.nameSpan, wrapsChain(field.chain)});
      }
      if (shape.fields.empty())
        complain(item.nameSpan, "E0525", "`" + shape.name + "` holds nothing.",
                 {"a struct is a group of named things"},
                 {"a group of none is `nothing`, which the language already has."});
    }

    // The cases, once every name is known, so that a case may name any type
    // this file declares — a struct, or another `one-of`.
    unsigned which = 0;
    for (const Item &item : program_.items) {
      if (item.kind != ItemKind::OneOf)
        continue;
      Shape &sum = result_.sums[which++];
      for (const Param &one : item.params) {
        for (const Field &already : sum.fields)
          if (already.name == one.name)
            complain(one.nameSpan, "E0502",
                     "`'" + one.name + "'` is already a case of `" + sum.name + "`.",
                     {"a name means one thing for as long as it stands"});
        // A case holding nothing is the case itself and no value, which is
        // exactly what `nothing` says. A struct field cannot say it, because a
        // field is something the struct holds; a case is something it may be.
        sum.fields.push_back(Field{one.name, typeOfChain(one.chain), one.nameSpan});
      }
      if (sum.fields.size() < 2)
        complain(item.nameSpan, "E0527",
                 "`" + sum.name + "` is one of " +
                     (sum.fields.empty() ? "nothing" : "one thing") + ".",
                 {"a `one-of` is a choice between cases"},
                 {"a choice between one is that one, and a `struct` or a plain type "
                  "says it without the asking."});
    }

    // A struct that holds itself has no size a machine could give it, and
    // neither has a `one-of` that can be itself.
    for (unsigned at = 0; at < result_.shapes.size(); ++at)
      if (reaches(at, at, 0))
        complain(result_.shapes[at].span, "E0526",
                 "`" + result_.shapes[at].name + "` holds itself.",
                 {"a struct is as big as the things in it"},
                 {"however many times it were laid out, there would always be one "
                  "more of it inside."});
    for (unsigned at = 0; at < result_.sums.size(); ++at)
      if (reachesSum(at, at, 0))
        complain(result_.sums[at].span, "E0526",
                 "`" + result_.sums[at].name + "` can be itself.",
                 {"a `one-of` is as big as the largest thing it can be"},
                 {"however much room it were given, one of its cases would want "
                  "that much and a tag as well."});
  }

  // Whether one `one-of` can be reached from another by walking cases, and
  // through the structs those cases name.
  bool reachesSum(unsigned from, unsigned to, unsigned depth) const {
    if (depth > result_.sums.size() + result_.shapes.size())
      return false;
    for (const Field &one : result_.sums[from].fields) {
      const Ty held = one.type;
      if (held.kind == Type::OneOf || held.element == Type::OneOf) {
        if (held.named == to || reachesSum(held.named, to, depth + 1))
          return true;
      } else if (held.kind == Type::Struct || held.element == Type::Struct) {
        if (structReachesSum(held.named, to, depth + 1))
          return true;
      }
    }
    return false;
  }

  bool structReachesSum(unsigned from, unsigned to, unsigned depth) const {
    if (depth > result_.sums.size() + result_.shapes.size() ||
        from >= result_.shapes.size())
      return false;
    for (const Field &field : result_.shapes[from].fields) {
      const Ty held = field.type;
      if (held.kind == Type::OneOf || held.element == Type::OneOf) {
        if (held.named == to || reachesSum(held.named, to, depth + 1))
          return true;
      } else if (held.kind == Type::Struct || held.element == Type::Struct) {
        if (structReachesSum(held.named, to, depth + 1))
          return true;
      }
    }
    return false;
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
    // A `print` says where it goes, and there is no default. Naming the
    // destination only earns its place because there is more than one, so there
    // are two — a program's answer and a program's complaint are different
    // things, and a reader piping one should not catch the other.
    functions_["print.stdout"] = Signature{{}, Type::Nothing, true, Span{}};
    functions_["print.stderr"] = Signature{{}, Type::Nothing, true, Span{}};
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
        for (const Param &param : item.params) {
          const Ty held = typeOfChain(param.chain);
          signature.params.push_back(held);
          result_.parameters[&param] = held;
        }
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

  // Whether a blank is anywhere in this type: `any`, or `many.any`, or one that
  // may hold nothing.
  static bool hasBlank(Ty type) {
    return type.kind == Type::Blank || type.element == Type::Blank;
  }

  // What the blank must be, given what a parameter asks for and what the caller
  // brought. `any` against `int64` says int64; `many.any` against `many.str`
  // says str.
  static Ty blankFrom(Ty wanted, Ty got) {
    if (wanted.kind == Type::Blank) {
      Ty filled = got;
      // A chain that already says how it holds this has said it. `loan.any 'v'`
      // is a read-borrow of whatever it is given, and a caller lending it for
      // writing is lending something that may also be read — so the blank is
      // `loan.int64` either way, and there is one copy rather than two.
      //
      // Taking it from the caller instead built `f$loanmut.int64` from a
      // parameter whose chain said `loan`, and then refused the very call that
      // had asked for it.
      if (wanted.held != Held::Owned)
        filled.held = wanted.held;
      return filled;
    }
    // Only when the blank is the whole type. In `loan.many.any 'xs'` the borrow
    // is of the `many`, and each place inside it is held no way at all — saying
    // otherwise filled `var.mut.any 'best'` in with `loan.int64`, so the body
    // declared a borrow and then wrote through it.
    if (wanted.element == Type::Blank && got.kind == wanted.kind)
      return elementOf(got);
    return Ty{};
  }

  // The same type with the blank filled in.
  static Ty filledIn(Ty type, Ty blank) {
    if (blank == Ty{}) {
      // A blank nothing filled in because the call was refused. What the call
      // answers is unknown, and it knows why — handing back the blank itself
      // would leave everything after this reading a hole as though it were a
      // type. Nothing else changes: a blank that was simply never reached is
      // still handed back as it was.
      if (blank.tracedBack() && (type.kind == Type::Blank || type.element == Type::Blank))
        return blank;
      return type;
    }
    if (type.kind == Type::Blank)
      return Ty{blank.kind, blank.element, type.orNothing, blank.named};
    if (type.element == Type::Blank)
      return Ty{type.kind, blank.kind, type.orNothing, blank.named};
    return type;
  }

  // Written down, which is what filling a blank into a chain needs. A struct is
  // spelled by the name it was given.
  // A type written the way a chain writes it, dots and all: `int64`, `point`,
  // `many.int64`, `or-nothing.many.point`.
  //
  // It used to be one word, which is every type a scalar and no type else. A
  // blank filled in with a `many` came out spelled `unknown`, and a blank filled
  // in with an `or-nothing` came out spelled as the thing inside it — so the
  // copy took a plain `int64` and refused the very call that asked for it. This
  // is what a blank is *for*, and it worked on scalars and structs only.
  std::string spelledAs(Ty type) const {
    std::string out;
    // How it is held is part of what the copy takes. A blank filled in with a
    // borrowed `int64` is a parameter spelled `loan.int64`, and a separate copy
    // from the one filled in with an owned one — they are different functions,
    // and a program that asks how its argument is held gets a different answer
    // in each.
    if (type.held == Held::Loan)
      out += "loan.";
    else if (type.held == Held::LoanMut)
      out += "loanmut.";
    if (type.orNothing)
      out += "or-nothing.";
    // One per level, and the outermost says whether it grows. Written as plain
    // `many.` whatever it was, a blank filled in with a `many many str` was
    // written out as taking a `many.str`, and one filled in with a
    // `many-growing` as taking a `many` — and the call that asked for the copy
    // was then refused by it. Twice, in two features, for one reason.
    for (unsigned at = 0; at < type.deep; ++at)
      out += type.grows && at == 0 ? "many-growing." : "many.";
    const Type inner = type.holds() ? type.element : type.kind;
    out += inner == Type::Struct ? shapeName(type.named) : std::string(name(inner));
    return out;
  }

  // Remembered so that the generic can be built for it later, once each however
  // often it is called.
  void calledWith(const std::string &what, Ty blank) {
    const std::string spelled = spelledAs(blank);
    for (const auto &[already, with] : result_.instantiations)
      if (already == what && with == spelled)
        return;
    result_.instantiations.push_back({what, spelled});
  }

  Ty exprKind(const Expr &e, Ty expected) {
    switch (e.kind) {
    case ExprKind::Name: {
      const Symbol *symbol = lookup(e.text);
      if (!symbol) {
        complain(e.span, "E0501", "`'" + e.text + "'` is not declared.",
                 {"a name means something only after a declaration says what it means"});
        return unknownFrom(e.span);
      }
      return symbol->type;
    }

    case ExprKind::Written:
      // A written value is one value. It takes its type from what it is written
      // into, and a `many` is not a type one value can take — the brackets that
      // make one are what goes there. Without this, `var.many.many.int64 'g' =
      // [*1* *2*]` was taken, and each written number claimed to be a whole
      // array.
      if (expected.holds()) {
        complain(e.span, "E0506",
                 "`*" + e.text + "*` is one value, and a `" + name(expected) +
                     "` holds several.",
                 {"a written value is one value"},
                 {"brackets where an item goes make a `many`: `[[*1* *2*] [*3*]]` is "
                  "two of them."});
        return unknownFrom(e.span);
      }
      if (expected == Type::Unknown) {
        // Written into something that is unknown for a reason already refused.
        // Saying it takes its type from a parameter or from itself would send
        // the reader looking at a value that was never the problem — the tip is
        // right in general and wrong here, so it is not given.
        if (expected.tracedBack()) {
          because(expected.from, e.span, "E0507",
                  "nothing here says what this written value is.",
                  {"a written value takes its type from what it is written into, from "
                   "the parameter it is passed to, or from itself"});
          return unknownFrom(expected.from);
        }
        // Which tip depends on where the value is standing. A comparison
        // declares nothing and wants nothing, so the reader needs to hear that
        // rather than a sentence about `print.stdout`.
        complain(e.span, "E0507", "nothing here says what this written value is.",
                 {"a written value takes its type from what it is written into, from the "
                  "parameter it is passed to, or from itself"},
                 comparing_
                     ? std::vector<std::string>{
                           "a comparison declares nothing, so neither side is told what "
                           "it is. The other side does not say it either: it is the "
                           "value standing beside this one, not a slot this one goes "
                           "into. `'n' > int8:*0*` says it."}
                     : std::vector<std::string>{
                           "`*1000*` is a number under `int64` and four characters under "
                           "`str`. `print.stdout` is a chain that calls rather than one "
                           "that declares a name, and a print declares no parameters "
                           "either, so nothing beside the value has said which it is and "
                           "the value is left to say it."});
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
      // The same notation says two things, and which one is settled by whether
      // the word names a type. `int32:*161*` says what a written value is; a
      // case name says which of the things a `one-of` may be this is. Both read
      // the same way: the left says how to read the right.
      if (typeNamed(e.text) == Type::Unknown)
        return madeCase(e, expected);
      const Ty stated = typeNamed(e.text);
      if (!e.children.empty())
        expr(*e.children[0], stated);
      return stated;
    }

    case ExprKind::Borrow:
      // What a transfer means is the ownership pass's business; the type of the
      // thing transferred is the type of what it names.
      //
      // Lending for writing is one of the ways a name changes without a `set`
      // of it appearing anywhere, so it is noted here.
      if (e.text == "loanmut" && !e.children.empty() &&
          e.children[0]->kind == ExprKind::Name)
        if (Symbol *held = lookupToChange(e.children[0]->text))
          held->everChanged = true;
      if (e.children.empty())
        return Type::Unknown;
      {
        // The type of what is lent, held the way this lends it. What a transfer
        // means is still the ownership pass's business; this is only so that a
        // program asking how something is held gets an answer.
        Ty lent = expr(*e.children[0], expected);
        // `move` is the third word here and is not a borrow: it hands the value
        // over for good, and what comes out the other side is held outright.
        if (e.text == "loan")
          lent.held = Held::Loan;
        else if (e.text == "loanmut")
          lent.held = Held::LoanMut;
        return lent;
      }

    case ExprKind::Group:
      return e.children.empty() ? Type::Unknown : expr(*e.children[0], expected);

    case ExprKind::Unary: {
      const Ty inner = e.children.empty() ? Ty{} : expr(*e.children[0], Type::Bool);
      couldNotCheck(inner, e.span, "`not` was not checked here.",
                    "`not` asks about a `bool`, and what this is could not be worked out");
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
        return unknownFrom(e.span);
      }
      return expected;

    case ExprKind::Several: {
      // Brackets where an item goes: several values, made where they stand.
      // What they are is what the `many` around them holds, one level in.
      if (!expected.holds()) {
        complain(e.span, "E0506",
                 expected.kind == Type::Unknown
                     ? std::string("nothing here says what these several values are.")
                     : "a `" + name(expected) + "` is one value, and these are several.",
                 {"brackets where an item goes make a `many`"},
                 {"they take their type from the `many` they go into, and there is "
                  "none here to take it from."});
        for (const ExprPtr &child : e.children)
          if (child)
            expr(*child, Ty{});
        return unknownFrom(e.span);
      }
      const Ty holds = elementOf(expected);
      for (const ExprPtr &child : e.children) {
        if (!child)
          continue;
        const Ty got = expr(*child, holds);
        couldNotCheck(got, child->span,
                      "this was not checked against a `" + name(holds) + "`.",
                      "a `many` holds one type, and what this is could not be worked "
                      "out");
        if (got != Ty{} && got != holds)
          complain(child->span, "E0506",
                   "this is a `" + name(got) + "`, and a `" + name(expected) +
                       "` holds `" + name(holds) + "`.",
                   {"nothing converts on its own"});
      }
      return expected;
    }

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
    if (of == Ty{}) {
      couldNotCheck(of, e.span, "`" + e.text + "` was not looked for here.",
                    "a field is one of the things a struct holds, and what this is "
                    "could not be worked out");
      return of; // Carries the trace on, rather than starting a fresh unknown.
    }
    if (!of.isStruct() || of.orNothing) {
      complain(e.span, "E0527",
               "a `" + name(of) + "` has no fields.",
               {"a field is one of the things a struct holds"},
               of.orNothing ? std::vector<std::string>{
                                  "what may hold nothing has to be asked before it "
                                  "can be reached into."}
                            : std::vector<std::string>{});
      return unknownFrom(e.span);
    }
    const Shape &shape = result_.shapes[of.named];
    for (const Field &one : shape.fields)
      if (one.name == e.text)
        return one.type;
    complain(e.span, "E0528",
             "`" + shape.name + "` has no field called `" + e.text + "`.",
             {"a field is one of the things a struct holds"},
             {"what it does hold is written where it was declared."});
    return unknownFrom(e.span);
  }

  // `'xs'[*2*]` — the place a value sits, and the type of what sits there.
  Ty element(const Expr &e) {
    if (!e.children.empty())
      expr(*e.children[0], Type::Int64);
    // What is being reached into: a name, or something already reached into.
    // `'g'[*0*][*1*]` is the second, and there is no name on it — the first
    // reach is what it reaches into.
    const std::string called = e.children.size() > 1 ? std::string("this")
                                                     : "`'" + e.text + "'`";
    Ty of;
    if (e.children.size() > 1) {
      of = expr(*e.children[1], Ty{});
    } else {
      const Symbol *symbol = lookup(e.text);
      if (!symbol) {
        complain(e.span, "E0501", "`'" + e.text + "'` is not declared.",
                 {"a name means something only after a declaration says what it means"});
        return unknownFrom(e.span);
      }
      of = symbol->type;
    }
    if (!of.holds()) {
      if (of.kind == Type::Unknown) {
        couldNotCheck(of, e.span,
                      called + " was not read as something holding several.",
                      "an element is one of the values a `many` holds, and what this "
                      "is could not be worked out");
        return of;
      }
      complain(e.span, "E0514",
               called + " is a `" + name(of) +
                   "`, and holds one value rather than several.",
               {"an element is one of the values a `many` holds"},
               {"a name holding one value is that value, and there is no first of it."});
      return unknownFrom(e.span);
    }
    if (!e.children.empty()) {
      const Ty where = result_.of(e.children[0].get());
      couldNotCheck(where, e.children[0]->span, "this index was not checked.",
                    "an index is an `int64`, and what this is could not be worked out");
      if (where != Ty{} && where != Ty{Type::Int64})
        complain(e.children[0]->span, "E0506",
                 "an index is an `int64`, and this is a `" + name(where) + "`.",
                 {"nothing converts on its own"},
                 {"`count` answers an `int64`, and two sizes never meet on their own."});
    }
    return elementOf(of);
  }

  // What a condition has to be, which depends on whether a name is being lent
  // what it holds. Without `holds` it is a `bool`; with it, a thing that may
  // hold nothing — and asking a `bool` to hold something is its own mistake,
  // because a `bool` is never absent.
  Ty asked(const Expr &condition, const std::string &holds, Span where,
           const char *what) {
    if (holds.empty()) {
      const Ty type = expr(condition, Type::Bool);
      couldNotCheck(type, condition.span, "this condition was not checked.",
                    "a condition is a `bool`, and what this is could not be worked out");
      if (type != Ty{} && type != Ty{Type::Bool})
        complain(condition.span, "E0506",
                 std::string(what) + " asks a `bool`, and this is a `" + name(type) + "`.",
                 {"nothing converts on its own"});
      return type;
    }
    const Ty type = expr(condition, Ty{});
    couldNotCheck(type, where, "this was not checked for whether it may be missing.",
                  "`holds` lends what may not be there, and what this is could not be "
                  "worked out");
    if (type != Ty{} && !type.mayBeNothing())
      complain(where, "E0519",
               "`holds` lends what may not be there, and a `" + name(type) +
                   "` is always something.",
               {"a type says whether it may hold nothing"},
               {"without `or-nothing` there is no absence to ask about, so the arm "
                "would run every time and lend the same value every time."});
    return type;
  }

  // Whether what is being read stands in a comparison, or in a sum inside one —
  // the two places where nothing declares a type for a written value to take.
  bool comparing_ = false;

  Ty binary(const Expr &e, Ty expected) {
    const std::string &op = e.text;
    const bool comparing = op == "<" || op == ">" || op == "<==" || op == ">==" ||
                           op == "==" || op == "!==";
    const bool logical = op == "and" || op == "or";

    // Arithmetic answers with what it was given, so the type wanted here is the
    // type wanted of it — and it reaches both sides, which is how a written
    // value in a sum gets a size at all. `set 'total' = ['total' + *1*];` asks
    // both sides for what `'total'` was declared as.
    //
    // A comparison wants nothing, so neither side is told anything. It used to
    // read the left and then hand *that* to the right, which was the one place
    // in the language where a value took its type from the thing standing beside
    // it rather than from the slot it goes into. Every other written value has a
    // destination whose type was declared — a name, a parameter, a field, a
    // loop's counter — and `*0*` in `'n' > *0*` has none. It says its own now:
    // `'n' > int8:*0*`.
    //
    // The same for a sum with nowhere to land. In `if ('n' x *4*) > 'limit'` the
    // `x` is asked for nothing, so `*4*` has no slot either, and borrowing one
    // from `'n'` was the same sleight of hand one level down.
    // A blank reaches both sides too. `any` is a declared type — the signature
    // said it — and a written value taking it is taking what the slot it goes
    // into was declared as, which is the rule rather than an exception to it.
    // It is filled in with everything else when the generic is written out.
    const bool wanted = isNumber(expected) || expected.kind == Type::Blank;
    const Ty asked = logical ? Ty{Type::Bool}
                             : (comparing ? Ty{} : (wanted ? expected : Ty{}));
    // While either side is read, so a value with nothing to take a type from can
    // be told why there is nothing.
    const bool nothingAsked = comparing || (!logical && asked == Ty{});
    const bool was = comparing_;
    comparing_ = nothingAsked;

    Ty left, right;
    if (!nothingAsked) {
      left = expr(*e.children[0], asked);
      right = expr(*e.children[1], asked);
    } else {
      // A blank is the one type a written value cannot be told to be, because
      // naming it is the caller's to do and the author has no word for it. So
      // beside a blank the value takes the blank, and is filled in with it when
      // the generic is written out. Everywhere else a comparison tells neither
      // side anything.
      //
      // The side that is not a written value is read first, so this works
      // whichever way round the two were written.
      const bool writtenLeft = e.children[0]->kind == ExprKind::Written;
      const bool writtenRight = e.children[1]->kind == ExprKind::Written;
      if (writtenLeft && !writtenRight) {
        right = expr(*e.children[1], Ty{});
        left = expr(*e.children[0], right.kind == Type::Blank ? right : Ty{});
      } else {
        left = expr(*e.children[0], Ty{});
        right = expr(*e.children[1], left.kind == Type::Blank ? left : Ty{});
      }
    }
    comparing_ = was;

    // One side could not be worked out, because something it was built from was
    // already refused. Nothing here can be checked against anything, and every
    // check below would pass over it without a word — which is how a reader
    // loses sight of how far one mistake reached. Said once for the whole
    // operation rather than once per side: it is one thing that went wrong.
    const Ty broken = eitherTrace(left, right);
    if (broken.tracedBack()) {
      because(broken.from, e.span, "E0506",
              "`" + op + "` was not checked here.",
              {"an operation is checked against what its sides are, and what one of "
               "them is could not be worked out"});
      return comparing || logical ? Ty{Type::Bool} : unknownFrom(broken.from);
    }

    if (comparing) {
      // Two `one-of`s are the same when they are in the same case *and* what
      // they hold is the same, and what they hold is a different type in every
      // case. Comparing them as one value asked the same question of two
      // different things and the engines gave different answers.
      for (Ty side : {left, right})
        if (side.kind == Type::OneOf) {
          complain(e.span, "E0506",
                   "a `" + name(side) + "` is one of several things, and there is no "
                   "comparing which without asking.",
                   {"two values are compared when they are the same kind of thing"},
                   {"`when` is the asking, and what each case holds is compared on "
                    "its own."});
          return Type::Bool;
        }
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
    return answered == Type::Unknown ? eitherTrace(left, right).kind == Type::Unknown
                                            ? eitherTrace(left, right)
                                            : Ty{Type::Unknown}
                                    : answered;
  }

  Ty call(const Expr &e, Ty expected) {
    const std::string path = joined(e.path);

    // A struct named where an item goes makes one there: `point[*0* *0*]`. It
    // is written as a call because that is what a word before a bracket is,
    // and a word can never be mistaken for an index the way a name could.
    for (unsigned which = 0; which < result_.shapes.size(); ++which)
      if (result_.shapes[which].name == path) {
        if (e.args.values.size() != 1) {
          complain(e.span, "E0529",
                   "`" + path + "` holds " +
                       std::to_string(result_.shapes[which].fields.size()) +
                       ", and this is " + std::to_string(e.args.values.size()) + ".",
                   {"a struct is made with one value for each of the things it holds"});
          return structNamed(which);
        }
        return filled(e.args.values[0].items, e.args.values[0].span, structNamed(which));
      }

    // `count` asks how many, of a `str` and of a `many` alike: the same
    // question, and the type already says what is being counted.
    if (path == "count" && e.args.values.size() == 1) {
      const Ty got = value(e.args.values[0], Ty{});
      couldNotCheck(got, e.args.values[0].span, "this was not checked as something to count.",
                    "`count` counts a `str` or a `many`, and what this is could not be "
                    "worked out");
      if (got != Ty{} && got.kind != Type::Str && !got.holds())
        complain(e.args.values[0].span, "E0506",
                 "`count` counts a `str` or a `many`, and this is a `" + name(got) + "`.",
                 {"nothing converts on its own"});
      return Type::Int64;
    }

    // A number out of text, which is where text stops being text. Which number
    // is the question the chain beside it has already answered, the same way
    // `fill` knows what it is filling.
    // Nothing converts on its own, so this is how it is asked for. It answers a
    // `str` rather than `or-nothing` of one, because every number has a way of
    // being written and this cannot fail.
    if (path == "convert-to-str") {
      if (e.args.values.size() != 1) {
        complain(e.span, "E0505",
                 "`convert-to-str` is given " + std::to_string(e.args.values.size()) +
                     " and wants 1.",
                 {"a call gives a function what its parameters ask for"});
        return Type::Str;
      }
      const Ty got = value(e.args.values[0], Ty{});
      couldNotCheck(got, e.args.values[0].span,
                    "this was not checked as something with a way of being written.",
                    "a value is written out the way it is shown, and what this is could "
                    "not be worked out");
      // Whatever a print can write, this can write into a `str`, because it is
      // the same walk: a `many` and a struct write what they hold, one value
      // after another. What is left is text, which is already text, and an
      // absence, which is not a value to write.
      Ty absent;
      std::string where;
      const bool several = got.holds() || got.isStruct();
      if (got != Ty{} && !isNumber(got) && got != Ty{Type::Bool} && !several &&
          got.kind != Type::OneOf)
        complain(e.args.values[0].span, "E0535",
                 got == Ty{Type::Str}
                     ? std::string("this is already text.")
                     : "there is no one way to write a `" + name(got) + "` out.",
                 {"a value is written out the way it is shown"},
                 got == Ty{Type::Str}
                     ? std::vector<std::string>{"a `str` is text, so there is nothing "
                                                "here to convert."}
                     : std::vector<std::string>{
                           "`holds` and `when` open what may be missing."});
      else if (got.kind == Type::OneOf)
        complain(e.args.values[0].span, "E0535",
                 "a `" + name(got) + "` is one of several things, and there is no "
                 "writing which without asking.",
                 {"a value is written out the way it is shown"},
                 {"`when` is the asking, and every case it covers has a way of "
                  "being written."});
      else if (several && holdsNothingSomewhere(got, absent, where))
        complain(e.args.values[0].span, "E0535",
                 "`" + where + "` inside this " +
                     (absent.kind == Type::OneOf ? "is one of several things"
                                                 : "may hold nothing") +
                     ", and there is no writing it without asking.",
                 {"a value is written out the way it is shown"},
                 {"`holds` and `when` open what may be missing."});
      return Type::Str;
    }

    if (path == "convert-to-number") {
      const Ty wanted = expected.mayBeNothing() ? expected.within() : expected;
      if (!isNumber(wanted)) {
        complain(e.span, "E0523",
                 expected.kind == Type::Unknown
                     ? "nothing here says what number this would be."
                     : "`convert-to-number` answers a number, and a `" + name(expected) +
                           "` is not one.",
                 {"a size is always written, and only sizes the standard defines"},
                 {"text that is not a number has no number in it, so this answers "
                  "`or-nothing` of whichever number was asked for — and the chain "
                  "beside it is what asks."});
        for (const Value &v : e.args.values)
          (void)value(v, Ty{});
        return unknownFrom(e.span);
      }
      if (e.args.values.size() != 1)
        complain(e.span, "E0505",
                 "`convert-to-number` is given " + std::to_string(e.args.values.size()) +
                     " and wants 1.",
                 {"a call gives a function what its parameters ask for"});
      if (!e.args.values.empty()) {
        const Ty got = value(e.args.values[0], Ty{Type::Str});
        couldNotCheck(got, e.args.values[0].span, "this was not checked as text.",
                      "`convert-to-number` reads text, and what this is could not be "
                      "worked out");
        if (got != Ty{} && got != Ty{Type::Str})
          complain(e.args.values[0].span, "E0506",
                   "`convert-to-number` reads text, and this is a `" + name(got) + "`.",
                   {"nothing converts on its own"});
      }
      return orNothingOf(wanted);
    }

    // `fill` writes one value into every place, so it needs a value that can be
    // copied — there is no copying a `str`, and nothing to put in each place.
    if (path == "fill") {
      if (!expected.holds()) {
        complain(e.span, "E0507", "nothing here says what `fill` is filling.",
                 {"a written value takes its type from what it is written into, from the "
                  "parameter it is passed to, or from itself"},
                 {"`fill` answers a `many`, and which `many` is a question the chain "
                  "beside it has already answered everywhere it is allowed to stand."});
        for (const Value &v : e.args.values)
          (void)value(v, Ty{});
        return unknownFrom(e.span);
      }
      const Ty holds = elementOf(expected);
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
      // What the language has, where the name written is nearly one of them.
      // The tip that was here said a variable wears marks and a bare word does
      // not, which is about a different mistake entirely — somebody who wrote
      // `print.stderr` was told about quote marks and never told what the
      // `print` family holds.
      std::vector<std::string> tips = {
          "a variable is a name and wears marks; a bare word is a function."};
      if (path.rfind("print.", 0) == 0)
        tips = {"a `print` says where it goes, and there is `stdout` and `stderr`."};
      else if (path.rfind("read.", 0) == 0)
        tips = {"the only `read` is `read.stdin`."};
      complain(e.span, "E0504", "`" + path + "` is not a function.",
               {"a word followed by `[` is a call, and a call needs something to call"},
               tips);
      for (const Value &value : e.args.values)
        for (const ExprPtr &item : value.items)
          expr(*item, Type::Unknown);
      return Type::Unknown;
    }

    const Signature &signature = found->second;
    if (signature.variadic) {
      // A print states no parameter types, so each value must say what it is.
      for (const Value &v : e.args.values)
        for (const ExprPtr &item : v.items)
          showable(*item, expr(*item, Ty{}));
      return signature.result;
    }

    if (e.args.values.size() != signature.params.size()) {
      complain(e.span, "E0505",
               "`" + path + "` is given " + std::to_string(e.args.values.size()) +
                   " and wants " + std::to_string(signature.params.size()) + ".",
               {"a call gives a function what its parameters ask for"});
    }
    // A blank is the caller's to fill. The first argument whose parameter has one
    // says what it is, and every other `any` in the signature is then that —
    // which is what makes `any` one blank rather than a wildcard per spot.
    Ty blank{};
    for (unsigned i = 0; i < e.args.values.size(); ++i) {
      const Ty asked = i < signature.params.size() ? signature.params[i] : Ty{};
      if (blank == Ty{} && hasBlank(asked)) {
        blank = blankFrom(asked, value(e.args.values[i], Ty{}));
        // What the blank said it would take, asked here — the one place where
        // the caller and the type they brought are both in view.
        //
        // This is the whole of what a family word buys. Without it the call goes
        // through, and `largest` is refused somewhere inside itself for using
        // `>` on a `point` — at a line the caller never wrote and cannot read as
        // being about them. With it, the refusal lands on the call.
        if (blank != Ty{} && !inFamily(blank, asked.asks)) {
          complain(e.args.values[i].span, "E0539",
                   "`" + name(blank) + "` is not " + asksFor(asked.asks) + ", and `" +
                       path + "` asks for one.",
                   {"a blank takes what it says it takes"},
                   {"bare `any` takes anything, and what can be done with it is what "
                    "can be done with every type. Every word after it buys one thing "
                    "more, and turns away the types that cannot do it."});
          // Refused, so what this call answers is not known — and everything
          // built on it is this refusal showing up again rather than news.
          blank = unknownFrom(e.args.values[i].span);
        }
        continue;
      }
      const Ty want = filledIn(asked, blank);
      const Ty got = value(e.args.values[i], want);
      // Same as anywhere else a check has to be skipped: say that it was, and
      // say what made it impossible, rather than passing over it in silence.
      if (want.kind != Type::Unknown && got.tracedBack()) {
        because(got.from, e.args.values[i].span, "E0506",
                "this was not checked against what `" + path + "` wants.",
                {"a call gives a function what its parameters ask for, and what this "
                 "is could not be worked out"});
        continue;
      }
      if (want != Type::Unknown && got != Type::Unknown && got != want)
        complain(e.args.values[i].span, "E0506",
                 "this is a `" + std::string(name(got)) + "` and `" + path + "` wants a `" +
                     std::string(name(want)) + "`.",
                 {"nothing converts on its own"});
    }
    if (blank != Ty{}) {
      calledWith(path, blank);
      result_.blankAt[&e] = spelledAs(blank);
    }
    return filledIn(signature.result, blank);
  }

  // Showing writes one piece after another, so what it writes has to be one
  // piece. Three things are not, and each is refused where it stands.
  //
  // Only the `many` was refused until 2026-09-07. A struct and an `or-nothing`
  // wrote nothing at all and the program ran to the end saying so — the same
  // silence in all three engines, which is why the oracle never saw it: they
  // agreed, and agreeing about nothing is agreeing.
  void showable(const Expr &item, Ty got) {
    couldNotCheck(got, item.span, "this was not checked as something showable.",
                  "showing writes one piece after another, and what this is could not "
                  "be worked out");
    Ty absent;
    std::string where;
    if (holdsNothingSomewhere(got, absent, where)) {
      const bool oneOf = absent.kind == Type::OneOf;
      const std::string what = oneOf ? " is one of several things"
                                     : " may hold nothing";
      complain(item.span, "E0536",
               where.empty() ? "this" + what + ", and showing it would not say which."
                             : "`" + where + "` inside this" + what +
                                   ", and showing it would not say which.",
               {"there is no way to reach what is inside without asking first"},
               oneOf
                   ? std::vector<std::string>{"`when` is the asking, and every case "
                                              "it covers can be shown on its own."}
                   : std::vector<std::string>{
                         "`holds` and `when` are the asking. Written straight out, an "
                         "absent `str` and an empty one would look the same, and what "
                         "else an absence should look like is a decision nobody has "
                         "made."});
    }
  }

  // Whether anything inside this, however deep, may hold nothing. A `many` of
  // them and a struct with one among its fields are the same question: showing
  // writes what is there, and an absence is not there to be written.
  bool holdsNothingSomewhere(Ty got, Ty &absent, std::string &where,
                             unsigned depth = 0) const {
    if (depth > 32) // a struct cannot hold itself (`E0526`), so this is a guard
      return false; // against a shape table that never finished being built
    // Both are values that are one of several things, and writing one out would
    // not say which — an absence has nothing to write, and a `one-of` in one
    // case looks like the same characters as another in a different one.
    if (got.mayBeNothing() || got.kind == Type::OneOf) {
      absent = got;
      return true;
    }
    if (got.holds())
      return holdsNothingSomewhere(elementOf(got), absent, where, depth + 1);
    if (!got.isStruct() || got.named >= result_.shapes.size())
      return false;
    for (const Field &field : result_.shapes[got.named].fields) {
      std::string deeper;
      if (!holdsNothingSomewhere(field.type, absent, deeper, depth + 1))
        continue;
      // Named from the outside in, so the reader can follow it down.
      where = deeper.empty() ? field.name : field.name + "." + deeper;
      return true;
    }
    return false;
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
          couldNotCheck(got, only.span,
                        "this value was not checked against a `" + name(expected) + "`.",
                        "a name holds what it was given, and what this is could not be "
                        "worked out");
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
      couldNotCheck(got, v.span,
                    "this value was not checked against a `" + name(expected) + "`.",
                    "a name holds what it was given, and what this is could not be "
                    "worked out");
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
      couldNotCheck(got, item->span, "this piece was not checked as text.",
                    "pieces side by side join, and what this is could not be worked out");
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
    const Ty holds = elementOf(want);
    if (v.items.empty())
      return want;
    if (v.items.size() == 1 && selfTyped(*v.items[0])) {
      const Ty got = expr(*v.items[0], want);
      couldNotCheck(got, v.items[0]->span,
                    "this was not checked against a `" + name(want) + "`.",
                    "a `many` holds one type, and what this is could not be worked out");
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
      couldNotCheck(got, item->span,
                    "this was not checked against a `" + name(holds) + "`.",
                    "a `many` holds one type, and what this is could not be worked out");
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
  Ty grouped(const Value &v, Ty want) { return filled(v.items, v.span, want); }

  Ty filled(const std::vector<ExprPtr> &items, Span where, Ty want) {
    const Value v{where, {}};
    (void)v;
    const Shape &shape = result_.shapes[want.named];
    if (items.size() == 1 && selfTyped(*items[0])) {
      const Ty got = expr(*items[0], want);
      if (got == want || got == Ty{})
        return want;
    } else if (items.size() != shape.fields.size()) {
      complain(where, "E0529",
               "`" + shape.name + "` holds " + std::to_string(shape.fields.size()) +
                   ", and this is " + std::to_string(items.size()) + ".",
               {"a struct is made with one value for each of the things it holds"},
               {"they go in the order the struct was written in, so leaving one out "
                "would silently move every one after it."});
      // Which item was meant for which of them is not knowable once the count
      // is wrong, so pairing them off and complaining about each is guessing —
      // and it said the same span was wrong twice for the one mistake.
      return want;
    }
    for (unsigned i = 0; i < items.size(); ++i) {
      const Ty wanted = i < shape.fields.size() ? shape.fields[i].type : Ty{};
      // What goes into something that may hold nothing is asked for as the
      // thing itself, the way a value list already asks for it — so a sum can
      // be written there, and a written number in one knows what it is. Asked
      // with the `or-nothing` still on, `*1.5* x *2*` had nothing saying what
      // its pieces were, and the same expression into a *name* of the same type
      // was taken without a word.
      //
      // An absence is the exception: it is the whole of the type rather than
      // what the type holds, and it is what says so.
      const Ty asked = wanted.mayBeNothing() && items[i]->kind != ExprKind::Nothing
                           ? wanted.within()
                           : wanted;
      const Ty got = expr(*items[i], asked);
      if (wanted != Ty{})
        couldNotCheck(got, items[i]->span,
                      "this was not checked against what `'" + shape.fields[i].name +
                          "'` is.",
                      "a struct is made with one value for each of the things it holds, "
                      "and what this is could not be worked out");
      // A value going into something that may hold nothing is that value, held
      // — which is what a name is given, and was not what one of the things a
      // struct holds was given. `var.or-nothing.int8 'o' = ['n'];` was taken and
      // the same value into the same type inside a struct was refused, which is
      // one rule answered two ways.
      const bool heldInstead = wanted.mayBeNothing() && got == wanted.within();
      if (wanted != Ty{} && got != Ty{} && got != wanted && !heldInstead)
        complain(items[i]->span, "E0506",
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
    sayWhatWasNotNeeded(scopes_.back());
    scopes_.pop_back();
  }

  // Said as the scope closes, when everything that could have changed a name
  // has been read.
  void sayWhatWasNotNeeded(const std::unordered_map<std::string, Symbol> &scope) {
    for (const auto &[name, held] : scope) {
      if (!held.changeable || held.everChanged)
        continue;
      warn(held.span, "W0003", "`'" + name + "'` never changes.",
           {"a chain says what is unusual, and says nothing else"},
           {"`mut` asks for something that is then not done: nothing sets it, "
            "nothing writes a place or a field of it, and nothing lends it out "
            "for writing. Without the word it would be the same name."});
    }
  }

  void statement(const Stmt &s) {
    switch (s.kind) {
    case StmtKind::Declare: {
      const Ty type = typeOfChain(s.chain);
      result_.declarations[&s] = type;
      onlyValueChecked(s.value, type, s.span);
      Symbol made{type, changeable(s.chain), s.nameSpan, wrapsChain(s.chain)};
      if (isWhole(type) && !made.wraps)
        result_.intoPlainNames.push_back(s.span);
      if (made.wraps)
        result_.mayWrap.insert(&s);
      if (isWhole(type)) {
        __int128 given = 0;
        if (wholeItemOf(s.value, given)) {
          made.knownStart = true;
          made.start = given;
        }
      }
      declare(s.name, made);
      break;
    }

    case StmtKind::Add: {
      Symbol *held = lookupToChange(s.name);
      const Symbol *said = lookup(s.name);
      if (!said) {
        complain(s.nameSpan, "E0501", "`'" + s.name + "'` is not declared.",
                 {"a name means something only after a declaration says what it means"});
        onlyValue(s.value, Ty{});
        break;
      }
      if (!said->type.grows) {
        complain(s.nameSpan, "E0546",
                 "`'" + s.name + "'` is a `" + name(said->type) +
                     "`, and how many places it has was settled when it was made.",
                 {"a `many` holds the places it was made with, and a `many-growing` "
                  "may hold more"},
                 {"`many-growing` is the one that grows. They are two types because "
                  "growing may move every place, and a borrow into one cannot outlive "
                  "that."});
        onlyValue(s.value, Ty{});
        break;
      }
      // Adding is changing, so a name that does not change cannot be added to —
      // the same rule, and the same word, as writing one of its places.
      if (held)
        held->everChanged = true;
      if (!said->changeable) {
        complain(s.nameSpan, "E0508", "`'" + s.name + "'` does not change.",
                 {"a chain says `mut` when a name may be written through"},
                 {"growing is changing: there is one more place afterwards than "
                  "there was."});
      }
      onlyValueChecked(s.value, elementOf(said->type), s.span);
      break;
    }

    case StmtKind::Set: {
      if (Symbol *held = lookupToChange(s.name)) {
        held->knownStart = false;
        held->everChanged = true;
      }
      // What the sum is written into: the name, or the field the path arrives
      // at. `wrapping` is said where a thing is declared, and one of the things
      // a struct holds is declared in the struct.
      if (const Symbol *said = lookup(s.name)) {
        bool wraps = said->wraps;
        Ty here = said->type;
        for (const std::string &step : s.fields) {
          const Field *field = fieldNamed(here, step);
          if (!field)
            break;
          wraps = field->wraps;
          here = field->type;
        }
        if (isWhole(here)) {
          if (wraps)
            result_.mayWrap.insert(&s);
          else
            result_.intoPlainNames.push_back(s.span);
        }
      }
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
          result_.setPath.erase(&s);
          want = Ty{};
          break;
        }
        const Shape &shape = result_.shapes[want.named];
        bool found = false;
        for (unsigned which = 0; which < shape.fields.size(); ++which)
          if (shape.fields[which].name == s.fields[i]) {
            want = shape.fields[which].type;
            result_.setPath[&s].push_back(which);
            found = true;
            break;
          }
        if (!found) {
          result_.setPath.erase(&s); // half a path is worse than none
          complain(s.fieldSpans[i], "E0528",
                   "`" + shape.name + "` has no field called `" + s.fields[i] + "`.",
                   {"a field is one of the things a struct holds"});
          want = Ty{};
          break;
        }
      }
      if (s.index) {
        const Ty at = expr(*s.index, Type::Int64);
        couldNotCheck(at, s.index->span, "this index was not checked.",
                      "an index is an `int64`, and what this is could not be worked out");
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
          want = elementOf(symbol->type);
        }
      }
      onlyValueChecked(s.value, want, s.span);
      break;
    }

    case StmtKind::LoopParts: {
      // What it walks has to be a struct, and has to be one *here* — by the time
      // this is read a generic has been written out at a real type, so there is
      // no waiting to find out.
      const Ty subject = onlyValue(s.value, Ty{});
      couldNotCheck(subject, s.value.span, "this was not checked as something to walk.",
                    "`loop.parts` walks a struct, and what this is could not be "
                    "worked out");
      if (subject == Ty{})
        break;
      if (!subject.isStruct() || subject.orNothing) {
        complain(s.value.span, "E0544",
                 "a `" + name(subject) + "` has no parts to walk.",
                 {"`loop.parts` walks a struct, and nothing else"},
                 {"a `many` holds one type in every place, so a counted loop already "
                  "reaches them — and a place has a position rather than a name, so "
                  "there would be nothing for `.name` to hold."});
        break;
      }
      // The body is not read here at all: what a field holds is a different type
      // on every turn, so there is no one reading of it. Each turn is its own
      // copy, with its own `'part'` declared at the top of it, and those are
      // read once they have been written.
      //
      // What the body *changes* is still noted, though. It is part of the
      // program — every turn of it is — and a name changed only in here looked
      // like a name never changed at all, so `W0003` said `'out'` never changes
      // about a line with `set 'out'` two lines under it.
      changedSomewhereIn(s.body);
      result_.walksParts[&s] = subject.named;
      break;
    }

    case StmtKind::Whichever: {
      // Decided here, and nowhere else. The subject has a concrete type by the
      // time this runs — a generic has already been written out once per type
      // it was called with — so which arm is meant is a question with one
      // answer, and the arms that were not chosen are never read at all. That
      // is the point of them: `is str` may call `count` on something that is a
      // `str` only in the copy where it is one.
      const Ty subject = s.condition ? expr(*s.condition, Ty{}) : Ty{};
      if (s.condition)
        couldNotCheck(subject, s.condition->span,
                      "this was not checked for what kind of thing it is.",
                      "a `whichever` chooses by what kind of thing something is, and "
                      "what this is could not be worked out");
      if (subject == Ty{})
        break;

      std::vector<Family> asked(s.branches.size(), Family::Anything);
      bool everyWordKnown = true;
      for (unsigned i = 0; i < s.branches.size(); ++i) {
        const Branch &arm = s.branches[i];
        if (!namesFamily(arm.family)) {
          complain(arm.familySpan, "E0540",
                   "`" + arm.family + "` is not a kind of thing.",
                   {"a `whichever` chooses by kind, and the kinds are a fixed list"},
                   {"`number`, `int`, `uint`, `bin`, `deci`, `str`, `bool`, `many`, "
                    "`or-nothing`, `struct`. A name of a type is not one of them: "
                    "twenty-odd arms doing the same thing is what the kinds are for."});
          everyWordKnown = false;
          continue;
        }
        asked[i] = familyNamed(arm.family);
      }
      if (!everyWordKnown)
        break;

      bool overlapping = false;
      // One `whichever` asks one question. What a value is and how it is held
      // are two, and a borrowed number answers a word from each — so an arm from
      // each list would leave two arms both answering with no level to pick
      // between them, which is not the mistake the overlap rule was written for.
      // Two statements say it, and nesting one inside the other says both.
      for (unsigned i = 1; i < s.branches.size(); ++i)
        if (axisOf(asked[i]) != axisOf(asked[0])) {
          const bool howFirst = axisOf(asked[0]) == Axis::How;
          complain(s.branches[i].familySpan, "E0543",
                   "`" + s.branches[i].family + "` and `" + s.branches[0].family +
                       "` are answers to different questions.",
                   {"a `whichever` asks one question"},
                   {std::string("what a value is and how it is held are two "
                                "questions, and a borrowed number answers one word "
                                "from each. Ask them in two `whichever`s — one "
                                "inside the other, if both matter. `owned`, `loan` "
                                "and `loanmut` say how; every other word says what."),
                    howFirst ? "this one says what it is, and the first says how it "
                               "is held."
                             : "this one says how it is held, and the first says "
                               "what it is."},
                   "here", {Note{s.branches[0].familySpan, "the question being asked"}});
          overlapping = true;
        }
      if (overlapping)
        break;

      // Two arms that could both answer leave the compiler picking, and there
      // is no rule here saying one word is nearer than another — the language
      // has no such idea anywhere else, and adding one for this would be adding
      // it everywhere.
      for (unsigned i = 0; i < s.branches.size(); ++i)
        for (unsigned j = i + 1; j < s.branches.size(); ++j)
          if (overlaps(asked[i], asked[j])) {
            complain(s.branches[j].familySpan, "E0541",
                     asked[i] == asked[j]
                         ? "`" + s.branches[j].family + "` is asked twice here."
                         : "`" + s.branches[i].family + "` and `" +
                               s.branches[j].family + "` both answer to some types.",
                     {"a `whichever` asks each kind once, and no two arms may overlap"},
                     {"pick a level: `number`, or the four under it. Refusing costs "
                      "nothing today, and going from refused to allowed later breaks "
                      "nothing written before it."},
                     "here", {Note{s.branches[i].familySpan, "and this one here"}});
            overlapping = true;
          }
      if (overlapping)
        break;

      unsigned chose = s.branches.size();
      for (unsigned i = 0; i < s.branches.size(); ++i)
        if (inFamily(subject, asked[i])) {
          chose = i;
          break;
        }
      if (chose == s.branches.size()) {
        complain(s.condition->span, "E0542",
                 "nothing here covers a `" + name(subject) + "`.",
                 {"every case a `whichever` covers is written out"},
                 {"which arm this is turned out to be is settled while compiling, so "
                  "an uncovered one is not a case that might never come up — it is "
                  "this program, now, with nothing to do."});
        break;
      }
      result_.chosenArm[&s] = chose;
      // Only the arm that was chosen. Reading the rest would be reading code
      // written for a type that is not here.
      scopes_.emplace_back();
      for (const StmtPtr &inner : s.branches[chose].body.stmts)
        statement(*inner);
      scopes_.pop_back();
      break;
    }

    case StmtKind::When: {
      // Every case, once each. The compiler insisting on that is the whole
      // reason to write a `when` rather than an `if` — a case nobody wrote is a
      // case nobody thought about, and it would be found by the program running
      // rather than by reading it.
      const Ty subject = s.condition ? expr(*s.condition, Ty{}) : Ty{};
      if (s.condition)
        couldNotCheck(subject, s.condition->span,
                      "this was not checked for what it could be.",
                      "a `when` chooses between the things a value could be, and what "
                      "this is could not be worked out");
      if (subject.kind == Type::OneOf && !subject.mayBeNothing()) {
        whenOverASum(s, subject);
        break;
      }
      if (subject != Ty{} && !subject.mayBeNothing()) {
        complain(s.condition->span, "E0520",
                 "a `" + name(subject) + "` is only ever one thing, so there is "
                 "nothing here to choose between.",
                 {"a `when` chooses between the things a value could be"},
                 {"`or-nothing` in the chain is what gives a value a second shape, "
                  "and a `one-of` is what gives it as many as it names; without "
                  "either there is one case and an `if` says it better."});
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

    case StmtKind::Unsafe: {
      ++insideUnsafe_;
      block(s.body);
      --insideUnsafe_;
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
      askedOutsideUnsafe(s.chain);
      const Ty type = typeOfChain(s.chain);
      result_.declarations[&s] = type;
      if (s.value.values.size() != 2)
        complain(s.value.span, "E0505", "a counted loop runs between two values.",
                 {"`[first, last]` says where a count starts and stops"});
      // `E0531` stood here and is gone. It refused a loop counting to the most
      // its counter could hold, because the loop stepped the counter and then
      // asked whether it had gone too far — and that one more step came round,
      // so the loop never finished. The loop asks before it steps now, so it
      // finishes like any other and there is nothing to refuse.
      //
      // It was only ever half a guard anyway: it could see an end written down
      // and not one worked out while the program runs, and the second never
      // finished either. Both end now.

      // Where a count starts and stops is counted in, so both are the
      // counter's own type. Asking and throwing the answer away let a `bin64`
      // or a `str` stand as a bound, which the engines then disagreed about:
      // the interpreters counted no times and the native code counted three.
      for (const Value &v : s.value.values) {
        const Ty got = value(v, type);
        if (type != Type::Unknown)
          couldNotCheck(got, v.span,
                        "this bound was not checked against a `" + name(type) + "`.",
                        "where a count starts and stops is counted in, and what this is "
                        "could not be worked out");
        if (type != Type::Unknown && got != Type::Unknown && got != type)
          complain(v.span, "E0506",
                   "this is a `" + std::string(name(got)) + "` and a `" +
                       std::string(name(type)) + "` was wanted.",
                   {"nothing converts on its own"},
                   {"a counted loop counts in its own type, so where it starts and "
                    "stops are that type too."});
      }
      const bool keeps = keepsCounter(s.chain);
      if (keeps)
        declare(s.name, Symbol{type, false, s.nameSpan});
      scopes_.emplace_back();
      if (!keeps)
        declare(s.name, Symbol{type, false, s.nameSpan});
      // Before the body is walked, not after: a `set` in it is what says the
      // name no longer holds what it was given, and this is asking where it
      // started from.
      countingUp(s, type);
      reachingInto(s);
      ++loopDepth_;
      for (const StmtPtr &inner : s.body.stmts)
        statement(*inner);
      --loopDepth_;
      scopes_.pop_back();
      break;
    }

    case StmtKind::LoopWhile: {
      askedOutsideUnsafe(s.chain);
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
    // What this holds could not be worked out, because something it was built
    // from was already refused. This used to be passed over without a word,
    // which left the reader unable to see how far one mistake had reached —
    // and quietly not checking a thing is worse than saying so.
    //
    // Only when the name here is a real type: a declaration whose own type word
    // was refused has said so on this very line, and pointing at it again would
    // be telling somebody twice.
    if (want.kind != Type::Unknown && got.tracedBack()) {
      because(got.from, list.values[0].span.begin ? list.values[0].span : where, "E0506",
              "this value was not checked against a `" + std::string(name(want)) + "`.",
              {"a name holds what it was given, and what this is could not be worked out"});
      return;
    }
    if (want != Type::Unknown && got != Type::Unknown && got != want)
      complain(list.values[0].span.begin ? list.values[0].span : where, "E0506",
               "this is a `" + std::string(name(got)) + "` and a `" + std::string(name(want)) +
                   "` was wanted.",
               {"nothing converts on its own"});
  }

  // Whether every way out of here hands back an answer. A loop does not count:
  // it may run no times at all, and then it has answered nothing.
  bool alwaysGives(const Block &block) const {
    for (const StmtPtr &s : block.stmts) {
      if (s->kind == StmtKind::Give)
        return true;
      // A `whichever` is the arm it chose and nothing else, so it answers when
      // that arm answers. Asking every arm would refuse a `show` that gives from
      // all of them — which is the shape everybody writes — because an arm that
      // is not here cannot be read, and an arm that is here is the whole of it.
      if (s->kind == StmtKind::Whichever) {
        const auto chose = result_.chosenArm.find(s.get());
        if (chose != result_.chosenArm.end() && chose->second < s->branches.size() &&
            alwaysGives(s->branches[chose->second].body))
          return true;
        continue;
      }
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
    case ItemKind::OneOf:
      break; // read before anything else, and it has no body to walk

    case ItemKind::Const:
      // What it answers with, written down against it the way a function's is.
      // A constant lowers to a body that answers with its value, so everything
      // below wants the same answer for both — and nothing below the checker
      // should be reading a chain to work one out.
      result_.items[&item] = typeOfChain(item.chain);
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
                                   param.nameSpan, wrapsChain(param.chain)});
      // A generic's body is not read with the blank still in it. Almost nothing
      // in it would hold: reaching into a `many.any` looks like taking a value
      // that does not copy out of a place that must hold one, because whether
      // it copies is exactly what the blank has not said yet. The body is read
      // once per type it is called with, after the blank is filled, and that is
      // where its mistakes are found.
      const bool generic = hasBlank(giving_) || [&] {
        for (const Param &param : item.params)
          if (hasBlank(typeOfChain(param.chain)))
            return true;
        return false;
      }();
      for (const StmtPtr &s : item.body.stmts)
        if (!generic)
          statement(*s);
      if (!generic && giving_ != Type::Nothing && giving_ != Type::Unknown &&
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
