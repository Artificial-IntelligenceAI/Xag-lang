#include "xag/Check.h"
#include "xag/Fold.h"
#include "xag/Mir.h"
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

// The first thing said about a body, refusal or not, so a warning can be asked
// about as easily as a refusal.
std::string saidIn(const std::string &body) {
  const Checked c = inStart(body);
  return c.checked.diagnostics.empty() ? "" : c.checked.diagnostics.front().code;
}

// The first thing any pass of the front end says, whichever pass says it. What
// is refused is often refused by the reader rather than the checker, and a test
// that only asks one of them reads a refusal it cannot see as no refusal.
std::string refused(const std::string &text) {
  const xag::Source source("test.xag", text);
  const xag::LexResult lexed = xag::lex(source);
  if (!lexed.diagnostics.empty())
    return lexed.diagnostics.front().code;
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (!parsed.diagnostics.empty())
    return parsed.diagnostics.front().code;
  const xag::CheckResult checked = xag::check(source, parsed.program);
  if (!checked.diagnostics.empty())
    return checked.diagnostics.front().code;
  return "";
}

std::string refusedIn(const std::string &body) {
  return refused("START {\n" + body + "\n}\n");
}

// The first code from the pass that runs once the middle layer is built, or ""
// when it had nothing to say.
std::string built(const std::string &text) {
  const xag::Source source("test.xag", text);
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  if (!lexed.ok() || !parsed.ok() || !checked.ok())
    return "(did not reach it)";
  xag::MirResult made = xag::build(source, parsed.program, checked);
  xag::elaborate(made.mir);
  const xag::FoldResult folded = xag::fold(source, made.mir);
  return folded.diagnostics.empty() ? "" : folded.diagnostics.front().code;
}

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

// Where a count starts and stops is counted in, so both are the counter's own
// type. Asking and throwing the answer away let anything at all stand as a
// bound, and the engines then disagreed about what it meant.
void aCountedLoopCountsInItsOwnType() {
  CHECK(inStart("loop.range.int64 'i' = [*0*, *5*] { }").ok());
  CHECK(inStart("var.int64 'n' = [*5*];\n"
                "    loop.range.int64 'i' = [*0*, 'n'] { }").ok());

  CHECK(inStart("var.int8 'a' = [*3*];\n"
                "    loop.range.int64 'i' = ['a', *5*] { }").code(0) == "E0506");
  CHECK(inStart("var.bin64 'f' = [*2.5*];\n"
                "    loop.range.int64 'i' = [*0*, 'f'] { }").code(0) == "E0506");
  CHECK(inStart("var.str 's' = [*hi*];\n"
                "    loop.range.int64 'i' = [*0*, 's'] { }").code(0) == "E0506");
}

// A counted loop adds one past where it stops to know it is done. When the last
// value is the most the counter can hold, that one more does not fit, so the
// loop cannot finish — and this is certain rather than suspected.
// What is written down is worked out at build time, and what is written down
// and certainly wrong is refused rather than left to stop when it is reached.
// These come from the pass after the middle layer is built, so they arrive
// through `xagc check` rather than from the checker itself.
void whatIsWrittenDownIsWorkedOut() {
  CHECK(built("START {\n    var.int64 'n' = [*5* / *0*];\n"
              "    print.stdout['n' \\n];\n}\n") == "E0533");
  CHECK(built("START {\n    var.int64 'n' = [*5* mod *0*];\n"
              "    print.stdout['n' \\n];\n}\n") == "E0533");
  CHECK(built("START {\n    var.many.int64 'xs' = [*10* *20* *30*];\n"
              "    print.stdout['xs'[*7*] \\n];\n}\n") == "E0532");

  // A place that is there, and a divisor that is not zero, are left alone.
  CHECK(built("START {\n    var.many.int64 'xs' = [*10* *20* *30*];\n"
              "    print.stdout['xs'[*2*] \\n];\n}\n") == "");
  CHECK(built("START {\n    var.int64 'n' = [*5* / *2*];\n"
              "    print.stdout['n' \\n];\n}\n") == "");

  // A name holding something written down is that thing, so this is caught too.
  CHECK(built("START {\n    var.many.int64 'xs' = [*10* *20*];\n"
              "    var.int64 'i' = [*9*];\n"
              "    print.stdout['xs'['i'] \\n];\n}\n") == "E0532");

  // Writing one asks the same question reading one does.
  CHECK(built("START {\n    var.mut.many.int64 'xs' = [*1* *2*];\n"
              "    set 'xs'[*5*] = [*9*];\n}\n") == "E0532");
  CHECK(built("START {\n    var.mut.many.int64 'xs' = [*1* *2*];\n"
              "    set 'xs'[*1*] = [*9*];\n}\n") == "");

  // Pieces side by side, all written down, are one written thing.
  CHECK(built("START {\n    var.str 's' = [*a* *b* *c*];\n"
              "    print.stdout['s' \\n];\n}\n") == "");

  // Nothing is claimed about a place that is not known until it runs.
  CHECK(built("fn.nothing 'at' [ref.many.int64 'xs', int64 'i'] {\n"
              "    print.stdout['xs'['i'] \\n];\n}\n"
              "START {\n    var.many.int64 'ns' = [*10* *20*];\n"
              "    at[ref 'ns', *1*];\n}\n") == "");
}

void aLoopThatCannotFinishIsRefused() {
  CHECK(inStart("loop.range.int8 'i' = [*0*, *127*] { }").code(0) == "E0531");
  CHECK(inStart("loop.range.uint8 'i' = [*0*, *255*] { }").code(0) == "E0531");
  CHECK(inStart("loop.range.int16 'i' = [*0*, *32767*] { }").code(0) == "E0531");
  CHECK(inStart("loop.range.int8 'i' = [*0*, *126*] { }").ok());
  CHECK(inStart("loop.range.int64 'i' = [*0*, *10*] { }").ok());
}

// A name may say that a sum which does not fit is meant to come round, and then
// nothing is said about it. The word stands where the chain says what is
// unusual, and writing the default is refused as everywhere else.
void aNameMaySayItWraps() {
  CHECK(inStart("var.mut.wrapping.int8 'sum' = [*0*];").ok());
  CHECK(inStart("var.wrapping.int8 'n' = [*1*];").ok());
}

// A counted loop with both ends written down runs a known number of times, so
// what it adds up is a number rather than a guess.
void aCountedLoopSaysHowFarItGets() {
  // Provably past what the type holds: certain, so refused.
  CHECK(saidIn("var.mut.int8 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + *20*]; }")
        == "E0534");
  // Provably inside it: nothing is said.
  CHECK(saidIn("var.mut.int8 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *5*] { set 'sum' = ['sum' + *20*]; }")
        == "");
  // Said to be meant, so nothing is said.
  CHECK(saidIn("var.mut.wrapping.int8 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + *20*]; }")
        == "");

  // The counter's own largest step is what the loop counts to, and the shapes
  // built out of it are bounded too — these are what real programs write, and
  // a warning on every one of them would be worth nothing.
  CHECK(saidIn("var.mut.int64 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + 'i']; }")
        == "");
  CHECK(saidIn("var.mut.int64 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + ('i' x *3*)]; }")
        == "");
  CHECK(saidIn("var.mut.int64 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + ('i' mod *7*)]; }")
        == "");
  CHECK(saidIn("var.mut.int64 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + ('i' / *7*)]; }")
        == "");
  // A remainder is bounded by what it is taken against, so an `int8` that
  // cannot hold ten of them is still caught.
  CHECK(saidIn("var.mut.int8 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + ('i' mod *100*)]; }")
        == "E0534");

  // A step it cannot follow is a warning, not a refusal: the program builds.
  CHECK(saidIn("var.mut.int8 'sum' = [*0*];\n    var.int8 'step' = [*3*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + 'step']; }")
        == "W0001");
  CHECK(inStart("var.mut.int8 'sum' = [*0*];\n    var.int8 'step' = [*3*];\n"
                "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + 'step']; }")
        .ok());
}

// Nothing converts on its own, so this is how a number is asked to become text.
// It answers a `str` rather than `or-nothing` of one: every number has a way of
// being written, so it cannot fail — which is the whole difference between it
// and `convert-to-number` going the other way.
void aNumberIsAskedToBecomeText() {
  CHECK(inStart("var.int64 'n' = [*42*];\n"
                "    var.str 's' = [*x = * convert-to-str['n']];").ok());
  CHECK(inStart("var.bool 'b' = [*true*];\n"
                "    var.str 's' = [convert-to-str['b']];").ok());
  CHECK(inStart("var.deci64 'd' = [*1.10*];\n"
                "    var.str 's' = [convert-to-str['d']];").ok());

  // Text is already text.
  CHECK(inStart("var.str 't' = [*hi*];\n"
                "    var.str 's' = [convert-to-str[ref 't']];").code(0) == "E0535");

  // What holds several things has no one way of being written out — the same
  // reason showing one is refused — and there is no text of nothing.
  CHECK(inStart("var.many.int64 'xs' = [*1* *2*];\n"
                "    var.str 's' = [convert-to-str[ref 'xs']];").code(0) == "E0535");
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n"
            "START {\n    var.point 'p' = [*1* *2*];\n"
            "    var.str 's' = [convert-to-str[ref 'p']];\n}\n").code(0) == "E0535");
  CHECK(inStart("var.or-nothing.int64 'n' = [*1*];\n"
                "    var.str 's' = [convert-to-str['n']];").code(0) == "E0535");

  // One value, one answer.
  CHECK(inStart("var.int64 'n' = [*1*];\n"
                "    var.str 's' = [convert-to-str['n', 'n']];").code(0) == "E0505");
}

// What Xag has not got yet, written down as things it still refuses.
//
// These are not tests of a feature; they are the record of an absence, and a
// failure here is good news wrongly reported: something in this list has
// arrived, and whatever says so elsewhere — `design/syntax.md`, the website's
// answer about what is missing — has just become wrong and needs the same
// change. An absence that is only remembered goes stale on the ordinary
// afternoon somebody removes it, and nothing about it looks broken.
void whatIsNotHereYet() {
  // No visibility, so no `export` and no more than one file.
  CHECK(refused("export.fn.int64 'f' [] { give [*1*]; }\n") == "E0104");

  // Nothing converts on its own. `convert-to-str` is how it is asked for; what
  // is still refused is having it happen without being asked.
  CHECK(refusedIn("var.int64 'n' = [*1*];\n    var.str 's' = ['n'];") == "E0506");

  // A `many` is one level deep, and there is no way to show one.
  CHECK(refusedIn("var.many.many.int64 'g' = [];") == "E0210");
  CHECK(refusedIn("var.many.int64 'xs' = [*1*];\n"
                  "    print.stdout['xs' \\n];") == "E0516");
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
  aCountedLoopCountsInItsOwnType();
  whatIsWrittenDownIsWorkedOut();
  aLoopThatCannotFinishIsRefused();
  aNameMaySayItWraps();
  aNumberIsAskedToBecomeText();
  whatIsNotHereYet();
  aCountedLoopSaysHowFarItGets();
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
