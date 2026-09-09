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

// Every function called by name in a tree. Used to ask whether a generic is
// still wanted after the copies have been written: one generic can call
// another, and the call inside a generic body is not read until that body has
// been written out at a type and read for the first time.
void namesCalled(const Expr *expr, std::unordered_set<std::string> &called);

void namesCalled(const ValueList &list, std::unordered_set<std::string> &called) {
  for (const Value &value : list.values)
    for (const ExprPtr &item : value.items)
      namesCalled(item.get(), called);
}

void namesCalled(const Expr *expr, std::unordered_set<std::string> &called) {
  if (!expr)
    return;
  if (expr->kind == ExprKind::Call && expr->path.size() == 1)
    called.insert(expr->path[0]);
  for (const ExprPtr &child : expr->children)
    namesCalled(child.get(), called);
  namesCalled(expr->args, called);
}

void namesCalled(const Block &block, std::unordered_set<std::string> &called);

void namesCalled(const Stmt &s, std::unordered_set<std::string> &called) {
  namesCalled(s.index.get(), called);
  namesCalled(s.value, called);
  namesCalled(s.condition.get(), called);
  namesCalled(s.call.get(), called);
  for (const Branch &branch : s.branches) {
    namesCalled(branch.condition.get(), called);
    namesCalled(branch.body, called);
  }
  namesCalled(s.body, called);
}

void namesCalled(const Block &block, std::unordered_set<std::string> &called) {
  for (const StmtPtr &s : block.stmts)
    if (s)
      namesCalled(*s, called);
}

} // namespace

namespace {

void pruneBlock(Block &block, const CheckResult &checked, unsigned &done);

void pruneStmt(Stmt &s, const CheckResult &checked, unsigned &done) {
  for (Branch &branch : s.branches)
    pruneBlock(branch.body, checked, done);
  pruneBlock(s.body, checked, done);
}

void pruneBlock(Block &block, const CheckResult &checked, unsigned &done) {
  std::vector<StmtPtr> kept;
  kept.reserve(block.stmts.size());
  for (StmtPtr &s : block.stmts) {
    if (!s)
      continue;
    // Deeper first, so a `whichever` inside the arm that was chosen is settled
    // before that arm is lifted out of the statement holding it.
    pruneStmt(*s, checked, done);
    if (s->kind != StmtKind::Whichever) {
      kept.push_back(std::move(s));
      continue;
    }
    auto chose = checked.chosenArm.find(s.get());
    if (chose == checked.chosenArm.end() || chose->second >= s->branches.size()) {
      kept.push_back(std::move(s)); // Never reached, so never decided.
      continue;
    }
    // The arm stands where the statement stood. Its block is not kept as a
    // block: a `whichever` is not a scope of its own, it is a choice about
    // which lines are here at all.
    for (StmtPtr &inner : s->branches[chose->second].body.stmts)
      if (inner)
        kept.push_back(std::move(inner));
    ++done;
  }
  block.stmts = std::move(kept);
}

} // namespace

namespace {

// `'part'.name` becomes the field's name written out as text, and
// `'part'.value` becomes that field of the thing being walked. Anything else
// naming the turn was refused where it was written, so nothing else can be here.
//
// Every expression is reached through the slot holding it, because the thing
// being replaced is often the whole of one — `convert-to-str['part'.value]` has
// it standing alone as an argument. Walking children alone missed exactly those.
void writeInTheField(ExprPtr &slot, const std::string &part, const std::string &field,
                     const ExprPtr &walking);

void writeInTheField(ValueList &list, const std::string &part,
                     const std::string &field, const ExprPtr &walking) {
  for (Value &value : list.values)
    for (ExprPtr &item : value.items)
      writeInTheField(item, part, field, walking);
}

bool namesTheTurn(const ExprPtr &e, const std::string &part) {
  return e && e->kind == ExprKind::Field && !e->children.empty() &&
         e->children[0] && e->children[0]->kind == ExprKind::Name &&
         e->children[0]->text == part;
}

void writeInTheField(ExprPtr &slot, const std::string &part, const std::string &field,
                     const ExprPtr &walking) {
  if (!slot)
    return;
  if (namesTheTurn(slot, part)) {
    const Span at = slot->span;
    if (slot->text == "name") {
      // `str:*x*` — the field's name, as text, which is what it is.
      auto written = std::make_unique<Expr>();
      written->kind = ExprKind::Written;
      written->span = at;
      written->text = field;
      auto typed = std::make_unique<Expr>();
      typed->kind = ExprKind::Typed;
      typed->span = at;
      typed->text = "str";
      typed->children.push_back(std::move(written));
      slot = std::move(typed);
      return;
    }
    if (slot->text == "value") {
      // `'p'.thatField`, reached where it stands — so it copies, moves and
      // borrows exactly as it would had the reader written it out themselves.
      auto reach = std::make_unique<Expr>();
      reach->kind = ExprKind::Field;
      reach->span = at;
      reach->text = field;
      reach->children.push_back(clone(walking));
      slot = std::move(reach);
      return;
    }
  }
  for (ExprPtr &child : slot->children)
    writeInTheField(child, part, field, walking);
  writeInTheField(slot->args, part, field, walking);
}

void writeInTheField(Block &block, const std::string &part, const std::string &field,
                     const ExprPtr &walking);

void writeInTheField(Stmt &s, const std::string &part, const std::string &field,
                     const ExprPtr &walking) {
  writeInTheField(s.index, part, field, walking);
  writeInTheField(s.value, part, field, walking);
  writeInTheField(s.condition, part, field, walking);
  writeInTheField(s.call, part, field, walking);
  for (Branch &branch : s.branches) {
    writeInTheField(branch.condition, part, field, walking);
    writeInTheField(branch.body, part, field, walking);
  }
  writeInTheField(s.body, part, field, walking);
}

void writeInTheField(Block &block, const std::string &part, const std::string &field,
                     const ExprPtr &walking) {
  for (StmtPtr &s : block.stmts)
    if (s)
      writeInTheField(*s, part, field, walking);
}

void unrollBlock(Block &block, const CheckResult &checked, unsigned &done);

void unrollStmt(Stmt &s, const CheckResult &checked, unsigned &done) {
  for (Branch &branch : s.branches)
    unrollBlock(branch.body, checked, done);
  unrollBlock(s.body, checked, done);
}

void unrollBlock(Block &block, const CheckResult &checked, unsigned &done) {
  std::vector<StmtPtr> kept;
  kept.reserve(block.stmts.size());
  for (StmtPtr &s : block.stmts) {
    if (!s)
      continue;
    if (s->kind != StmtKind::LoopParts) {
      unrollStmt(*s, checked, done);
      kept.push_back(std::move(s));
      continue;
    }
    const auto walks = checked.walksParts.find(s.get());
    if (walks == checked.walksParts.end() || walks->second >= checked.shapes.size() ||
        s->value.values.empty() || s->value.values[0].items.empty()) {
      kept.push_back(std::move(s)); // Never reached, so never worked out.
      continue;
    }
    const Shape &shape = checked.shapes[walks->second];
    const ExprPtr &walking = s->value.values[0].items[0];
    // One copy of the body per field, in the order the struct was written in.
    // Not a scope of its own: the turns stand where the statement stood, the
    // same way a `whichever`'s arm does, because this is not a loop and there
    // is nothing to be inside.
    for (const Field &field : shape.fields) {
      Block turn = clone(s->body);
      writeInTheField(turn, s->name, field.name, walking);
      for (StmtPtr &inner : turn.stmts)
        if (inner)
          kept.push_back(std::move(inner));
    }
    ++done;
  }
  block.stmts = std::move(kept);
}

} // namespace

unsigned unroll(Program &program, const CheckResult &checked) {
  unsigned done = 0;
  for (Item &item : program.items)
    unrollBlock(item.body, checked, done);
  return done;
}

unsigned prune(Program &program, const CheckResult &checked) {
  unsigned done = 0;
  for (Item &item : program.items)
    pruneBlock(item.body, checked, done);
  return done;
}

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

  // A copy already standing is not written a second time. A generic that calls
  // itself asks for its own type again on the round that reads its body, and
  // that copy is the very one doing the asking.
  std::unordered_set<std::string> standing;
  for (const Item &item : program.items)
    if (!isGeneric(item))
      standing.insert(item.name);

  out = Program{};
  for (const Item &item : program.items) {
    if (!isGeneric(item)) {
      out.items.push_back(clone(item));
      continue;
    }
    // The generic itself is not written out here. It holds a blank that nothing
    // after this could read.
    auto found = wanted.find(item.name);
    if (found == wanted.end())
      continue;
    for (const std::string &type : found->second) {
      const std::string name = calledAs(item.name, type);
      if (!standing.insert(name).second)
        continue;
      Item copy = clone(item);
      fillTheBlank(copy, type);
      copy.name = name;
      out.items.push_back(std::move(copy));
    }
  }

  // A generic still called by name has to stay, unread blank and all, because
  // the call naming it is inside a copy that has only just been written and has
  // never been read. The next round reads that copy, learns the type, and this
  // is where the generic is waiting when it does. On the round after that
  // nothing names it any more and it goes.
  std::unordered_set<std::string> called;
  for (const Item &item : out.items)
    namesCalled(item.body, called);
  for (const Item &item : program.items)
    if (isGeneric(item) && called.count(item.name))
      out.items.push_back(clone(item));

  return true;
}

} // namespace xag
