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
                                             std::move(tips)});
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

    for (const ChainSegment &seg : c.segments)
      if (!seg.isName && isDefault(seg.text))
        complain(seg.span, "E0201",
                 "`" + seg.text + "` is what a name is when nothing says otherwise.",
                 {"a chain says what is unusual, and says nothing else"},
                 {"a bare chain is the safest chain, so nothing risky can hide in a "
                  "word that is not there."});
    return c;
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
    case TokenKind::Name:
      advance();
      return make(ExprKind::Name, token.span, token.text);
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

    const Token op = advance();
    ExprPtr right = primary();

    if (left->kind == ExprKind::Binary) {
      // `a + b mod c` — the settled operator was already taken, so say so.
      complainAmbiguous(*left->children[0], left->text, *left->children[1], op.text,
                        *right, Span{left->span.begin, right->span.end});
    } else if (atSettledOperator()) {
      // `a mod b + c` — the settled operator is still to come.
      const Token next = peek();
      const std::string nextOp = describeOperator(next);
      advance();
      ExprPtr tail = primary();
      complainAmbiguous(*left, op.text, *right, nextOp, *tail,
                        Span{left->span.begin, tail->span.end});
    } else if (atUnsettled()) {
      complain(Span{left->span.begin, peek().span.end}, "E0301",
               "`" + op.text + "` and `" + peek().text +
                   "` have no agreed order, so this could be read two ways.",
               {"precedence is kept where mathematics settled it, and invented nowhere"},
               {}, "which of these first?");
      advance();
    }

    Span span{left->span.begin, right->span.end};
    auto node = make(ExprKind::Binary, span, op.text);
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
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
        // `set 'xs'[*2*] = …` — the index, not a value list.
        advance();
        s->index.push_back(item());
        expect(TokenKind::RBracket, "`]`");
      }
      expect(TokenKind::Equals, "`=`");
      s->value = valueList();
      expect(TokenKind::Semicolon, "`;`");
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
        if (!isElse)
          branch.condition = item();
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
      const bool isWhile = s->chain.segments.size() > 1 &&
                           s->chain.segments[1].text == "while";
      if (isWhile) {
        s->kind = StmtKind::LoopWhile;
        s->condition = item();
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
      if (check(TokenKind::Word)) {
        const Token name = advance();
        out.name = name.text;
        out.nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a function is named with a word.",
                 {"a word names a function, and a name wears marks and holds a value"},
                 {}, std::string("found ") + describe(peek().kind));
      }
      if (expect(TokenKind::LBracket, "`[`")) {
        while (!check(TokenKind::RBracket) && !atEnd()) {
          Param param;
          param.span.begin = peek().span.begin;
          param.chain = chain();
          if (check(TokenKind::Name)) {
            const Token name = advance();
            param.name = name.text;
            param.nameSpan = name.span;
          } else {
            complain(peek().span, "E0101", "a parameter is a name.", {}, {},
                     std::string("found ") + describe(peek().kind));
          }
          param.span.end = previous().span.end;
          out.params.push_back(std::move(param));
          if (!accept(TokenKind::Comma))
            break;
        }
        expect(TokenKind::RBracket, "`]`");
      }
      out.body = block();
    } else if (out.chain.startsWith("const")) {
      out.kind = ItemKind::Const;
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
               "a file holds constants, functions and `START`.",
               {"`const`, `fn` and `START` are what stands at the top of a file"},
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
