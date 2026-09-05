#include "xag/Lexer.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #cond "\n";            \
      ++failures;                                                                        \
    }                                                                                    \
  } while (false)

xag::LexResult lexText(const std::string &text) {
  static std::vector<xag::Source> kept;
  kept.emplace_back("test.xag", text);
  return xag::lex(kept.back());
}

std::vector<xag::TokenKind> kinds(const xag::LexResult &result) {
  std::vector<xag::TokenKind> out;
  for (const xag::Token &token : result.tokens)
    out.push_back(token.kind);
  return out;
}

void marksCarryTheirContents() {
  const xag::LexResult r = lexText("'greeting' *Hello, world*");
  CHECK(r.ok());
  CHECK(kinds(r) == (std::vector<xag::TokenKind>{xag::TokenKind::Name, xag::TokenKind::Written,
                                                xag::TokenKind::End}));
  CHECK(r.tokens[0].text == "greeting");
  CHECK(r.tokens[1].text == "Hello, world");
}

void aMarkIsWrittenWithABackslash() {
  const xag::LexResult r = lexText("*a\\*b* 'it\\'s'");
  CHECK(r.ok());
  CHECK(r.tokens[0].text == "a*b");
  CHECK(r.tokens[1].text == "it's");
}

void escapesStandOutside() {
  // Inside a mark a backslash is just a backslash; only the closing mark escapes.
  const xag::LexResult inside = lexText("*a\\nb*");
  CHECK(inside.ok());
  CHECK(inside.tokens[0].text == "a\\nb");

  const xag::LexResult beside = lexText("*line one* \\n *line two*");
  CHECK(beside.ok());
  CHECK(kinds(beside) == (std::vector<xag::TokenKind>{xag::TokenKind::Written,
                                                     xag::TokenKind::Escape,
                                                     xag::TokenKind::Written,
                                                     xag::TokenKind::End}));
  CHECK(beside.tokens[1].text == "n");
}

void chainsAreWordsAndDots() {
  const xag::LexResult r = lexText("var.mut.int64 ['total'] = [*0*];");
  CHECK(r.ok());
  CHECK(r.tokens[0].kind == xag::TokenKind::Word && r.tokens[0].text == "var");
  CHECK(r.tokens[1].kind == xag::TokenKind::Dot);
  CHECK(r.tokens[2].text == "mut");
  CHECK(r.tokens[4].text == "int64");
  CHECK(r.tokens.back().kind == xag::TokenKind::End);
}

void aHyphenJoinsAWordButDoesNotSubtract() {
  const xag::LexResult joined = lexText("sum-to");
  CHECK(joined.ok());
  CHECK(kinds(joined) == (std::vector<xag::TokenKind>{xag::TokenKind::Word, xag::TokenKind::End}));
  CHECK(joined.tokens[0].text == "sum-to");

  // A `-` joins only between two word characters.
  for (const std::string &spaced : {"count - *1*", "count- *1*", "count -*1*"}) {
    const xag::LexResult r = lexText(spaced);
    CHECK(r.ok());
    CHECK(r.tokens.size() == 4);
    CHECK(r.tokens[0].text == "count");
    CHECK(r.tokens[1].kind == xag::TokenKind::Minus);
    CHECK(r.tokens[2].kind == xag::TokenKind::Written);
  }
}

void aWordIsPlainerThanAName() {
  const xag::LexResult underscore = lexText("sum_to");
  CHECK(underscore.ok());
  CHECK(underscore.tokens[0].text == "sum_to");

  // An emoji is fine in a name and is not a word.
  const xag::LexResult emoji = lexText("sum\xF0\x9F\x98\x80to");
  CHECK(!emoji.ok());
  CHECK(emoji.diagnostics[0].code == "E0008");

  const xag::LexResult name = lexText("'sum_to \xF0\x9F\x98\x80'");
  CHECK(name.ok());
  CHECK(name.tokens[0].kind == xag::TokenKind::Name);
}

void arithmeticIsFiveThings() {
  const xag::LexResult r = lexText("'a' x 'b' ^ *2* + *1* - *3* / *4*");
  CHECK(r.ok());
  CHECK(kinds(r) == (std::vector<xag::TokenKind>{
                        xag::TokenKind::Name, xag::TokenKind::Word, xag::TokenKind::Name,
                        xag::TokenKind::Caret, xag::TokenKind::Written, xag::TokenKind::Plus,
                        xag::TokenKind::Written, xag::TokenKind::Minus, xag::TokenKind::Written,
                        xag::TokenKind::Slash, xag::TokenKind::Written, xag::TokenKind::End}));
  // `x` is a letter, so it reaches the parser as a word like any other.
  CHECK(r.tokens[1].text == "x");

  // And a word merely starting with x is one word, not an operator.
  const xag::LexResult xs = lexText("xs");
  CHECK(xs.ok() && xs.tokens[0].text == "xs" && xs.tokens.size() == 2);

  // The operators spelled with letters need no tokens of their own.
  const xag::LexResult words = lexText("'a' mod 'b' and not 'c' or 'd'");
  CHECK(words.ok());
  for (unsigned i : {1u, 3u, 4u, 6u}) {
    CHECK(words.tokens[i].kind == xag::TokenKind::Word);
  }
  CHECK(words.tokens[1].text == "mod");
  CHECK(words.tokens[3].text == "and");
  CHECK(words.tokens[4].text == "not");
  CHECK(words.tokens[6].text == "or");
}

void commentsRunToEndOfLine() {
  const xag::LexResult r = lexText("# 'not' *a* token\n'yes'");
  CHECK(r.ok());
  CHECK(kinds(r) == (std::vector<xag::TokenKind>{xag::TokenKind::Name, xag::TokenKind::End}));
  CHECK(r.tokens[0].text == "yes");
}

void comparisonsCarryTheWholeEquality() {
  const xag::LexResult r = lexText("< <== > >== == !== =");
  CHECK(r.ok());
  CHECK(kinds(r) == (std::vector<xag::TokenKind>{
                        xag::TokenKind::Less, xag::TokenKind::LessEqual, xag::TokenKind::Greater,
                        xag::TokenKind::GreaterEqual, xag::TokenKind::EqualEqual,
                        xag::TokenKind::BangEqual, xag::TokenKind::Equals, xag::TokenKind::End}));
}

void halfAnEqualityIsRefused() {
  for (const std::string &written : {"<=", ">=", "!="}) {
    const xag::LexResult r = lexText("'a' " + written + " 'b'");
    CHECK(!r.ok());
    CHECK(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].code == "E0009");
  }
}

void aQuoteOffersBothMarks() {
  const xag::LexResult r = lexText("\"hello\"");
  CHECK(!r.ok());
  CHECK(r.diagnostics.size() == 1);
  CHECK(r.diagnostics[0].code == "E0003");
}

void aBareNumberIsToldToWearMarks() {
  const xag::LexResult r = lexText("1000");
  CHECK(!r.ok());
  CHECK(r.diagnostics.size() == 1);
  CHECK(r.diagnostics[0].code == "E0005");
}

void anUnclosedMarkIsReported() {
  const xag::LexResult r = lexText("*hello\n'world'");
  CHECK(!r.ok());
  CHECK(r.diagnostics.size() == 1);
  // Recovery continues on the next line rather than swallowing the file.
  CHECK(r.tokens.size() == 2);
  CHECK(r.tokens[0].kind == xag::TokenKind::Name && r.tokens[0].text == "world");
}

void everyMistakeIsReported() {
  const xag::LexResult r = lexText("1 \"x\" 2");
  CHECK(r.diagnostics.size() == 3);
}

void diagnosticsPointAtTheRightPlace() {
  const xag::Source source("test.xag", "var.str ['s'] = [1000];");
  const xag::LexResult r = xag::lex(source);
  CHECK(!r.ok());
  std::ostringstream rendered;
  xag::render(source, r.diagnostics[0], rendered);
  const std::string text = rendered.str();
  CHECK(text.find("test.xag:1:18") != std::string::npos);
  CHECK(text.find("^^^^ here") != std::string::npos);
  CHECK(text.find("Error code: E0005") != std::string::npos);
  CHECK(text.find("Rule(s) broken:") != std::string::npos);
  // A diagnostic never says what to type instead.
  CHECK(text.find("Suggested fix") == std::string::npos);
}

} // namespace

int main() {
  marksCarryTheirContents();
  aMarkIsWrittenWithABackslash();
  escapesStandOutside();
  chainsAreWordsAndDots();
  aHyphenJoinsAWordButDoesNotSubtract();
  aWordIsPlainerThanAName();
  arithmeticIsFiveThings();
  commentsRunToEndOfLine();
  comparisonsCarryTheWholeEquality();
  halfAnEqualityIsRefused();
  aQuoteOffersBothMarks();
  aBareNumberIsToldToWearMarks();
  anUnclosedMarkIsReported();
  everyMistakeIsReported();
  diagnosticsPointAtTheRightPlace();

  if (failures == 0)
    std::cout << "all lexer tests passed\n";
  return failures == 0 ? 0 : 1;
}
