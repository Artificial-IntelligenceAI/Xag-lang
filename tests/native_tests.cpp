// The native backend is checked here for building a module LLVM will accept,
// raw and optimised. Whether it *answers the same* as the other engines is the
// oracle's question, not this file's.

#include "xag/Check.h"
#include "xag/Fold.h"
#include "xag/Lexer.h"
#include "xag/Mir.h"
#include "xag/Native.h"
#include "xag/Own.h"
#include "xag/Parser.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void emits(const std::string &program, const std::string &wanted, int line) {
  const xag::Source source("test.xag", program);
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  const xag::OwnResult owned = xag::own(source, parsed.program);
  if (!lexed.ok() || !parsed.ok() || !checked.ok() || !owned.ok()) {
    std::cerr << "FAIL line " << line << ": the program did not get as far as codegen\n";
    ++failures;
    return;
  }
  xag::MirResult built = xag::build(source, parsed.program, checked);
  xag::elaborate(built.mir);
  // The same road the compiler takes. Without this the module under test is not
  // the module that gets built.
  xag::fold(source, built.mir);

  for (bool optimise : {false, true}) {
    const xag::NativeResult emitted = xag::emitIr(built.mir, optimise);
    if (!emitted.ok()) {
      std::cerr << "FAIL line " << line << (optimise ? " (optimised): " : " (raw): ")
                << emitted.trouble << '\n';
      ++failures;
      continue;
    }
    // An optimiser that deleted the program is the shape a bug takes here, so
    // the raw module is asked whether the work is in it at all.
    if (!optimise && !wanted.empty() &&
        emitted.ir.find(wanted) == std::string::npos) {
      std::cerr << "FAIL line " << line << ": the IR does not mention " << wanted << '\n';
      ++failures;
    }
    if (optimise && emitted.ir.find("define noundef i32 @main") == std::string::npos &&
        emitted.ir.find("define i32 @main") == std::string::npos) {
      std::cerr << "FAIL line " << line << ": the optimiser left no main\n";
      ++failures;
    }
  }
}

#define EMITS(program, wanted) emits(program, wanted, __LINE__)

// Some bugs are a call that should not be there rather than one that should.
void rejects(const std::string &program, const std::string &unwanted, int line) {
  const xag::Source source("test.xag", program);
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  const xag::OwnResult owned = xag::own(source, parsed.program);
  if (!lexed.ok() || !parsed.ok() || !checked.ok() || !owned.ok()) {
    std::cerr << "FAIL line " << line << ": the program did not get as far as codegen\n";
    ++failures;
    return;
  }
  xag::MirResult built = xag::build(source, parsed.program, checked);
  xag::elaborate(built.mir);
  // The same road the compiler takes. Without this the module under test is not
  // the module that gets built.
  xag::fold(source, built.mir);
  const xag::NativeResult emitted = xag::emitIr(built.mir, false);
  if (!emitted.ok()) {
    std::cerr << "FAIL line " << line << ": " << emitted.trouble << '\n';
    ++failures;
    return;
  }
  if (emitted.ir.find(unwanted) != std::string::npos) {
    std::cerr << "FAIL line " << line << ": the IR calls " << unwanted << '\n';
    ++failures;
  }
}

#define REJECTS(program, unwanted) rejects(program, unwanted, __LINE__)

void itEmitsWholePrograms() {
  EMITS("START { print.stdout[str:*hello* \\n]; }\n", "xag_print");
  EMITS("START { var.int64 'n' = [*2* + *3*]; print.stdout['n' \\n]; }\n", "xag_print_int");
  EMITS("START { var.str 's' = [*hi*]; print.stdout['s' \\n]; }\n", "xag_str_drop");
  EMITS("const.int64 'LIMIT' = [*10*];\nSTART { print.stdout['LIMIT' \\n]; }\n",
        "xag_const__LIMIT_");
}

void itEmitsFunctionsAndLoans() {
  EMITS("fn.int64 'twice' [int64 'n'] { give ['n' + 'n']; }\n"
        "START { print.stdout[twice[*21*] \\n]; }\n", "xag_twice");
  EMITS("fn.int64 'size' [ref.str 'text'] { give [count['text']]; }\n"
        "START { var.str 's' = [*café*]; print.stdout[size[ref 's'] \\n]; }\n",
        "xag_str_count");
  // A function that lends its answer back must not look like one that hands it
  // over: getting that wrong once made the optimiser delete the program.
  EMITS("fn.ref.'life'.str 'longer' [ref.'life'.str 'a', ref.'life'.str 'b'] {\n"
        "  if count['a'] >== count['b'] { give ['a']; } else { give ['b']; } }\n"
        "START { var.str 'x' = [*hello*]; var.str 'y' = [*hi*];\n"
        "  var.ref.str 'w' = [longer[ref 'x', ref 'y']];\n"
        "  print.stdout['w' \\n]; }\n",
        "xag_longer");
}

void itEmitsControlFlow() {
  EMITS("START { var.mut.int64 't' = [*0*];\n"
        "  loop.range.int64 'i' = [*1*, *10*] { set 't' = ['t' + 'i']; }\n"
        "  print.stdout['t' \\n]; }\n", "block");
  EMITS("START { var.int64 'n' = [*1*];\n"
        "  if 'n' == *1* { print.stdout[str:*one* \\n]; } else { print.stdout[str:*other* \\n]; } }\n",
        "br i1");
}

void itGuardsAConditionalDrop() {
  EMITS("fn.nothing 'keep' [str 't'] { print.stdout['t' \\n]; }\n"
        "START { var.str 's' = [*hi*]; var.int64 'n' = [*1*];\n"
        "  if 'n' == *1* { keep[move 's']; } }\n",
        "xag_str_drop");
}

void itEmitsAMany() {
  // The check is written out rather than called, so what a reach past the end
  // reaches is the half that stops — and only that half. The place has to be
  // one nobody knows yet, since a written one is settled before this runs.
  EMITS("fn.nothing 'at' [ref.many.int64 'xs', int64 'i'] {\n"
        "  print.stdout['xs'['i'] \\n]; }\n"
        "START { var.many.int64 'xs' = [*1* *2* *3*];\n"
        "  at[ref 'xs', *0*]; }\n", "call void @xag_many_out_of_range");
  EMITS("START { var.many.int64 'xs' = [*1* *2*];\n"
        "  print.stdout[(count[ref 'xs']) \\n]; }\n", "xag_many_new");
  // A `many` of text lets go of what sits in every place, not only the buffer.
  EMITS("START { var.many.str 'ws' = [*a* *b*];\n"
        "  print.stdout['ws'[*1*] \\n]; }\n", "xag_many_drop_str");
  EMITS("START { var.many.int64 'xs' = [fill[*0*, *4*]];\n"
        "  print.stdout['xs'[*3*] \\n]; }\n", "xag_many_fill");
}

void itEmitsSomethingOrNothing() {
  // A written value would be folded into a constant on the way in, so this
  // holds something the optimiser cannot see the far side of.
  EMITS("fn.int64 'three' [] { give [*3*]; }\n"
        "START { var.or-nothing.int64 'n' = [three[]];\n"
        "  if 'n' holds 'v' { print.stdout['v' \\n]; } }\n", "insertvalue");
  // What may be missing lets go of what is inside only when there is something.
  EMITS("START { var.or-nothing.str 's' = [*hi*];\n"
        "  if 's' holds 't' { print.stdout['t' \\n]; } }\n", "xag_str_drop");
}

void aLoanPassedOnIsTheLoan() {
  // Lending something already borrowed passes the loan along rather than making
  // a loan of the pointer. Every borrow through two functions read rubbish
  // before this, and no vote between the interpreters would have said so.
  EMITS("fn.int64 'size' [ref.str 't'] { give [count['t']]; }\n"
        "fn.int64 'outer' [ref.str 'u'] { give [size[ref 'u']]; }\n"
        "START { var.str 's' = [*hello*]; print.stdout[(outer[ref 's']) \\n]; }\n",
        "xag_str_count");
}

void itEmitsAGroupOfNamedThings() {
  EMITS("struct 'point' [int64 'x', int64 'y']\n"
        "START { var.mut.point 'p' = [*3* *4*];\n"
        "  set 'p'.y = [*9*];\n"
        "  print.stdout['p'.x str:* * 'p'.y \\n]; }\n",
        "xag_print_int");
  // What it holds that has an owner is let go one at a time.
  EMITS("struct 'tag' [str 'name', int64 'runs']\n"
        "START { var.tag 't' = [*ada* *36*];\n"
        "  print.stdout['t'.name \\n]; }\n",
        "xag_str_drop");
  EMITS("struct 'tag' [str 'name']\nstruct 'pair' [tag 'one', tag 'two']\n"
        "START { var.tag 'a' = [*ada*];\n  var.tag 'b' = [*bob*];\n"
        "  var.pair 'p' = [move 'a' move 'b'];\n"
        "  print.stdout['p'.two.name \\n]; }\n",
        "xag_str_drop");
  // Taking one out of a struct leaves nothing there for the drop to find.
  EMITS("fn.nothing 'keep' [str 't'] { print.stdout['t' \\n]; }\n"
        "struct 'tag' [str 'name', int64 'runs']\n"
        "START { var.tag 't' = [*ada* *36*];\n"
        "  keep[move 't'.name];\n"
        "  print.stdout['t'.runs \\n]; }\n",
        "xag_str_drop");
  EMITS("struct 'point' [int64 'x', int64 'y']\n"
        "START { var.point 'a' = [*1* *2*];\n  var.point 'b' = [*3* *4*];\n"
        "  var.many.point 'ps' = [move 'a' move 'b'];\n"
        "  print.stdout['ps'[*1*].x \\n]; }\n",
        "xag_many_new");
  EMITS("struct 'point' [int64 'x', int64 'y']\n"
        "fn.int64 'across' [ref.point 'p'] { give ['p'.x + 'p'.y]; }\n"
        "START { var.point 'p' = [*20* *22*];\n"
        "  print.stdout[(across[ref 'p']) \\n]; }\n",
        "@xag_across");
  // A `many` of them, and one behind `or-nothing`. The second was invalid IR:
  // the group was built as though the absence were one of the fields.
  EMITS("struct 'tag' [str 'name']\n"
        "START { var.tag 'a' = [*ada*];\n  var.tag 'b' = [*bob*];\n"
        "  var.many.tag 'ts' = [move 'a' move 'b'];\n"
        "  print.stdout['ts'[*1*].name \\n]; }\n",
        "xag_str_drop");
  EMITS("struct 'tag' [str 'name']\n"
        "START { var.or-nothing.tag 't' = [*ada*];\n"
        "  when 't' {\n"
        "    is 'one'   { print.stdout['one'.name \\n]; }\n"
        "    is nothing { print.stdout[str:*none* \\n]; } } }\n",
        "xag_str_drop");
}

// Letting go of one walks what it is made of. Choosing between three runtime
// calls with a chain of `?:` sent a struct held inside a struct to
// `xag_many_drop`, which was handed something never allocated and aborted.
void aStructLetsGoOfWhatItHolds() {
  // A struct of numbers owns nothing, so nothing is written at all.
  REJECTS("struct 'point' [int64 'x', int64 'y']\n"
          "START { var.point 'p' = [*1* *2*];\n"
          "  print.stdout['p'.x \\n]; }\n",
          "call void @xag_many_drop");
  // Nor does a struct holding one of those.
  REJECTS("struct 'point' [int64 'x', int64 'y']\n"
          "struct 'line' [point 'from', point 'to']\n"
          "START { var.point 'a' = [*1* *2*];\n  var.point 'b' = [*3* *4*];\n"
          "  var.line 'l' = [move 'a' move 'b'];\n"
          "  print.stdout['l'.to.x \\n]; }\n",
          "call void @xag_many_drop");
  // A struct inside a struct goes down to the text, and never to the array.
  REJECTS("struct 'tag' [str 'name']\nstruct 'pair' [tag 'one', int64 'n']\n"
          "START { var.tag 'a' = [*ada*];\n"
          "  var.pair 'p' = [move 'a' *7*];\n"
          "  print.stdout['p'.one.name \\n]; }\n",
          "call void @xag_many_drop");
  EMITS("struct 'tag' [str 'name']\nstruct 'pair' [tag 'one', int64 'n']\n"
        "START { var.tag 'a' = [*ada*];\n"
        "  var.pair 'p' = [move 'a' *7*];\n"
        "  print.stdout['p'.one.name \\n]; }\n",
        "xag_str_drop");
}

// Dividing is an instruction with a question in front of it, not a call. A call
// is something the optimiser cannot see through, and `mod` against a written
// number was costing about twenty times a machine's own remainder because of it.
void dividingIsAnInstruction() {
  // Both numbers arrive while it runs: written ones are worked out before
  // codegen ever sees them, and then there is no instruction to look for.
  const char *kMod = "fn.nothing 'go' [int64 'n', int64 'd'] {\n"
                     "  print.stdout[('n' mod 'd') \\n]; }\n"
                     "START { go[*100*, *7*]; }\n";
  const char *kDiv = "fn.nothing 'go' [int64 'n', int64 'd'] {\n"
                     "  print.stdout[('n' / 'd') \\n]; }\n"
                     "START { go[*100*, *7*]; }\n";
  EMITS(kMod, "srem");
  EMITS(kDiv, "sdiv");
  REJECTS(kMod, "call i128 @xag_int_mod");
  REJECTS(kDiv, "call i128 @xag_int_div");

  // Unsigned asks the unsigned instruction, and neither needs the signed pair
  // guarded against.
  EMITS("fn.nothing 'go' [uint32 'n', uint32 'd'] {\n"
        "  print.stdout[('n' mod 'd') \\n]; }\n"
        "START { go[*100*, *7*]; }\n",
        "urem");
  EMITS("fn.nothing 'go' [uint32 'n', uint32 'd'] {\n"
        "  print.stdout[('n' / 'd') \\n]; }\n"
        "START { go[*100*, *7*]; }\n",
        "udiv");

  // Dividing by zero still stops, and says which of the two it was.
  EMITS(kMod, "xag_stop");

  // Raising to a power is a loop rather than an instruction, so it stays a call.
  EMITS("START { var.int64 'n' = [*2*]; var.int64 'p' = [*10*];\n"
        "  print.stdout[('n' ^ 'p') \\n]; }\n",
        "xag_int_pow");
}

// A place shown to be one the `many` has is not asked about again while it
// runs. Asking costs a compare and a branch in the middle of every loop, and
// `out-of-range` is the one default that costs anything at all.
void aSettledPlaceIsNotAskedAgain() {
  REJECTS("START { var.many.int64 'xs' = [*10* *20* *30*];\n"
          "  print.stdout['xs'[*1*] \\n]; }\n",
          "call void @xag_many_out_of_range");
  REJECTS("START { var.mut.many.int64 'xs' = [*1* *2* *3*];\n"
          "  set 'xs'[*1*] = [*9*];\n"
          "  print.stdout['xs'[*1*] \\n]; }\n",
          "call void @xag_many_out_of_range");

  // A place that is not known until it runs is still asked about.
  EMITS("fn.nothing 'at' [ref.many.int64 'xs', int64 'i'] {\n"
        "  print.stdout['xs'['i'] \\n]; }\n"
        "START { var.many.int64 'ns' = [*10* *20*];\n"
        "  at[ref 'ns', *1*]; }\n",
        "xag_many_out_of_range");

  // A loop counting the places a `many` has reaches one it has every time
  // round, because a `many` is a fixed length once it is made. This is the
  // shape every program that walks an array writes.
  REJECTS("START { var.many.int64 'xs' = [*1* *2* *3*];\n"
          "  loop.range.int64 'i' = [*0*, (count[ref 'xs'] - *1*)] {\n"
          "    print.stdout['xs'['i'] \\n]; } }\n",
          "call void @xag_many_out_of_range");

  // Counting one and reaching into another says nothing about the other.
  EMITS("START { var.many.int64 'xs' = [*1* *2* *3*];\n"
        "  var.many.int64 'ys' = [*1* *2*];\n"
        "  loop.range.int64 'i' = [*0*, (count[ref 'xs'] - *1*)] {\n"
        "    print.stdout['ys'['i'] \\n]; } }\n",
        "call void @xag_many_out_of_range");

  // Nor does reaching with anything but the counter.
  EMITS("START { var.many.int64 'xs' = [*1* *2* *3*];\n"
        "  loop.range.int64 'i' = [*0*, (count[ref 'xs'] - *1*)] {\n"
        "    var.int64 'j' = ['i' + *5*];\n"
        "    print.stdout['xs'['j'] \\n]; } }\n",
        "call void @xag_many_out_of_range");

  // A `many` never changes length, but a name can be given a different one —
  // and then how many places it has is no longer what was counted.
  EMITS("START { var.mut.many.int64 'xs' = [*1* *2* *3*];\n"
        "  loop.range.int64 'i' = [*0*, (count[ref 'xs'] - *1*)] {\n"
        "    set 'xs' = [*9*];\n"
        "    print.stdout['xs'['i'] \\n]; } }\n",
        "call void @xag_many_out_of_range");
}

} // namespace

int main() {
  itEmitsWholePrograms();
  itEmitsFunctionsAndLoans();
  itEmitsControlFlow();
  itGuardsAConditionalDrop();
  itEmitsAMany();
  itEmitsSomethingOrNothing();
  aLoanPassedOnIsTheLoan();
  itEmitsAGroupOfNamedThings();
  aStructLetsGoOfWhatItHolds();
  dividingIsAnInstruction();
  aSettledPlaceIsNotAskedAgain();

  if (failures == 0)
    std::cout << "all native tests passed\n";
  return failures == 0 ? 0 : 1;
}
