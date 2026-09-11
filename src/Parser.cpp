#include "xag/Parser.h"

#include <array>
#include <string>

namespace xag {
namespace {

// The words a chain leaves out when they are what a name already is. Writing one
// is an error: a chain says what is unusual, and says nothing else.
constexpr std::array<std::string_view, 5> kDefaults{"immut", "own", "file", "temp",
                                                   "checked"};

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

// The words a blank may be narrowed with, kept here rather than reached for
// across the wall: this reader needs to know the shape `any.number` makes, and
// nothing else about what the words mean.
bool namesAFamily(std::string_view word) {
  // `loan` and `loanmut` are chain words too, and there is no collision: this
  // only ever looks at the word directly after `any`, and `loan.any` puts it
  // directly before.
  for (const char *known : {"number", "int", "uint", "bin", "deci", "str", "bool",
                            "many", "or-nothing", "struct", "owned", "loan",
                            "loanmut"})
    if (word == known)
      return true;
  return false;
}

enum class Slot {
  Unknown,
  Kind,       // var, fn, const, loop
  Visibility, // export / program, default file
  Mutability, // mut, default immut
  Ownership,  // loan / loanmut, default own
  Lifetime,   // 'life' — a name for a loan
  Counter,    // perm, default temp
  Form,       // range / while
  Overflow,   // wrapping, default checked
  Trying,     // no-itmt — the compiler does not run this one while compiling
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
  case Slot::Overflow:   return "whether a sum that does not fit is a mistake";
  case Slot::Trying:     return "whether the compiler runs this one while compiling";
  case Slot::Unknown:    break;
  }
  return "nothing";
}

Slot slotOf(std::string_view word) {
  if (word == "var" || word == "fn" || word == "const" || word == "loop" ||
      word == "struct" || word == "one-of")
    return Slot::Kind;
  if (word == "export" || word == "program" || word == "file")
    return Slot::Visibility;
  if (word == "mut" || word == "immut")
    return Slot::Mutability;
  if (word == "loan" || word == "loanmut" || word == "own")
    return Slot::Ownership;
  if (word == "perm" || word == "temp")
    return Slot::Counter;
  if (word == "range" || word == "while" || word == "parts")
    return Slot::Form;
  if (word == "wrapping" || word == "checked")
    return Slot::Overflow;
  if (word == "no-itmt")
    return Slot::Trying;
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
                {Slot::Mutability, Slot::Ownership, Slot::Overflow, Slot::Unknown}};
// `wrapping` sits where it does on a `var` — straight after ownership — so one
// thing has one spelling wherever it is written. A struct's fields are read
// against this role too, so both say it the same way.
const Role kParam{"a parameter", "", true,
                  {Slot::Mutability, Slot::Ownership, Slot::Overflow, Slot::Lifetime}};
const Role kFn{"a `fn`", "fn", true,
               {Slot::Visibility, Slot::Ownership, Slot::Lifetime, Slot::Unknown}};
const Role kStruct{"a `struct`", "struct", false,
                   {Slot::Unknown, Slot::Unknown, Slot::Unknown, Slot::Unknown}};
const Role kOneOf{"a `one-of`", "one-of", false,
                  {Slot::Unknown, Slot::Unknown, Slot::Unknown, Slot::Unknown}};
const Role kConst{"a `const`", "const", true,
                  {Slot::Visibility, Slot::Unknown, Slot::Unknown, Slot::Unknown}};
// `no-itmt` comes first, because it is about the loop rather than about the name
// the loop declares — `perm` answers a question about the counter, and this does
// not.
const Role kLoopRange{"a counted `loop`", "loop", true,
                      {Slot::Trying, Slot::Counter, Slot::Form, Slot::Unknown}};
const Role kLoopWhile{"a `loop.while`", "loop", false,
                      {Slot::Trying, Slot::Form, Slot::Unknown, Slot::Unknown}};
// No type and no counter: what it walks is written in the struct, and how many
// turns there are is written there too. It is not really a loop — the body is
// one copy per field, written out while compiling — but it reads like one, and
// reading like one is what it is for.
const Role kLoopParts{"a `loop.parts`", "loop", false,
                      {Slot::Trying, Slot::Form, Slot::Unknown, Slot::Unknown}};

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

  // A file is one of two shapes, and every block in the shape is written
  // whether or not there is anything in it:
  //
  //     READ_ME { }      what it says, in prose        READ_ME { }
  //     PREP { }         what lasts the whole program  LIBRARY { }   what it offers
  //     START { }        the part that runs            ITMT { }
  //     ITMT { }         what is run while building
  //
  // A program on the left, a library on the right. The second block says which:
  // `PREP` or `LIBRARY`. A library has no `START` because it has no moment of its
  // own — everything in it is a declaration, and what runs is what a program
  // calls. Both have `ITMT`, which is run two ways while building and never
  // ships; it is how a library is exercised on its own.
  //
  // Written even when empty, because a shape that is sometimes there is a shape
  // a reader has to look for. This one is always in the same place.
  ParseResult run() {
    readMeBlock();
    if (checkWord("LIBRARY")) {
      result_.program.library = true;
      libraryBlock();
    } else {
      prepBlock();
      startBlock();
    }
    itmtBlock();

    // Nothing stands outside the blocks.
    while (!atEnd()) {
      const unsigned before = at_;
      complain(peek().span, "E0110",
               result_.program.library
                   ? "a library is `READ_ME`, `LIBRARY` and `ITMT`, and this is "
                     "outside all three."
                   : "a program is `READ_ME`, `PREP`, `START` and `ITMT`, and this "
                     "is outside all four.",
               {"a file is its blocks, in that order"},
               {result_.program.library
                    ? "what the library offers goes in `LIBRARY`; what exercises it "
                      "while building goes in `ITMT`."
                    : "what lasts the whole program goes in `PREP`; what runs goes in "
                      "`START`; what is only run while building goes in `ITMT`."},
               std::string("found ") + describe(peek().kind));
      advance();
      if (at_ == before)
        ++at_;
    }
    return std::move(result_);
  }

  // `READ_ME { … }` — prose, kept exactly as written and read by nobody.
  void readMeBlock() {
    if (!checkWord("READ_ME")) {
      missing("READ_ME", "what this file says, which may be nothing yet");
      return;
    }
    advance();
    if (!expect(TokenKind::LBrace, "`{`"))
      return;
    if (check(TokenKind::Markdown))
      result_.program.readMe = advance().text;
    expect(TokenKind::RBrace, "`}`");
  }

  // `PREP { … }` — the structs, the constants and the functions. Everything
  // that is there for as long as the program is.
  void prepBlock() {
    if (!checkWord("PREP")) {
      missing("PREP", "what the program is made of, which may be nothing yet");
      return;
    }
    advance(); // PREP
    declarations();
  }

  // `LIBRARY { … }` — the same declarations, in a file that offers them to
  // programs rather than running anything itself.
  void libraryBlock() {
    advance(); // LIBRARY, already seen
    declarations();
  }

  // The braces and what is inside them, after the word that named the block.
  void declarations() {
    if (!expect(TokenKind::LBrace, "`{`"))
      return;
    while (!check(TokenKind::RBrace) && !atEnd()) {
      const unsigned before = at_;
      topItem();
      if (at_ == before)
        ++at_;
    }
    expect(TokenKind::RBrace, "`}`");
  }

  // `ITMT { … }` — statements the compiler runs while building, two ways, and
  // refuses the build if they disagree. Never shipped. A program's `START` is
  // run the same way and does ship; this is for what should only ever run here.
  void itmtBlock() {
    if (!checkWord("ITMT")) {
      missing("ITMT", "what is run while building and never shipped, which may be "
                      "nothing yet");
      return;
    }
    Item out;
    out.kind = ItemKind::Itmt;
    out.span.begin = peek().span.begin;
    advance(); // ITMT
    out.body = block();
    out.span.end = previous().span.end;
    result_.program.items.push_back(std::move(out));
  }

  void startBlock() {
    if (!checkWord("START")) {
      missing("START", "the part that runs, which may be nothing yet");
      return;
    }
    Item out;
    out.span.begin = peek().span.begin;
    startItem(out);
    out.span.end = previous().span.end;
    result_.program.items.push_back(std::move(out));
  }

  void missing(const char *word, const char *what) {
    complain(peek().span, "E0111",
             std::string("this file has no `") + word + "`.",
             {result_.program.library
                  ? "a library is `READ_ME`, then `LIBRARY`, then `ITMT`"
                  : "a program is `READ_ME`, then `PREP`, then `START`, then `ITMT`"},
             {std::string("`") + word + " { }` says " + what +
              ". Every block is written whether or not there is anything in it, "
              "so a reader finds them in the same place every time."},
             std::string("found ") + describe(peek().kind));
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
  // A statement that is only a value, worked out and then dropped on the floor.
  //
  // Xag has no expression statements: every statement declares, changes, calls,
  // or decides. `int32:*1* + int32:*2*` is none of those, and what the parser
  // used to say about it was three errors — one about `int32` not being a call,
  // and two about the `:` after it, which was never the trouble. The trouble is
  // the whole line, and now it says so.
  //
  // The span runs to the `;` or the `}`, because it is the *value* that has
  // nowhere to go and not any one token in it.
  void aValueGoingNowhere() {
    const Span from = peek().span;
    unsigned last = at_;
    while (!atEnd() && !check(TokenKind::Semicolon) && !check(TokenKind::RBrace)) {
      last = at_;
      advance();
    }
    complain(Span{from.begin, tokens_[last].span.end}, "E0109",
             "this works something out, and nothing is done with it.",
             {"a statement declares, changes, calls, or decides, and a value on its "
              "own is none of those"},
             {"a value goes somewhere: `var.int32 'n' = [...]` gives it a name, "
              "`set 'n' = [...]` changes one that has a name already, and "
              "`print.stdout[...]` shows it."});
    accept(TokenKind::Semicolon);
  }

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
    // `any.number` is one type region rather than a type with a stray word in
    // front of it: the blank says what it will take, and the word saying so
    // stands nearest the name. Knowing this shape is the same kind of knowledge
    // as knowing `many` and `or-nothing`, which this reader already has —
    // whether `number` means anything is still the checker's question.
    if (role.endsInType && upTo > 0 && !c.segments[upTo].isName &&
        !c.segments[upTo - 1].isName && c.segments[upTo - 1].text == "any" &&
        namesAFamily(c.segments[upTo].text))
      --upTo;
    std::size_t deep = 0;
    while (upTo > 0 && !c.segments[upTo - 1].isName &&
           (c.segments[upTo - 1].text == "many" ||
            c.segments[upTo - 1].text == "many-growing")) {
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

      if (!seg.isName && (seg.text == "many" || seg.text == "many-growing")) {
        complain(seg.span, "E0209",
                 "`" + seg.text + "` says what the type holds, and stands with it.",
                 {"the segment nearest the name is the type"},
                 {"`var.many.int64` is many `int64`; nothing further along the chain "
                  "is a type for it to hold."});
        continue;
      }

      if (slot == Slot::Unknown) {
        // Which word is being read as the type is this reader's own rule, and
        // saying it out loud is the difference between somebody seeing their
        // mistake and somebody staring at the one word in the chain that was
        // fine. `fn.int64.number` used to say that `int64` answers no question,
        // which reads as a claim about `int64` rather than about where it is
        // standing — and `number`, the word actually at fault, went unmentioned.
        //
        // Whether the word nearest the name is a type is still the checker's
        // question. Nothing here answers it, and nothing here needs to: the
        // reader is being told where the type is, not what it is.
        const bool sayWhichIsTheType =
            role.endsInType && last > 0 && !c.segments[last - 1].isName;
        if (sayWhichIsTheType)
          complain(seg.span, "E0202",
                   "`" + seg.text + "` is not one of the words a chain says, and the "
                   "type here is `" + c.segments[last - 1].text + "`.",
                   {"the type is the word nearest the name, and every word before it "
                    "answers a question the language asks"},
                   {"if `" + seg.text + "` was meant to be the type, it has to be the "
                    "word nearest the name.",
                    "a chain is read by what each word means, not by counting to the "
                    "last one, so a word that means nothing cannot be passed over."});
        else
          complain(seg.span, "E0202",
                   "`" + seg.text + "` answers no question a chain asks.",
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
    if ((&role == &kLoopRange || &role == &kLoopWhile || &role == &kLoopParts) &&
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

  // What an `if`, a `when` or a `loop.while` asks. The same as any other value
  // except that it takes no brackets: `if` bounds it on the left and `{` on the
  // right, so there is nothing for a `[` to be doing there — and now that `[`
  // opens several values where an item goes, one written here would quietly be
  // read as a `many` rather than refused.
  ExprPtr askedItem() {
    ExprPtr asked = item();
    if (asked && asked->kind == ExprKind::Several)
      complain(asked->span, "E0105", "a condition takes no brackets.",
               {"a condition is one value, bounded by the word before it and the `{` "
                "after it"},
               {"brackets bound a list that nothing else bounds, and this is already "
                "bounded."});
    return asked;
  }

  ExprPtr primary() {
    const Token &token = peek();
    switch (token.kind) {
    // `[…]` where an item goes: several values made where they stand, which is
    // how a `many` of a `many` is written. A `[` after a *name* is an index, and
    // that is decided before this is reached — nothing here can be one, because
    // there is no name in front of it.
    case TokenKind::LBracket: {
      advance();
      auto several = make(ExprKind::Several, token.span, std::string());
      while (!check(TokenKind::RBracket) && !atEnd()) {
        const unsigned before = at_;
        several->children.push_back(item());
        if (at_ == before)
          break;
      }
      several->span.end = peek().span.end;
      expect(TokenKind::RBracket, "`]`");
      return several;
    }
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
        // `'g'[*0*][*1*]` — reaching into what was just reached. What is being
        // reached into rides as a second child, because the first one is the
        // index and everything already written reads it there.
        while (check(TokenKind::LBracket)) {
          advance();
          ExprPtr deeper = item();
          Span wider{token.span.begin, peek().span.end};
          expect(TokenKind::RBracket, "`]`");
          auto again = make(ExprKind::Index, wider, std::string());
          again->children.push_back(std::move(deeper));
          again->children.push_back(std::move(so_far));
          so_far = std::move(again);
        }
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

      // `loan 'x'`, `loanmut 'x'`, `move 'x'` — a transfer, always spelled.
      if (token.text == "loan" || token.text == "loanmut" || token.text == "move") {
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
    } else if (expr->path.size() == 1) {
      // A word on its own, which may name a case of a `one-of` that holds
      // nothing — `gave-up`, the way `nothing` is written bare. Whether it
      // names one is the checker's question, and it is asked of the same node a
      // case with something in it makes.
      auto made = make(ExprKind::Typed, first.span);
      made->text = expr->path[0];
      made->span = Span{first.span.begin, previous().span.end};
      return made;
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

    // `add 'lines' = […];` — one more place at the end. Written like `set`
    // because it is the same kind of act: something happens to a name that
    // already exists, and what happens is spelled where it happens.
    if (checkWord("add")) {
      advance();
      s->kind = StmtKind::Add;
      if (check(TokenKind::Name)) {
        const Token name = advance();
        s->name = name.text;
        s->nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a name is wanted here.",
                 {"what is being added to is a name, and a name wears marks"}, {},
                 std::string("found ") + describe(peek().kind));
      }
      expect(TokenKind::Equals, "`=`");
      s->value = valueList();
      expect(TokenKind::Semicolon, "`;`");
      s->span.end = previous().span.end;
      return s;
    }

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
      s->condition = askedItem();
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
          } else if (peek().kind == TokenKind::Word) {
            // A case of a `one-of`: the word says which case, and a name after
            // it is what the case holds, lent for the arm. A case holding
            // nothing has no name after it, the same way `is nothing` has none.
            const Token got = advance();
            arm.family = got.text;
            arm.familySpan = got.span;
            arm.span.end = got.span.end;
            if (check(TokenKind::Name)) {
              const Token bound = advance();
              arm.holds = bound.text;
              arm.holdsSpan = bound.span;
            }
          } else {
            complain(peek().span, "E0108",
                     "an `is` says a case, a name to lend what is there to, or "
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

    // `whichever 'x' { is number { … } is str { … } }`
    //
    // It looks like `when` and is not: a `when` is a choice the program makes
    // while running, with both arms in it, and a `whichever` is decided while
    // compiling — only the arm that was chosen survives into the program. The
    // word says the choosing rather than the asking, which is what is actually
    // happening.
    if (checkWord("whichever")) {
      s->kind = StmtKind::Whichever;
      advance();
      s->condition = askedItem();
      if (expect(TokenKind::LBrace, "`{`")) {
        while (!check(TokenKind::RBrace) && !atEnd()) {
          Branch arm;
          arm.span.begin = peek().span.begin;
          arm.hasCondition = false;
          if (!checkWord("is")) {
            complain(peek().span, "E0108",
                     "a `whichever` is made of `is` and nothing else.",
                     {"every case a `whichever` covers is written out"}, {},
                     std::string("found ") + describe(peek().kind));
            recover();
            break;
          }
          advance();
          // Any word, and whether it names a kind of thing is the checker's
          // question — the same wall that keeps type words out of here.
          if (check(TokenKind::Word)) {
            const Token got = advance();
            arm.family = got.text;
            arm.familySpan = got.span;
          } else {
            complain(peek().span, "E0108",
                     "an `is` here says what kind of thing the subject is.",
                     {"every case a `whichever` covers is written out"},
                     {"`whichever` chooses by kind, and a kind is a word: `number`, "
                      "`str`, `many`, `struct`. A `when` is the one that takes a name."},
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

    // A block where the compiler is allowed to be told to do less. It holds
    // statements like any other block and changes nothing about them; what it
    // grants is asked for inside it, by name, so that grepping for the word
    // finds every place a check was turned off.
    if (checkWord("UNSAFE")) {
      advance(); // UNSAFE
      s->kind = StmtKind::Unsafe;
      s->body = block();
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
          branch.condition = askedItem();
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
      bool isWhile = false, isParts = false;
      for (const ChainSegment &seg : s->chain.segments) {
        if (seg.isName)
          continue;
        if (seg.text == "while")
          isWhile = true;
        else if (seg.text == "parts")
          isParts = true;
      }
      validate(s->chain, isWhile ? kLoopWhile : isParts ? kLoopParts : kLoopRange);
      if (isWhile) {
        s->kind = StmtKind::LoopWhile;
        s->condition = askedItem();
        holdsName(s->holds, s->holdsSpan);
      } else {
        // `loop.parts` reads exactly like a counted loop — a name, `=`, and what
        // it walks — so it is read by the same lines. What differs is what the
        // name is bound to on each turn, and that is the checker's business.
        s->kind = isParts ? StmtKind::LoopParts : StmtKind::LoopRange;
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
      // A name, or a written value, at the start of a statement: whatever
      // follows, this is a value and not a statement.
      if (check(TokenKind::Name) || check(TokenKind::Written) ||
          check(TokenKind::LParen)) {
        aValueGoingNowhere();
        return nullptr;
      }
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

    // A chain not followed by `[` is not a call, and not followed by a name is
    // not a declaration — so it is a value, and a value is not a statement.
    const bool calls = check(TokenKind::LBracket);
    at_ = mark;
    if (!calls) {
      aValueGoingNowhere();
      return nullptr;
    }
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

    if (!check(TokenKind::Word)) {
      complain(peek().span, "E0104",
               result_.program.library
                   ? "a `LIBRARY` holds structs, constants and functions."
                   : "a `PREP` holds structs, constants and functions.",
               {result_.program.library
                    ? "what a library offers is written in `LIBRARY`"
                    : "what lasts the whole program is written in `PREP`"},
               {"a `var` belongs inside something that runs, and that is `START` — "
                "or `ITMT`, if it should only ever run while building."},
               std::string("found ") + describe(peek().kind));
      advance();
      return;
    }

    // `import 'text';` — this file uses a unit the manifest knows by that name.
    // Marks on the name because it is one: it comes back as the prefix on every
    // name reached through it.
    if (checkWord("import")) {
      out.kind = ItemKind::Import;
      advance();
      if (check(TokenKind::Name)) {
        const Token name = advance();
        out.name = name.text;
        out.nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "`import` names the unit it brings in.",
                 {"a name wears marks where it is given, and a word does not"},
                 {"`import 'text';` — the name is the one the library's manifest "
                  "gives it."},
                 std::string("found ") + describe(peek().kind));
      }
      expect(TokenKind::Semicolon, "`;`");
      out.span.end = previous().span.end;
      result_.program.items.push_back(std::move(out));
      return;
    }

    out.chain = chain();
    if (out.chain.startsWith("fn")) {
      out.kind = ItemKind::Function;
      validate(out.chain, kFn);
      // Marked where it is named, bare where it is called: naming a function
      // and calling one are different acts, and only the first is naming.
      if (check(TokenKind::Name)) {
        const Token name = advance();
        out.name = name.text;
        out.nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a declaration marks what it names.",
                 {"a name wears marks where it is given, and a word does not"},
                 {"`longer` calls it; `'longer'` is what it is called."},
                 std::string("found ") + describe(peek().kind));
        // A bare word here is the mistake being reported, so it is taken as the
        // name anyway and reading goes on from where it would have. Leaving it
        // where it sat left the fields to be read starting at the name.
        if (check(TokenKind::Word)) {
          const Token name = advance();
          out.name = name.text;
          out.nameSpan = name.span;
        }
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
      if (check(TokenKind::Name)) {
        const Token name = advance();
        out.name = name.text;
        out.nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a declaration marks what it names.",
                 {"a name wears marks where it is given, and a word does not"},
                 {"`point` is the type; `'point'` is what it is called."},
                 std::string("found ") + describe(peek().kind));
        if (check(TokenKind::Word)) {
          const Token name = advance();
          out.name = name.text;
          out.nameSpan = name.span;
        }
      }
      readFields(out.params);
    } else if (out.chain.startsWith("one-of")) {
      // Declared exactly as a `struct` is, because what is written is the same:
      // a name, then a list of typed names. What differs is that a value is one
      // of them rather than all of them, which is a question for the checker.
      out.kind = ItemKind::OneOf;
      validate(out.chain, kOneOf);
      if (out.chain.segments.size() > 1)
        complain(out.chain.span, "E0212",
                 "a `one-of` says nothing but what it is called.",
                 {"a chain says what is unusual, and says nothing else"},
                 {"what a `one-of` can be is written in its cases, and each of "
                  "them has a chain of its own."});
      if (check(TokenKind::Name)) {
        const Token name = advance();
        out.name = name.text;
        out.nameSpan = name.span;
      } else {
        complain(peek().span, "E0101", "a declaration marks what it names.",
                 {"a name wears marks where it is given, and a word does not"},
                 {"`token` is the type; `'token'` is what it is called."},
                 std::string("found ") + describe(peek().kind));
        if (check(TokenKind::Word)) {
          const Token name = advance();
          out.name = name.text;
          out.nameSpan = name.span;
        }
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
               "a `PREP` holds types, constants and functions.",
               {"`struct`, `one-of`, `const` and `fn` are what a `PREP` is made of"},
               {"a `var` belongs inside something that runs, and the part that runs "
                "is `START`."});
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
