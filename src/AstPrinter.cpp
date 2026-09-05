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
    case ExprKind::Index:   out << "element of '" << e.text << "'\n"; break;
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
    case StmtKind::Set:
      out << "set '" << s.name << "'\n";
      if (s.index) {
        indent(depth + 1);
        out << "at\n";
        expr(*s.index, depth + 2);
      }
      values(s.value, depth + 1);
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
    case ItemKind::Start:
      out << "START\n";
      block(i.body, 1);
      break;
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
  for (const Item &item : program.items)
    printer.item(item);
}

} // namespace xag
