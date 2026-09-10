#include "xag/Typed.h"

#include "xag/Check.h"
#include "xag/Lexer.h"
#include "xag/Parser.h"
#include "as_file.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                        \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #cond "\n";            \
      ++failures;                                                                        \
    }                                                                                    \
  } while (false)

// The tree, printed. Everything here asks about what is written in it rather
// than about node kinds, because what the printout says is what the passes
// reading the tree will see.
std::string tree(const std::string &text) {
  const xag::Source source("test.xag", xag::asFile(text));
  const xag::LexResult lexed = xag::lex(source);
  const xag::ParseResult parsed = xag::parse(source, lexed.tokens);
  const xag::CheckResult checked = xag::check(source, parsed.program);
  if (!lexed.ok() || !parsed.ok() || !checked.ok())
    return "(not checked)";
  return xag::printed(xag::typedTree(source, parsed.program, checked).program);
}

bool says(const std::string &text, const std::string &wanted) {
  return tree(text).find(wanted) != std::string::npos;
}

// Nothing in the tree is left unknown. An `Unknown` here would be the checker's
// answer going missing between the map it was written into and the node that
// asked, which is the whole failure this layer exists to make impossible.
void everythingCarriesItsType() {
  const char *programs[] = {
      "START { var.int64 'n' = [*7*];\n  print.stdout['n' \\n]; }\n",
      "struct 'point' [int64 'x', int64 'y']\n"
      "START { var.point 'p' = [*1* *2*];\n  print.stdout['p'.x \\n]; }\n",
      "one-of 'answer' [int64 'ok', nothing 'no']\n"
      "START { var.answer 'a' = [ok:*7*];\n"
      "  when 'a' { is ok 'n' { print.stdout['n' \\n]; } is no { } } }\n",
      "START { var.many.many.int64 'g' = [[*1* *2*] [*3*]];\n"
      "  print.stdout['g'[*1*][*2*] \\n]; }\n",
      "fn.or-nothing.int64 'half' [int64 'n'] {\n"
      "  if 'n' == *0* { give [nothing]; }\n  give ['n' / *2*]; }\n"
      "START { when half[*4*] { is 'v' { print.stdout['v' \\n]; } is nothing { } } }\n",
  };
  for (const char *one : programs) {
    const std::string said = tree(one);
    CHECK(said != "(not checked)");
    if (said.find(": ?") != std::string::npos) {
      std::cerr << "FAIL a node came out unknown:\n" << said;
      ++failures;
    }
  }
}

// The same notation says two things, and the tree has already told them apart:
// one is a written value wearing its type, the other is a case of a `one-of`.
void aCaseAndAWrittenValueAreDifferentNodes() {
  CHECK(says("START { print.stdout[int32:*161* \\n]; }\n", "written 161 : int32"));
  CHECK(says("one-of 'answer' [int64 'ok', nothing 'no']\n"
             "START { var.answer 'a' = [ok:*7*];\n"
             "  when 'a' { is ok 'n' { } is no { } } }\n",
             "case ok #0 : answer"));
  // The one that holds nothing is a case too, and says which.
  CHECK(says("one-of 'answer' [int64 'ok', nothing 'no']\n"
             "START { var.answer 'a' = [no];\n"
             "  when 'a' { is ok 'n' { } is no { } } }\n",
             "case no #1 : answer"));
}

// A word before a bracket is a call or a struct made where it stands, and which
// it is was settled before this.
void aStructMadeWhereItStandsIsNotACall() {
  CHECK(says("struct 'point' [int64 'x', int64 'y']\n"
             "struct 'line' [point 'from', point 'to']\n"
             "START { var.line 'l' = [point[*1* *2*] point[*3* *4*]];\n"
             "  print.stdout['l'.from.x \\n]; }\n",
             "made point : point"));
  CHECK(says("START { var.str 's' = [*hello*];\n"
             "  var.int64 'n' = [count[loan 's']];\n"
             "  print.stdout['n' \\n]; }\n",
             "call count : int64"));
}

// A field and a `when` arm are numbers here, the way they are everywhere below
// the checker. A name is what the reader wrote; a number is what the machine
// wants, and working it out twice is what this layer is for.
void namesBecomeNumbers() {
  CHECK(says("struct 'point' [int64 'x', int64 'y']\n"
             "START { var.point 'p' = [*1* *2*];\n"
             "  print.stdout['p'.y \\n]; }\n",
             "field y #1 : int64"));
  CHECK(says("one-of 'answer' [int64 'ok', bool 'flag', nothing 'no']\n"
             "START { var.answer 'a' = [flag:*true*];\n"
             "  when 'a' { is ok 'n' { } is flag 'b' { } is no { } } }\n",
             "arm flag #1 'b' : bool"));
}

// Brackets that only group are gone: grouping is the shape of the tree, and a
// node that says nothing but "there were brackets here" is one more thing for
// every pass below to walk past.
void bracketsThatOnlyGroupAreGone() {
  const std::string said = tree("START { var.int64 'n' = [(*1* + *2*) x *3*];\n"
                                "  print.stdout['n' \\n]; }\n");
  CHECK(said.find("group") == std::string::npos);
  CHECK(said.find("binary x : int64") != std::string::npos);
  CHECK(said.find("binary + : int64") != std::string::npos);
}

} // namespace

int main() {
  everythingCarriesItsType();
  aCaseAndAWrittenValueAreDifferentNodes();
  aStructMadeWhereItStandsIsNotACall();
  namesBecomeNumbers();
  bracketsThatOnlyGroupAreGone();

  if (failures == 0)
    std::cout << "all typed tree tests passed\n";
  return failures == 0 ? 0 : 1;
}
