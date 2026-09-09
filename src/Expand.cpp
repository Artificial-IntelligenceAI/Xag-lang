#include "xag/Expand.h"

#include "xag/AstClone.h"

#include <unordered_map>
#include <unordered_set>

namespace xag {
namespace {

bool blankIn(const Chain &chain) {
  for (const ChainSegment &seg : chain.segments)
    if (!seg.isName && seg.text == "any")
      return true;
  return false;
}

bool isGeneric(const Item &item) {
  if (item.kind != ItemKind::Function)
    return false;
  if (blankIn(item.chain))
    return true;
  for (const Param &param : item.params)
    if (blankIn(param.chain))
      return true;
  return false;
}

std::string calledAs(const std::string &what, const std::string &type) {
  return what + "$" + type;
}

// Every call in a tree, pointed at the copy it meant rather than the generic.
//
// A call the checker never reached — inside a branch it did not walk, say — is
// left as it was, and the name it names will not be there. That is a mistake
// reported by the check that comes after this rather than a silence.
void pointCallsAt(Expr *expr, const CheckResult &checked);

void pointCallsAt(ValueList &list, const CheckResult &checked) {
  for (Value &value : list.values)
    for (ExprPtr &item : value.items)
      pointCallsAt(item.get(), checked);
}

void pointCallsAt(Expr *expr, const CheckResult &checked) {
  if (!expr)
    return;
  if (expr->kind == ExprKind::Call && expr->path.size() == 1) {
    auto found = checked.blankAt.find(expr);
    if (found != checked.blankAt.end())
      expr->path[0] = calledAs(expr->path[0], found->second);
  }
  for (ExprPtr &child : expr->children)
    pointCallsAt(child.get(), checked);
  pointCallsAt(expr->args, checked);
}

void pointCallsAt(Block &block, const CheckResult &checked);

void pointCallsAt(Stmt &s, const CheckResult &checked) {
  pointCallsAt(s.index.get(), checked);
  pointCallsAt(s.value, checked);
  pointCallsAt(s.condition.get(), checked);
  pointCallsAt(s.call.get(), checked);
  for (Branch &branch : s.branches) {
    pointCallsAt(branch.condition.get(), checked);
    pointCallsAt(branch.body, checked);
  }
  pointCallsAt(s.body, checked);
}

void pointCallsAt(Block &block, const CheckResult &checked) {
  for (StmtPtr &s : block.stmts)
    if (s)
      pointCallsAt(*s, checked);
}

} // namespace

bool expand(Program &program, const CheckResult &checked, Program &out) {
  std::unordered_set<std::string> generic;
  for (const Item &item : program.items)
    if (isGeneric(item))
      generic.insert(item.name);
  if (generic.empty())
    return false;

  // Before anything is copied, and on the tree the checker actually walked.
  for (Item &item : program.items)
    pointCallsAt(item.body, checked);

  // What each generic was called with, in the order it was first met, so that a
  // file compiles the same way twice.
  std::unordered_map<std::string, std::vector<std::string>> wanted;
  for (const auto &[what, type] : checked.instantiations)
    if (generic.count(what))
      wanted[what].push_back(type);

  out = Program{};
  for (const Item &item : program.items) {
    if (!isGeneric(item)) {
      out.items.push_back(clone(item));
      continue;
    }
    // The generic itself is not written out. Nothing calls it by that name any
    // more, and it holds a blank that nothing after this could read.
    auto found = wanted.find(item.name);
    if (found == wanted.end())
      continue;
    for (const std::string &type : found->second) {
      Item copy = clone(item);
      fillTheBlank(copy, type);
      copy.name = calledAs(item.name, type);
      out.items.push_back(std::move(copy));
    }
  }
  return true;
}

} // namespace xag
