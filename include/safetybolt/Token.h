#pragma once

#include "safetybolt/Source.h"

#include <string>

namespace sb {

enum class TokenKind {
  End,
  Word,    // a bare word: var, mut, i64, START, print, stdout
  Name,    // 'greeting'
  Written, // *hello*
  Escape,  // \n \t \r \\ — an item in its own right, never inside a mark
  Dot,
  Comma,
  Semicolon,
  Colon,
  Equals,
  LBracket,
  RBracket,
  LBrace,
  RBrace,
  LParen,
  RParen,
  Plus,
  Minus,
  Slash,
  Less,
  Greater,
  LessEqual,
  GreaterEqual,
  EqualEqual,
  BangEqual,
};

const char *describe(TokenKind kind);

struct Token {
  TokenKind kind = TokenKind::End;
  Span span;
  // Name and Written carry their contents with the mark's own escape resolved.
  // Word carries the word. Escape carries one of n, t, r, \.
  std::string text;
};

} // namespace sb
