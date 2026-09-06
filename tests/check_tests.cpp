#include "xag/Check.h"
#include "xag/Lexer.h"
#include "xag/Parser.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #cond "\n";            \
      ++failures;                                                                        \
    }                                                                                    \
  } while (false)

struct Checked {
  xag::Source source;
  xag::LexResult lexed;
  xag::ParseResult parsed;
  xag::CheckResult checked;

  bool ok() const { return lexed.ok() && parsed.ok() && checked.ok(); }
  std::string code(unsigned i) const {
    return i < checked.diagnostics.size() ? checked.diagnostics[i].code : "(none)";
  }
};

Checked run(const std::string &text) {
  Checked c{xag::Source("test.xag", text), {}, {}, {}};
  c.lexed = xag::lex(c.source);
  c.parsed = xag::parse(c.source, c.lexed.tokens);
  c.checked = xag::check(c.source, c.parsed.program);
  return c;
}

Checked inStart(const std::string &body) { return run("START {\n" + body + "\n}\n"); }

void aNameMustBeDeclared() {
  CHECK(inStart("print.stdout['nope' \\n];").code(0) == "E0501");
  CHECK(inStart("var.int64 'n' = [*1*];\n    print.stdout['n' \\n];").ok());
}

void aNameIsDeclaredOnce() {
  CHECK(inStart("var.int64 'n' = [*1*];\n    var.int64 'n' = [*2*];").code(0) == "E0502");
}

void aTypeMustExist() {
  CHECK(inStart("var.i65 'n' = [*1*];").code(0) == "E0503");
}

void aWrittenValueSaysWhatItIs() {
  // A print states no parameter types, so each written value must say its own.
  CHECK(inStart("print.stdout[*Hello* \\n];").code(0) == "E0507");
  CHECK(inStart("print.stdout[str:*Hello* \\n];").ok());
  // A chain says it, so nothing is repeated.
  CHECK(inStart("var.str 's' = [*Hello*];").ok());
}

void aWrittenValueFitsItsType() {
  CHECK(inStart("var.int64 'n' = [*abc*];").code(0) == "E0509");
  CHECK(inStart("var.bool 'b' = [*maybe*];").code(0) == "E0509");
  CHECK(inStart("var.int64 'n' = [*-12*];").ok());
  CHECK(inStart("var.bool 'b' = [*true*];").ok());
}

void aSizeIsAlwaysWritten() {
  // There is no `int` on its own, because there is no size to assume.
  CHECK(inStart("var.int 'n' = [*1*];").code(0) == "E0503");
  CHECK(inStart("var.uint 'n' = [*1*];").code(0) == "E0503");
  for (const char *type : {"int8", "int16", "int32", "int64", "int128", "uint8",
                           "uint16", "uint32", "uint64", "uint128"})
    CHECK(inStart(std::string("var.") + type + " 'n' = [*1*];").ok());
}

void aWrittenNumberHasToFit() {
  CHECK(inStart("var.int8 'n' = [*127*];").ok());
  CHECK(inStart("var.int8 'n' = [*128*];").code(0) == "E0509");
  CHECK(inStart("var.int8 'n' = [*-128*];").ok());
  CHECK(inStart("var.int8 'n' = [*-129*];").code(0) == "E0509");
  CHECK(inStart("var.uint8 'n' = [*255*];").ok());
  CHECK(inStart("var.uint8 'n' = [*256*];").code(0) == "E0509");
  // Unsigned holds nothing below zero, whatever its width.
  CHECK(inStart("var.uint64 'n' = [*-1*];").code(0) == "E0509");
  CHECK(inStart("var.uint128 'n' = [*340282366920938463463374607431768211455*];").ok());
}

void binaryHoldsWhatIEEESaysItHolds() {
  for (const char *type : {"bin16", "bin32", "bin64"})
    CHECK(inStart(std::string("var.") + type + " 'n' = [*1.5*];").ok());
  CHECK(inStart("var.bin64 'n' = [*-0.25*];").ok());
  CHECK(inStart("var.bin64 'n' = [*1e300*];").ok());
  CHECK(inStart("var.bin64 'n' = [*3*];").ok()); // a whole number is a fine `bin`
  CHECK(inStart("var.bin64 'n' = [*abc*];").code(0) == "E0509");
  // A value that would arrive as infinity was not the value written down.
  CHECK(inStart("var.bin64 'n' = [*1e400*];").code(0) == "E0509");
  CHECK(inStart("var.bin32 'n' = [*1e300*];").code(0) == "E0509");
  // What a print writes, a program may write back.
  CHECK(inStart("var.bin64 'n' = [*infinity*];").ok());
  CHECK(inStart("var.bin64 'n' = [*not-a-number*];").ok());
  // And a `bin` is not a whole number, here as anywhere.
  CHECK(inStart("var.bin64 'a' = [*1.5*];\n    var.int64 'b' = ['a' + *1*];").code(0) ==
        "E0506");
}

void everyTypeHasSomethingBehindItNow() {
  for (const char *type : {"bin128", "deci32", "deci64", "deci128"})
    CHECK(inStart(std::string("var.") + type + " 'n' = [*1.5*];").ok());
  CHECK(inStart("var.bin128 'n' = [*abc*];").code(0) == "E0509");
  CHECK(inStart("var.deci64 'n' = [*abc*];").code(0) == "E0509");
  // A decimal and a binary are no more alike than any other two types.
  CHECK(inStart("var.deci64 'a' = [*1*];\n    var.bin64 'b' = ['a' + *1*];").code(0) ==
        "E0506");
}

void sizesDoNotMixOnTheirOwn() {
  CHECK(inStart("var.int32 'a' = [*1*];\n    var.int64 'b' = ['a' + *1*];").code(0) ==
        "E0506");
  CHECK(inStart("var.int64 'a' = [*1*];\n    var.uint64 'b' = ['a' + *1*];").code(0) ==
        "E0506");
  CHECK(inStart("var.int32 'a' = [*1*];\n    var.int32 'b' = ['a' + *1*];").ok());
  // A comparison takes its type from its left side, so the right side fits it.
  CHECK(inStart("var.uint8 'a' = [*200*];\n    var.bool 'b' = ['a' > *100*];").ok());
  CHECK(inStart("var.uint8 'a' = [*200*];\n    var.bool 'b' = ['a' > *300*];").code(0) ==
        "E0509");
}

void nothingConvertsOnItsOwn() {
  CHECK(inStart("var.str 's' = [*a*];\n    var.int64 'n' = ['s'];").code(0) == "E0506");
  CHECK(inStart("var.int64 'n' = [*1* + *2*];").ok());
  CHECK(inStart("var.str 's' = [*a*];\n    var.int64 'n' = ['s' + *1*];").code(0) == "E0506");
}

void piecesSideBySideJoin() {
  CHECK(inStart("var.str 'n' = [*Hello, * *world*];").ok());
  // Joining builds text, so an int64 cannot be one of the pieces.
  CHECK(inStart("var.int64 'n' = [*1*];\n    var.str 's' = [*x* 'n'];").code(0) == "E0506");
}

void anImmutableNameDoesNotChange() {
  CHECK(inStart("var.int64 'n' = [*1*];\n    set 'n' = [*2*];").code(0) == "E0508");
  CHECK(inStart("var.mut.int64 'n' = [*1*];\n    set 'n' = [*2*];").ok());
}

void aBorrowSaysWhetherItWrites() {
  CHECK(run("fn.nothing 'excite' [refmut.str 'text'] { set 'text' = ['text' *!*]; }\n"
            "START { }\n").ok());
  // `ref` lends for reading, so writing through it is the same mistake as
  // writing to anything else that does not change.
  CHECK(run("fn.nothing 'excite' [ref.str 'text'] { set 'text' = ['text' *!*]; }\n"
            "START { }\n").code(0) == "E0508");
}

void callsAreChecked() {
  CHECK(run("fn.int64 'twice' [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.int64 'a' = [twice[*2*]]; }\n").ok());
  CHECK(run("fn.int64 'twice' [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.int64 'a' = [twice[*2*, *3*]]; }\n").code(0) == "E0505");
  CHECK(run("fn.int64 'twice' [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.str 's' = [*x*]; var.int64 'a' = [twice['s']]; }\n").code(0) == "E0506");
  CHECK(inStart("nosuch[*1*];").code(0) == "E0504");
}

void signaturesAreReadBeforeBodies() {
  // Two functions may call each other, and a constant may be used above itself.
  CHECK(run("fn.int64 'odd' [int64 'n'] { give [even['n']]; }\n"
            "fn.int64 'even' [int64 'n'] { give ['n']; }\n"
            "START { var.int64 'a' = [odd[*3*]]; }\n").ok());
  CHECK(run("fn.int64 'limit' [] { give ['LIMIT']; }\n"
            "const.int64 'LIMIT' = [*10*];\n"
            "START { var.int64 'a' = [limit[]]; }\n").ok());
}

void giveAnswersItsFunction() {
  CHECK(inStart("give [*1*];").code(0) == "E0511");
  CHECK(run("fn.nothing 'quiet' [] { give [*1*]; }\nSTART { }\n").code(0) == "E0511");
  CHECK(run("fn.str 'greet' [] { give [*hi*]; }\nSTART { }\n").ok());
  CHECK(run("fn.str 'greet' [] { give [*1*]; }\nSTART { }\n").ok()); // *1* is text under str
}

void aFunctionAnswersEveryWayOut() {
  CHECK(run("fn.int64 'f' [] { }\nSTART { }\n").code(0) == "E0513");
  CHECK(run("fn.int64 'f' [int64 'n'] { print.stdout[str:*hi* \\n]; }\nSTART { }\n")
            .code(0) == "E0513");
  CHECK(run("fn.int64 'f' [] { give [*1*]; }\nSTART { }\n").ok());

  // Every arm and an `else`, so there is no way out that says nothing.
  CHECK(run("fn.int64 'f' [int64 'n'] {\n"
            "  if 'n' > *0* { give [*1*]; } else { give [*0*]; } }\nSTART { }\n").ok());
  // No `else`, so one way out is left silent.
  CHECK(run("fn.int64 'f' [int64 'n'] {\n"
            "  if 'n' > *0* { give [*1*]; } }\nSTART { }\n").code(0) == "E0513");
  // A loop may run no times at all, and then it has answered nothing.
  CHECK(run("fn.int64 'f' [] {\n"
            "  loop.range.int64 'i' = [*1*, *3*] { give [*1*]; } }\nSTART { }\n")
            .code(0) == "E0513");
  // `nothing` is a real answer, and needs none given.
  CHECK(run("fn.nothing 'f' [] { print.stdout[str:*hi* \\n]; }\nSTART { }\n").ok());
}

void breakNeedsALoop() {
  CHECK(inStart("break;").code(0) == "E0510");
  CHECK(inStart("loop.range.int64 'i' = [*1*, *3*] { break; }").ok());
}

void conditionsAskABool() {
  CHECK(inStart("var.int64 'n' = [*1*];\n    if 'n' { }").code(0) == "E0506");
  CHECK(inStart("var.int64 'n' = [*1*];\n    if 'n' > *0* { }").ok());
}

void aPermCounterOutlivesItsLoop() {
  CHECK(inStart("loop.perm.range.int64 'i' = [*1*, *3*] { }\n"
                "    print.stdout['i' \\n];").ok());
  // And it is a name like any other afterwards, so it cannot be taken twice.
  CHECK(inStart("var.int64 'i' = [*0*];\n"
                "    loop.perm.range.int64 'i' = [*1*, *3*] { }").code(0) == "E0502");
  // `temp` is what a counter is when nothing says otherwise, so writing it is
  // writing a default — refused where every other default is, in the chain.
  CHECK(!inStart("loop.temp.range.int64 'i' = [*1*, *3*] { }").ok());
}

void theCounterIsInScopeOnlyInTheLoop() {
  CHECK(inStart("loop.range.int64 'i' = [*1*, *3*] { print.stdout['i' \\n]; }").ok());
  CHECK(inStart("loop.range.int64 'i' = [*1*, *3*] { }\n    print.stdout['i' \\n];")
            .code(0) == "E0501");
}

void aManyHoldsSeveralOfOneType() {
  CHECK(inStart("var.many.int64 'xs' = [*1* *2* *3*];").ok());
  CHECK(inStart("var.many.str 'ws' = [*a* *b*];").ok());
  CHECK(inStart("var.many.int64 'none' = [];").ok());

  // Items under a `many` are its places, so each is one of what it holds.
  CHECK(inStart("var.many.int64 'xs' = [*1* *a*];").code(0) == "E0509");
  CHECK(inStart("var.many.int64 'xs' = [*1* *true*];").code(0) == "E0509");

  // Holding nothing is a length; one value is not, and has to be there.
  CHECK(inStart("var.int64 'n' = [];").code(0) == "E0517");
  CHECK(inStart("var.str 's' = [];").code(0) == "E0517");
}

void anElementIsOneOfWhatItHolds() {
  CHECK(inStart("var.many.int64 'xs' = [*1*];\n"
                "    var.int64 'n' = ['xs'[*0*]];").ok());
  CHECK(inStart("var.many.int64 'xs' = [*1*];\n"
                "    var.str 's' = ['xs'[*0*]];").code(0) == "E0506");

  // A name holding one value is that value, and there is no first of it.
  CHECK(inStart("var.int64 'n' = [*1*];\n"
                "    print.stdout['n'[*0*] \\n];").code(0) == "E0514");
  CHECK(inStart("var.mut.int64 'n' = [*1*];\n"
                "    set 'n'[*0*] = [*2*];").code(0) == "E0514");

  // An index is an `int64`, because that is what `count` answers with.
  CHECK(inStart("var.many.int64 'xs' = [*1*];\n"
                "    print.stdout['xs'[int32:*0*] \\n];").code(0) == "E0506");
}

void countAsksHowManyOfEither() {
  CHECK(inStart("var.many.int64 'xs' = [*1* *2*];\n"
                "    print.stdout[(count[ref 'xs']) \\n];").ok());
  CHECK(inStart("var.str 's' = [*hi*];\n"
                "    print.stdout[(count[ref 's']) \\n];").ok());
  CHECK(inStart("var.int64 'n' = [*1*];\n"
                "    print.stdout[(count['n']) \\n];").code(0) == "E0506");
}

void fillNeedsAValueThatCopies() {
  CHECK(inStart("var.many.int64 'xs' = [fill[*0*, *4*]];").ok());
  CHECK(inStart("var.many.str 'ws' = [fill[*hi*, *4*]];").code(0) == "E0515");
  // Nothing here says what it is filling.
  CHECK(inStart("var.int64 'n' = [fill[*0*, *4*]];").code(0) == "E0507");
}

void showingAManyIsRefused() {
  CHECK(inStart("var.many.int64 'xs' = [*1*];\n"
                "    print.stdout['xs' \\n];").code(0) == "E0516");
}

void aManyTravelsWhole() {
  // A lone item that is already the array is the array; anything else is one
  // of its places.
  CHECK(run("fn.many.int64 'f' [] {\n"
            "    var.many.int64 'xs' = [*1* *2*];\n"
            "    give ['xs'];\n}\n").ok());
  CHECK(run("fn.int64 'g' [ref.many.int64 'xs'] { give ['xs'[*0*]]; }\n").ok());
}

void nothingNeedsSomewhereToBe() {
  CHECK(inStart("var.or-nothing.str 's' = [nothing];").ok());
  CHECK(inStart("var.str 's' = [nothing];").code(0) == "E0518");
  CHECK(inStart("print.stdout[nothing \\n];").code(0) == "E0518");
}

void aValueGoesInWithoutAWord() {
  // No choice about what it could mean, so nothing is written.
  CHECK(inStart("var.or-nothing.str 's' = [*hi*];").ok());
  CHECK(inStart("var.or-nothing.int64 'n' = [*3*];").ok());
  CHECK(run("fn.or-nothing.int64 'f' [] { give [*3*]; }\n").ok());
  // But it still has to be the thing it holds.
  CHECK(inStart("var.or-nothing.int64 'n' = [*hi*];").code(0) == "E0509");
}

void holdsAsksSomethingThatMayBeMissing() {
  CHECK(run("fn.or-nothing.int64 'f' [] { give [nothing]; }\n"
            "START { var.or-nothing.int64 'n' = [f[]];\n"
            "  if 'n' holds 'v' { print.stdout['v' \\n]; } }\n").ok());

  // A `bool` is never absent, so there is nothing to ask about.
  CHECK(inStart("var.bool 'b' = [*true*];\n"
                "    if 'b' holds 'x' { }").code(0) == "E0519");
  // And without `holds`, a thing that may hold nothing is not a condition.
  CHECK(inStart("var.or-nothing.int64 'n' = [*1*];\n"
                "    if 'n' { }").code(0) == "E0506");
}

void whatIsHeldIsTheTypeWithoutTheAbsence() {
  CHECK(run("fn.int64 'twice' [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.or-nothing.int64 'n' = [*2*];\n"
            "  if 'n' holds 'v' { print.stdout[twice['v'] \\n]; } }\n").ok());
  // Arithmetic on the whole thing has no answer when it holds none.
  CHECK(inStart("var.or-nothing.int64 'n' = [*1*];\n"
                "    var.int64 'm' = ['n' + *1*];").code(0) == "E0506");
}

void aWhenCoversEveryCase() {
  CHECK(inStart("var.or-nothing.str 's' = [*hi*];\n"
                "    when 's' { is 't' { print.stdout['t' \\n]; } is nothing { } }").ok());

  // A case nobody wrote is a case nobody thought about.
  CHECK(inStart("var.or-nothing.str 's' = [*hi*];\n"
                "    when 's' { is 't' { } }").code(0) == "E0522");
  CHECK(inStart("var.or-nothing.str 's' = [*hi*];\n"
                "    when 's' { is nothing { } }").code(0) == "E0522");

  // And each case once.
  CHECK(inStart("var.or-nothing.str 's' = [*hi*];\n"
                "    when 's' { is 't' { } is 'u' { } is nothing { } }").code(0) ==
        "E0521");
  CHECK(inStart("var.or-nothing.str 's' = [*hi*];\n"
                "    when 's' { is 't' { } is nothing { } is nothing { } }").code(0) ==
        "E0521");

  // Something with one shape has nothing to choose between.
  CHECK(inStart("var.int64 'n' = [*1*];\n"
                "    when 'n' { is 'v' { } is nothing { } }").code(0) == "E0520");
}

void aWhenArmLendsWhatWasThere() {
  CHECK(run("fn.int64 'twice' [int64 'n'] { give ['n' + 'n']; }\n"
            "START { var.or-nothing.int64 'n' = [*2*];\n"
            "  when 'n' { is 'v' { print.stdout[twice['v'] \\n]; } is nothing { } } }\n")
            .ok());
  // The name belongs to its arm and nowhere else.
  CHECK(inStart("var.or-nothing.int64 'n' = [*2*];\n"
                "    when 'n' { is 'v' { } is nothing { } }\n"
                "    print.stdout['v' \\n];").code(0) == "E0501");
}

void aStructIsAGroupOfNamedThings() {
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n"
            "START {\n    var.point 'p' = [*1* *2*];\n"
            "    print.stdout['p'.y \\n];\n}\n").ok());

  // One value for each of the things it holds, in the order it was written in.
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n"
            "START {\n    var.point 'p' = [*1*];\n}\n").code(0) == "E0529");
  CHECK(run("struct 'point' [int64 'x', str 'name']\n"
            "START {\n    var.int64 'n' = [*1*];\n"
            "    var.point 'p' = [*1* 'n'];\n}\n").code(0) == "E0506");

  // A field has to be one it holds, and only a struct has any.
  CHECK(run("struct 'point' [int64 'x']\n"
            "START {\n    var.mut.point 'p' = [*1*];\n"
            "    set 'p'.z = [*2*];\n}\n").code(0) == "E0528");
  CHECK(run("START {\n    var.mut.int64 'n' = [*1*];\n"
            "    set 'n'.x = [*2*];\n}\n").code(0) == "E0527");

  // A group of none is `nothing`, which the language already has.
  CHECK(run("struct 'empty' []\n").code(0) == "E0525");

  // However many times it were laid out, there would be one more inside.
  CHECK(run("struct 'node' [int64 'v', node 'next']\n").code(0) == "E0526");
  CHECK(run("struct 'a' [b 'to']\nstruct 'b' [a 'back']\n").code(0) == "E0526");

  // They may name each other, so a later one is in scope for an earlier one.
  CHECK(run("struct 'line' [point 'from', point 'to']\n"
            "struct 'point' [int64 'x', int64 'y']\n"
            "START {\n    var.point 'a' = [*0* *0*];\n"
            "    var.point 'b' = [*1* *1*];\n"
            "    var.line 'l' = [move 'a' move 'b'];\n"
            "    print.stdout['l'.to.x \\n];\n}\n").ok());

  // Which struct a `many` holds has to survive being asked for. Every one of
  // these resolved to whichever struct was declared first, so a program with
  // only one of them could not tell.
  CHECK(run("struct 'point' [int64 'x', int64 'y']\nstruct 'tag' [str 'name']\n"
            "START {\n    var.tag 'a' = [*ada*];\n    var.tag 'b' = [*bob*];\n"
            "    var.many.tag 'ts' = [move 'a' move 'b'];\n"
            "    print.stdout['ts'[*1*].name \\n];\n}\n").ok());
  CHECK(run("struct 'tag' [str 'name']\nstruct 'point' [int64 'x', int64 'y']\n"
            "START {\n    var.point 'a' = [*1* *2*];\n    var.point 'b' = [*3* *4*];\n"
            "    var.many.point 'ps' = [move 'a' move 'b'];\n"
            "    print.stdout['ps'[*0*].y \\n];\n}\n").ok());
  // And the wrong one is still refused, rather than quietly allowed.
  CHECK(run("struct 'point' [int64 'x', int64 'y']\nstruct 'tag' [str 'name']\n"
            "START {\n    var.point 'p' = [*1* *2*];\n"
            "    var.many.tag 'ts' = [move 'p'];\n}\n").code(0) == "E0506");

  // A name is a name, whichever kind it is.
  CHECK(run("struct 'point' [int64 'x']\nstruct 'point' [int64 'y']\n").code(0) == "E0502");
  CHECK(run("struct 'point' [int64 'x', int64 'x']\n").code(0) == "E0502");
}

// A group of items where one item goes is a struct made there, and what it is
// comes from the thing it fills — there is no telling one group of two numbers
// from another by looking at it.
void aStructIsNamedWhereItIsMade() {
  const char *kShapes = "struct 'point' [int64 'x', int64 'y']\n"
                        "struct 'line' [point 'from', point 'to']\n";
  CHECK(run(std::string(kShapes) + "START {\n"
            "    var.line 'l' = [point[*0* *0*] point[*1* *2*]];\n"
            "    print.stdout['l'.to.y \\n];\n}\n").ok());
  CHECK(run(std::string(kShapes) + "START {\n"
            "    var.many.point 'ps' = [point[*1* *2*] point[*3* *4*]];\n"
            "    print.stdout['ps'[*1*].x \\n];\n}\n").ok());

  // The count is still one for each, one level down as well.
  CHECK(run(std::string(kShapes) + "START {\n"
            "    var.line 'l' = [point[*0*] point[*1* *2*]];\n}\n").code(0) == "E0529");

  // What it makes is what it is named, whatever was wanted of it.
  CHECK(run(std::string(kShapes) + "START {\n"
            "    var.int64 'n' = [point[*1* *2*]];\n}\n").code(0) == "E0506");

  // One mistake is reported once: pairing items off after the count is wrong
  // is guessing, and it said the same span was wrong twice.
  CHECK(run(std::string(kShapes) + "START {\n"
            "    var.line 'l' = [point[*0* *0*]];\n}\n").code(1) == "(none)");
}

void aStructTravelsWithTheRest() {
  // It stands where a type stands: in a `many`, behind `or-nothing`, in a
  // function's parameters and in what it answers.
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n"
            "fn.point 'middle' [ref.many.point 'ps'] {\n"
            "    var.point 'p' = [*0* *0*];\n    give [move 'p'];\n}\n").ok());
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n"
            "START {\n    var.or-nothing.point 'p' = [nothing];\n"
            "    if 'p' holds 'one' { print.stdout['one'.x \\n]; }\n}\n").ok());
}

// A struct is handed over whole, as a `many` is, however little is in it: the
// places it holds are its own, and there is only ever one of them.
void aStructIsHandedOverRatherThanCopied() {
  CHECK(run("struct 'tag' [str 'name']\nstruct 'pair' [tag 'one', tag 'two']\n"
            "START {\n    var.tag 'a' = [*ada*];\n    var.tag 'b' = [*bob*];\n"
            "    var.pair 'p' = [move 'a' move 'b'];\n"
            "    print.stdout['p'.two.name \\n];\n}\n").ok());
}

} // namespace

int main() {
  aNameMustBeDeclared();
  aNameIsDeclaredOnce();
  aTypeMustExist();
  aWrittenValueSaysWhatItIs();
  aWrittenValueFitsItsType();
  aSizeIsAlwaysWritten();
  aWrittenNumberHasToFit();
  binaryHoldsWhatIEEESaysItHolds();
  everyTypeHasSomethingBehindItNow();
  sizesDoNotMixOnTheirOwn();
  nothingConvertsOnItsOwn();
  piecesSideBySideJoin();
  anImmutableNameDoesNotChange();
  aBorrowSaysWhetherItWrites();
  callsAreChecked();
  signaturesAreReadBeforeBodies();
  giveAnswersItsFunction();
  aFunctionAnswersEveryWayOut();
  breakNeedsALoop();
  conditionsAskABool();
  aPermCounterOutlivesItsLoop();
  theCounterIsInScopeOnlyInTheLoop();
  aManyHoldsSeveralOfOneType();
  anElementIsOneOfWhatItHolds();
  countAsksHowManyOfEither();
  fillNeedsAValueThatCopies();
  showingAManyIsRefused();
  aManyTravelsWhole();
  nothingNeedsSomewhereToBe();
  aValueGoesInWithoutAWord();
  holdsAsksSomethingThatMayBeMissing();
  whatIsHeldIsTheTypeWithoutTheAbsence();
  aWhenCoversEveryCase();
  aWhenArmLendsWhatWasThere();
  aStructIsAGroupOfNamedThings();
  aStructIsNamedWhereItIsMade();
  aStructTravelsWithTheRest();
  aStructIsHandedOverRatherThanCopied();

  if (failures == 0)
    std::cout << "all check tests passed\n";
  return failures == 0 ? 0 : 1;
}
