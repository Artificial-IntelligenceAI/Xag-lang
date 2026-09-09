#pragma once

#include "xag/Ast.h"
#include "xag/Diagnostic.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
  // A blank, written `any`: the type is the caller's to pick, and every `any` in
  // one signature is the same one. It never reaches the middle layer — a
  // generic is written once and built once per type it is called with, so what
  // gets compiled has no blanks left in it.
  Blank,
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

// What a blank will take. `any` on its own takes everything; a word after it
// narrows that, and the words are ones the language already has — the same list
// `is` asks with, read in the other direction.
//
// Bare `any` stays the floor: it takes anything, and what can be done with it is
// what can be done with every type. Every word added buys one thing more, and
// costs the types it turns away.
enum class Family {
  Anything, // `any`, with nothing after it
  Number,
  Int,
  Uint,
  Bin,
  Deci,
  Str,
  Bool,
  Many,
  OrNothing,
  Struct,
};

// The family a word names. `Anything` when the word names no family at all, so
// a caller has to ask `namesFamily` first if it cares about the difference.
Family familyNamed(std::string_view word);
bool namesFamily(std::string_view word);

// What a family asks for, said the way a sentence would say it: "a number",
// "something that may hold nothing".
const char *asksFor(Family family);

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
  // Where an `Unknown` came from: the mistake that made this type unknowable.
  // An `Unknown` used to be anonymous, so everything downstream met a type it
  // could say nothing about and could not tell whether that was the reader's
  // doing or an earlier refusal's. It carried on regardless, which is how one
  // typo produced a second error blaming a value that was fine.
  //
  // Only ever set when `kind` is `Unknown`, and it travels: a type that came
  // out unknown because its operand was unknown keeps the operand's, so what a
  // diagnostic finally names is the mistake that started the chain rather than
  // the last link in it.
  Span from{};
  // What this blank will take, when it is one. Bare `any` asks for anything.
  Family asks = Family::Anything;

  constexpr Ty() = default;
  constexpr Ty(Type k) : kind(k) {}
  constexpr Ty(Type k, Type e) : kind(k), element(e) {}
  constexpr Ty(Type k, Type e, bool n) : kind(k), element(e), orNothing(n) {}
  constexpr Ty(Type k, Type e, bool n, unsigned w)
      : kind(k), element(e), named(w), orNothing(n) {}

  constexpr bool holds() const { return kind == Type::Many; }
  constexpr bool mayBeNothing() const { return orNothing; }
  // What is inside, once the `or-nothing` is taken off.
  constexpr Ty within() const {
    Ty inside{kind, element, false, named};
    inside.from = from;
    return inside;
  }
  constexpr bool isStruct() const { return kind == Type::Struct; }
  // Whether this is unknown because of something already refused, rather than
  // unknown for a reason nobody has worked out.
  constexpr bool tracedBack() const {
    return kind == Type::Unknown && (from.begin != 0 || from.end != 0);
  }
};

// An unknown type that knows what made it unknown. Everything that hands one of
// these on hands the span on with it.
constexpr Ty unknownFrom(Span where) {
  Ty t;
  t.from = where;
  return t;
}

// Where two types disagree, an unknown that was traced back beats one that was
// not: the chain is only as good as its first link, and a link with no span is
// no link.
constexpr Ty eitherTrace(Ty a, Ty b) { return a.tracedBack() ? a : b; }

// Whether two families could both answer to one type, so that a `whichever`
// asking both would have a choice to make and no rule to make it by. Every pair
// is disjoint except the four number families under `number`.
bool overlaps(Family a, Family b);

// Whether a type is one of the things a family holds. A blank asking for a
// family is answered by this at the call that fills it in.
bool inFamily(Ty type, Family family);

// `from` and `asks` are deliberately not compared. One says where a type came
// from and the other what a blank would accept — neither says what a type *is*.
// Two unknowns are the same unknown however they were arrived at, and a blank is
// compared to see whether it is a blank rather than to tell two of them apart:
// one signature has one blank, so there are never two to tell apart.
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

// One of the things a `many` holds. Which struct it is has to come along:
// building the element as `Ty{array.element}` said `Type::Struct` and left the
// number behind, so every `many` of a struct resolved to whichever struct was
// declared first, and only a program with one of them looked right.
constexpr Ty elementOf(Ty array) {
  return array.element == Type::Struct ? structNamed(array.named)
                                       : Ty{array.element};
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

  // What the bounds worked out about sums in counted loops — `E0534` and
  // `W0001` — held back rather than reported.
  //
  // A bound only ever says *at most*, and at most is sometimes wrong in the
  // direction that refuses a working program. Reported here, it would stop
  // compilation before there was a middle layer to run, and a run cannot
  // overturn a refusal that already happened. So these wait for `ahead`, which
  // either lets them stand or drops them for having actually run the loop.
  std::vector<Diagnostic> aboutSums;

  // Where a sum coming round is worth telling somebody about: every statement
  // writing a whole number into a name whose chain did *not* say `wrapping`.
  //
  // Both ends of that matter. `wrapping` is the reader saying it is meant, and
  // a checksum coming round is the checksum working. But `wrapping` is written
  // on a **name**, and a sum happens between values — so a sum whose answer
  // never becomes a name has nowhere for the word to go, and refusing one would
  // be refusing a program with no way to answer back:
  //
  //     var.mut.bool 'b' = [(int16:*234*) >== ('n' x *4*)];
  //
  // That multiply may come round and there is nothing anybody could write to
  // say it is meant to. So a run says nothing about it, and the language having
  // no way to say it is the open question rather than the reader's problem.
  std::vector<Span> intoPlainNames;

  // Which generic was called with what, in the order they were first met. The
  // spelling rather than the type, because filling a blank in writes a word into
  // a chain. One entry per pair however often it is called.
  std::vector<std::pair<std::string, std::string>> instantiations;

  // Which call filled which blank, so that expanding knows what each one should
  // be pointed at. Keyed by node, like everything else the checker works out.
  std::unordered_map<const Expr *, std::string> blankAt;

  // Which arm of each `whichever` was chosen, by the type the subject turned
  // out to be. The statement is replaced by that arm's block before ownership
  // or the middle layer run, so nothing downstream ever meets the word — the
  // same erasure a generic gets, and for the same reason: the arms that were
  // not chosen are written against types this copy does not have.
  std::unordered_map<const Stmt *, unsigned> chosenArm;

  // The most times any counted loop in this file goes round, where both its
  // ends are written down. Free to work out — the number is already computed to
  // bound what the loop adds up — and it is what lets a run say, before it
  // starts, roughly what it is about to spend.
  __int128 mostRounds = 0;
  Span longestLoop;

  // What the checker worked out, so that nothing after it has to work the same
  // thing out again. Keyed by node, which is stable for as long as the tree is.
  std::unordered_map<const Expr *, Ty> expressions;
  std::unordered_map<const Stmt *, Ty> declarations;
  std::unordered_map<const Item *, Ty> items;
  // The structs a file declared, so nothing after the checker has to read them
  // out of the tree again. A `Ty` naming one is an index into this.
  std::vector<Shape> shapes;
  // Reaches into a `many` that were shown to be places it has, so that nothing
  // asks again while the program runs. A `many` is a fixed length once it is
  // made, which is what makes a loop counting to `count[…]` answerable here.
  std::unordered_set<const Expr *> settled;

  Ty of(const Expr *e) const {
    auto found = expressions.find(e);
    return found == expressions.end() ? Ty{} : found->second;
  }

  bool ok() const { return !anyErrors(diagnostics); }
};

// Names and types. Ownership is a separate pass, and does not exist yet.
CheckResult check(const Source &source, const Program &program);

} // namespace xag
