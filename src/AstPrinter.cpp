#include "xag/Parser.h"

#include <ostream>

namespace xag {
namespace {

struct Printer {
  std::ostream &out;

  void indent(unsigned depth) {
    for (unsigned i = 0; i < depth; ++i)
      out << "  ";
  }

  void chain(const Chain &c) {
    for (unsigned i = 0; i < c.segments.size(); ++i) {
      if (i)
        out << '.';
      if (c.segments[i].isName)
        out << '\'' << c.segments[i].text << '\'';
      else
        out << c.segments[i].text;
    }
  }

  void expr(const Expr &e, unsigned depth) {
    indent(depth);
    switch (e.kind) {
    case ExprKind::Name:    out << "name '" << e.text << "'\n"; break;
    case ExprKind::Written: out << "written *" << e.text << "*\n"; break;
    case ExprKind::Escape:  out << "escape \\" << e.text << '\n'; break;
    case ExprKind::Typed:   out << "typed " << e.text << '\n'; break;
    case ExprKind::Borrow:  out << "transfer " << e.text << '\n'; break;
    case ExprKind::Index:
      if (e.children.size() > 1)
        out << "element of what was reached\n";
      else
        out << "element of '" << e.text << "'\n";
      break;
    case ExprKind::Several: out << "several\n"; break;
    case ExprKind::Nothing: out << "nothing\n"; break;
    case ExprKind::Field:   out << "field " << e.text << '\n'; break;
    case ExprKind::Unary:   out << "unary " << e.text << '\n'; break;
    case ExprKind::Binary:  out << "binary " << e.text << '\n'; break;
    case ExprKind::Group:   out << "group\n"; break;
    case ExprKind::Call:
      out << "call ";
      for (unsigned i = 0; i < e.path.size(); ++i)
        out << (i ? "." : "") << e.path[i];
      out << '\n';
      break;
    }
    for (const ExprPtr &child : e.children)
      expr(*child, depth + 1);
    if (e.kind == ExprKind::Call)
      values(e.args, depth + 1);
  }

  void values(const ValueList &list, unsigned depth) {
    for (const Value &value : list.values) {
      indent(depth);
      out << "value\n";
      for (const ExprPtr &item : value.items)
        expr(*item, depth + 1);
    }
  }

  void block(const Block &b, unsigned depth) {
    for (const StmtPtr &s : b.stmts)
      stmt(*s, depth);
  }

  void stmt(const Stmt &s, unsigned depth) {
    indent(depth);
    switch (s.kind) {
    case StmtKind::Declare:
      out << "declare ";
      chain(s.chain);
      out << " '" << s.name << "'\n";
      values(s.value, depth + 1);
      break;
    case StmtKind::Unsafe:
      out << "UNSAFE\n";
      block(s.body, depth + 1);
      break;
    case StmtKind::LoopParts:
      out << "loop.parts '" << s.name << "'\n";
      values(s.value, depth + 1);
      block(s.body, depth + 1);
      break;
    case StmtKind::Whichever:
      out << "whichever\n";
      if (s.condition)
        expr(*s.condition, depth + 1);
      for (const Branch &arm : s.branches) {
        indent(depth + 1);
        out << "is " << arm.family << '\n';
        block(arm.body, depth + 2);
      }
      break;
    case StmtKind::Add:
      out << "add '" << s.name << "'\n";
      values(s.value, depth + 1);
      break;
    case StmtKind::Set:
      out << "set '" << s.name << "'";
      for (const std::string &field : s.fields)
        out << '.' << field;
      out << '\n';
      if (s.index) {
        indent(depth + 1);
        out << "at\n";
        expr(*s.index, depth + 2);
      }
      values(s.value, depth + 1);
      break;
    case StmtKind::When:
      out << "when\n";
      if (s.condition)
        expr(*s.condition, depth + 1);
      for (const Branch &arm : s.branches) {
        indent(depth + 1);
        out << (arm.matchesNothing ? "is nothing\n" : "is '" + arm.holds + "'\n");
        block(arm.body, depth + 2);
      }
      break;
    case StmtKind::If:
      out << "if\n";
      for (const Branch &branch : s.branches) {
        indent(depth + 1);
        out << (branch.hasCondition ? "arm\n" : "otherwise\n");
        if (branch.condition)
          expr(*branch.condition, depth + 2);
        block(branch.body, depth + 2);
      }
      break;
    case StmtKind::LoopRange:
      out << "loop ";
      chain(s.chain);
      out << " '" << s.name << "'\n";
      values(s.value, depth + 1);
      block(s.body, depth + 1);
      break;
    case StmtKind::LoopWhile:
      out << "loop while\n";
      if (s.condition)
        expr(*s.condition, depth + 1);
      block(s.body, depth + 1);
      break;
    case StmtKind::Break: out << "break\n"; break;
    case StmtKind::Give:
      out << "give\n";
      values(s.value, depth + 1);
      break;
    case StmtKind::Call:
      out << "do\n";
      if (s.call)
        expr(*s.call, depth + 1);
      break;
    }
  }

  void item(const Item &i) {
    switch (i.kind) {
    case ItemKind::Struct:
      out << "struct " << i.name << '\n';
      for (const Param &field : i.params) {
        indent(1);
        chain(field.chain);
        out << " '" << field.name << "'\n";
      }
      return;
    case ItemKind::Start:
      out << "START\n";
      block(i.body, 1);
      break;
    case ItemKind::Itmt:
      out << "ITMT\n";
      block(i.body, 1);
      break;
    case ItemKind::Import:
      out << "import '" << i.name << "'\n";
      return;
    case ItemKind::OneOf:
      out << "one-of " << i.name << '\n';
      for (const Param &field : i.params) {
        indent(1);
        chain(field.chain);
        out << " '" << field.name << "'\n";
      }
      return;
    case ItemKind::Const:
      out << "const ";
      chain(i.chain);
      out << " '" << i.name << "'\n";
      values(i.value, 1);
      break;
    case ItemKind::Function:
      out << "fn ";
      chain(i.chain);
      out << ' ' << i.name << '\n';
      for (const Param &p : i.params) {
        indent(1);
        out << "param ";
        chain(p.chain);
        out << " '" << p.name << "'\n";
      }
      block(i.body, 1);
      break;
    }
  }
};

} // namespace

void print(const Program &program, std::ostream &out) {
  Printer printer{out};
  // What the file says, shown as one line saying how much of it there is. The
  // prose itself is not the tree's business, and printing it would put markdown
  // in the middle of a printed tree.
  out << "READ_ME " << program.readMe.size() << " character(s)\n";
  for (const Item &item : program.items)
    printer.item(item);
}

} // namespace xag
