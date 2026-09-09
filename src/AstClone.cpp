#include "xag/AstClone.h"

#include "xag/Check.h"

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
    ChainSegment &seg = chain.segments[i];
    if (seg.isName || seg.text != "any")
      continue;
    seg.text = std::string(spelled);
    ++filled;
    // What the blank asked for goes with it. `any.number` filled in at `int64`
    // is `int64`, not `int64.number` — the question has been answered, and the
    // word asking it has nothing left to say. Leaving it behind wrote out a
    // chain the checker then refused, naming a word the reader never typed
    // beside that type.
    if (i + 1 < chain.segments.size() && !chain.segments[i + 1].isName &&
        namesFamily(chain.segments[i + 1].text)) {
      seg.span.end = chain.segments[i + 1].span.end;
      chain.segments.erase(chain.segments.begin() + static_cast<long>(i) + 1);
    }
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
