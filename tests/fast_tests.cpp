// The fast interpreter, held against the one that is meant to be believed.
//
// Every program here is run on both, and what they say must match exactly. It
// is a poor test that only checks the fast one against what somebody expected:
// the whole reason it exists is to be a second opinion, and a second opinion
// that was written from the first is not one.

#include "xag/Check.h"
#include "xag/Fast.h"
#include "xag/Interpret.h"
#include "as_file.h"

#include "xag/Lexer.h"
#include "xag/Mir.h"
#include "xag/Own.h"
#include "xag/Parser.h"
#include "xag_runtime.h"

#include <cstdio>
#include <iostream>
#include <string>

namespace {

int failures = 0;

struct Said {
  bool ran = false;
  std::string trouble;
  std::string out;
  std::string err;   // and what it wrote to standard error, which a print names
  int64_t leaked = 0;
};

Said capture(const xag::Mir &mir, bool quick) {
  Said said;
  const int64_t before = xag_live_allocations();
  std::FILE *sink = std::tmpfile();
  std::FILE *grumbles = std::tmpfile();
  xag_set_output(sink);
  xag_set_error(grumbles);
  if (quick) {
    const xag::FastResult result = xag::runFast(mir);
    said.ran = result.ran;
    said.trouble = result.trouble;
  } else {
    const xag::InterpretResult result = xag::interpret(mir);
    said.ran = result.ran;
    said.trouble = result.trouble;
  }
  xag_set_output(nullptr);
  xag_set_error(nullptr);
  said.leaked = xag_live_allocations() - before;
  // A program that stops holds whatever it was holding — a stop does not reach
  // the end of a scope. That is not this run's mistake and it must not become
  // the next one's: the fast engine asks whether the balance is clear for the
  // whole process, so anything left here would be reported against whatever
  // runs after it.
  if (!said.ran)
    xag_forget_allocations(before);

  std::fflush(sink);
  std::rewind(sink);
  char buffer[8192];
  const size_t got = std::fread(buffer, 1, sizeof(buffer), sink);
  said.out.assign(buffer, got);
  std::fclose(sink);

  std::fflush(grumbles);
  std::rewind(grumbles);
  const size_t grumbled = std::fread(buffer, 1, sizeof(buffer), grumbles);
  said.err.assign(buffer, grumbled);
  std::fclose(grumbles);
  return said;
}

// `under` is what the file's manifest would have said: every item is read
// under it, the way `applySettings` does for a unit.
void agree(const std::string &text, int line, xag::Settings under = {}) {
  const xag::Source source("test.xag", xag::asFile(text));
  const xag::LexResult lexed = xag::lex(source);
  xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  for (xag::Item &item : parsed.program.items)
    item.settings = under;
  const xag::CheckResult checked = xag::check(source, parsed.program);
  const xag::OwnResult owned = xag::own(source, parsed.program, checked);
  if (!lexed.ok() || !parsed.ok() || !checked.ok() || !owned.ok()) {
    std::cerr << "FAIL line " << line << ": the program did not compile\n";
    ++failures;
    return;
  }
  xag::MirResult built = xag::build(source, parsed.program, checked);
  xag::elaborate(built.mir);

  const Said slow = capture(built.mir, false);
  const Said quick = capture(built.mir, true);

  // Both streams, not just the one. Writing every print to standard output is
  // an engine getting the destination wrong, and comparing the two engines'
  // output alone they would agree about it perfectly.
  if (slow.out != quick.out || slow.err != quick.err || slow.ran != quick.ran ||
      (!slow.ran && slow.trouble != quick.trouble)) {
    std::cerr << "FAIL line " << line << ": the engines disagree\n"
              << "    test: \"" << slow.out << "\" / err \"" << slow.err << "\" ("
              << (slow.ran ? "ran" : slow.trouble) << ")\n"
              << "    fast: \"" << quick.out << "\" / err \"" << quick.err << "\" ("
              << (quick.ran ? "ran" : quick.trouble) << ")\n";
    ++failures;
  }
  // Only where it finished. A program that stops holds whatever it was holding,
  // and letting go of it is what the end of a scope does — which a stop does not
  // reach. Both engines are untidy in the same way, and that they agree about
  // the answer is what is being asked here.
  if (quick.ran && quick.leaked != 0) {
    std::cerr << "FAIL line " << line << ": the fast engine ended holding "
              << quick.leaked << '\n';
    ++failures;
  }
}

#define AGREE(program) agree(program, __LINE__)
#define AGREE_UNDER(program, settings) agree(program, __LINE__, settings)

void onTheOrdinaryThings() {
  AGREE("START { print.stdout[str:*hello* \\n]; }\n");
  AGREE("START { var.str 'w' = [*world*];"
        " print.stdout[str:*Hello, * 'w' str:*!* \\n]; }\n");
  AGREE("START { var.int64 'n' = [*2* + *3* x *4*]; print.stdout['n' \\n]; }\n");
  AGREE("START { var.mut.int64 't' = [*0*];\n"
        "  loop.range.int64 'i' = [*1*, *10*] { set 't' = ['t' + 'i']; }\n"
        "  print.stdout['t' \\n]; }\n");
  AGREE("START { var.int64 'n' = [*5*];\n"
        "  if 'n' > int64:*3* { print.stdout[str:*big* \\n]; }"
        "  else { print.stdout[str:*small* \\n]; } }\n");
  AGREE("START { loop.perm.range.int64 'i' = [*1*, *100*] {"
        " if 'i' x 'i' > int64:*10* { break; } }"
        " print.stdout['i' \\n]; }\n");
  AGREE("START { loop.range.int64 'i' = [*1*, *100*] {"
        " if 'i' > int64:*3* { break; } print.stdout['i' str:* *]; } }\n");
  // Counting to the most the counter holds. Stepping first and asking after,
  // that last step came round and the loop never finished.
  AGREE("START { var.mut.wrapping.int64 't' = [*0*];\n"
        "  loop.range.uint8 'i' = [*0*, *255*] { set 't' = ['t' + *1*]; }\n"
        "  print.stdout['t' \\n]; }\n");
}

void onEverySizeAndFamily() {
  // `wrapping`, because coming round is the point of these two and without the
  // word both engines stop instead — which they also have to agree about, and
  // do in `onASumThatDoesNotFit`.
  AGREE("START { var.mut.wrapping.uint8 'n' = [*255*]; set 'n' = ['n' + *1*];"
        " print.stdout['n' \\n]; }\n");
  AGREE("START { var.mut.wrapping.int8 'n' = [*127*]; set 'n' = ['n' + *1*];"
        " print.stdout['n' \\n]; }\n");
  AGREE("START { var.uint128 'n' = [*340282366920938463463374607431768211455*];"
        " print.stdout['n' \\n]; }\n");
  AGREE("START { var.bin64 'a' = [*0.1*]; var.bin64 'b' = [*0.2*];"
        " print.stdout[('a' + 'b') \\n]; }\n");
  AGREE("START { var.bin32 'a' = [*0.1*]; print.stdout[('a' x bin32:*3*) \\n]; }\n");
  AGREE("START { var.bin64 'z' = [*0*]; print.stdout[(bin64:*1* / 'z') str:* * ('z' / 'z') \\n]; }\n");
  AGREE("START { var.bin128 'a' = [*1e30*];"
        " print.stdout[('a' + bin128:*1*) \\n]; }\n");
  AGREE("START { var.deci64 'a' = [*0.1*]; var.deci64 'b' = [*0.2*];"
        " print.stdout[('a' + 'b') \\n]; }\n");
  AGREE("START { var.deci64 'p' = [*1.10*];"
        " print.stdout[('p' + deci64:*2.00*) \\n]; }\n");
  AGREE("START { print.stdout[(deci128:*1* / deci128:*3*) \\n]; }\n");
}

void onCallsAndBorrows() {
  AGREE("fn.int64 'sum-to' [int64 'n'] {\n"
        "  var.mut.int64 't' = [*0*];\n"
        "  loop.range.int64 'i' = [*1*, 'n'] { set 't' = ['t' + 'i']; }\n"
        "  give ['t']; }\n"
        "START { print.stdout[sum-to[*10*] \\n]; }\n");
  AGREE("fn.int64 'size' [loan.str 't'] { give [count['t']]; }\n"
        "START { var.str 's' = [*café*]; print.stdout[size[loan 's'] \\n]; }\n");
  AGREE("fn.nothing 'edit' [loanmut.str 't'] { set 't' = ['t' *!*]; }\n"
        "START { var.mut.str 's' = [*hi*]; edit[loanmut 's'];"
        " print.stdout['s' \\n]; }\n");
  AGREE("fn.nothing 'keep' [str 't'] { print.stdout['t' \\n]; }\n"
        "START { var.str 's' = [*taken*]; keep[move 's']; }\n");
  AGREE("fn.nothing 'keep' [str 't'] { print.stdout['t' \\n]; }\n"
        "START { var.str 's' = [*hi*]; var.int64 'n' = [*1*];\n"
        "  if 'n' == int64:*1* { keep[move 's']; } }\n");
  AGREE("fn.loan.'life'.str 'longer' [loan.'life'.str 'a', loan.'life'.str 'b'] {\n"
        "  if count['a'] >== count['b'] { give ['a']; } else { give ['b']; } }\n"
        "START { var.str 'x' = [*hello*]; var.str 'y' = [*hi*];\n"
        "  var.loan.str 'w' = [longer[loan 'x', loan 'y']];\n"
        "  print.stdout['w' \\n]; }\n");
  AGREE("const.int64 'LIMIT' = [*10*];\n"
        "START { print.stdout['LIMIT' \\n]; }\n");
  AGREE("const.str 'GREETING' = [*Hello*];\n"
        "START { print.stdout['GREETING' str:*!* \\n]; }\n");
}

void onHoldingSeveralValues() {
  AGREE("START { var.many.int64 'xs' = [*10* *20* *30*];\n"
        "  print.stdout['xs'[*1*] str:* * 'xs'[*3*] str:* of * (count[loan 'xs']) \\n]; }\n");
  AGREE("START { var.mut.many.int64 'xs' = [fill[*3*, *5*]];\n"
        "  set 'xs'[*5*] = [*9*];\n"
        "  loop.range.int64 'i' = [*1*, *5*] { print.stdout['xs'['i'] \\n]; } }\n");
  AGREE("START { var.mut.many.str 'ws' = [*one* *two* *three*];\n"
        "  set 'ws'[*2*] = [*TWO*];\n"
        "  loop.range.int64 'i' = [*1*, count[loan 'ws']] {\n"
        "    print.stdout['ws'['i'] str:* * (count['ws'['i']]) \\n]; } }\n");
  AGREE("fn.int64 'total' [loan.many.int64 'xs'] {\n"
        "  var.mut.int64 't' = [*0*];\n"
        "  loop.range.int64 'i' = [*1*, count['xs']] {\n"
        "    set 't' = ['t' + 'xs'['i']]; }\n"
        "  give ['t']; }\n"
        "START { var.many.int64 'xs' = [*1* *2* *3* *4*];\n"
        "  print.stdout[total[loan 'xs'] \\n]; }\n");
  AGREE("START { var.many.int64 'none' = [];\n"
        "  print.stdout[(count[loan 'none']) \\n]; }\n");
  // Showing the whole of one: every place, one value after another.
  AGREE("START { var.many.int64 'xs' = [*1* *2* *3*];\n"
        "  print.stdout['xs' \\n]; }\n");
  AGREE("START { var.many.many.int64 'g' = [[*1* *2*] [*3*]];\n"
        "  print.stdout['g' \\n]; }\n");
  AGREE("START { var.many.str 'ws' = [*one* *two*];\n"
        "  print.stdout['ws' str:*|* (convert-to-str[loan 'ws']) \\n]; }\n");
  AGREE("START { var.many.deci64 'ds' = [*1.10* *0.1*];\n"
        "  print.stdout['ds' \\n]; }\n");
  AGREE("START { var.mut.many-growing.int64 'g' = [*1* *2*];\n"
        "  add 'g' = [*3*];\n  print.stdout['g' \\n]; }\n");
}

void onHoldingNothing() {
  AGREE("fn.or-nothing.int64 'half' [int64 'n'] {\n"
        "  if 'n' == int64:*0* { give [nothing]; }\n"
        "  give ['n' / *2*]; }\n"
        "START { loop.range.int64 'i' = [*0*, *4*] {\n"
        "  var.or-nothing.int64 'h' = [half['i']];\n"
        "  if 'h' holds 'v' { print.stdout['v' \\n]; } } }\n");
  AGREE("START { var.or-nothing.str 'a' = [*text*];\n"
        "  if 'a' holds 't' { print.stdout['t' str:* * (count['t']) \\n]; } }\n");
  AGREE("START { var.or-nothing.many.int64 'xs' = [*1* *2* *3*];\n"
        "  if 'xs' holds 'held' { print.stdout[(count[loan 'held']) \\n]; } }\n");
}

void onChoosingBetweenCases() {
  AGREE("fn.or-nothing.int64 'half' [int64 'n'] {\n"
        "  if 'n' == int64:*0* { give [nothing]; }\n"
        "  give ['n' / *2*]; }\n"
        "START { loop.range.int64 'i' = [*0*, *4*] {\n"
        "  when half['i'] {\n"
        "    is 'v'     { print.stdout['i' str:* -> * 'v' \\n]; }\n"
        "    is nothing { print.stdout['i' str:* -> none* \\n]; } } } }\n");
  AGREE("START { var.or-nothing.str 's' = [*text*];\n"
        "  when 's' {\n"
        "    is nothing { print.stdout[str:*none* \\n]; }\n"
        "    is 't'     { print.stdout['t' str:* * (count['t']) \\n]; } } }\n");
}

void onBeingOneOfSeveralThings() {
  AGREE("one-of 'answer' [int64 'ok', nothing 'gave-up']\n"
        "START { var.answer 'a' = [ok:*7*];\n"
        "  when 'a' { is ok 'n' { print.stdout['n' \\n]; }\n"
        "             is gave-up { print.stdout[str:*none* \\n]; } } }\n");
  AGREE("one-of 'answer' [int64 'ok', bool 'flag', deci64 'money', nothing 'no']\n"
        "fn.answer 'pick' [int64 'n'] {\n"
        "  if 'n' == int64:*0* { give [no]; }\n"
        "  if 'n' == int64:*1* { give [flag:*true*]; }\n"
        "  if 'n' == int64:*2* { give [money:*1.25*]; }\n"
        "  give [ok:'n']; }\n"
        "START { loop.range.int64 'i' = [*0*, *3*] {\n"
        "  when pick['i'] {\n"
        "    is ok 'n'    { print.stdout[str:*ok * 'n' \\n]; }\n"
        "    is flag 'b'  { print.stdout[str:*flag * 'b' \\n]; }\n"
        "    is money 'd' { print.stdout[str:*money * 'd' \\n]; }\n"
        "    is no        { print.stdout[str:*no* \\n]; } } } }\n");
  AGREE("one-of 'answer' [int64 'ok', nothing 'no']\n"
        "struct 'box' [answer 'a', int64 'n']\n"
        "START { var.box 'b' = [ok:*1* *2*];\n"
        "  when 'b'.a { is ok 'n' { print.stdout['n' \\n]; } is no { } } }\n");
  AGREE("one-of 'answer' [int64 'ok', nothing 'no']\n"
        "START { var.many.answer 'as' = [ok:*1* no ok:*3*];\n"
        "  loop.range.int64 'i' = [*1*, count[loan 'as']] {\n"
        "    when 'as'['i'] { is ok 'n' { print.stdout['n']; }\n"
        "                     is no     { print.stdout[str:*-*]; } } }\n"
        "  print.stdout[\\n]; }\n");
}

void onACaseThatOwnsSomething() {
  AGREE("one-of 'thing' [str 'text', int64 'n', nothing 'no']\n"
        "START { var.thing 't' = [text:*hello*];\n"
        "  when 't' { is text 's' { print.stdout['s' \\n]; }\n"
        "             is n 'x' { } is no { } } }\n");
  AGREE("one-of 'thing' [many.str 'words', int64 'n']\n"
        "START { var.thing 't' = [words:[*a* *b* *c*]];\n"
        "  when 't' { is words 'w' { print.stdout['w' \\n]; } is n 'x' { } } }\n");
  AGREE("struct 'pair' [str 'a', int64 'b']\n"
        "one-of 'thing' [pair 'both', int64 'n']\n"
        "START { var.thing 't' = [both:pair[*p* *9*]];\n"
        "  when 't' { is both 'p' { print.stdout['p'.a str:* * 'p'.b \\n]; }\n"
        "             is n 'x' { } } }\n");
  AGREE("one-of 'thing' [str 'text', nothing 'no']\n"
        "fn.nothing 'say' [thing 't'] {\n"
        "  when 't' { is text 's' { print.stdout['s' \\n]; } is no { } } }\n"
        "START { var.mut.thing 't' = [text:*first*];\n"
        "  set 't' = [text:*second*];\n"
        "  loop.range.int64 'i' = [*1*, *3*] {\n"
        "    var.thing 'c' = [text:*round*];\n"
        "    say[move 'c']; } }\n");
}

void onShowingWhatSeveralThingsHold() {
  AGREE("struct 'point' [int64 'x', int64 'y']\n"
        "START { var.point 'p' = [*1* *2*];\n"
        "  print.stdout['p' str:*|* (convert-to-str[loan 'p']) \\n]; }\n");
  AGREE("struct 'bag' [many.int64 'ns', str 'label']\n"
        "START { var.bag 'b' = [[*7* *8*] *tag*];\n"
        "  print.stdout['b' \\n]; }\n");
  AGREE("struct 'held' [loan.str 'what', deci64 'much']\n"
        "START { var.str 'name' = [*ada*];\n"
        "  var.held 'h' = [loan 'name' *1.25*];\n"
        "  print.stdout['h' \\n]; }\n");
}

// A `print` says where it goes, and both engines have to send it there.
// Text that is here and empty against text that is not here at all. The two
// were one bit pattern until a `str` was made never to point nowhere, and the
// backend now spends that: `or-nothing str` is as wide as a `str`. If the
// distinction were lost, this is where it would show.
// A sum that does not fit stops, and both engines stop in the same place with
// the same words. What is written down is refused before it runs, so this asks
// with a number that came from outside.
void onASumThatDoesNotFit() {
  // `unchecked`: no check, no stop, in both — the answer comes round.
  AGREE("START { UNSAFE {\n"
        "  var.mut.unchecked.int8 'n' = [*120*];\n"
        "  loop.range.int8 'i' = [*1*, *10*] { set 'n' = ['n' + *1*]; }\n"
        "  print.stdout['n' \\n]; } }\n");
  AGREE("START { var.mut.int8 'n' = [*100*];\n"
        "  loop.range.int8 'i' = [*1*, *10*] { set 'n' = ['n' + *20*]; }\n"
        "  print.stdout['n' \\n]; }\n");
  // Holding text when it stops, which neither engine unwinds — a stopped
  // program is not a tidy one, and they have to be untidy the same way.
  AGREE("START { var.str 's' = [*abc*];\n"
        "  var.mut.int8 'n' = [*100*];\n"
        "  loop.range.int8 'i' = [*1*, *10*] { set 'n' = ['n' + *20*]; }\n"
        "  print.stdout['s' \\n]; }\n");
  // And with the word, both come round instead — still together.
  AGREE("START { var.mut.wrapping.int8 'n' = [*100*];\n"
        "  loop.range.int8 'i' = [*1*, *10*] { set 'n' = ['n' + *20*]; }\n"
        "  print.stdout['n' \\n]; }\n");
}

void onEmptyTextAgainstNoTextAtAll() {
  AGREE("START { var.or-nothing.str 'a' = [**];\n"
        "  when 'a' { is 'x' { print.stdout[str:*here[* 'x' str:*]* \\n]; }\n"
        "             is nothing { print.stdout[str:*none* \\n]; } } }\n");
  AGREE("START { var.or-nothing.str 'a' = [nothing];\n"
        "  when 'a' { is 'x' { print.stdout[str:*here[* 'x' str:*]* \\n]; }\n"
        "             is nothing { print.stdout[str:*none* \\n]; } } }\n");
  AGREE("one-of 'maybe' [str 'some', nothing 'none']\n"
        "START { var.maybe 'a' = [some:**]; var.maybe 'b' = [none];\n"
        "  when 'a' { is some 'x' { print.stdout[str:*a[* 'x' str:*]* \\n]; }\n"
        "             is none { print.stdout[str:*a none* \\n]; } }\n"
        "  when 'b' { is some 'x' { print.stdout[str:*b[* 'x' str:*]* \\n]; }\n"
        "             is none { print.stdout[str:*b none* \\n]; } } }\n");
  // Joined and counted, so an empty one that owns nothing is still a value.
  AGREE("START { var.str 'e' = [**]; var.str 'j' = ['e' 'e'];\n"
        "  print.stdout[(count[loan 'j']) str:*|* 'j' str:*|* \\n]; }\n");
  // The number lives in a case's padding here, and every case has to reach
  // around it — including one holding a struct and one holding nothing.
  AGREE("struct 'p' [int64 'a', bool 'b']\n"
        "one-of 'k' [p 'x', int64 'y', nothing 'z']\n"
        "START { var.k 'a' = [x:p[*7* *true*]]; var.k 'b' = [y:*99*];\n"
        "  var.k 'c' = [z];\n"
        "  when 'a' { is x 'v' { print.stdout['v'.a str:* * 'v'.b \\n]; }\n"
        "             is y 'v' { } is z { } }\n"
        "  when 'b' { is x 'v' { } is y 'v' { print.stdout['v' \\n]; } is z { } }\n"
        "  when 'c' { is x 'v' { } is y 'v' { } is z { print.stdout[str:*z* \\n]; } } }\n");
}

void onSayingWhereAPrintGoes() {
  AGREE("START { print.stderr[str:*complaint* \\n]; }\n");
  AGREE("START { print.stdout[str:*a* \\n]; print.stderr[str:*b* \\n];\n"
        "  print.stdout[str:*c* \\n]; }\n");
  AGREE("struct 'point' [int64 'x', int64 'y']\n"
        "START { var.point 'p' = [*1* *2*];\n"
        "  print.stderr['p' str:* * 'p'.y \\n]; }\n");
  AGREE("START { var.many.int64 'ns' = [*1* *2* *3*];\n"
        "  print.stderr['ns' \\n]; print.stdout[(count[str:*ab*]) \\n]; }\n");
}

void onGroupingNamedThings() {
  // A struct whose one thing is a struct, both engines the same way. One item
  // that is a struct reads two ways — the whole thing, or the first of the
  // things it holds — and the middle layer took the first reading whenever the
  // item was a struct at all, whichever struct it was.
  AGREE("struct 'inner' [int64 'a']\nstruct 'outer' [inner 'i']\n"
        "START { var.outer 'o' = [inner[*5*]];\n"
        "  print.stdout['o'.i.a \\n]; }\n");
  AGREE("struct 'holds' [loanmut.int64 'seen']\nstruct 'wrap' [holds 'inner']\n"
        "START { var.mut.int64 'n' = [*4*];\n"
        "  var.mut.wrap 'w' = [holds[loanmut 'n']];\n"
        "  set 'w'.inner.seen = [*7*];\n"
        "  print.stdout['n' \\n]; }\n");
  // A field read and a field written, with the rest left as it was.
  AGREE("struct 'point' [int64 'x', int64 'y']\n"
        "START { var.mut.point 'p' = [*3* *4*];\n"
        "  print.stdout['p'.x str:* * 'p'.y \\n];\n"
        "  set 'p'.y = [*9*];\n"
        "  print.stdout['p'.x str:* * 'p'.y \\n]; }\n");
  // Text held by a struct is the struct's own, and let go with it.
  AGREE("struct 'tag' [str 'name', int64 'runs']\n"
        "START { var.mut.tag 't' = [*ada* *36*];\n"
        "  print.stdout['t'.name str:* * 't'.runs \\n];\n"
        "  set 't'.name = [*bob*];\n"
        "  print.stdout['t'.name str:* * (count['t'.name]) \\n]; }\n");
  // A struct of structs, read and written down a path.
  AGREE("struct 'point' [int64 'x', int64 'y']\n"
        "struct 'runner' [str 'name', point 'at']\n"
        "fn.nothing 'bump' [loanmut.runner 'r'] { set 'r'.at.x = ['r'.at.x + *1*]; }\n"
        "START { var.mut.point 'a' = [*1* *2*];\n"
        "  var.mut.runner 'r' = [*ada* move 'a'];\n"
        "  bump[loanmut 'r'];\n"
        "  print.stdout['r'.name str:* * 'r'.at.x str:* * 'r'.at.y \\n]; }\n");
  // One field handed over on its own, with the rest still there to read.
  AGREE("fn.nothing 'keep' [str 't'] { print.stdout['t' \\n]; }\n"
        "struct 'tag' [str 'name', int64 'runs']\n"
        "START { var.tag 't' = [*ada* *36*];\n"
        "  keep[move 't'.name];\n"
        "  print.stdout['t'.runs \\n]; }\n");
  // Structs in a `many`, one of them replaced.
  AGREE("struct 'tag' [str 'name']\n"
        "START { var.tag 'a' = [*ada*];\n  var.tag 'b' = [*bob*];\n"
        "  var.mut.many.tag 'ts' = [move 'a' move 'b'];\n"
        "  var.tag 'c' = [*cy*];\n"
        "  set 'ts'[*1*] = [move 'c'];\n"
        "  print.stdout['ts'[*1*].name str:* * 'ts'[*2*].name \\n]; }\n");
  // A struct behind a loan, through a function.
  AGREE("struct 'point' [int64 'x', int64 'y']\n"
        "fn.int64 'across' [loan.point 'p'] { give ['p'.x + 'p'.y]; }\n"
        "START { var.point 'p' = [*20* *22*];\n"
        "  print.stdout[(across[loan 'p']) \\n]; }\n");
  // A struct behind `or-nothing`, both ways, under `when`.
  AGREE("struct 'tag' [str 'name']\n"
        "START { var.or-nothing.tag 't' = [*ada*];\n"
        "  when 't' {\n"
        "    is 'one'   { print.stdout['one'.name \\n]; }\n"
        "    is nothing { print.stdout[str:*none* \\n]; } }\n"
        "  var.or-nothing.tag 'u' = [nothing];\n"
        "  when 'u' {\n"
        "    is 'one'   { print.stdout['one'.name \\n]; }\n"
        "    is nothing { print.stdout[str:*none* \\n]; } } }\n");
}

void onTurningNumbersIntoText() {
  AGREE("START { var.int64 'n' = [*42*];\n"
        "  var.str 's' = [str:*x = * convert-to-str['n']];\n"
        "  print.stdout['s' \\n]; }\n");
  // Every family, each exactly as print writes it: the trailing zero of a
  // decimal kept, a binary read back as it was written, all thirty-nine digits
  // of a uint128.
  AGREE("START { var.bin64 'a' = [*0.1*]; print.stdout[convert-to-str['a'] \\n]; }\n");
  AGREE("START { var.deci64 'b' = [*1.10*]; print.stdout[convert-to-str['b'] \\n]; }\n");
  AGREE("START { var.int8 'c' = [*-5*]; print.stdout[convert-to-str['c'] \\n]; }\n");
  AGREE("START { var.bool 'd' = [*true*]; print.stdout[convert-to-str['d'] \\n]; }\n");
  AGREE("START { var.uint128 'e' = [*340282366920938463463374607431768211455*];\n"
        "  print.stdout[convert-to-str['e'] \\n]; }\n");
  AGREE("START { var.bin128 'f' = [*0.5*]; print.stdout[convert-to-str['f'] \\n]; }\n");
  // Made in a function and afresh on every turn of a loop; the balance is
  // checked after, so text not let go of would be caught here.
  AGREE("fn.str 'spell' [int64 'n'] { give [convert-to-str['n']]; }\n"
        "START { var.int64 'n' = [*7*];\n"
        "  loop.range.int64 'i' = [*1*, *3*] {\n"
        "    var.str 's' = [spell['n']];\n"
        "    print.stdout['s' str:* * (count[loan 's']) \\n]; } }\n");
  // Behind a loan, which is the shape that once answered 0 from the test
  // interpreter and would not build natively; and printing a borrowed number,
  // which had been wrong the same way for as long as printing has existed.
  AGREE("fn.str 'spell' [loan.int64 'n'] { give [convert-to-str['n']]; }\n"
        "START { var.int64 'n' = [*7*];\n"
        "  var.str 's' = [spell[loan 'n']];\n"
        "  print.stdout['s' str:* * (count[loan 's']) \\n]; }\n");
  AGREE("fn.nothing 'show' [loan.int64 'a', loan.deci64 'c'] {\n"
        "  print.stdout['a' str:* * 'c' str:* * convert-to-str['c'] \\n]; }\n"
        "START { var.int64 'a' = [*300*]; var.deci64 'c' = [*1.10*];\n"
        "  show[loan 'a', loan 'c']; }\n");
  // A struct holding a borrow. Nothing had ever written one — no example, no
  // test, and the generator does not — so it was laid out with a whole number
  // where a pointer goes and would not build at all, while the interpreter read
  // it and answered. Three engines cannot disagree about a program nobody
  // writes.
  AGREE("struct 'holder' [loan.int64 'x']\n"
        "START { var.int64 'n' = [*7*]; var.holder 'h' = [loan 'n'];\n"
        "  print.stdout['h'.x \\n]; }\n");
  // Growing inside a loop, where the loop is one the compiler can run on its own
  // and write the answer in for. Growing was invisible to the pass that decides
  // what a loop touches, so a loop whose only work was growing something read as
  // a loop that just counts — and was replaced by its effect on the counter,
  // taking the growth with it. In the built program alone, which is the only one
  // given a rewritten middle layer.
  AGREE("START { var.mut.many-growing.int64 'g' = [*1* *2*];\n"
        "  var.mut.uint128 'i' = [*0*];\n"
        "  loop.while 'i' < uint128:*1* { add 'g' = [*22*]; set 'i' = ['i' + *1*]; }\n"
        "  var.mut.many.uint128 'm' = [*248* 'i'];\n"
        "  print.stdout[(count[loan 'g']) \\n]; }\n");
  AGREE("START { var.mut.many-growing.str 'w' = [];\n"
        "  loop.range.int64 'i' = [*1*, *4*] { add 'w' = [*x*]; }\n"
        "  print.stdout[(count[loan 'w']) str:*|* 'w'[*4*] \\n]; }\n");

  // A `many` that grows. A second type, because growing may move every place it
  // has — so it keeps room it is not using yet, and the room is the last field,
  // which is why counting one, reaching into one and letting one go all read it
  // exactly as they read a `many`.
  AGREE("START { var.mut.many-growing.int64 'xs' = [];\n"
        "  loop.range.int64 'i' = [*1*, *5*] { add 'xs' = ['i' x 'i']; }\n"
        "  print.stdout[(count[loan 'xs']) str:*|* 'xs'[*5*] \\n]; }\n");
  // Text, so that every place owns something and growing has to hand it over.
  AGREE("START { var.mut.many-growing.str 'w' = [];\n"
        "  var.str 't' = [*hello*];\n"
        "  add 'w' = [move 't'];\n  add 'w' = [*there*];\n"
        "  print.stdout[(count[loan 'w']) str:*|* 'w'[*1*] str:*|* 'w'[*2*] \\n]; }\n");
  // Made with places already, and grown past them: the room it took to begin
  // with runs out, and everything moves.
  AGREE("START { var.mut.many-growing.int64 'xs' = [*1* *2*];\n"
        "  loop.range.int64 'i' = [*1*, *9*] { add 'xs' = ['i']; }\n"
        "  print.stdout[(count[loan 'xs']) str:*|* 'xs'[*11*] \\n]; }\n");
  // And through a borrow, where reading one is reading a `many`.
  AGREE("fn.int64 'total' [loan.many-growing.int64 'g'] {\n"
        "  var.mut.int64 'sum' = [*0*];\n"
        "  loop.range.int64 'i' = [*1*, count['g']] {\n"
        "    set 'sum' = ['sum' + 'g'['i']];\n  }\n  give ['sum'];\n}\n"
        "START { var.mut.many-growing.int64 'xs' = [];\n"
        "  add 'xs' = [*3*]; add 'xs' = [*4*];\n"
        "  print.stdout[(total[loan 'xs']) \\n]; }\n");

  // A `many` of a `many`, which was `E0210` until there was something to build
  // it as. Brackets where an item goes make one; a name in front of them is
  // still an index, and `'g'[*1*][*2*]` reaches into what was just reached.
  AGREE("START { var.many.many.int64 'g' = [[*1* *2* *3*] [*4* *5*]];\n"
        "  print.stdout[(count[loan 'g']) str:*|* (count[loan 'g'[*1*]]) \\n];\n"
        "  print.stdout['g'[*1*][*3*] str:*|* 'g'[*2*][*1*] \\n]; }\n");
  // One row. A lone item that is already the whole array is the whole array —
  // and one row of a `many` of a `many` is a `many` too, so asking only whether
  // the item was *a* `many` put the row itself where the array goes. Letting go
  // of it then walked one `str` as though it were an array of them.
  AGREE("START { var.many.many.str 'w' = [[*ab*]];\n"
        "  print.stdout['w'[*1*][*1*] str:*|* (count[loan 'w']) \\n]; }\n");
  AGREE("START { var.many.many.int64 'g' = [[*7*]]; print.stdout['g'[*1*][*1*] \\n]; }\n");
  // And a lone item that really is the whole array still is one.
  AGREE("START { var.many.str 'a' = [*x* *y*]; var.many.str 'b' = [move 'a'];\n"
        "  print.stdout['b'[*2*] \\n]; }\n");

  // Text in one, so that every place owns something and the whole of it has to
  // be let go of a level at a time.
  AGREE("START { var.many.many.str 'w' = [[*ab* *cd*] [*ef*]];\n"
        "  print.stdout['w'[*1*][*2*] str:*|* 'w'[*2*][*1*] \\n]; }\n");
  // Writing a whole place. What goes in one may itself be several, and reading
  // it as a lone value joined two pieces of text into one and put that where a
  // `many str` goes.
  AGREE("START { var.mut.many.many.str 'w' = [[*ab*] [*ef*]];\n"
        "  set 'w'[*2*] = [*x* *y* *z*];\n"
        "  print.stdout[(count[loan 'w'[*2*]]) str:*|* 'w'[*2*][*3*] \\n]; }\n");
  // Walked both ways round, through a borrow, in a function that never sees
  // where it came from.
  AGREE("fn.int64 'total' [loan.many.many.int64 'g'] {\n"
        "  var.mut.int64 'sum' = [*0*];\n"
        "  loop.range.int64 'i' = [*1*, count['g']] {\n"
        "    loop.range.int64 'j' = [*1*, count['g'['i']]] {\n"
        "      set 'sum' = ['sum' + 'g'['i']['j']];\n"
        "    }\n  }\n  give ['sum'];\n}\n"
        "START { var.many.many.int64 'g' = [[*1* *2* *3*] [*4* *5*] [*6*]];\n"
        "  print.stdout[(total[loan 'g']) \\n]; }\n");
  // Three deep, because nothing says two.
  AGREE("START { var.many.many.many.int64 'g' = [[[*1*] [*2* *3*]] [[*4*]]];\n"
        "  print.stdout['g'[*1*][*2*][*2*] \\n]; }\n");

  // A sum going into a field that may hold nothing. What goes into one of these
  // is asked for as the thing itself — an `or-nothing bin64` is not a number,
  // so a sum asked for with the wrapper still on had nothing saying what its
  // pieces were. The same sum into a *name* of the same type was taken.
  AGREE("struct 'w' [or-nothing.bin64 'm', int64 'n']\n"
        "START { var.w 'g' = [*1.5* x *2* *2*];\n"
        "  if 'g'.m holds 'x' { print.stdout['x' \\n]; } }\n");

  // A written value going into a field that may hold nothing, at every kind of
  // number there is. A name carries its own type; one of the things a struct
  // holds is given the *field's*, so the `or-nothing` was still on it when each
  // engine asked what kind of value it was reading.
  //
  // The fast engine answered "no kind I know" and kept the digits as text,
  // which read back as zero and as `false`. The backend asked `ConstantFP` for
  // a `bin32` while holding the two-field wrapper, and was handed a
  // `ppc_fp128` to sit in a slot shaped for a `float`.
  AGREE("struct 'w' [or-nothing.int64 'm', int64 'n']\n"
        "START { var.w 'g' = [*7* *2*];\n"
        "  if 'g'.m holds 'x' { print.stdout['x' \\n]; } print.stdout['g'.n \\n]; }\n");
  AGREE("struct 'w' [or-nothing.bool 'm', int64 'n']\n"
        "START { var.w 'g' = [*true* *2*];\n"
        "  if 'g'.m holds 'x' { print.stdout['x' \\n]; } print.stdout['g'.n \\n]; }\n");
  AGREE("struct 'w' [or-nothing.bin32 'm', int64 'n']\n"
        "START { var.w 'g' = [*2.5* *2*];\n"
        "  if 'g'.m holds 'x' { print.stdout['x' \\n]; } print.stdout['g'.n \\n]; }\n");
  AGREE("struct 'w' [or-nothing.deci64 'm', int64 'n']\n"
        "START { var.w 'g' = [*1.50* *2*];\n"
        "  if 'g'.m holds 'x' { print.stdout['x' \\n]; } print.stdout['g'.n \\n]; }\n");

  // A value with a name going into a field that may hold nothing. It was taken
  // for a name — `var.or-nothing.int8 'o' = ['n'];` — and refused for one of the
  // things a struct holds, which is one rule answered two ways.
  AGREE("struct 'holds' [or-nothing.int8 'm']\n"
        "START { var.int8 'n' = [*3*]; var.holds 'g' = ['n'];\n"
        "  if 'g'.m holds 'v' { print.stdout['v' \\n]; } }\n");
  // A struct with exactly one field is where two readings meet: a lone item is
  // the whole struct, except when the struct holds one thing. Every one-field
  // struct asked for its value to be handed over, however little was in it,
  // while the same value into a struct with two fields was taken without a
  // word.
  AGREE("struct 'one' [int8 'm']\n"
        "START { var.int8 'n' = [*3*]; var.one 'g' = ['n'];\n"
        "  print.stdout['g'.m \\n]; }\n");

  // A struct holding something that may hold nothing. The interpreters ran it
  // and answered; the module the backend built was ill-formed, because a value
  // was written straight into a field shaped `{ is it there, what it is }`.
  AGREE("struct 'holds' [or-nothing.str 'm', int64 'n']\n"
        "START { var.holds 'g' = [*hi* *2*];\n"
        "  if 'g'.m holds 'got' { print.stdout['got' str:*|* 'g'.n \\n]; } }\n");
  // And the absence itself, which writes its own pair and must not be wrapped
  // a second time.
  AGREE("struct 'holds' [or-nothing.str 'm', int64 'n']\n"
        "START { var.holds 'g' = [nothing *2*];\n"
        "  if 'g'.m holds 'got' { print.stdout['got' \\n]; }\n"
        "  print.stdout['g'.n \\n]; }\n");
  // Asking a borrowed one whether it holds something reads through the borrow
  // first: a loan is a pointer, and a pointer has no first field to take out.
  AGREE("struct 'holds' [loan.or-nothing.str 'm']\n"
        "START { var.or-nothing.str 'o' = [*hello*];\n"
        "  var.holds 'g' = [loan 'o'];\n"
        "  if 'g'.m holds 'got' { print.stdout['got' \\n]; } }\n");

  // A borrowed `str` field, which asks the question the others do not: who
  // ends it. Nobody — a borrow owns nothing, whatever it borrows, and what it
  // points at is still the lender's afterwards. Read as an owned one it was
  // freed twice, and the built program stopped where the interpreters ran on.
  AGREE("struct 'holds' [str 'kept', loan.str 'seen']\n"
        "START { var.str 'a' = [*ab* *cd*]; var.str 'b' = [*x*];\n"
        "  var.holds 'h' = [move 'b' loan 'a'];\n"
        "  print.stdout['h'.kept str:*|* 'h'.seen str:*|* 'a' \\n]; }\n");
  // The same, with nothing reading it at all: the double free was in letting
  // go, so a struct nobody looks at is the sharper case.
  AGREE("struct 'holds' [str 'kept', loan.str 'seen']\n"
        "START { var.str 'a' = [*ab* *cd*];\n"
        "  var.holds 'h' = [*alpha* loan 'a'];\n"
        "  print.stdout[str:*made* \\n]; }\n");

  AGREE("struct 'both' [loan.int64 'a', str 'b']\n"
        "START { var.int64 'n' = [*4*]; var.both 'p' = [loan 'n' *hi*];\n"
        "  print.stdout['p'.a str:*|* 'p'.b \\n]; }\n");

  // Arithmetic and comparison on borrowed numbers, and a number written
  // through a loan. Every loan any program had made was a loan of text until
  // one lent a number, and the sum of two borrowed numbers was no step at all.
  AGREE("fn.nothing 'add-into' [loanmut.int64 'total', loan.int64 'step'] {\n"
        "  loop.range.int64 'i' = [*1*, *3*] { set 'total' = ['total' + 'step']; } }\n"
        "START { var.mut.int64 't' = [*0*]; var.int64 's' = [*3*];\n"
        "  add-into[loanmut 't', loan 's'];\n"
        "  print.stdout['t' \\n]; }\n");
  AGREE("fn.nothing 'scale' [loanmut.bin64 'x', loan.deci64 'd'] {\n"
        "  set 'x' = ['x' x *2.5*];\n"
        "  print.stdout['x' str:* * ('d' + deci64:*0.01*) \\n]; }\n"
        "START { var.mut.bin64 'x' = [*0.1*]; var.deci64 'd' = [*1.10*];\n"
        "  scale[loanmut 'x', loan 'd'];\n"
        "  print.stdout['x' \\n]; }\n");
  AGREE("fn.str 'order' [loan.int64 'a', loan.int64 'b'] {\n"
        "  if 'a' < 'b' { give [*less*]; }\n"
        "  if 'a' == 'b' { give [*same*]; }\n"
        "  give [*more*]; }\n"
        "START { var.int64 'p' = [*2*]; var.int64 'q' = [*10*];\n"
        "  print.stdout[(order[loan 'p', loan 'q']) str:* * (order[loan 'q', loan 'p'])"
        " str:* * (order[loan 'p', loan 'p']) \\n]; }\n");
  // And the other way, under its new name.
  AGREE("START { var.str 't' = [*250*];\n"
        "  var.or-nothing.uint8 'n' = [convert-to-number[loan 't']];\n"
        "  when 'n' {\n"
        "    is 'v'     { print.stdout['v' \\n]; }\n"
        "    is nothing { print.stdout[str:*none* \\n]; } } }\n");
}

} // namespace

// The four `[defaults]` settings, each at its other value. Every value of a
// setting is a language of its own that both engines have to agree under.
void onTheOtherValueOfEverySetting() {
  xag::Settings asksBoth;
  asksBoth.asksBoth = true;
  AGREE_UNDER("fn.bool 'loud' [str 't'] { print.stdout['t' \\n]; give [*true*]; }\n"
              "START { var.bool 'a' = [bool:*false* and loud[str:*asked*]];\n"
              "  var.bool 'b' = [bool:*true* or loud[str:*asked too*]];\n"
              "  print.stdout['a' str:* * 'b' \\n]; }\n",
              asksBoth);
  // And the default, for the difference.
  AGREE("fn.bool 'loud' [str 't'] { print.stdout['t' \\n]; give [*true*]; }\n"
        "START { var.bool 'a' = [bool:*false* and loud[str:*asked*]];\n"
        "  print.stdout['a' \\n]; }\n");

  xag::Settings floored;
  floored.floored = true;
  AGREE_UNDER("START { var.int64 'a' = [*-7*]; var.int64 'b' = [*2*];\n"
              "  var.int8 'c' = [*-128*]; var.int8 'd' = [*3*];\n"
              "  var.bin64 'x' = [*-7.5*]; var.bin64 'y' = [*2.0*];\n"
              "  var.bin128 'w' = [*7.5*]; var.bin128 'v' = [*-2.0*];\n"
              "  var.deci64 'p' = [*-6.00*]; var.deci64 'q' = [*2.0*];\n"
              "  print.stdout[('a' / 'b') str:* * ('a' mod 'b') str:* * ('c' / 'd') str:* * ('c' mod 'd') \\n];\n"
              "  print.stdout[('x' mod 'y') str:* * ('w' mod 'v') str:* * ('p' mod 'q') \\n]; }\n",
              floored);

  xag::Settings letters;
  letters.letters = true;
  AGREE_UNDER("START { var.str 't' = [*🧑‍🧑‍🧒‍🧒 🇹🇭 café*];\n"
              "  print.stdout[count[loan 't'] \\n]; }\n",
              letters);

  xag::Settings stops;
  stops.noNumberStops = true;
  // A number is a number, and is handed on.
  AGREE_UNDER("START { var.bin64 'x' = [*1.5*]; var.bin64 'y' = [*0.5*];\n"
              "  print.stdout[('x' / 'y') str:* * ('x' mod 'y') \\n]; }\n",
              stops);
  // An infinity stops, in the same words from both.
  AGREE_UNDER("START { var.bin64 'x' = [*1.5*]; var.bin64 'z' = [*0*];\n"
              "  print.stdout[str:*before* \\n];\n"
              "  print.stdout[('x' / 'z') \\n]; }\n",
              stops);
  // So does a not-a-number, and a `bin128`'s, and one that came from a sum
  // running past the largest `bin32`.
  AGREE_UNDER("START { var.bin64 'z' = [*0*]; print.stdout[('z' / 'z') \\n]; }\n", stops);
  AGREE_UNDER("START { var.bin128 'x' = [*1*]; var.bin128 'z' = [*0*];\n"
              "  print.stdout[('x' / 'z') \\n]; }\n",
              stops);
  AGREE_UNDER("START { var.bin32 'x' = [*3e38*]; print.stdout[('x' x 'x') \\n]; }\n", stops);
  // And under the default, the same programs carry on.
  AGREE("START { var.bin64 'x' = [*1.5*]; var.bin64 'z' = [*0*];\n"
        "  print.stdout[('x' / 'z') str:* * ('z' / 'z') \\n]; }\n");
}

// A type's own operators are calls, so both interpreters run them as calls.
void onATypeThatAnswersOperators() {
  AGREE("struct 'vec' [int64 'x', int64 'y']\n"
        "fn.vec '+' [loan.vec 'a', loan.vec 'b'] { give [vec[('a'.x + 'b'.x) ('a'.y + 'b'.y)]]; }\n"
        "fn.bool '<' [loan.vec 'a', loan.vec 'b'] { give ['a'.x < 'b'.x]; }\n"
        "fn.bool '==' [loan.vec 'a', loan.vec 'b'] { give [('a'.x == 'b'.x) and ('a'.y == 'b'.y)]; }\n"
        "fn.str 'convert-to-str' [loan.vec 'v'] { give [str:*(* convert-to-str['v'.x] str:*,* convert-to-str['v'.y] str:*)*]; }\n"
        "START { var.vec 'a' = [*1* *2*]; var.vec 'b' = [*10* *20*];\n"
        "  var.mut.vec 't' = [*0* *0*];\n"
        "  loop.range.int64 'i' = [*1*, *3*] { set 't' = ['t' + 'a' + 'b']; }\n"
        "  print.stdout['t' str:* * ('a' + 'b') + 'a' str:* * ('a' < 'b') str:* * ('t' == 't') \\n];\n"
        "  var.str 's' = [convert-to-str['a']]; print.stdout['s' count[loan 's'] \\n]; }\n");
  // Text made and let go inside the operator, and the answer lent on again.
  AGREE("struct 'tag' [str 'name']\n"
        "fn.tag '+' [loan.tag 'a', loan.tag 'b'] {\n"
        "  var.str 'n' = ['a'.name str:*-* 'b'.name]; give [tag[move 'n']]; }\n"
        "fn.str 'convert-to-str' [loan.tag 't'] { give ['t'.name str:*!*]; }\n"
        "START { var.tag 'a' = [*x*]; var.tag 'b' = [*y*];\n"
        "  print.stdout[('a' + 'b') + ('b' + 'a') \\n]; print.stdout['a' \\n]; }\n");
}

int main() {
  onATypeThatAnswersOperators();
  onTheOrdinaryThings();
  onEverySizeAndFamily();
  onCallsAndBorrows();
  onHoldingSeveralValues();
  onHoldingNothing();
  onChoosingBetweenCases();
  onBeingOneOfSeveralThings();
  onACaseThatOwnsSomething();
  onShowingWhatSeveralThingsHold();
  onASumThatDoesNotFit();
  onEmptyTextAgainstNoTextAtAll();
  onSayingWhereAPrintGoes();
  onGroupingNamedThings();
  onTurningNumbersIntoText();
  onTheOtherValueOfEverySetting();

  if (failures == 0)
    std::cout << "the two interpreters agree everywhere asked\n";
  return failures == 0 ? 0 : 1;
}
