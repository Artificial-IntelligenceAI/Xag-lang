#include "safetybolt/Lexer.h"

#include <cctype>

namespace sb {

const char *describe(TokenKind kind) {
  switch (kind) {
  case TokenKind::End:          return "end of file";
  case TokenKind::Word:         return "a word";
  case TokenKind::Name:         return "a name";
  case TokenKind::Written:      return "a written value";
  case TokenKind::Escape:       return "an escape";
  case TokenKind::Dot:          return "`.`";
  case TokenKind::Comma:        return "`,`";
  case TokenKind::Semicolon:    return "`;`";
  case TokenKind::Colon:        return "`:`";
  case TokenKind::Equals:       return "`=`";
  case TokenKind::LBracket:     return "`[`";
  case TokenKind::RBracket:     return "`]`";
  case TokenKind::LBrace:       return "`{`";
  case TokenKind::RBrace:       return "`}`";
  case TokenKind::LParen:       return "`(`";
  case TokenKind::RParen:       return "`)`";
  case TokenKind::Plus:         return "`+`";
  case TokenKind::Minus:        return "`-`";
  case TokenKind::Slash:        return "`/`";
  case TokenKind::Less:         return "`<`";
  case TokenKind::Greater:      return "`>`";
  case TokenKind::LessEqual:    return "`<=`";
  case TokenKind::GreaterEqual: return "`>=`";
  case TokenKind::EqualEqual:   return "`==`";
  case TokenKind::BangEqual:    return "`!=`";
  }
  return "something";
}

namespace {

// A word is a function name, a chain segment, or a type. It is written in
// letters, digits and `_`, joined by `-`. A variable never comes through here:
// variables are names, and names wear marks, so they may hold anything.
bool startsWord(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool continuesWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class Lexer {
public:
  Lexer(const Source &source) : text_(source.text()) {}

  LexResult run() {
    while (true) {
      skipTrivia();
      if (at_ >= text_.size())
        break;
      one();
    }
    result_.tokens.push_back(Token{TokenKind::End, Span{at_, at_}, {}});
    return std::move(result_);
  }

private:
  std::string_view text_;
  unsigned at_ = 0;
  LexResult result_;

  char peek(unsigned ahead = 0) const {
    return at_ + ahead < text_.size() ? text_[at_ + ahead] : '\0';
  }

  void emit(TokenKind kind, unsigned begin, std::string body = {}) {
    result_.tokens.push_back(Token{kind, Span{begin, at_}, std::move(body)});
  }

  void complain(Span span, std::string code, std::string message,
                std::vector<std::string> rules, std::vector<std::string> tips) {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             "here", std::move(rules), std::move(tips)});
  }

  void skipTrivia() {
    while (at_ < text_.size()) {
      char c = text_[at_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++at_;
      } else if (c == '#') {
        while (at_ < text_.size() && text_[at_] != '\n')
          ++at_;
      } else {
        return;
      }
    }
  }

  // Both marks read the same way: everything between them is the character it
  // looks like, except the closing mark itself, which is written with a
  // backslash because it could not otherwise appear.
  void marked(TokenKind kind, char mark, const char *what) {
    const unsigned begin = at_;
    ++at_; // opening mark
    std::string body;
    while (at_ < text_.size()) {
      char c = text_[at_];
      if (c == '\\' && peek(1) == mark) {
        body.push_back(mark);
        at_ += 2;
        continue;
      }
      if (c == mark) {
        ++at_;
        emit(kind, begin, std::move(body));
        return;
      }
      if (c == '\n')
        break;
      body.push_back(c);
      ++at_;
    }

    const std::string markText(1, mark);
    complain(Span{begin, at_}, "E0002", std::string("this ") + what + " is never closed.",
             {"a mark opens and closes on the same line"},
             {"the closing mark is the one character that cannot appear between the "
              "marks, which is why `\\" + markText + "` exists."});
  }

  void one() {
    const unsigned begin = at_;
    const char c = text_[at_];

    switch (c) {
    case '\'': marked(TokenKind::Name, '\'', "name"); return;
    case '*':  marked(TokenKind::Written, '*', "written value"); return;

    case '\\': {
      ++at_;
      const char kind = peek();
      if (kind == 'n' || kind == 't' || kind == 'r' || kind == '\\') {
        ++at_;
        emit(TokenKind::Escape, begin, std::string(1, kind));
        return;
      }
      // Do not step over the offending character: it may well be a real token.
      complain(Span{begin, at_}, "E0004", "this is not one of the escapes.",
               {"the escapes are `\\n`, `\\t`, `\\r` and `\\\\`"},
               {"escapes stand beside a written value rather than inside one, so "
                "reading a piece of text never means working out which of its "
                "characters were instructions."});
      return;
    }

    case '"': {
      // Take the whole quoted run, so a reader who reached for the wrong mark is
      // told once rather than once per quote.
      ++at_;
      while (at_ < text_.size() && text_[at_] != '"' && text_[at_] != '\n')
        ++at_;
      if (at_ < text_.size() && text_[at_] == '"')
        ++at_;
      complain(Span{begin, at_}, "E0003",
               "SafetyBolt does not write anything between double quotes.",
               {"a name is quoted with `'`, and a written value goes between `*` marks"},
               {"the two are kept apart on purpose, so a quoted thing is always a name "
                "and never has to be read as a value depending on where it sits."});
      return;
    }

    case '.': ++at_; emit(TokenKind::Dot, begin); return;
    case ',': ++at_; emit(TokenKind::Comma, begin); return;
    case ';': ++at_; emit(TokenKind::Semicolon, begin); return;
    case ':': ++at_; emit(TokenKind::Colon, begin); return;
    case '[': ++at_; emit(TokenKind::LBracket, begin); return;
    case ']': ++at_; emit(TokenKind::RBracket, begin); return;
    case '{': ++at_; emit(TokenKind::LBrace, begin); return;
    case '}': ++at_; emit(TokenKind::RBrace, begin); return;
    case '(': ++at_; emit(TokenKind::LParen, begin); return;
    case ')': ++at_; emit(TokenKind::RParen, begin); return;
    case '+': ++at_; emit(TokenKind::Plus, begin); return;
    case '-': ++at_; emit(TokenKind::Minus, begin); return;
    case '/': ++at_; emit(TokenKind::Slash, begin); return;

    case '=':
      at_ += peek(1) == '=' ? 2 : 1;
      emit(at_ - begin == 2 ? TokenKind::EqualEqual : TokenKind::Equals, begin);
      return;
    case '<':
      at_ += peek(1) == '=' ? 2 : 1;
      emit(at_ - begin == 2 ? TokenKind::LessEqual : TokenKind::Less, begin);
      return;
    case '>':
      at_ += peek(1) == '=' ? 2 : 1;
      emit(at_ - begin == 2 ? TokenKind::GreaterEqual : TokenKind::Greater, begin);
      return;
    case '!':
      if (peek(1) == '=') {
        at_ += 2;
        emit(TokenKind::BangEqual, begin);
        return;
      }
      ++at_;
      complain(Span{begin, at_}, "E0006", "`!` on its own means nothing here.",
               {"`!=` is the only place `!` appears"}, {});
      return;

    default: break;
    }

    if (startsWord(c)) {
      ++at_;
      while (at_ < text_.size()) {
        if (continuesWord(text_[at_])) {
          ++at_;
          continue;
        }
        // A `-` joins a word only between two word characters, which is why
        // `sum-to` is one word and `count - *1*` is three tokens.
        if (text_[at_] == '-' && at_ + 1 < text_.size() && continuesWord(text_[at_ + 1])) {
          at_ += 2;
          continue;
        }
        break;
      }
      emit(TokenKind::Word, begin, std::string(text_.substr(begin, at_ - begin)));
      return;
    }


    if (std::isdigit(static_cast<unsigned char>(c))) {
      while (at_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[at_])))
        ++at_;
      complain(Span{begin, at_}, "E0005",
               "a number is a written value, and written values wear marks.",
               {"`*…*` writes a value, and its type says what it means"},
               {"there is no separate mark for text and numbers, because the type "
                "already answers that: `*1000*` is a number under `i64` and four "
                "characters under `str`."});
      return;
    }

    // A byte outside ASCII is almost always someone writing a word the way they
    // would write a name, so say which of the two they are looking at.
    if (static_cast<unsigned char>(c) >= 0x80) {
      while (at_ < text_.size() && static_cast<unsigned char>(text_[at_]) >= 0x80)
        ++at_;
      complain(Span{begin, at_}, "E0008", "this is not written in a word.",
               {"a word is written in letters, digits and `_`, joined by `-`"},
               {"a name may hold any character, including this one, because its marks "
                "say where it stops; a word has no marks and so has to be plainer."});
      return;
    }

    ++at_;
    complain(Span{begin, at_}, "E0001", "this character means nothing in SafetyBolt.", {}, {});
  }
};

} // namespace

LexResult lex(const Source &source) { return Lexer(source).run(); }

} // namespace sb
