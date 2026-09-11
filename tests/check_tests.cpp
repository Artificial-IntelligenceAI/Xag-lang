#include "xag/Check.h"
#include "xag/Expand.h"

#include <deque>
#include <set>
#include "xag/Fold.h"
#include "xag/Mir.h"
#include "as_file.h"

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
  // Everything the first complaint offered as a tip, joined, so a test can ask
  // whether it named the thing a reader needed to hear.
  // A chain is read by the parser, so what it refuses is reported there rather
  // than here — `E0201` for writing a default, `E0203` for a word this kind of
  // chain never asks.
  std::string chainCode(unsigned i) const {
    return i < parsed.diagnostics.size() ? parsed.diagnostics[i].code : "(none)";
  }
  std::string tip() const {
    std::string out;
    if (checked.diagnostics.empty())
      return out;
    for (const std::string &one : checked.diagnostics.front().tips)
      out += one + " ";
    return out;
  }
  std::string code(unsigned i) const {
    return i < checked.diagnostics.size() ? checked.diagnostics[i].code : "(none)";
  }
};

Checked run(const std::string &text) {
  Checked c{xag::Source("test.xag", xag::asFile(text)), {}, {}, {}};
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

// What the bounds worked out about a sum, which is held back rather than
// reported — `ahead` runs the loop and then either lets it stand or drops it.
// These tests are about the bounds themselves, so they ask the bounds.
std::string boundedIn(const std::string &body) {
  const Checked c = inStart(body);
  return c.checked.aboutSums.empty() ? "" : c.checked.aboutSums.front().code;
}

// The first code from the pass that runs once the middle layer is built, or ""
// when it had nothing to say.
std::string builtWith(const std::string &text, xag::Rewriting rewriting) {
  const xag::Source source("test.xag", xag::asFile(text));
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  if (!lexed.ok() || !parsed.ok() || !checked.ok())
    return "(did not reach it)";
  xag::MirResult made = xag::build(source, parsed.program, checked);
  xag::elaborate(made.mir);
  const xag::FoldResult folded = xag::fold(source, made.mir, rewriting);
  return folded.diagnostics.empty() ? "" : folded.diagnostics.front().code;
}

// The first code from the pass that runs once the middle layer is built, under
// both settings: what a program is refused for cannot depend on which engine is
// about to run it, and for a while it did — `xagc build` turned one down that
// `xagc run` was happy with.
std::string builtEitherWay(const std::string &text) {
  const std::string optimised = builtWith(text, xag::Rewriting::Yes);
  const std::string plain = builtWith(text, xag::Rewriting::No);
  return optimised == plain ? optimised : optimised + " but " + plain;
}

std::string built(const std::string &text) { return builtEitherWay(text); }

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
  CHECK(inStart("var.uint8 'a' = [*200*];\n    var.bool 'b' = ['a' > uint8:*100*];").ok());
  CHECK(inStart("var.uint8 'a' = [*200*];\n    var.bool 'b' = ['a' > uint8:*300*];").code(0) ==
        "E0509");
}

void nothingConvertsOnItsOwn() {
  CHECK(inStart("var.str 's' = [*a*];\n    var.int64 'n' = ['s'];").code(0) == "E0506");
  CHECK(inStart("var.int64 'n' = [*1* + *2*];").ok());
  CHECK(inStart("var.str 's' = [*a*];\n    var.int64 'n' = ['s' + *1*];").code(0) == "E0506");
  // The other way too: `convert-to-str` is how a number becomes text, and what
  // stays refused is having it happen without being asked.
  CHECK(inStart("var.int64 'n' = [*1*];\n    var.str 's' = ['n'];").code(0) == "E0506");
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
  CHECK(run("fn.nothing 'excite' [loanmut.str 'text'] { set 'text' = ['text' *!*]; }\n"
            "START { }\n").ok());
  // `loan` lends for reading, so writing through it is the same mistake as
  // writing to anything else that does not change.
  CHECK(run("fn.nothing 'excite' [loan.str 'text'] { set 'text' = ['text' *!*]; }\n"
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

// A `print` says where it goes, and there are two places for it to go.
// A comparison declares nothing, so neither side is told what it is. It is the
// one place a written value used to take its type from the thing standing
// beside it rather than from a slot it goes into.
// `wrapping` is written wherever a thing is declared, because a sum written into
// it is meant to come round wherever that thing is.
void wrappingIsSaidWhereAThingIsDeclared() {
  CHECK(inStart("var.mut.wrapping.int8 'n' = [*0*]; set 'n' = ['n' + *1*];").ok());
  CHECK(run("fn.nothing 'mix' [loanmut.wrapping.uint32 's', loan.uint32 'by'] {\n"
            "  set 's' = ['s' x *2654435761*]; }\n"
            "START { var.mut.uint32 'h' = [*1*]; var.uint32 'b' = [*2*];\n"
            "  mix[loanmut 'h', loan 'b']; print.stdout['h' \\n]; }\n").ok());
  CHECK(run("struct 'digest' [wrapping.uint32 'state', int64 'length']\n"
            "START { var.mut.digest 'd' = [*1* *0*];\n"
            "  set 'd'.state = ['d'.state x uint32:*31*];\n"
            "  print.stdout['d'.state \\n]; }\n").ok());

  // `checked` is what a thing is when nothing says otherwise, so writing it asks
  // for nothing and is refused like every other default.
  CHECK(inStart("var.mut.checked.int8 'n' = [*0*]; set 'n' = ['n' + int8:*1*];")
            .chainCode(0) == "E0201");
  // Not a `const`, which is worked out while compiling — a sum in one that does
  // not fit is refused there, and there is no run for a word to change.
  CHECK(run("const.wrapping.int8 'K' = [*100*];\nSTART { }\n").chainCode(0) == "E0203");
}

void aComparisonTellsNeitherSideWhatItIs() {
  CHECK(inStart("var.int8 'n' = [*5*];\n  var.bool 'b' = ['n' > *0*];").code(0) == "E0507");
  CHECK(inStart("var.int8 'n' = [*5*];\n  var.bool 'b' = ['n' > int8:*0*];").ok());
  // Either way round, so which was written first does not decide it.
  CHECK(inStart("var.int8 'n' = [*5*];\n  var.bool 'b' = [*0* < 'n'];").code(0) == "E0507");
  CHECK(inStart("var.int8 'n' = [*5*];\n  var.bool 'b' = [int8:*0* < 'n'];").ok());
  // And a sum inside one has no slot either, one level down.
  CHECK(inStart("var.int8 'n' = [*5*];\n  var.int8 'm' = [*9*];\n"
                "  var.bool 'b' = [('n' x *4*) > 'm'];").code(0) == "E0507");
  CHECK(inStart("var.int8 'n' = [*5*];\n  var.int8 'm' = [*9*];\n"
                "  var.bool 'b' = [('n' x int8:*4*) > 'm'];").ok());
  // A slot still reaches through a sum to the value inside it.
  CHECK(inStart("var.mut.int8 'n' = [*5*];\n  set 'n' = ['n' + *1*];").ok());
  CHECK(run("fn.nothing 'f' [int8 'n'] { }\nSTART { f[*1* + *2*]; }\n").ok());

  // Inside a generic the type is the caller's, so the value names the blank and
  // it is filled in with everything else when the copy is written.
  CHECK(run("fn.any.number 'down' [any.number 'n'] {\n"
            "  if 'n' <== any:*0* { give ['n']; }\n"
            "  give [down['n' - any:*1*]]; }\n"
            "START { var.int64 'a' = [*5*]; print.stdout[down['a'] \\n]; }\n").ok());
  // The tip names what is missing rather than talking about quote marks.
  const Checked bare = inStart("var.int8 'n' = [*5*];\n  var.bool 'b' = ['n' > *0*];");
  CHECK(bare.tip().find("a comparison declares nothing") != std::string::npos);
}

void bothStreamsArePrintedTo() {
  CHECK(inStart("print.stdout[str:*a* \\n];").ok());
  CHECK(inStart("print.stderr[str:*a* \\n];").ok());
  // Everything showable goes to either of them, and either takes as many items
  // as are written.
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n"
            "START { var.point 'p' = [*1* *2*];\n"
            "  print.stderr['p' str:* * 'p'.x \\n]; }\n").ok());

  // And a name near one of them is told what the family holds, rather than
  // about quote marks — which is what the tip used to say to everybody who
  // named a function that is not there, whatever they had written.
  const Checked missed = inStart("print.stdlog[str:*a* \\n];");
  CHECK(missed.code(0) == "E0504");
  CHECK(missed.tip().find("`stdout` and `stderr`") != std::string::npos);
  const Checked reading = inStart("var.str 's' = [read.stdlog[]];");
  CHECK(reading.tip().find("`read.stdin`") != std::string::npos);
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
            "  if 'n' > int64:*0* { give [*1*]; } else { give [*0*]; } }\nSTART { }\n").ok());
  // No `else`, so one way out is left silent.
  CHECK(run("fn.int64 'f' [int64 'n'] {\n"
            "  if 'n' > int64:*0* { give [*1*]; } }\nSTART { }\n").code(0) == "E0513");
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
  CHECK(inStart("var.int64 'n' = [*1*];\n    if 'n' > int64:*0* { }").ok());
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
              "    print.stdout['xs'[*8*] \\n];\n}\n") == "E0532");

  // A place that is there, and a divisor that is not zero, are left alone.
  CHECK(built("START {\n    var.many.int64 'xs' = [*10* *20* *30*];\n"
              "    print.stdout['xs'[*3*] \\n];\n}\n") == "");
  CHECK(built("START {\n    var.int64 'n' = [*5* / *2*];\n"
              "    print.stdout['n' \\n];\n}\n") == "");

  // A name holding something written down is that thing, so this is caught too.
  CHECK(built("START {\n    var.many.int64 'xs' = [*10* *20*];\n"
              "    var.int64 'i' = [*9*];\n"
              "    print.stdout['xs'['i'] \\n];\n}\n") == "E0532");

  // Writing one asks the same question reading one does.
  CHECK(built("START {\n    var.mut.many.int64 'xs' = [*1* *2*];\n"
              "    set 'xs'[*6*] = [*9*];\n}\n") == "E0532");
  CHECK(built("START {\n    var.mut.many.int64 'xs' = [*1* *2*];\n"
              "    set 'xs'[*2*] = [*9*];\n}\n") == "");

  // Pieces side by side, all written down, are one written thing.
  CHECK(built("START {\n    var.str 's' = [*a* *b* *c*];\n"
              "    print.stdout['s' \\n];\n}\n") == "");

  // Nothing is claimed about a place that is not known until it runs.
  CHECK(built("fn.nothing 'at' [loan.many.int64 'xs', int64 'i'] {\n"
              "    print.stdout['xs'['i'] \\n];\n}\n"
              "START {\n    var.many.int64 'ns' = [*10* *20*];\n"
              "    at[loan 'ns', *1*];\n}\n") == "");
}

// Counting to the most a counter can hold used to be refused (`E0531`): the
// loop stepped and then asked, so that last step came round and it never
// finished. It asks before it steps now, so these are ordinary loops.
void aLoopMayCountToTheMostItsCounterHolds() {
  CHECK(inStart("loop.range.int8 'i' = [*0*, *127*] { }").ok());
  CHECK(inStart("loop.range.uint8 'i' = [*0*, *255*] { }").ok());
  CHECK(inStart("loop.range.int16 'i' = [*0*, *32767*] { }").ok());
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
  CHECK(boundedIn("var.mut.int8 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + *20*]; }")
        == "E0534");
  // Provably inside it: nothing is said.
  CHECK(boundedIn("var.mut.int8 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *5*] { set 'sum' = ['sum' + *20*]; }")
        == "");
  // Said to be meant, so nothing is said.
  CHECK(boundedIn("var.mut.wrapping.int8 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + *20*]; }")
        == "");

  // The counter's own largest step is what the loop counts to, and the shapes
  // built out of it are bounded too — these are what real programs write, and
  // a warning on every one of them would be worth nothing.
  CHECK(boundedIn("var.mut.int64 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + 'i']; }")
        == "");
  CHECK(boundedIn("var.mut.int64 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + ('i' x *3*)]; }")
        == "");
  CHECK(boundedIn("var.mut.int64 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + ('i' mod *7*)]; }")
        == "");
  CHECK(boundedIn("var.mut.int64 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + ('i' / *7*)]; }")
        == "");
  // A remainder is bounded by what it is taken against, so an `int8` that
  // cannot hold ten of them is still caught.
  CHECK(boundedIn("var.mut.int8 'sum' = [*0*];\n"
               "    loop.range.int64 'i' = [*1*, *10*] { set 'sum' = ['sum' + ('i' mod *100*)]; }")
        == "E0534");

  // A step it cannot follow is a warning, not a refusal: the program builds.
  CHECK(boundedIn("var.mut.int8 'sum' = [*0*];\n    var.int8 'step' = [*3*];\n"
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
                "    var.str 's' = [convert-to-str[loan 't']];").code(0) == "E0535");

  // What holds several things is written the way a print writes it: what it
  // holds, one value after another.
  CHECK(inStart("var.many.int64 'xs' = [*1* *2*];\n"
                "    var.str 's' = [convert-to-str[loan 'xs']];").ok());
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n"
            "START {\n    var.point 'p' = [*1* *2*];\n"
            "    var.str 's' = [convert-to-str[loan 'p']];\n}\n").ok());
  // There is no writing an absence, wherever it sits.
  CHECK(inStart("var.or-nothing.int64 'n' = [*1*];\n"
                "    var.str 's' = [convert-to-str['n']];").code(0) == "E0535");
  CHECK(run("struct 'maybe' [int64 'n', or-nothing.str 's']\n"
            "START {\n    var.maybe 'm' = [*1* *hi*];\n"
            "    var.str 't' = [convert-to-str[loan 'm']];\n}\n")
            .code(0) == "E0535");

  // One value, one answer.
  CHECK(inStart("var.int64 'n' = [*1*];\n"
                "    var.str 's' = [convert-to-str['n', 'n']];").code(0) == "E0505");
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
                "    var.int64 'n' = ['xs'[*1*]];").ok());
  CHECK(inStart("var.many.int64 'xs' = [*1*];\n"
                "    var.str 's' = ['xs'[*1*]];").code(0) == "E0506");

  // A name holding one value is that value, and there is no first of it.
  CHECK(inStart("var.int64 'n' = [*1*];\n"
                "    print.stdout['n'[*1*] \\n];").code(0) == "E0514");
  CHECK(inStart("var.mut.int64 'n' = [*1*];\n"
                "    set 'n'[*1*] = [*2*];").code(0) == "E0514");

  // An index is an `int64`, because that is what `count` answers with.
  CHECK(inStart("var.many.int64 'xs' = [*1*];\n"
                "    print.stdout['xs'[int32:*0*] \\n];").code(0) == "E0506");
}

void countAsksHowManyOfEither() {
  CHECK(inStart("var.many.int64 'xs' = [*1* *2*];\n"
                "    print.stdout[(count[loan 'xs']) \\n];").ok());
  CHECK(inStart("var.str 's' = [*hi*];\n"
                "    print.stdout[(count[loan 's']) \\n];").ok());
  CHECK(inStart("var.int64 'n' = [*1*];\n"
                "    print.stdout[(count['n']) \\n];").code(0) == "E0506");
}

void fillNeedsAValueThatCopies() {
  CHECK(inStart("var.many.int64 'xs' = [fill[*0*, *4*]];").ok());
  CHECK(inStart("var.many.str 'ws' = [fill[*hi*, *4*]];").code(0) == "E0515");
  // Nothing here says what it is filling.
  CHECK(inStart("var.int64 'n' = [fill[*0*, *4*]];").code(0) == "E0507");
}

// A `many` and a struct write what they hold, one value after another with
// nothing between them. `E0516` refused both until 2026-09-10, for a decision
// about separators that turned out not to need making: a print writes its
// pieces side by side, and these are pieces.
// A type that is one of several things, each with a name and a type of its own
// — where a struct is all of its fields at once.
void aOneOfIsOneOfItsCases() {
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [ok:*7*];\n"
            "    when 'a' { is ok 'n' { } is gave-up { } }\n}\n").ok());
  // A choice between one is that one.
  CHECK(run("one-of 'lonely' [int64 'only']\nSTART { }\n").code(0) == "E0527");
  // One that can be itself has no size a machine could give it.
  CHECK(run("one-of 'tree' [int64 'leaf', tree 'branch']\nSTART { }\n")
            .code(0) == "E0526");
  // Every case, once each, and only cases this type has.
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [ok:*7*];\n"
            "    when 'a' { is ok 'n' { } }\n}\n").code(0) == "E0522");
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [ok:*7*];\n"
            "    when 'a' { is ok 'n' { } is ok 'm' { } is gave-up { } }\n}\n")
            .code(0) == "E0521");
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [ok:*7*];\n"
            "    when 'a' { is ok 'n' { } is nope 'm' { } is gave-up { } }\n}\n")
            .code(0) == "E0503");
  // A case holds what its type says it holds, in both directions.
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [gave-up:*7*];\n}\n").code(0) == "E0528");
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [ok];\n}\n").code(0) == "E0528");
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [ok:*7*];\n"
            "    when 'a' { is ok { } is gave-up { } }\n}\n").code(0) == "E0528");
  // A word that names no case is the mistake it always was.
  CHECK(inStart("var.int64 'n' = [nope];").code(0) == "E0107");
  // What it is going into says which `one-of` a case belongs to.
  CHECK(run("one-of 'a' [int64 'ok', nothing 'no']\n"
            "one-of 'b' [bool 'ok', nothing 'no']\n"
            "START {\n    print.stdout[ok:*7* \\n];\n}\n").code(0) == "E0503");
  // Showing one would not say which of the things it is, and neither would
  // writing it out or comparing two.
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [ok:*7*];\n"
            "    print.stdout['a' \\n];\n}\n").code(0) == "E0536");
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [ok:*7*];\n"
            "    var.str 's' = [convert-to-str[loan 'a']];\n}\n").code(0) == "E0535");
  CHECK(run("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
            "START {\n    var.answer 'a' = [ok:*7*];\n"
            "    var.answer 'b' = [ok:*7*];\n"
            "    if 'a' == 'b' { }\n}\n").code(0) == "E0506");
  // A case holding something with an owner: what it holds goes when the value
  // does, and which case is live is read where it ends.
  CHECK(run("struct 'pair' [str 'a', int64 'b']\n"
            "one-of 'thing' [str 'text', many.str 'words', pair 'both',\n"
            "                int64 'n', nothing 'no']\n"
            "START {\n    var.thing 't' = [both:pair[*p* *9*]];\n"
            "    when 't' {\n"
            "        is text 's'  { }\n"
            "        is words 'w' { }\n"
            "        is both 'p'  { print.stdout['p'.b \\n]; }\n"
            "        is n 'x'     { }\n"
            "        is no        { } }\n}\n").ok());
}

void showingSeveralThingsWritesEachOfThem() {
  CHECK(inStart("var.many.int64 'xs' = [*1*];\n"
                "    print.stdout['xs' \\n];").ok());
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n"
              "START {\n"
              "    var.point 'p' = [*1* *2*];\n"
              "    print.stdout['p' \\n];\n}\n")
            .ok());
  // A field at a time is still a field at a time.
  CHECK(run("struct 'point' [int64 'x', int64 'y']\n"
              "START {\n"
              "    var.point 'p' = [*1* *2*];\n"
              "    print.stdout['p'.x \\n];\n}\n")
            .ok());
  // An absence anywhere inside is still refused, and says where it is.
  CHECK(run("struct 'maybe' [int64 'n', or-nothing.str 's']\n"
              "START {\n"
              "    var.maybe 'm' = [*1* *hi*];\n"
              "    print.stdout['m' \\n];\n}\n")
            .code(0) == "E0536");
  CHECK(run("struct 'maybe' [int64 'n', or-nothing.str 's']\n"
              "struct 'holder' [many.maybe 'ms']\n"
              "START {\n"
              "    var.maybe 'm' = [*1* *hi*];\n"
              "    var.holder 'h' = [[move 'm']];\n"
              "    print.stdout['h' \\n];\n}\n")
            .code(0) == "E0536");
}

void showingAMaybeIsRefused() {
  // Nothing reaches inside without asking, and a print was reaching.
  CHECK(inStart("var.or-nothing.int64 'n' = [nothing];\n"
                "    print.stdout['n' \\n];").code(0) == "E0536");
  CHECK(inStart("var.or-nothing.int64 'n' = [*5*];\n"
                "    print.stdout['n' \\n];").code(0) == "E0536");
  // Asking first is what makes it showable.
  CHECK(inStart("var.or-nothing.int64 'n' = [*5*];\n"
                "    if 'n' holds 'v' { print.stdout['v' \\n]; }").ok());
}

void aManyTravelsWhole() {
  // A lone item that is already the array is the array; anything else is one
  // of its places.
  CHECK(run("fn.many.int64 'f' [] {\n"
            "    var.many.int64 'xs' = [*1* *2*];\n"
            "    give ['xs'];\n}\n").ok());
  CHECK(run("fn.int64 'g' [loan.many.int64 'xs'] { give ['xs'[*1*]]; }\n").ok());
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
            "    print.stdout['ts'[*2*].name \\n];\n}\n").ok());
  CHECK(run("struct 'tag' [str 'name']\nstruct 'point' [int64 'x', int64 'y']\n"
            "START {\n    var.point 'a' = [*1* *2*];\n    var.point 'b' = [*3* *4*];\n"
            "    var.many.point 'ps' = [move 'a' move 'b'];\n"
            "    print.stdout['ps'[*1*].y \\n];\n}\n").ok());
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
            "    print.stdout['ps'[*2*].x \\n];\n}\n").ok());

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
            "fn.point 'middle' [loan.many.point 'ps'] {\n"
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

// `no-itmt` asks for something and `UNSAFE` grants it, and neither alone does
// anything. The word is in capitals so that looking for it finds every place a
// check was turned off, and one that worked without it would be a check turned
// off where nothing says so.
void askingToSkipNeedsSayingSo() {
  CHECK(run("START {\n"
            "    UNSAFE {\n"
            "        loop.no-itmt.range.int64 'i' = [*1*, *3*] { }\n"
            "    }\n}\n")
            .ok());
  CHECK(inStart("loop.no-itmt.range.int64 'i' = [*1*, *3*] { }").code(0) == "E0212");
  // A `while` may ask too, and is the one that most needs to: its ends are not
  // written down, so a run of it may never finish.
  CHECK(run("START {\n"
            "    var.mut.int64 'n' = [*0*];\n"
            "    UNSAFE {\n"
            "        loop.no-itmt.while 'n' < int64:*3* { set 'n' = ['n' + *1*]; }\n"
            "    }\n}\n")
            .ok());
  // The chain is the parser's to judge, so these are asked of it rather than of
  // the checker. It comes before the counter, because it is about the loop and
  // `perm` is about the name the loop declares.
  const auto readAs = [](const std::string &text) {
    const Checked c = run(text);
    return c.parsed.diagnostics.empty() ? std::string("(none)")
                                        : c.parsed.diagnostics.front().code;
  };
  CHECK(readAs("START {\n"
               "    UNSAFE {\n"
               "        loop.perm.no-itmt.range.int64 'i' = [*1*, *3*] { }\n"
               "    }\n}\n") == "E0205");
  // And there is no word for the ordinary case, because not writing one is it.
  CHECK(readAs("START {\n"
               "    loop.itmt.range.int64 'i' = [*1*, *3*] { }\n}\n") == "E0202");
}

// `mut` asks for something. A name nothing ever changes did not need it, and a
// chain says what is unusual.
void aMutThatWasNotNeededIsSaidSo() {
  CHECK(saidIn("var.mut.int64 'n' = [*3*];") == "W0003");
  // Changed by any of the three ways a name changes, and nothing is said.
  CHECK(saidIn("var.mut.int64 'n' = [*3*];\n    set 'n' = [*4*];") == "");
  CHECK(saidIn("var.mut.many.int64 'xs' = [*1* *2*];\n"
               "    set 'xs'[*1*] = [*9*];") == "");
  CHECK(saidIn("var.mut.int64 'n' = [*3*];\n"
               "    var.loanmut.int64 'p' = [loanmut 'n'];\n"
               "    set 'p' = [*4*];") == "");
  // Without the word there is nothing to say.
  CHECK(saidIn("var.int64 'n' = [*3*];") == "");
}

// A generic is written once and built once per type it is called with, so no
// pass reads its body while the blank is still in it. Almost nothing in one
// would hold: whether a thing copies, how wide it is, and what an instruction
// on it means are all exactly what the blank has not said yet.
void aGenericBodyIsNotReadWithTheBlankInIt() {
  const std::string generic =
      "fn.any 'largest' [loan.many.any 'xs'] {\n"
      "    var.mut.any 'best' = ['xs'[*1*]];\n"
      "    loop.range.int64 'i' = [*1*, *2*] {\n"
      "        if 'xs'['i'] > 'best' { set 'best' = ['xs'['i']]; }\n"
      "    }\n"
      "    give ['best'];\n"
      "}\n"
      "START { }\n";
  // Read with the blank in it, reaching into a `many.any` looks like taking a
  // value that does not copy out of a place that has to hold one — `E0412` —
  // when for a `many.int64` it is a copy and nothing moves.
  CHECK(run(generic).ok());

  // A `fn` with no blank is read as it always was.
  CHECK(run("fn.int64 'twice' [int64 'n'] { give ['n' x *2*]; }\nSTART { }\n").ok());
  CHECK(run("fn.int64 'twice' [int64 'n'] { }\nSTART { }\n").code(0) == "E0513");
}

// Written once, built once per type it is called with, and no blank left in
// anything that follows.
void aGenericIsWrittenOutPerType() {
  const std::string program =
      "fn.any 'same' [any 'x'] { give ['x']; }\n"
      "START {\n"
      "    var.int64 'n' = [*3*];\n"
      "    var.bool 'b' = [*true*];\n"
      "    print.stdout[same['n'] same['b'] \\n];\n"
      "}\n";
  const xag::Source source("test.xag", xag::asFile(program));
  const xag::LexResult lexed = xag::lex(source);
  xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  CHECK(parsed.ok());
  const xag::CheckResult checked = xag::check(source, parsed.program);
  CHECK(checked.ok());
  CHECK(checked.instantiations.size() == 2);

  xag::Program expanded;
  CHECK(xag::expand(parsed.program, checked, expanded));

  // The generic itself is gone, and one copy stands for each type.
  bool generic = false, forInt = false, forBool = false;
  for (const xag::Item &item : expanded.items) {
    generic = generic || item.name == "same";
    forInt = forInt || item.name == "same$int64";
    forBool = forBool || item.name == "same$bool";
  }
  CHECK(!generic);
  CHECK(forInt);
  CHECK(forBool);

  // And the copies read clean, which the generic never could have.
  CHECK(xag::check(source, expanded).ok());

  // Called twice at one type is one copy, not two.
  const std::string twice =
      "fn.any 'same' [any 'x'] { give ['x']; }\n"
      "START {\n"
      "    var.int64 'n' = [*3*];\n"
      "    print.stdout[same['n'] same['n'] \\n];\n"
      "}\n";
  const xag::Source other("test.xag", xag::asFile(twice));
  const xag::LexResult lexed2 = xag::lex(other);
  xag::ParseResult parsed2 = xag::parse(other, lexed2.tokens);
  const xag::CheckResult checked2 = xag::check(other, parsed2.program);
  CHECK(checked2.instantiations.size() == 1);

  // A generic nothing calls is written out not at all, rather than written out
  // with a blank still in it.
  const std::string unused =
      "fn.any 'same' [any 'x'] { give ['x']; }\nSTART { }\n";
  const xag::Source third("test.xag", xag::asFile(unused));
  const xag::LexResult lexed3 = xag::lex(third);
  xag::ParseResult parsed3 = xag::parse(third, lexed3.tokens);
  const xag::CheckResult checked3 = xag::check(third, parsed3.program);
  xag::Program none;
  CHECK(xag::expand(parsed3.program, checked3, none));
  // What is left is the two blocks that run — `START` and `ITMT` — and nothing
  // of the generic.
  CHECK(none.items.size() == 2);
  CHECK(none.items.front().kind == xag::ItemKind::Start);
  CHECK(none.items.back().kind == xag::ItemKind::Itmt);
}

// One generic calling another, and one calling itself. Neither call is read
// until the body holding it has been written out at a type, so this takes more
// than one round.
void aGenericCallingAGenericIsWrittenOutToo() {
  const std::string nested =
      "fn.any 'same' [any 'x'] { give ['x']; }\n"
      "fn.any 'twice' [any 'y'] { give [same['y']]; }\n"
      "START {\n"
      "    var.int64 'n' = [*7*];\n"
      "    var.bool 'b' = [*true*];\n"
      "    print.stdout[twice['n'] twice['b'] \\n];\n"
      "}\n";
  const xag::Source source("test.xag", xag::asFile(nested));
  const xag::LexResult lexed = xag::lex(source);
  xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  CHECK(parsed.ok());

  // Round by round, the way the driver does it, until no generic is left.
  std::deque<xag::Program> rounds;
  xag::CheckResult checked = xag::check(source, parsed.program);
  CHECK(checked.ok());
  const xag::Program *program = &parsed.program;
  while (rounds.size() < 64) {
    xag::Program &next = rounds.emplace_back();
    if (!xag::expand(const_cast<xag::Program &>(*program), checked, next)) {
      rounds.pop_back();
      break;
    }
    program = &next;
    checked = xag::check(source, next);
    CHECK(checked.ok());
  }
  // It settles rather than going round for ever.
  CHECK(rounds.size() < 64);

  // Four copies, one generic left, and nothing read a blank on the way.
  std::set<std::string> names;
  for (const xag::Item &item : program->items)
    names.insert(item.name);
  CHECK(names.count("same$int64") == 1);
  CHECK(names.count("same$bool") == 1);
  CHECK(names.count("twice$int64") == 1);
  CHECK(names.count("twice$bool") == 1);
  CHECK(names.count("same") == 0);
  CHECK(names.count("twice") == 0);

  // A generic that calls itself asks for its own type again on the round that
  // reads its body. That copy is the one asking, and it is not written twice.
  const std::string itself =
      "fn.any 'down' [any 'n'] {\n"
      "    if 'n' <== any:*0* { give [*0*]; }\n"
      "    give [down['n' - any:*1*]];\n"
      "}\n"
      "START {\n"
      "    var.int64 'a' = [*5*];\n"
      "    print.stdout[down['a'] \\n];\n"
      "}\n";
  const xag::Source other("test.xag", xag::asFile(itself));
  const xag::LexResult lexed2 = xag::lex(other);
  xag::ParseResult parsed2 = xag::parse(other, lexed2.tokens);
  CHECK(parsed2.ok());

  std::deque<xag::Program> again;
  xag::CheckResult read = xag::check(other, parsed2.program);
  CHECK(read.ok());
  const xag::Program *settled = &parsed2.program;
  while (again.size() < 64) {
    xag::Program &next = again.emplace_back();
    if (!xag::expand(const_cast<xag::Program &>(*settled), read, next)) {
      again.pop_back();
      break;
    }
    settled = &next;
    read = xag::check(other, next);
    CHECK(read.ok());
  }
  CHECK(again.size() < 64);

  unsigned copies = 0;
  for (const xag::Item &item : settled->items)
    if (item.name == "down$int64")
      ++copies;
  CHECK(copies == 1);
}

// One typo, and everything it broke shown underneath it rather than as
// refusals of its own — and the ones that used to be passed over in silence
// brought back.
void oneMistakeIsReportedAsOneMistake() {
  const Checked c = run(
      "fn.int64 'double' [int64 'v'] { give ['v' + 'v']; }\n"
      "START {\n"
      "    var.in64 'n' = [*3*];\n"
      "    var.str 's' = ['n'];\n"
      "    var.int64 'a' = ['n' + *1*];\n"
      "    var.int64 'c' = [double['n']];\n"
      "    var.bool 'ok' = ['n'];\n"
      "}\n");
  CHECK(!c.checked.ok());

  // The root is the type word, and every other refusal knows it came from
  // there. Three of these five used to be said in total silence.
  const std::vector<xag::Diagnostic> &raw = c.checked.diagnostics;
  CHECK(raw.size() == 7);
  unsigned roots = 0, followOns = 0;
  xag::Span rootSpan{};
  for (const xag::Diagnostic &one : raw) {
    if (one.follows.begin == 0 && one.follows.end == 0) {
      ++roots;
      rootSpan = one.span;
      CHECK(one.code == "E0503");
    } else {
      ++followOns;
    }
  }
  CHECK(roots == 1);
  // Six, not five: the sum is refused twice, once by the operation and once by
  // the declaration it was written into. They point at the same place, and
  // folding shows that place once.
  CHECK(followOns == 6);
  // Every one of them names the mistake that started it, not the last link.
  for (const xag::Diagnostic &one : raw)
    if (one.follows.begin != 0 || one.follows.end != 0)
      CHECK(one.follows.begin == rootSpan.begin);

  // And what the reader is shown is one error with five places under it.
  const std::vector<xag::Diagnostic> folded = xag::foldFollowOns(raw);
  CHECK(folded.size() == 1);
  CHECK(folded.front().code == "E0503");
  CHECK(folded.front().notes.size() == 5);
  for (const xag::Note &note : folded.front().notes)
    CHECK(note.label.rfind("and because of that,", 0) == 0);

  // A mistake that started by itself is left exactly as it was.
  const Checked alone = run("START {\n    print.stdout[*1000* \\n];\n}\n");
  CHECK(alone.code(0) == "E0507");
  CHECK(alone.checked.diagnostics.front().follows.begin == 0);
  CHECK(xag::foldFollowOns(alone.checked.diagnostics).size() == 1);
  // With its tip intact: it is the tip that is wrong on a follow-on, not here.
  CHECK(!alone.checked.diagnostics.front().tips.empty());

  // Every place a check has to be skipped says so, rather than passing over it.
  // Ten shapes, all of which used to be silent, all folding under one typo.
  const Checked wide = run(
      "struct 'point' [int64 'x', int64 'y']\n"
      "START {\n"
      "    var.in64 'n' = [*3*];\n"
      "    var.int64 'a' = [count['n']];\n"
      "    var.str 'b' = [convert-to-str['n']];\n"
      "    var.int64 'c' = ['n'.x];\n"
      "    var.many.int64 'd' = [*1* 'n' *3*];\n"
      "    var.point 'e' = ['n' *2*];\n"
      "    var.str 'f' = [str:*x = * 'n'];\n"
      "    if 'n' { }\n"
      "    loop.range.int64 'i' = [*0*, 'n'] { }\n"
      "    var.bool 'g' = [not 'n'];\n"
      "}\n");
  const std::vector<xag::Diagnostic> wideFolded = xag::foldFollowOns(wide.checked.diagnostics);
  CHECK(wideFolded.size() == 1);
  CHECK(wideFolded.front().code == "E0503");
  CHECK(wideFolded.front().notes.size() == 10);

  // A root that is not a type word works the same way: an undeclared name makes
  // the sum built from it unreadable, and the sum says so under the name.
  const Checked undeclared = run("START {\n    var.int64 'm' = ['q' + *1*];\n}\n");
  const std::vector<xag::Diagnostic> under = xag::foldFollowOns(undeclared.checked.diagnostics);
  CHECK(under.size() == 1);
  CHECK(under.front().code == "E0501");
  CHECK(under.front().notes.size() == 1);
  // Underlined where the sum is, which is wider than the name it starts at —
  // folding by where a span begins alone used to throw this one away.
  CHECK(under.front().notes[0].span.begin == under.front().span.begin);
  CHECK(under.front().notes[0].span.end > under.front().span.end);

  // A follow-on whose root nobody printed is kept rather than lost.
  std::vector<xag::Diagnostic> orphan;
  orphan.push_back(xag::Diagnostic{xag::Span{40, 44}, "E0506", "this was not checked."});
  orphan.back().follows = xag::Span{9, 13};
  CHECK(xag::foldFollowOns(orphan).size() == 1);
}

// A blank may say what it will take, and the refusal then lands on the call
// rather than inside a body the caller never wrote.
void aBlankMaySayWhatItTakes() {
  const std::string program =
      "struct 'point' [int64 'x', int64 'y']\n"
      "fn.any.number 'twice' [any.number 'x'] { give ['x' + 'x']; }\n"
      "START {\n"
      "    var.int64 'n' = [*7*];\n"
      "    var.point 'p' = [*1* *2*];\n"
      "    print.stdout[twice['p'] \\n];\n"
      "}\n";
  const Checked c = run(program);
  CHECK(!c.checked.ok());
  CHECK(c.code(0) == "E0539");
  CHECK(c.checked.diagnostics.front().message ==
        "`point` is not a number, and `twice` asks for one.");
  // On the caller's own line — the print — and not inside `twice`. Found by
  // looking rather than counted, so that what a file has to be wrapped in
  // cannot make this say the wrong thing.
  const std::string_view whole = c.source.text();
  const std::string_view::size_type calls = whole.find("twice['p']");
  CHECK(calls != std::string_view::npos);
  CHECK(c.source.positionOf(c.checked.diagnostics.front().span.begin).line ==
        c.source.positionOf(static_cast<unsigned>(calls)).line);

  // The same generic at a type it does take is an ordinary call, and the word
  // saying what it takes does not survive into the copy: filling `any.number`
  // in at `int64` is `int64`, not `int64.number`.
  const std::string fine =
      "fn.any.number 'twice' [any.number 'x'] { give ['x' + 'x']; }\n"
      "START {\n"
      "    var.int64 'n' = [*7*];\n"
      "    print.stdout[twice['n'] \\n];\n"
      "}\n";
  const xag::Source source("test.xag", xag::asFile(fine));
  const xag::LexResult lexed = xag::lex(source);
  xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  CHECK(checked.ok());
  xag::Program expanded;
  CHECK(xag::expand(parsed.program, checked, expanded));
  CHECK(xag::check(source, expanded).ok());
  for (const xag::Item &item : expanded.items)
    if (item.name == "twice$int64")
      for (const xag::ChainSegment &seg : item.chain.segments)
        CHECK(seg.isName || seg.text != "number");

  // A `many` of a `many`. What a blank was filled in with is written out as the
  // copy's own type, and that was written with a single `many` however many
  // there were — so a generic handed a `many many str` was written out as
  // taking a `many str`, and then refused the very call that had asked for it.
  const std::string nestedMany =
      "fn.int64 'echo' [loan.any 'v'] { give [*0*]; }\n"
      "START {\n"
      "    var.many.many.str 'g' = [[*a* *b*] [*c*]];\n"
      "    var.many.str 'f' = [*x*];\n"
      "    print.stdout[echo[loan 'g'] echo[loan 'f'] \\n];\n"
      "}\n";
  const Checked deep = run(nestedMany);
  CHECK(deep.checked.ok());
  std::set<std::string> asked;
  for (const auto &[what, with] : deep.checked.instantiations)
    asked.insert(with);
  CHECK(asked == std::set<std::string>({"loan.many.many.str", "loan.many.str"}));

  // And one filled in with a `many` that grows. The same fault as the nested
  // one above and for the same reason: what a blank was filled in with is
  // written out as the copy's own type, and that said plain `many` whatever it
  // was — so a generic handed a `many-growing` was written out as taking a
  // `many`, and refused the call that had asked for it.
  const Checked growing = run(
      "fn.int64 'echo' [loan.any 'v'] { give [*0*]; }\n"
      "START {\n"
      "    var.mut.many-growing.int64 'g' = [];\n"
      "    add 'g' = [*1*];\n"
      "    var.many.int64 'm' = [*1* *2*];\n"
      "    print.stdout[echo[loan 'g'] echo[loan 'm'] \\n];\n"
      "}\n");
  CHECK(growing.checked.ok());
  std::set<std::string> grown;
  for (const auto &[what, with] : growing.checked.instantiations)
    grown.insert(with);
  CHECK(grown == std::set<std::string>({"loan.many-growing.int64", "loan.many.int64"}));

  // A blank filled in with a type that is more than one word. Filling wrote one
  // segment, which is every type a scalar and no type else: a `many` came out
  // spelled `unknown`, and an `or-nothing` came out spelled as the thing inside
  // it, so the copy took a plain `int64` and refused the call that asked for it.
  const std::string wider =
      "fn.int64 'echo' [any 'v'] { give [*0*]; }\n"
      "START {\n"
      "    var.many.int64 'a' = [*1* *2*];\n"
      "    var.or-nothing.str 'b' = [nothing];\n"
      "    print.stdout[echo[move 'a'] echo[move 'b'] \\n];\n"
      "}\n";
  const xag::Source held("test.xag", xag::asFile(wider));
  const xag::LexResult heldLex = xag::lex(held);
  xag::ParseResult heldParsed = xag::parse(held, heldLex.tokens);
  const xag::CheckResult heldChecked = xag::check(held, heldParsed.program);
  CHECK(heldChecked.ok());
  CHECK(heldChecked.instantiations.size() == 2);
  CHECK(heldChecked.instantiations[0].second == "many.int64");
  CHECK(heldChecked.instantiations[1].second == "or-nothing.str");

  xag::Program wide;
  CHECK(xag::expand(heldParsed.program, heldChecked, wide));
  // The copies read clean, which is the whole test: a chain spelled `unknown`
  // does not, and neither does one that lost its `or-nothing`.
  CHECK(xag::check(held, wide).ok());
  bool asMany = false, asMaybe = false;
  for (const xag::Item &item : wide.items) {
    asMany = asMany || item.name == "echo$many.int64";
    asMaybe = asMaybe || item.name == "echo$or-nothing.str";
    if (item.name == "echo$many.int64") {
      // Two segments where the blank stood, not one.
      CHECK(item.params.size() == 1);
      const std::vector<xag::ChainSegment> &segs = item.params[0].chain.segments;
      CHECK(segs.size() == 2);
      CHECK(segs[0].text == "many");
      CHECK(segs[1].text == "int64");
    }
  }
  CHECK(asMany);
  CHECK(asMaybe);

  // Every family word, against a type inside it and a type outside it.
  CHECK(xag::inFamily(xag::Ty{xag::Type::Int64}, xag::Family::Number));
  CHECK(!xag::inFamily(xag::Ty{xag::Type::Str}, xag::Family::Number));
  CHECK(xag::inFamily(xag::Ty{xag::Type::Uint8}, xag::Family::Uint));
  CHECK(!xag::inFamily(xag::Ty{xag::Type::Int8}, xag::Family::Uint));
  CHECK(xag::inFamily(xag::Ty{xag::Type::Int8}, xag::Family::Int));
  CHECK(xag::inFamily(xag::Ty{xag::Type::Deci64}, xag::Family::Deci));
  CHECK(xag::inFamily(xag::Ty{xag::Type::Bin64}, xag::Family::Bin));
  CHECK(xag::inFamily(xag::many(xag::Type::Int64), xag::Family::Many));
  CHECK(!xag::inFamily(xag::many(xag::Type::Int64), xag::Family::Number));
  CHECK(xag::inFamily(xag::orNothingOf(xag::Ty{xag::Type::Int64}),
                      xag::Family::OrNothing));
  // A number that may be missing is not a number: arithmetic on one has no
  // answer when it holds none, which is the whole reason the type exists.
  CHECK(!xag::inFamily(xag::orNothingOf(xag::Ty{xag::Type::Int64}),
                       xag::Family::Number));
  CHECK(xag::inFamily(xag::structNamed(0), xag::Family::Struct));
  // Bare `any` still takes everything.
  CHECK(xag::inFamily(xag::structNamed(0), xag::Family::Anything));
  CHECK(xag::inFamily(xag::Ty{xag::Type::Str}, xag::Family::Anything));
}

// The first code a generic's own body is refused with. A generic is not read
// while the blank is still in it, so anything inside one is only ever said about
// the copies — which means these are found by the second reading, the way the
// driver finds them.
std::string insideTheCopies(const std::string &text) {
  const xag::Source source("test.xag", xag::asFile(text));
  const xag::LexResult lexed = xag::lex(source);
  xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  if (!parsed.ok())
    return "(not parsed)";
  const xag::CheckResult first = xag::check(source, parsed.program);
  if (!first.ok())
    return first.diagnostics.front().code;
  xag::Program expanded;
  if (!xag::expand(parsed.program, first, expanded))
    return "(nothing to expand)";
  const xag::CheckResult second = xag::check(source, expanded);
  return second.diagnostics.empty() ? "(none)" : second.diagnostics.front().code;
}

// Choosing an arm while compiling, and carrying only that arm into the program.
void whicheverKeepsOneArm() {
  const std::string program =
      "fn.str 'show' [any 'v'] {\n"
      "    whichever 'v' {\n"
      "        is number { give [convert-to-str['v']]; }\n"
      "        is str    { give ['v']; }\n"
      "        is bool   { give [str:*a bool*]; }\n"
      "    }\n"
      "}\n"
      "START {\n"
      "    var.int64 'n' = [*42*];\n"
      "    var.bool 'b' = [*true*];\n"
      "    print.stdout[show['n'] show['b'] \\n];\n"
      "}\n";
  const xag::Source source("test.xag", xag::asFile(program));
  const xag::LexResult lexed = xag::lex(source);
  xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  CHECK(parsed.ok());
  const xag::CheckResult checked = xag::check(source, parsed.program);
  CHECK(checked.ok());

  xag::Program expanded;
  CHECK(xag::expand(parsed.program, checked, expanded));
  const xag::CheckResult again = xag::check(source, expanded);
  CHECK(again.ok());
  // One `whichever` in each copy, and each chose a different arm: `int64` took
  // the first, `bool` the third.
  CHECK(again.chosenArm.size() == 2);
  std::set<unsigned> chosen;
  for (const auto &[where, arm] : again.chosenArm)
    chosen.insert(arm);
  CHECK(chosen == std::set<unsigned>({0u, 2u}));

  // And after pruning the word is gone, with one arm's statements standing
  // where the whole statement stood.
  CHECK(xag::prune(expanded, again) == 2);
  for (const xag::Item &item : expanded.items)
    for (const xag::StmtPtr &s : item.body.stmts)
      CHECK(s->kind != xag::StmtKind::Whichever);
  for (const xag::Item &item : expanded.items)
    if (item.name == "show$bool") {
      CHECK(item.body.stmts.size() == 1);
      CHECK(item.body.stmts[0]->kind == xag::StmtKind::Give);
    }

  // Two arms that could both answer leave the compiler picking, and there is no
  // rule here saying one word is nearer than another.
  CHECK(insideTheCopies("fn.str 'f' [any 'v'] {\n"
            "    whichever 'v' { is number { give [str:*n*]; } is int { give [str:*i*]; } }\n"
            "}\n"
            "START { var.int64 'n' = [*1*]; print.stdout[f['n'] \\n]; }\n") == "E0541");
  CHECK(insideTheCopies("fn.str 'f' [any 'v'] {\n"
            "    whichever 'v' { is str { give [str:*a*]; } is str { give [str:*b*]; } }\n"
            "}\n"
            "START { var.str 's' = [*x*]; print.stdout[f[move 's'] \\n]; }\n") == "E0541");

  // Which arm this is turned out to be is settled while compiling, so an
  // uncovered one is this program, now, with nothing to do.
  CHECK(insideTheCopies("fn.str 'f' [any 'v'] {\n"
            "    whichever 'v' { is number { give [str:*n*]; } }\n"
            "}\n"
            "START { var.bool 'b' = [*true*]; print.stdout[f['b'] \\n]; }\n") == "E0542");

  CHECK(insideTheCopies("fn.str 'f' [any 'v'] {\n"
            "    whichever 'v' { is banana { give [str:*n*]; } }\n"
            "}\n"
            "START { var.int64 'n' = [*1*]; print.stdout[f['n'] \\n]; }\n") == "E0540");

  // A function answering from every arm answers: asking every arm rather than
  // the one that is here would refuse the shape everybody writes.
  CHECK(run(program).checked.ok());
}

// How a thing is held is a second question, asked with its own words, and a
// value that is borrowed answers one word from each list.
void howAThingIsHeldIsItsOwnQuestion() {
  const std::string program =
      "fn.str 'howHeld' [any 'v'] {\n"
      "    whichever 'v' {\n"
      "        is owned   { give [str:*owned*]; }\n"
      "        is loan    { give [str:*a loan*]; }\n"
      "        is loanmut { give [str:*a loanmut*]; }\n"
      "    }\n"
      "}\n"
      "START {\n"
      "    var.mut.int64 'n' = [*7*];\n"
      "    print.stdout[howHeld['n'] howHeld[loan 'n'] howHeld[loanmut 'n'] \\n];\n"
      "}\n";
  const xag::Source source("test.xag", xag::asFile(program));
  const xag::LexResult lexed = xag::lex(source);
  xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  CHECK(parsed.ok());
  const xag::CheckResult checked = xag::check(source, parsed.program);
  CHECK(checked.ok());
  // Three copies for one type, because how it is held is part of what the copy
  // takes — and a program that asks gets a different answer in each.
  CHECK(checked.instantiations.size() == 3);
  std::set<std::string> spelled;
  for (const auto &[what, with] : checked.instantiations)
    spelled.insert(with);
  CHECK(spelled == std::set<std::string>({"int64", "loan.int64", "loanmut.int64"}));

  // `move` is the third word beside `loan` and `loanmut` and is not a borrow:
  // what comes out of it is held outright, so it asks for no copy of its own.
  const Checked moved = run(
      "fn.int64 'echo' [any 'v'] { give [*0*]; }\n"
      "START {\n"
      "    var.str 's' = [*x*];\n"
      "    print.stdout[echo[move 's'] \\n];\n"
      "}\n");
  CHECK(moved.checked.ok());
  CHECK(moved.checked.instantiations.size() == 1);
  CHECK(moved.checked.instantiations[0].second == "str");

  // A borrowed number answers a word from each list, so one `whichever` may not
  // ask both — there is no level to pick between them.
  CHECK(insideTheCopies(
            "fn.str 'f' [any 'v'] {\n"
            "    whichever 'v' { is number { give [str:*n*]; } is loan { give [str:*l*]; } }\n"
            "}\n"
            "START { var.int64 'n' = [*1*]; print.stdout[f['n'] \\n]; }\n") == "E0543");
  // Within one list nothing overlaps: a value is held exactly one way.
  CHECK(insideTheCopies(
            "fn.str 'f' [any 'v'] {\n"
            "    whichever 'v' {\n"
            "        is owned { give [str:*o*]; } is loan { give [str:*l*]; }\n"
            "        is loanmut { give [str:*m*]; }\n"
            "    }\n"
            "}\n"
            "START { var.int64 'n' = [*1*]; print.stdout[f['n'] \\n]; }\n") == "(none)");

  // And the other direction: a blank may ask for a borrow, and the refusal
  // lands on the call.
  const Checked wanted = run("fn.int64 'f' [any.loan 'v'] { give [*1*]; }\n"
                             "START {\n"
                             "    var.int64 'n' = [*7*];\n"
                             "    print.stdout[f['n'] \\n];\n"
                             "}\n");
  CHECK(wanted.code(0) == "E0539");
  CHECK(wanted.checked.diagnostics.front().message ==
        "`int64` is not a borrow, and `f` asks for one.");
  CHECK(run("fn.int64 'f' [any.loan 'v'] { give [*1*]; }\n"
            "START {\n"
            "    var.int64 'n' = [*7*];\n"
            "    print.stdout[f[loan 'n'] \\n];\n"
            "}\n")
            .checked.ok());

  // How a thing is held belongs to the chain, and a chain that already says it
  // does not say it twice. `loan.any` filled in with a borrowed `int64` is
  // `loan.int64`, and the copy takes exactly what a hand-written one would.
  const std::string lent =
      "fn.int64 'f' [loan.any 'v'] { give [*0*]; }\n"
      "fn.int64 'g' [loanmut.any 'v'] { give [*0*]; }\n"
      "START {\n"
      "    var.mut.int64 'n' = [*1*];\n"
      "    print.stdout[f[loan 'n'] g[loanmut 'n'] \\n];\n"
      "}\n";
  const xag::Source held2("test.xag", xag::asFile(lent));
  const xag::LexResult lentLex = xag::lex(held2);
  xag::ParseResult lentParsed = xag::parse(held2, lentLex.tokens);
  const xag::CheckResult lentChecked = xag::check(held2, lentParsed.program);
  CHECK(lentChecked.ok());
  xag::Program lentOut;
  CHECK(xag::expand(lentParsed.program, lentChecked, lentOut));
  CHECK(xag::check(held2, lentOut).ok());
  for (const xag::Item &item : lentOut.items)
    if (item.name == "f$loan.int64" || item.name == "g$loanmut.int64") {
      unsigned saidHowHeld = 0;
      for (const xag::ChainSegment &seg : item.params[0].chain.segments)
        if (!seg.isName && (seg.text == "loan" || seg.text == "loanmut"))
          ++saidHowHeld;
      CHECK(saidHowHeld == 1);
    }

  // A caller lending for writing where the chain asks to read is one copy, not
  // two, and is refused by the same rule that refuses it for any function.
  const Checked tighter = run(
      "fn.int64 'f' [loan.any 'v'] { give [*0*]; }\n"
      "START {\n"
      "    var.mut.int64 'n' = [*1*];\n"
      "    print.stdout[f[loanmut 'n'] \\n];\n"
      "}\n");
  CHECK(tighter.checked.instantiations.size() == 1);
  CHECK(tighter.checked.instantiations[0].second == "loan.int64");

  // In `loan.many.any` the borrow is of the `many`, and each place inside it is
  // held no way at all — saying otherwise filled `var.mut.any 'best'` in with
  // `loan.int64`, so the body declared a borrow and then wrote through it.
  const Checked inside = run(
      "fn.any 'largest' [loan.many.any 'xs'] {\n"
      "    var.mut.any 'best' = ['xs'[*1*]];\n"
      "    give ['best'];\n"
      "}\n"
      "START {\n"
      "    var.many.int64 'ints' = [*3* *9* *4*];\n"
      "    print.stdout[(largest[loan 'ints']) \\n];\n"
      "}\n");
  CHECK(inside.checked.ok());
  CHECK(inside.checked.instantiations.size() == 1);
  CHECK(inside.checked.instantiations[0].second == "int64");

  CHECK(xag::axisOf(xag::Family::Loan) == xag::Axis::How);
  CHECK(xag::axisOf(xag::Family::Owned) == xag::Axis::How);
  CHECK(xag::axisOf(xag::Family::Number) == xag::Axis::What);
  // Nothing overlaps across the two questions — that pair is refused where it
  // is written instead, which is where the reader can see both words.
  CHECK(!xag::overlaps(xag::Family::Number, xag::Family::Loan));
  CHECK(!xag::overlaps(xag::Family::Owned, xag::Family::Loan));
  CHECK(xag::overlaps(xag::Family::Loan, xag::Family::Loan));
}

// Walking a struct: one copy of the body per field, written out while
// compiling, because what a field holds is a different type on every turn.
void loopPartsWritesOneCopyPerField() {
  const std::string program =
      "struct 'point' [int64 'x', int64 'y']\n"
      "fn.str 'show' [loan.point 'p'] {\n"
      "    var.mut.str 'out' = [str:*<*];\n"
      "    loop.parts 'part' = ['p'] {\n"
      "        set 'out' = ['out' 'part'.name str:*: * (convert-to-str['part'.value])];\n"
      "    }\n"
      "    give ['out' str:*>*];\n"
      "}\n"
      "START {\n"
      "    var.point 'p' = [*20* *22*];\n"
      "    print.stdout[(show[loan 'p']) \\n];\n"
      "}\n";
  const xag::Source source("test.xag", xag::asFile(program));
  const xag::LexResult lexed = xag::lex(source);
  xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  CHECK(parsed.ok());
  const xag::CheckResult checked = xag::check(source, parsed.program);
  CHECK(checked.ok());
  CHECK(checked.walksParts.size() == 1);

  // Two fields, two turns, each standing where the statement stood — a
  // `loop.parts` is not a scope, it is a body written out once per field.
  CHECK(xag::unroll(parsed.program, checked) == 2);
  for (const xag::Item &item : parsed.program.items)
    if (item.name == "show") {
      // var, then (declare the turn, set) twice, then give.
      CHECK(item.body.stmts.size() == 6);
      for (const xag::StmtPtr &st : item.body.stmts)
        CHECK(st->kind != xag::StmtKind::LoopParts);
    }
  // And a struct for each field, written in beside the walk.
  std::set<std::string> made;
  for (const xag::Item &item : parsed.program.items)
    if (item.kind == xag::ItemKind::Struct)
      made.insert(item.name);
  CHECK(made.count("part$point$x") == 1);
  CHECK(made.count("part$point$y") == 1);
  // `$` between the two, never a dot: a type is spelled with dots, and a blank
  // filled in with one of these is written back into a chain by splitting on
  // them — `part$point.x` came back as a `part$point` holding an `x`.
  for (const std::string &one : made)
    CHECK(one.find('.') == std::string::npos);
  // And what stands there now reads clean, with `'part'` nowhere in it.
  CHECK(xag::check(source, parsed.program).ok());

  // A `many` holds one type in every place, so a counted loop already reaches
  // them — and a place has a position rather than a name.
  CHECK(run("START {\n"
            "    var.many.int64 'm' = [*1* *2*];\n"
            "    loop.parts 'part' = ['m'] { print.stdout['part'.name \\n]; }\n"
            "}\n")
            .code(0) == "E0544");

  // The turn is a value of its own, so it can be handed about like anything
  // else — including to a generic that has never seen the struct it came from.
  const Checked handed = run(
      "struct 'point' [int64 'x', int64 'y']\n"
      "fn.str 'label' [loan.any 'part'] {\n"
      "    give ['part'.name str:*=* (convert-to-str['part'.value])];\n"
      "}\n"
      "START {\n"
      "    var.point 'p' = [*20* *22*];\n"
      "    var.mut.str 'out' = [str:**];\n"
      "    loop.parts 'part' = ['p'] {\n"
      "        set 'out' = ['out' (label[loan 'part']) str:* *];\n"
      "    }\n"
      "    print.stdout['out' \\n];\n"
      "}\n");
  CHECK(handed.checked.ok());
  // What a body carried into the program without being read still changes is
  // noted: `W0003` said `'out'` never changes, two lines above `set 'out'`.
  for (const xag::Diagnostic &one : handed.checked.diagnostics)
    CHECK(one.code != "W0003");
}

int main() {
  loopPartsWritesOneCopyPerField();
  howAThingIsHeldIsItsOwnQuestion();
  whicheverKeepsOneArm();
  aBlankMaySayWhatItTakes();
  oneMistakeIsReportedAsOneMistake();
  aGenericBodyIsNotReadWithTheBlankInIt();
  aGenericIsWrittenOutPerType();
  aGenericCallingAGenericIsWrittenOutToo();
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
  wrappingIsSaidWhereAThingIsDeclared();
  aComparisonTellsNeitherSideWhatItIs();
  bothStreamsArePrintedTo();
  signaturesAreReadBeforeBodies();
  giveAnswersItsFunction();
  aFunctionAnswersEveryWayOut();
  breakNeedsALoop();
  conditionsAskABool();
  aCountedLoopCountsInItsOwnType();
  whatIsWrittenDownIsWorkedOut();
  aLoopMayCountToTheMostItsCounterHolds();
  aNameMaySayItWraps();
  aNumberIsAskedToBecomeText();
  aCountedLoopSaysHowFarItGets();
  aPermCounterOutlivesItsLoop();
  theCounterIsInScopeOnlyInTheLoop();
  aManyHoldsSeveralOfOneType();
  anElementIsOneOfWhatItHolds();
  countAsksHowManyOfEither();
  fillNeedsAValueThatCopies();
  aOneOfIsOneOfItsCases();
  showingSeveralThingsWritesEachOfThem();
  askingToSkipNeedsSayingSo();
  aMutThatWasNotNeededIsSaidSo();
  showingAMaybeIsRefused();
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
