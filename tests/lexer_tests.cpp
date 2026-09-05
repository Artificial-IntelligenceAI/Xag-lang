#include "safetybolt/Lexer.h"

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

sb::LexResult lexText(const std::string &text) {
  static std::vector<sb::Source> kept;
  kept.emplace_back("test.sbls", text);
  return sb::lex(kept.back());
}

std::vector<sb::TokenKind> kinds(const sb::LexResult &result) {
  std::vector<sb::TokenKind> out;
  for (const sb::Token &token : result.tokens)
    out.push_back(token.kind);
  return out;
}

void marksCarryTheirContents() {
  const sb::LexResult r = lexText("'greeting' *Hello, world*");
  CHECK(r.ok());
  CHECK(kinds(r) == (std::vector<sb::TokenKind>{sb::TokenKind::Name, sb::TokenKind::Written,
                                                sb::TokenKind::End}));
  CHECK(r.tokens[0].text == "greeting");
  CHECK(r.tokens[1].text == "Hello, world");
}

void aMarkIsWrittenWithABackslash() {
  const sb::LexResult r = lexText("*a\\*b* 'it\\'s'");
  CHECK(r.ok());
  CHECK(r.tokens[0].text == "a*b");
  CHECK(r.tokens[1].text == "it's");
}

void escapesStandOutside() {
  // Inside a mark a backslash is just a backslash; only the closing mark escapes.
  const sb::LexResult inside = lexText("*a\\nb*");
  CHECK(inside.ok());
  CHECK(inside.tokens[0].text == "a\\nb");

  const sb::LexResult beside = lexText("*line one* \\n *line two*");
  CHECK(beside.ok());
  CHECK(kinds(beside) == (std::vector<sb::TokenKind>{sb::TokenKind::Written,
                                                     sb::TokenKind::Escape,
                                                     sb::TokenKind::Written,
                                                     sb::TokenKind::End}));
  CHECK(beside.tokens[1].text == "n");
}

void chainsAreWordsAndDots() {
  const sb::LexResult r = lexText("var.mut.i64 ['total'] = [*0*];");
  CHECK(r.ok());
  CHECK(r.tokens[0].kind == sb::TokenKind::Word && r.tokens[0].text == "var");
  CHECK(r.tokens[1].kind == sb::TokenKind::Dot);
  CHECK(r.tokens[2].text == "mut");
  CHECK(r.tokens[4].text == "i64");
  CHECK(r.tokens.back().kind == sb::TokenKind::End);
}

void aHyphenJoinsAWordButDoesNotSubtract() {
  const sb::LexResult joined = lexText("sum-to");
  CHECK(joined.ok());
  CHECK(kinds(joined) == (std::vector<sb::TokenKind>{sb::TokenKind::Word, sb::TokenKind::End}));
  CHECK(joined.tokens[0].text == "sum-to");

  // A `-` joins only between two word characters.
  for (const std::string &spaced : {"count - *1*", "count- *1*", "count -*1*"}) {
    const sb::LexResult r = lexText(spaced);
    CHECK(r.ok());
    CHECK(r.tokens.size() == 4);
    CHECK(r.tokens[0].text == "count");
    CHECK(r.tokens[1].kind == sb::TokenKind::Minus);
    CHECK(r.tokens[2].kind == sb::TokenKind::Written);
  }
}

void aWordIsPlainerThanAName() {
  const sb::LexResult underscore = lexText("sum_to");
  CHECK(underscore.ok());
  CHECK(underscore.tokens[0].text == "sum_to");

  // An emoji is fine in a name and is not a word.
  const sb::LexResult emoji = lexText("sum\xF0\x9F\x98\x80to");
  CHECK(!emoji.ok());
  CHECK(emoji.diagnostics[0].code == "E0008");

  const sb::LexResult name = lexText("'sum_to \xF0\x9F\x98\x80'");
  CHECK(name.ok());
  CHECK(name.tokens[0].kind == sb::TokenKind::Name);
}

void arithmeticIsFiveThings() {
  const sb::LexResult r = lexText("'a' x 'b' ^ *2* + *1* - *3* / *4*");
  CHECK(r.ok());
  CHECK(kinds(r) == (std::vector<sb::TokenKind>{
                        sb::TokenKind::Name, sb::TokenKind::Word, sb::TokenKind::Name,
                        sb::TokenKind::Caret, sb::TokenKind::Written, sb::TokenKind::Plus,
                        sb::TokenKind::Written, sb::TokenKind::Minus, sb::TokenKind::Written,
                        sb::TokenKind::Slash, sb::TokenKind::Written, sb::TokenKind::End}));
  // `x` is a letter, so it reaches the parser as a word like any other.
  CHECK(r.tokens[1].text == "x");

  // And a word merely starting with x is one word, not an operator.
  const sb::LexResult xs = lexText("xs");
  CHECK(xs.ok() && xs.tokens[0].text == "xs" && xs.tokens.size() == 2);
}

void commentsRunToEndOfLine() {
  const sb::LexResult r = lexText("# 'not' *a* token\n'yes'");
  CHECK(r.ok());
  CHECK(kinds(r) == (std::vector<sb::TokenKind>{sb::TokenKind::Name, sb::TokenKind::End}));
  CHECK(r.tokens[0].text == "yes");
}

void comparisonsCarryTheWholeEquality() {
  const sb::LexResult r = lexText("< <== > >== == !== =");
  CHECK(r.ok());
  CHECK(kinds(r) == (std::vector<sb::TokenKind>{
                        sb::TokenKind::Less, sb::TokenKind::LessEqual, sb::TokenKind::Greater,
                        sb::TokenKind::GreaterEqual, sb::TokenKind::EqualEqual,
                        sb::TokenKind::BangEqual, sb::TokenKind::Equals, sb::TokenKind::End}));
}

void halfAnEqualityIsRefused() {
  for (const std::string &written : {"<=", ">=", "!="}) {
    const sb::LexResult r = lexText("'a' " + written + " 'b'");
    CHECK(!r.ok());
    CHECK(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].code == "E0009");
  }
}

void aQuoteOffersBothMarks() {
  const sb::LexResult r = lexText("\"hello\"");
  CHECK(!r.ok());
  CHECK(r.diagnostics.size() == 1);
  CHECK(r.diagnostics[0].code == "E0003");
}

void aBareNumberIsToldToWearMarks() {
  const sb::LexResult r = lexText("1000");
  CHECK(!r.ok());
  CHECK(r.diagnostics.size() == 1);
  CHECK(r.diagnostics[0].code == "E0005");
}

void anUnclosedMarkIsReported() {
  const sb::LexResult r = lexText("*hello\n'world'");
  CHECK(!r.ok());
  CHECK(r.diagnostics.size() == 1);
  // Recovery continues on the next line rather than swallowing the file.
  CHECK(r.tokens.size() == 2);
  CHECK(r.tokens[0].kind == sb::TokenKind::Name && r.tokens[0].text == "world");
}

void everyMistakeIsReported() {
  const sb::LexResult r = lexText("1 \"x\" 2");
  CHECK(r.diagnostics.size() == 3);
}

void diagnosticsPointAtTheRightPlace() {
  const sb::Source source("test.sbls", "var.str ['s'] = [1000];");
  const sb::LexResult r = sb::lex(source);
  CHECK(!r.ok());
  std::ostringstream rendered;
  sb::render(source, r.diagnostics[0], rendered);
  const std::string text = rendered.str();
  CHECK(text.find("test.sbls:1:18") != std::string::npos);
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
