#include "xag/AstClone.h"

#include "xag/Check.h"

#include <vector>

namespace xag {

ExprPtr clone(const ExprPtr &expr) {
  if (!expr)
    return nullptr;
  auto out = std::make_unique<Expr>();
  out->kind = expr->kind;
  out->span = expr->span;
  out->text = expr->text;
  out->path = expr->path;
  out->args = clone(expr->args);
  out->children.reserve(expr->children.size());
  for (const ExprPtr &child : expr->children)
    out->children.push_back(clone(child));
  return out;
}

Value clone(const Value &value) {
  Value out;
  out.span = value.span;
  out.items.reserve(value.items.size());
  for (const ExprPtr &item : value.items)
    out.items.push_back(clone(item));
  return out;
}

ValueList clone(const ValueList &list) {
  ValueList out;
  out.span = list.span;
  out.values.reserve(list.values.size());
  for (const Value &value : list.values)
    out.values.push_back(clone(value));
  return out;
}

Branch clone(const Branch &branch) {
  Branch out;
  out.span = branch.span;
  out.hasCondition = branch.hasCondition;
  out.condition = clone(branch.condition);
  out.holds = branch.holds;
  out.holdsSpan = branch.holdsSpan;
  out.matchesNothing = branch.matchesNothing;
  out.family = branch.family;
  out.familySpan = branch.familySpan;
  out.body = clone(branch.body);
  return out;
}

StmtPtr clone(const StmtPtr &stmt) {
  if (!stmt)
    return nullptr;
  auto out = std::make_unique<Stmt>();
  out->kind = stmt->kind;
  out->span = stmt->span;
  out->chain = stmt->chain;
  out->nameSpan = stmt->nameSpan;
  out->name = stmt->name;
  out->index = clone(stmt->index);
  out->fields = stmt->fields;
  out->fieldSpans = stmt->fieldSpans;
  out->value = clone(stmt->value);
  out->condition = clone(stmt->condition);
  out->holds = stmt->holds;
  out->holdsSpan = stmt->holdsSpan;
  out->branches.reserve(stmt->branches.size());
  for (const Branch &branch : stmt->branches)
    out->branches.push_back(clone(branch));
  out->body = clone(stmt->body);
  out->call = clone(stmt->call);
  return out;
}

Block clone(const Block &block) {
  Block out;
  out.span = block.span;
  out.stmts.reserve(block.stmts.size());
  for (const StmtPtr &s : block.stmts)
    out.stmts.push_back(clone(s));
  return out;
}

Item clone(const Item &item) {
  Item out;
  out.kind = item.kind;
  out.span = item.span;
  out.chain = item.chain;
  out.nameSpan = item.nameSpan;
  out.name = item.name;
  out.params = item.params;
  out.value = clone(item.value);
  out.body = clone(item.body);
  return out;
}

namespace {

unsigned fillChain(Chain &chain, std::string_view spelled) {
  unsigned filled = 0;
  for (std::size_t i = 0; i < chain.segments.size(); ++i) {
    if (chain.segments[i].isName || chain.segments[i].text != "any")
      continue;
    // Every segment written here carries the blank's own span: the blank is
    // where the reader wrote this, and where anything said about it should
    // point.
    const Span at = chain.segments[i].span;

    // What the blank asked for goes with it, and goes first. `any.number`
    // filled in at `int64` is `int64`, not `int64.number` — the question has
    // been answered, and the word asking it has nothing left to say. Taken off
    // before anything is written in, so the indices below are counted against a
    // chain that is not about to change under them.
    if (i + 1 < chain.segments.size() && !chain.segments[i + 1].isName &&
        namesFamily(chain.segments[i + 1].text))
      chain.segments.erase(chain.segments.begin() + static_cast<long>(i) + 1);

    // A type is a chain fragment rather than a word: `many.int64` is two
    // segments and `or-nothing.many.point` is three. Written in as one segment
    // they became a word no reader ever wrote and no checker ever knew — which
    // is why a blank filled in with a `many` came out spelled `unknown`.
    std::vector<std::string> parts;
    for (std::size_t from = 0;;) {
      const std::size_t dot = spelled.find('.', from);
      const std::size_t to = dot == std::string_view::npos ? spelled.size() : dot;
      parts.emplace_back(spelled.substr(from, to - from));
      if (dot == std::string_view::npos)
        break;
      from = dot + 1;
    }
    // How a thing is held belongs to the chain, and a chain that already says
    // it does not say it twice. `loan.any` filled in with `loan.int64` is
    // `loan.int64`, not `loan.loan.int64`.
    if (parts.size() > 1 && (parts.front() == "loan" || parts.front() == "loanmut")) {
      bool alreadySaid = false;
      for (std::size_t at = 0; at < i; ++at)
        alreadySaid = alreadySaid || (!chain.segments[at].isName &&
                                      (chain.segments[at].text == "loan" ||
                                       chain.segments[at].text == "loanmut"));
      if (alreadySaid)
        parts.erase(parts.begin());
    }
    chain.segments[i].text = parts.front();
    ++filled;
    for (std::size_t extra = 1; extra < parts.size(); ++extra)
      chain.segments.insert(chain.segments.begin() + static_cast<long>(i + extra),
                            ChainSegment{at, parts[extra], false});
    i += parts.size() - 1;
  }
  return filled;
}

unsigned fillBlock(Block &block, std::string_view spelled);

unsigned fillStmt(Stmt &s, std::string_view spelled) {
  unsigned filled = fillChain(s.chain, spelled);
  for (Branch &branch : s.branches)
    filled += fillBlock(branch.body, spelled);
  filled += fillBlock(s.body, spelled);
  return filled;
}

unsigned fillBlock(Block &block, std::string_view spelled) {
  unsigned filled = 0;
  for (StmtPtr &s : block.stmts)
    if (s)
      filled += fillStmt(*s, spelled);
  return filled;
}

} // namespace

unsigned fillTheBlank(Item &item, std::string_view spelled) {
  unsigned filled = fillChain(item.chain, spelled);
  for (Param &param : item.params)
    filled += fillChain(param.chain, spelled);
  filled += fillBlock(item.body, spelled);
  return filled;
}

} // namespace xag
