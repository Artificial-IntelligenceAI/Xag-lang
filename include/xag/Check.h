#pragma once

#include "xag/Ast.h"
#include "xag/Diagnostic.h"

#include <string>
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
  Many,   // several of one type, however many were there when it was made
  Struct, // a group of named things, each with a type of its own
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

// A type as the checker knows it. Everything except `many` is a kind on its
// own; a `many` also says what it holds.
//
// One level, deliberately: `many.many.int64` is refused rather than half-built,
// because a second level is where a type stops fitting in a pair and wants a
// table of its own. A Type converts to a Ty on its own, so every scalar reads
// exactly as it did before this existed.
struct Ty {
  Type kind = Type::Unknown;
  Type element = Type::Unknown; // only when kind is Many
  // Which struct, when either of the above is one. Only one of them can be, so
  // one number says which — a `many` of a struct is the far side of that.
  unsigned named = 0;
  // Whether this may hold nothing instead. It stands outside the rest, so
  // `or-nothing.many.int64` is one of these and `many.or-nothing.int64` is not
  // — an array of maybes wants a table of types rather than a pair, which is
  // the same wall `many.many` stands at.
  bool orNothing = false;

  constexpr Ty() = default;
  constexpr Ty(Type k) : kind(k) {}
  constexpr Ty(Type k, Type e) : kind(k), element(e) {}
  constexpr Ty(Type k, Type e, bool n) : kind(k), element(e), orNothing(n) {}
  constexpr Ty(Type k, Type e, bool n, unsigned w)
      : kind(k), element(e), named(w), orNothing(n) {}

  constexpr bool holds() const { return kind == Type::Many; }
  constexpr bool mayBeNothing() const { return orNothing; }
  // What is inside, once the `or-nothing` is taken off.
  constexpr Ty within() const { return Ty{kind, element, false, named}; }
  constexpr bool isStruct() const { return kind == Type::Struct; }
};

constexpr bool operator==(Ty a, Ty b) {
  return a.kind == b.kind && a.element == b.element && a.orNothing == b.orNothing &&
         a.named == b.named;
}
constexpr bool operator!=(Ty a, Ty b) { return !(a == b); }

constexpr Ty many(Type element) { return Ty{Type::Many, element}; }
constexpr Ty orNothingOf(Ty inside) {
  return Ty{inside.kind, inside.element, true, inside.named};
}
constexpr Ty structNamed(unsigned which) {
  return Ty{Type::Struct, Type::Unknown, false, which};
}

// `many int64`, spelled the way it is written apart from the dots — which is
// also how the middle layer holds it, so nothing has to translate.
std::string name(Ty type);

// A thing that may hold nothing is not a number, however numeric what it holds
// would be: arithmetic on one has no answer when it holds none, which is the
// whole reason the type exists.
inline bool isWhole(Ty t) { return !t.holds() && !t.orNothing && isWhole(t.kind); }
inline bool isSigned(Ty t) { return !t.holds() && !t.orNothing && isSigned(t.kind); }
inline bool isBinary(Ty t) { return !t.holds() && !t.orNothing && isBinary(t.kind); }
inline bool isDecimal(Ty t) { return !t.holds() && !t.orNothing && isDecimal(t.kind); }
inline bool isNumber(Ty t) { return !t.holds() && !t.orNothing && isNumber(t.kind); }
inline unsigned widthOf(Ty t) { return t.holds() ? 0 : widthOf(t.kind); }

// What a struct is made of, in the order it was written.
struct Field {
  std::string name;
  Ty type;
  Span span;
};

struct Shape {
  std::string name;
  std::vector<Field> fields;
  Span span;
};

struct CheckResult {
  std::vector<Diagnostic> diagnostics;

  // What the checker worked out, so that nothing after it has to work the same
  // thing out again. Keyed by node, which is stable for as long as the tree is.
  std::unordered_map<const Expr *, Ty> expressions;
  std::unordered_map<const Stmt *, Ty> declarations;
  std::unordered_map<const Item *, Ty> items;
  // The structs a file declared, so nothing after the checker has to read them
  // out of the tree again. A `Ty` naming one is an index into this.
  std::vector<Shape> shapes;

  Ty of(const Expr *e) const {
    auto found = expressions.find(e);
    return found == expressions.end() ? Ty{} : found->second;
  }

  bool ok() const { return diagnostics.empty(); }
};

// Names and types. Ownership is a separate pass, and does not exist yet.
CheckResult check(const Source &source, const Program &program);

} // namespace xag
