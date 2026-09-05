#pragma once

#include "xag/Ast.h"
#include "xag/Diagnostic.h"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace xag {

// The types a value can have. `Unknown` is not a type a program can write: it
// means nothing in this position has said what the value is, which is itself
// the thing a written value has to answer for.
//
// A size is always written, and it is always one the standard defines: whole
// numbers at the widths a machine has, `bin` at IEEE 754's binary interchange
// formats, `deci` at its decimal ones. There is no `int` on its own, because
// there is no size to assume.
enum class Type {
  Unknown,
  Nothing,
  Bool,
  Str,
  Int8, Int16, Int32, Int64, Int128,
  Uint8, Uint16, Uint32, Uint64, Uint128,
  Bin16, Bin32, Bin64, Bin128,
  Deci32, Deci64, Deci128,
};

const char *name(Type type);

// The type a word names, or `Unknown` if no word names it.
Type typeNamed(std::string_view word);

bool isWhole(Type type);   // int or uint
bool isSigned(Type type);  // int
bool isBinary(Type type);  // bin
bool isDecimal(Type type); // deci
bool isNumber(Type type);
unsigned widthOf(Type type); // bits; 0 for the types that have no size

struct CheckResult {
  std::vector<Diagnostic> diagnostics;

  // What the checker worked out, so that nothing after it has to work the same
  // thing out again. Keyed by node, which is stable for as long as the tree is.
  std::unordered_map<const Expr *, Type> expressions;
  std::unordered_map<const Stmt *, Type> declarations;
  std::unordered_map<const Item *, Type> items;

  Type of(const Expr *e) const {
    auto found = expressions.find(e);
    return found == expressions.end() ? Type::Unknown : found->second;
  }

  bool ok() const { return diagnostics.empty(); }
};

// Names and types. Ownership is a separate pass, and does not exist yet.
CheckResult check(const Source &source, const Program &program);

} // namespace xag
