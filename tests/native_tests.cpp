// The native backend is checked here for building a module LLVM will accept,
// raw and optimised. Whether it *answers the same* as the other engines is the
// oracle's question, not this file's.

#include "xag/Check.h"
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

void itEmitsWholePrograms() {
  EMITS("START { print.stdout[str:*hello* \\n]; }\n", "xag_print");
  EMITS("START { var.int64 'n' = [*2* + *3*]; print.stdout['n' \\n]; }\n", "xag_print_int");
  EMITS("START { var.str 's' = [*hi*]; print.stdout['s' \\n]; }\n", "xag_str_drop");
  EMITS("const.int64 'LIMIT' = [*10*];\nSTART { print.stdout['LIMIT' \\n]; }\n",
        "xag_const__LIMIT_");
}

void itEmitsFunctionsAndLoans() {
  EMITS("fn.int64 twice [int64 'n'] { give ['n' + 'n']; }\n"
        "START { print.stdout[twice[*21*] \\n]; }\n", "xag_twice");
  EMITS("fn.int64 size [ref.str 'text'] { give [count['text']]; }\n"
        "START { var.str 's' = [*café*]; print.stdout[size[ref 's'] \\n]; }\n",
        "xag_str_count");
  // A function that lends its answer back must not look like one that hands it
  // over: getting that wrong once made the optimiser delete the program.
  EMITS("fn.ref.'life'.str longer [ref.'life'.str 'a', ref.'life'.str 'b'] {\n"
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
  EMITS("fn.nothing keep [str 't'] { print.stdout['t' \\n]; }\n"
        "START { var.str 's' = [*hi*]; var.int64 'n' = [*1*];\n"
        "  if 'n' == *1* { keep[move 's']; } }\n",
        "xag_str_drop");
}

void itEmitsAMany() {
  // The check is written out rather than called, so what a reach past the end
  // reaches is the half that stops — and only that half.
  EMITS("START { var.many.int64 'xs' = [*1* *2* *3*];\n"
        "  print.stdout['xs'[*0*] \\n]; }\n", "call void @xag_many_out_of_range");
  EMITS("START { var.many.int64 'xs' = [*1* *2*];\n"
        "  print.stdout[(count[ref 'xs']) \\n]; }\n", "xag_many_new");
  // A `many` of text lets go of what sits in every place, not only the buffer.
  EMITS("START { var.many.str 'ws' = [*a* *b*];\n"
        "  print.stdout['ws'[*1*] \\n]; }\n", "xag_many_drop_str");
  EMITS("START { var.many.int64 'xs' = [fill[*0*, *4*]];\n"
        "  print.stdout['xs'[*3*] \\n]; }\n", "xag_many_fill");
}

} // namespace

int main() {
  itEmitsWholePrograms();
  itEmitsFunctionsAndLoans();
  itEmitsControlFlow();
  itGuardsAConditionalDrop();
  itEmitsAMany();

  if (failures == 0)
    std::cout << "all native tests passed\n";
  return failures == 0 ? 0 : 1;
}
