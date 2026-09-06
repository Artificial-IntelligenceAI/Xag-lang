#include "xag/Parser.h"

#include <array>
#include <string>

namespace xag {
namespace {

// The words a chain leaves out when they are what a name already is. Writing one
// is an error: a chain says what is unusual, and says nothing else.
constexpr std::array<std::string_view, 4> kDefaults{"immut", "own", "file", "temp"};

bool isDefault(std::string_view word) {
  for (std::string_view d : kDefaults)
    if (d == word)
      return true;
  return false;
}

// Operators mathematics never settled an order for. They take brackets.
bool isUnsettled(std::string_view word) {
  return word == "mod" || word == "and" || word == "or";
}

// ---- what a chain may say
//
// A chain is a run of answers to questions the language asks, and the questions
// come in an order. Everything below exists so that a segment which answers no
// question at all — `var.banana.int64`, `var.arr.int64` — is refused where it
// stands, rather than being read past on the way to the type.

enum class Slot {
  Unknown,
  Kind,       // var, fn, const, loop
  Visibility, // export / program, default file
  Mutability, // mut, default immut
  Ownership,  // ref / refmut, default own
  Lifetime,   // 'life' — a name for a loan
  Counter,    // perm, default temp
  Form,       // range / while
};

// The question a slot answers, said the way the reader would ask it.
const char *question(Slot slot) {
  switch (slot) {
  case Slot::Kind:       return "what is being declared";
  case Slot::Visibility: return "who may see it";
  case Slot::Mutability: return "whether it changes";
  case Slot::Ownership:  return "whether it owns or borrows";
  case Slot::Lifetime:   return "which loan it is on";
  case Slot::Counter:    return "whether the counter outlives the loop";
  case Slot::Form:       return "which kind of loop this is";
  case Slot::Unknown:    break;
  }
  return "nothing";
}

Slot slotOf(std::string_view word) {
  if (word == "var" || word == "fn" || word == "const" || word == "loop" ||
      word == "struct")
    return Slot::Kind;
  if (word == "export" || word == "program" || word == "file")
    return Slot::Visibility;
  if (word == "mut" || word == "immut")
    return Slot::Mutability;
  if (word == "ref" || word == "refmut" || word == "own")
    return Slot::Ownership;
  if (word == "perm" || word == "temp")
    return Slot::Counter;
  if (word == "range" || word == "while")
    return Slot::Form;
  return Slot::Unknown;
}

// Which questions each kind of chain asks, in the order it asks them. A chain
// answers a subset of these, and answers them in this order, so that one thing
// has one spelling.
struct Role {
  const char *what;              // how the chain is named in a diagnostic
  std::string_view kind;         // the word it opens with, or empty
  bool endsInType;               // whether the last segment is the type
  std::array<Slot, 4> slots;     // in order; Slot::Unknown pads the end
};

// A `var` takes no lifetime: only a function's answer has a choice of loans to
// be on, and only its parameters can name one.
const Role kVar{"a `var`", "var", true,
                {Slot::Mutability, Slot::Ownership, Slot::Unknown, Slot::Unknown}};
const Role kParam{"a parameter", "", true,
                  {Slot::Mutability, Slot::Ownership, Slot::Lifetime, Slot::Unknown}};
const Role kFn{"a `fn`", "fn", true,
               {Slot::Visibility, Slot::Ownership, Slot::Lifetime, Slot::Unknown}};
const Role kStruct{"a `struct`", "struct", false,
                   {Slot::Unknown, Slot::Unknown, Slot::Unknown, Slot::Unknown}};
const Role kConst{"a `const`", "const", true,
                  {Slot::Visibility, Slot::Unknown, Slot::Unknown, Slot::Unknown}};
const Role kLoopRange{"a counted `loop`", "loop", true,
                      {Slot::Counter, Slot::Form, Slot::Unknown, Slot::Unknown}};
const Role kLoopWhile{"a `loop.while`", "loop", false,
                      {Slot::Form, Slot::Unknown, Slot::Unknown, Slot::Unknown}};

// Where a slot sits in this role's order, or -1 when the role never asks it.
int placeIn(const Role &role, Slot slot) {
  for (int i = 0; i < static_cast<int>(role.slots.size()); ++i)
    if (role.slots[i] == slot)
      return i;
  return -1;
}

class Parser {
public:
  Parser(const Source &source, const std::vector<Token> &tokens)
      : source_(source), tokens_(tokens) {}

  ParseResult run() {
    while (!atEnd()) {
      const unsigned before = at_;
      topItem();
      if (at_ == before) // never spin on a token nothing consumed
        ++at_;
    }
    return std::move(result_);
  }

private:
  const Source &source_;
  const std::vector<Token> &tokens_;
  unsigned at_ = 0;
  ParseResult result_;

  // ---- token access

  const Token &peek(unsigned ahead = 0) const {
    const unsigned i = at_ + ahead;
    return tokens_[i < tokens_.size() ? i : tokens_.size() - 1];
  }
  const Token &previous() const { return tokens_[at_ > 0 ? at_ - 1 : 0]; }
  bool atEnd() const { return peek().kind == TokenKind::End; }
  bool check(TokenKind kind) const { return peek().kind == kind; }
  bool checkWord(std::string_view word) const {
    return peek().kind == TokenKind::Word && peek().text == word;
  }
  const Token &advance() { return tokens_[at_ < tokens_.size() - 1 ? at_++ : at_]; }
  bool accept(TokenKind kind) {
    if (!check(kind))
      return false;
    advance();
    return true;
  }

  std::string slice(Span span) const {
    return std::string(source_.text().substr(span.begin, span.end - span.begin));
  }

  void complain(Span span, std::string code, std::string message,
                std::vector<std::string> rules, std::vector<std::string> tips = {},
                std::string label = "here") {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             std::move(label), std::move(rules),
                                             std::move(tips), {}});
  }

  void complainAt(Span span, std::string code, std::string message,
                  std::vector<std::string> rules, std::vector<std::string> tips,
                  std::string label, std::vector<Note> notes) {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             std::move(label), std::move(rules),
                                             std::move(tips), std::move(notes)});
  }

  bool expect(TokenKind kind, const char *what) {
    if (accept(kind))
      return true;
    complain(peek().span, "E0103", std::string("a ") + what + " is wanted here.",
             {}, {}, std::string("found ") + describe(peek().kind));
    return false;
  }

  // Abandon a broken statement at the next boundary and read the next one.
  void recover() {
    while (!atEnd()) {
      if (accept(TokenKind::Semicolon))
        return;
      if (check(TokenKind::RBrace))
        return;
      advance();
    }
  }

  // ---- chains

  Chain chain() {
    Chain c;
    c.span.begin = peek().span.begin;
    if (check(TokenKind::Word)) {
      const Token &first = advance();
      c.segments.push_back(ChainSegment{first.span, first.text, false});
    } else {
      complain(peek().span, "E0102", "a chain begins with a word.", {}, {},
               std::string("found ") + describe(peek().kind));
      c.span.end = c.span.begin;
      return c;
    }
    while (check(TokenKind::Dot)) {
      advance();
      if (check(TokenKind::Word) || check(TokenKind::Name)) {
        const Token &seg = advance();
        c.segments.push_back(
            ChainSegment{seg.span, seg.text, seg.kind == TokenKind::Name});
      } else {
        complain(peek().span, "E0102", "a chain segment is a word, or a name for a loan.",
                 {}, {}, std::string("found ") + describe(peek().kind));
        break;
      }
    }
    c.span.end = previous().span.end;
    return c;
  }

  // Read a chain against the questions its kind actually asks. Called once the
  // parser knows which kind it is looking at — a chain read speculatively, on
  // the way to finding out a line was a call, is never judged.
  void validate(const Chain &c, const Role &role) {
    const std::size_t last = c.segments.size();
    // The type is the segment nearest the name, and whether it is a type is a
    // question for the checker. `many` belongs to the type rather than to the
    // chain — it says the name holds several of what comes after it — so the
    // run of `many`s in front of the type is part of the type region too.
    std::size_t upTo = role.endsInType ? (last > 0 ? last - 1 : 0) : last;
    std::size_t deep = 0;
    while (upTo > 0 && !c.segments[upTo - 1].isName &&
           c.segments[upTo - 1].text == "many") {
      --upTo;
      ++deep;
    }

    // `or-nothing` stands outside `many`, because what may be missing is the
    // whole of it rather than one of its places.
    std::size_t empties = 0;
    while (upTo > 0 && !c.segments[upTo - 1].isName &&
           c.segments[upTo - 1].text == "or-nothing") {
      --upTo;
      ++empties;
    }
    if (empties > 1)
      complain(Span{c.segments[upTo].span.begin, c.segments[upTo + empties - 1].span.end},
               "E0211", "nothing twice over is still nothing.",
               {"a type either may hold nothing or may not"},
               {"there is no second kind of absence to tell apart from the first."});
    if (empties && deep)
      // `or-nothing.many.T` is fine; `many.or-nothing.T` is an array of them,
      // and that is a type that wants a table rather than a pair of words.
      (void)0;
    if (deep > 1)
      complain(Span{c.segments[upTo].span.begin, c.segments[upTo + deep - 1].span.end},
               "E0210", "a `many` holds values, and not more `many`s.",
               {"a `many` is one level deep"},
               {"a second level is a real thing to build rather than something to "
                "half-support, and it is not built yet."});

    int furthest = -1;               // the last place filled, so order can be read
    Span seen[8];                    // where each slot was answered
    Slot asked[8] = {};
    bool filled[8] = {};

    for (std::size_t i = 0; i < upTo; ++i) {
      const ChainSegment &seg = c.segments[i];
      const Slot slot = seg.isName ? Slot::Lifetime : slotOf(seg.text);

      // The kind opens the chain and is not one of its answers.
      if (slot == Slot::Kind) {
        if (i == 0 && !role.kind.empty() && seg.text == role.kind)
          continue;
        complain(seg.span, "E0203",
                 "`" + seg.text + "` is not something " + role.what + " chain says.",
                 {"each kind of chain asks its own questions"},
                 {"a chain opens with the one word saying what is being declared, "
                  "and says it once."});
        continue;
      }

      if (!seg.isName && seg.text == "or-nothing") {
        complain(seg.span, "E0209",
                 "`or-nothing` says what the type may not hold, and stands with it.",
                 {"the segment nearest the name is the type"},
                 {"`var.or-nothing.str` is a `str` or nothing; nothing further along "
                  "the chain is a type for it to be instead."});
        continue;
      }

      if (!seg.isName && seg.text == "many") {
        complain(seg.span, "E0209", "`many` says what the type holds, and stands with it.",
                 {"the segment nearest the name is the type"},
                 {"`var.many.int64` is many `int64`; nothing further along the chain "
                  "is a type for it to hold."});
        continue;
      }

      if (slot == Slot::Unknown) {
        complain(seg.span, "E0202", "`" + seg.text + "` answers no question a chain asks.",
                 {"every segment of a chain answers a question the language asks"},
                 {"a chain is read by what each word means, not by counting to the "
                  "last one, so a word that means nothing cannot be passed over."});
        continue;
      }

      if (!seg.isName && isDefault(seg.text)) {
        complain(seg.span, "E0201",
                 "`" + seg.text + "` is what a name is when nothing says otherwise.",
                 {"a chain says what is unusual, and says nothing else"},
                 {"a bare chain is the safest chain, so nothing risky can hide in a "
                  "word that is not there."});
        continue;
      }

      const int place = placeIn(role, slot);
      if (place < 0) {
        complain(seg.span, "E0203",
                 "`" + (seg.isName ? "'" + seg.text + "'" : seg.text) +
                     "` is not something " + role.what + " chain says.",
                 {"each kind of chain asks its own questions"},
                 {std::string("it answers ") + question(slot) +
                  ", and that is not a question this chain asks."});
        continue;
      }

      if (slot == Slot::Visibility) {
        complain(seg.span, "E0206",
                 "`" + seg.text + "` says who may see this, and there is nowhere else "
                 "to see it from.",
                 {"a word is written where there is a choice"},
                 {"a program is one file for now, so everything in it is already as "
                  "visible as it can be."});
        continue;
      }

      if (filled[place]) {
        complainAt(seg.span, "E0204",
                   "this chain answers " + std::string(question(slot)) + " twice.",
                   {"each segment answers one question, and one question is answered once"},
                   {}, "answered again here",
                   {Note{seen[place], "answered here first"}});
        continue;
      }

      if (place < furthest) {
        complainAt(seg.span, "E0205",
                   "this chain answers " + std::string(question(asked[furthest])) +
                       " before " + question(slot) + ", and they are read the other "
                       "way round.",
                   {"there is exactly one spelling"},
                   {"a chain asks its questions in one order, so two chains saying the "
                    "same thing are written the same way."},
                   "answered here", {Note{seen[furthest], "and this one before it"}});
        continue;
      }

      filled[place] = true;
      seen[place] = seg.span;
      asked[place] = slot;
      furthest = place;
    }

    // `range` and `while` have no default between them: a loop that says neither
    // has not said what it is.
    if ((&role == &kLoopRange || &role == &kLoopWhile) &&
        !filled[placeIn(role, Slot::Form)])
      complain(c.span, "E0207", "a `loop` says whether it counts or asks.",
               {"a word is written where there is a choice"},
               {"`range` runs between two values and `while` runs until a question "
                "answers no, and neither is the quieter one."});
  }

  // ---- expressions

  bool atUnsettled() const {
    return peek().kind == TokenKind::Word && isUnsettled(peek().text);
  }
  bool atSettledOperator() const {
    switch (peek().kind) {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Slash:
    case TokenKind::Caret:
    case TokenKind::Less:
    case TokenKind::Greater:
    case TokenKind::LessEqual:
    case TokenKind::GreaterEqual:
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
      return true;
    case TokenKind::Word:
      return peek().text == "x";
    default:
      return false;
    }
  }

  ExprPtr make(ExprKind kind, Span span, std::string text = {}) {
    auto e = std::make_unique<Expr>();
    e->kind = kind;
    e->span = span;
    e->text = std::move(text);
    return e;
  }

  // `(a op1 b) op2 c` and `a op1 (b op2 c)`, written out of the source itself.
  void complainAmbiguous(const Expr &a, const std::string &op1, const Expr &b,
                         const std::string &op2, const Expr &c, Span whole) {
    const std::string as = slice(a.span), bs = slice(b.span), cs = slice(c.span);
    complain(whole, "E0301",
             "`" + op1 + "` and `" + op2 + "` have no agreed order, so this could be "
             "read as `(" + as + " " + op1 + " " + bs + ") " + op2 + " " + cs +
             "` or as `" + as + " " + op1 + " (" + bs + " " + op2 + " " + cs + ")`.",
             {"precedence is kept where mathematics settled it, and invented nowhere"},
             {}, "which of these first?");
  }

  ExprPtr primary() {
    const Token &token = peek();
    switch (token.kind) {
    case TokenKind::Name: {
      advance();
      ExprPtr so_far;
      // A bare word followed by `[` is a call; a name followed by one is an
      // element of what the name holds. The marks say which before the bracket
      // is reached, so the two can never be read for each other.
      if (check(TokenKind::LBracket)) {
        advance();
        ExprPtr where = item();
        Span span{token.span.begin, peek().span.end};
        expect(TokenKind::RBracket, "`]`");
        so_far = make(ExprKind::Index, span, token.text);
        so_far->children.push_back(std::move(where));
      } else {
        so_far = make(ExprKind::Name, token.span, token.text);
      }

      // `'p'.x` — a field. The dot is unmistakable after a mark: a chain has no
      // marked name before it, and a call path has no mark anywhere.
      while (check(TokenKind::Dot)) {
        advance();
        if (!check(TokenKind::Word)) {
          complain(peek().span, "E0102", "a field is named with a word.",
                   {"a word names a field, and a name wears marks"}, {},
                   std::string("found ") + describe(peek().kind));
          break;
        }
        const Token field = advance();
        Span span{token.span.begin, field.span.end};
        auto reach = make(ExprKind::Field, span, field.text);
        reach->children.push_back(std::move(so_far));
        so_far = std::move(reach);
      }
      return so_far;
    }
    case TokenKind::Written:
      advance();
      return make(ExprKind::Written, token.span, token.text);
    case TokenKind::Escape:
      advance();
      return make(ExprKind::Escape, token.span, token.text);
    case TokenKind::LParen: {
      advance();
      ExprPtr inner = item();
      Span span{token.span.begin, peek().span.end};
      expect(TokenKind::RParen, "`)`");
      auto group = make(ExprKind::Group, span);
      group->children.push_back(std::move(inner));
      return group;
    }
    case TokenKind::Word: {
      // `str:*hello*` — a written value saying its own type, where no chain has.
      if (peek(1).kind == TokenKind::Colon) {
        const Token typeWord = advance();
        advance(); // ':'
        ExprPtr inner = primary();
        Span span{typeWord.span.begin, inner->span.end};
        auto typed = make(ExprKind::Typed, span, typeWord.text);
        typed->children.push_back(std::move(inner));
        return typed;
      }
      // `nothing` is the one value a word spells, because there is no mark for
      // an absence and nothing else it could mean.
      if (token.text == "nothing" && peek(1).kind != TokenKind::LBracket) {
        advance();
        return make(ExprKind::Nothing, token.span, "nothing");
      }

      // `ref 'x'`, `refmut 'x'`, `move 'x'` — a transfer, always spelled.
      if (token.text == "ref" || token.text == "refmut" || token.text == "move") {
        advance();
        ExprPtr inner = primary();
        Span span{token.span.begin, inner->span.end};
        auto borrow = make(ExprKind::Borrow, span, token.text);
        borrow->children.push_back(std::move(inner));
        return borrow;
      }
      return call();
    }
    default:
      complain(token.span, "E0105", "a value is wanted here.", {}, {},
               std::string("found ") + describe(token.kind));
      advance();
      return make(ExprKind::Name, token.span);
    }
  }

  // A bare word followed by `[` is a call. Nothing announces one.
  ExprPtr call() {
    const Token &first = peek();
    auto expr = make(ExprKind::Call, first.span);
    expr->path.push_back(advance().text);
    while (check(TokenKind::Dot) && peek(1).kind == TokenKind::Word) {
      advance();
      expr->path.push_back(advance().text);
    }
    if (check(TokenKind::LBracket)) {
      expr->args = valueList();
      expr->span = Span{first.span.begin, expr->args.span.end};
    } else {
      expr->span = Span{first.span.begin, previous().span.end};
      complain(expr->span, "E0107", "a word on its own is not a value.",
               {"a name is a value, and a word followed by `[` is a call"},
               {"words name functions, types and chain segments; a variable is a "
                "name, and names wear marks."});
    }
    return expr;
  }

  ExprPtr power() { // binds tightest, and leans right
    ExprPtr left = primary();
    if (check(TokenKind::Caret)) {
      const Token op = advance();
      ExprPtr right = power();
      Span span{left->span.begin, right->span.end};
      auto node = make(ExprKind::Binary, span, "^");
      node->children.push_back(std::move(left));
      node->children.push_back(std::move(right));
      return node;
    }
    return left;
  }

  ExprPtr binaryLevel(unsigned level) {
    static constexpr unsigned kLevels = 3; // 0: comparison, 1: + -, 2: x /
    if (level == kLevels)
      return power();

    ExprPtr left = binaryLevel(level + 1);
    while (true) {
      std::string op;
      switch (level) {
      case 0:
        if (check(TokenKind::Less)) op = "<";
        else if (check(TokenKind::Greater)) op = ">";
        else if (check(TokenKind::LessEqual)) op = "<==";
        else if (check(TokenKind::GreaterEqual)) op = ">==";
        else if (check(TokenKind::EqualEqual)) op = "==";
        else if (check(TokenKind::BangEqual)) op = "!==";
        break;
      case 1:
        if (check(TokenKind::Plus)) op = "+";
        else if (check(TokenKind::Minus)) op = "-";
        break;
      default:
        if (check(TokenKind::Slash)) op = "/";
        else if (checkWord("x")) op = "x";
        break;
      }
      if (op.empty())
        return left;
      advance();
      ExprPtr right = binaryLevel(level + 1);
      Span span{left->span.begin, right->span.end};
      auto node = make(ExprKind::Binary, span, op);
      node->children.push_back(std::move(left));
      node->children.push_back(std::move(right));
      left = std::move(node);
    }
  }

  // One item of a value. Settled operators nest by mathematics' table; the
  // unsettled ones may appear once, alone, and never beside a settled one.
  // `holds 'name'` after a condition: the arm runs when there is something
  // there, and that something is lent to the name for as long as the arm does.
  // Written the same way in both places a condition goes.
  bool holdsName(std::string &name, Span &where) {
    if (!(peek().kind == TokenKind::Word && peek().text == "holds"))
      return false;
    advance();
    if (!check(TokenKind::Name)) {
      complain(peek().span, "E0101", "`holds` lends what is there to a name.",
               {"a name wears marks, and a word does not"}, {},
               std::string("found ") + describe(peek().kind));
      return false;
    }
    const Token got = advance();
    name = got.text;
    where = got.span;
    return true;
  }

  ExprPtr item() {
    if (checkWord("not")) {
      const Token op = advance();
      ExprPtr inner = primary();
      Span span{op.span.begin, inner->span.end};
      auto node = make(ExprKind::Unary, span, "not");
      node->children.push_back(std::move(inner));
      if (atSettledOperator() || atUnsettled())
        complain(Span{span.begin, peek().span.end}, "E0301",
                 "`not` and `" + describeOperator(peek()) +
                     "` have no agreed order, so this could be read two ways.",
                 {"precedence is kept where mathematics settled it, and invented nowhere"},
                 {}, "which of these first?");
      return node;
    }

    ExprPtr left = binaryLevel(0);
    if (!atUnsettled())
      return left;

    const bool settledAlready = left->kind == ExprKind::Binary;
    const Token op = advance();
    ExprPtr right = primary();

    if (settledAlready)
      // `a + b mod c` — the settled operator was already taken, so say so.
      complainAmbiguous(*left->children[0], left->text, *left->children[1], op.text,
                        *right, Span{left->span.begin, right->span.end});

    auto join = [&](ExprPtr a, const std::string &word, ExprPtr b) {
      Span span{a->span.begin, b->span.end};
      auto node = make(ExprKind::Binary, span, word);
      node->children.push_back(std::move(a));
      node->children.push_back(std::move(b));
      return node;
    };

    ExprPtr node = join(std::move(left), op.text, std::move(right));

    // `and` and `or` are associative, so repeating one asks nothing of a reader
    // and needs no brackets. `mod` is not associative, so repeating it does.
    const bool associative = op.text == "and" || op.text == "or";
    while (associative && peek().kind == TokenKind::Word && peek().text == op.text) {
      advance();
      ExprPtr more = primary();
      node = join(std::move(node), op.text, std::move(more));
    }

    if (atSettledOperator()) {
      // `a mod b + c` — the settled operator is still to come.
      const std::string nextOp = describeOperator(peek());
      advance();
      ExprPtr tail = primary();
      complainAmbiguous(*node->children[0], op.text, *node->children[1], nextOp, *tail,
                        Span{node->span.begin, tail->span.end});
    } else if (atUnsettled()) {
      complain(Span{node->span.begin, peek().span.end}, "E0301",
               "`" + op.text + "` and `" + peek().text +
                   "` have no agreed order, so this could be read two ways.",
               {"precedence is kept where mathematics settled it, and invented nowhere"},
               {}, "which of these first?");
      advance();
    }

    return node;
  }

  static std::string describeOperator(const Token &token) {
    if (token.kind == TokenKind::Word)
      return token.text;
    std::string text = describe(token.kind);
    if (text.size() > 2 && text.front() == '`')
      return text.substr(1, text.size() - 2);
    return text;
  }

  // ---- values

  ValueList valueList() {
    ValueList list;
    list.span.begin = peek().span.begin;
    if (!expect(TokenKind::LBracket, "`[`")) {
      list.span.end = list.span.begin;
      return list;
    }
    if (!check(TokenKind::RBracket)) {
      while (true) {
        Value value;
        value.span.begin = peek().span.begin;
        while (!check(TokenKind::RBracket) && !check(TokenKind::Comma) && !atEnd()) {
          const unsigned before = at_;
          value.items.push_back(item());
          if (at_ == before)
            break;
        }
        value.span.end = previous().span.end;
        list.values.push_back(std::move(value));
        if (!accept(TokenKind::Comma))
          break;
      }
    }
    expect(TokenKind::RBracket, "`]`");
    list.span.end = previous().span.end;
    return list;
  }

  // ---- statements

  Block block() {
    Block b;
    b.span.begin = peek().span.begin;
    if (!expect(TokenKind::LBrace, "`{`")) {
      b.span.end = b.span.begin;
      return b;
    }
    while (!check(TokenKind::RBrace) && !atEnd()) {
      const unsigned before = at_;
      StmtPtr s = statement();
      if (s)
        b.stmts.push_back(std::move(s));
      if (at_ == before)
        ++at_;
    }
    expect(TokenKind::RBrace, "`}`");
    b.span.end = previous().span.end;
    return b;
  }

  StmtPtr statement() {
    auto s = std::make_unique<Stmt>();
    s->span.begin = peek().span.begin;

    if (checkWord("set")) {
      advance();
      s->kind = StmtKind::Set;
      if (check(TokenKind::Name)) {
        const Token name = advance();
        s->name = name.text;
        s->nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a name is wanted here.",
                 {"what is being set is a name, and a name wears marks"}, {},
                 std::string("found ") + describe(peek().kind));
      }
      if (check(TokenKind::LBracket) && peek(1).kind != TokenKind::RBracket) {
        // `set 'xs'[*2*] = …` — which place is written, rather than the value.
        advance();
        s->index = item();
        expect(TokenKind::RBracket, "`]`");
      }
      // `set 'p'.x = …` — which of the things it holds is written.
      while (check(TokenKind::Dot)) {
        advance();
        if (!check(TokenKind::Word)) {
          complain(peek().span, "E0102", "a field is named with a word.",
                   {"a word names a field, and a name wears marks"}, {},
                   std::string("found ") + describe(peek().kind));
          break;
        }
        const Token field = advance();
        s->fields.push_back(field.text);
        s->fieldSpans.push_back(field.span);
      }
      expect(TokenKind::Equals, "`=`");
      s->value = valueList();
      expect(TokenKind::Semicolon, "`;`");
      s->span.end = previous().span.end;
      return s;
    }

    // `when 'x' { is 'value' { … } is nothing { … } }`
    //
    // The subject is bounded by `when` on the left and `{` on the right, so it
    // takes no brackets — the same reason an `if`'s condition takes none.
    if (checkWord("when")) {
      s->kind = StmtKind::When;
      advance();
      s->condition = item();
      if (expect(TokenKind::LBrace, "`{`")) {
        while (!check(TokenKind::RBrace) && !atEnd()) {
          Branch arm;
          arm.span.begin = peek().span.begin;
          arm.hasCondition = false;
          if (!checkWord("is")) {
            complain(peek().span, "E0108", "a `when` is made of `is` and nothing else.",
                     {"every case a `when` covers is written out"}, {},
                     std::string("found ") + describe(peek().kind));
            recover();
            break;
          }
          advance();
          if (check(TokenKind::Name)) {
            const Token got = advance();
            arm.holds = got.text;
            arm.holdsSpan = got.span;
          } else if (peek().kind == TokenKind::Word && peek().text == "nothing") {
            arm.holdsSpan = advance().span;
            arm.matchesNothing = true;
          } else {
            complain(peek().span, "E0108",
                     "an `is` says either a name to lend what is there to, or "
                     "`nothing`.",
                     {"every case a `when` covers is written out"}, {},
                     std::string("found ") + describe(peek().kind));
            recover();
            break;
          }
          arm.body = block();
          arm.span.end = previous().span.end;
          s->branches.push_back(std::move(arm));
        }
        expect(TokenKind::RBrace, "`}`");
      }
      s->span.end = previous().span.end;
      return s;
    }

    if (checkWord("if")) {
      s->kind = StmtKind::If;
      while (true) {
        Branch branch;
        branch.span.begin = peek().span.begin;
        const bool isElse = checkWord("else");
        const bool isElseIf = checkWord("else-if");
        advance(); // if / else-if / else
        branch.hasCondition = !isElse;
        if (!isElse) {
          branch.condition = item();
          holdsName(branch.holds, branch.holdsSpan);
        }
        branch.body = block();
        branch.span.end = previous().span.end;
        s->branches.push_back(std::move(branch));
        if (isElse)
          break;
        if (!checkWord("else") && !checkWord("else-if"))
          break;
        (void)isElseIf;
      }
      s->span.end = previous().span.end;
      return s;
    }

    if (checkWord("give")) {
      advance();
      s->kind = StmtKind::Give;
      s->value = valueList();
      expect(TokenKind::Semicolon, "`;`");
      s->span.end = previous().span.end;
      return s;
    }

    if (checkWord("break")) {
      advance();
      s->kind = StmtKind::Break;
      expect(TokenKind::Semicolon, "`;`");
      s->span.end = previous().span.end;
      return s;
    }

    if (checkWord("loop")) {
      s->chain = chain();
      bool isWhile = false;
      for (const ChainSegment &seg : s->chain.segments)
        if (!seg.isName && seg.text == "while")
          isWhile = true;
      validate(s->chain, isWhile ? kLoopWhile : kLoopRange);
      if (isWhile) {
        s->kind = StmtKind::LoopWhile;
        s->condition = item();
        holdsName(s->holds, s->holdsSpan);
      } else {
        s->kind = StmtKind::LoopRange;
        if (check(TokenKind::Name)) {
          const Token name = advance();
          s->name = name.text;
          s->nameSpan = name.span;
        } else {
          complain(peek().span, "E0101", "a loop's counter is a name.", {}, {},
                   std::string("found ") + describe(peek().kind));
        }
        expect(TokenKind::Equals, "`=`");
        s->value = valueList();
      }
      s->body = block();
      s->span.end = previous().span.end;
      return s;
    }

    if (!check(TokenKind::Word)) {
      complain(peek().span, "E0106", "a statement begins with a word.", {}, {},
               std::string("found ") + describe(peek().kind));
      recover();
      return nullptr;
    }

    // A declaration and a call share their opening: a dotted run of words. What
    // follows tells them apart — a name is declared, a `[` is called.
    const unsigned mark = at_;
    Chain c = chain();
    if (check(TokenKind::Name)) {
      const Token name = advance();
      s->kind = StmtKind::Declare;
      validate(c, kVar);
      s->chain = std::move(c);
      s->name = name.text;
      s->nameSpan = name.span;
      expect(TokenKind::Equals, "`=`");
      s->value = valueList();
      expect(TokenKind::Semicolon, "`;`");
      s->span.end = previous().span.end;
      return s;
    }

    at_ = mark;
    s->kind = StmtKind::Call;
    s->call = call();
    expect(TokenKind::Semicolon, "`;`");
    s->span.end = previous().span.end;
    return s;
  }

  // ---- items

  // A field list and a parameter list are the same thing written down: typed
  // names, side by side, bracketed because nothing else bounds them.
  void readFields(std::vector<Param> &into) {
    if (!expect(TokenKind::LBracket, "`[`"))
      return;
    while (!check(TokenKind::RBracket) && !atEnd()) {
      Param field;
      field.span.begin = peek().span.begin;
      field.chain = chain();
      validate(field.chain, kParam);
      if (check(TokenKind::Name)) {
        const Token name = advance();
        field.name = name.text;
        field.nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a field is a name.", {}, {},
                 std::string("found ") + describe(peek().kind));
      }
      field.span.end = previous().span.end;
      into.push_back(std::move(field));
      if (!accept(TokenKind::Comma))
        break;
    }
    expect(TokenKind::RBracket, "`]`");
  }

  void startItem(Item &out) {
    out.kind = ItemKind::Start;
    advance(); // START
    out.body = block();
  }

  void topItem() {
    Item out;
    out.span.begin = peek().span.begin;

    if (checkWord("START")) {
      startItem(out);
      out.span.end = previous().span.end;
      result_.program.items.push_back(std::move(out));
      return;
    }

    if (!check(TokenKind::Word)) {
      complain(peek().span, "E0104", "a file holds constants, functions and `START`.",
               {}, {}, std::string("found ") + describe(peek().kind));
      advance();
      return;
    }

    out.chain = chain();
    if (out.chain.startsWith("fn")) {
      out.kind = ItemKind::Function;
      validate(out.chain, kFn);
      if (check(TokenKind::Word)) {
        const Token name = advance();
        out.name = name.text;
        out.nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a function is named with a word.",
                 {"a word names a function, and a name wears marks and holds a value"},
                 {}, std::string("found ") + describe(peek().kind));
      }
      readFields(out.params);
      out.body = block();
    } else if (out.chain.startsWith("struct")) {
      out.kind = ItemKind::Struct;
      validate(out.chain, kStruct);
      if (out.chain.segments.size() > 1)
        complain(out.chain.span, "E0212",
                 "a `struct` says nothing but what it is called.",
                 {"a chain says what is unusual, and says nothing else"},
                 {"what a `struct` holds is written in its fields, and each of "
                  "them has a chain of its own."});
      if (check(TokenKind::Word)) {
        const Token name = advance();
        out.name = name.text;
        out.nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a `struct` is named with a word.",
                 {"a word names a thing, and a name wears marks and holds a value"},
                 {}, std::string("found ") + describe(peek().kind));
      }
      readFields(out.params);
    } else if (out.chain.startsWith("const")) {
      out.kind = ItemKind::Const;
      validate(out.chain, kConst);
      if (check(TokenKind::Name)) {
        const Token name = advance();
        out.name = name.text;
        out.nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a constant is a name.", {}, {},
                 std::string("found ") + describe(peek().kind));
      }
      expect(TokenKind::Equals, "`=`");
      out.value = valueList();
      expect(TokenKind::Semicolon, "`;`");
    } else {
      complain(out.chain.span, "E0104",
               "a file holds structs, constants, functions and `START`.",
               {"`struct`, `const`, `fn` and `START` are what stands at the top of "
                "a file"},
               {"a `var` belongs inside something that runs; the top level does not run."});
      recover();
      return;
    }

    out.span.end = previous().span.end;
    result_.program.items.push_back(std::move(out));
  }
};

} // namespace

ParseResult parse(const Source &source, const std::vector<Token> &tokens) {
  return Parser(source, tokens).run();
}

} // namespace xag
