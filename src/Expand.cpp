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

// A turn is a real value with a real type: a struct of a `str` called `name` and
// whatever the field holds, called `value`. So each turn declares one, and
// `'part'.name` and `'part'.value` are then ordinary field reads — nothing
// special anywhere, and the turn can be handed about like anything else.
//
// Writing the two reads in directly was tried first and does less: it cannot
// hand the turn over, because there is nothing to hand.

// Every turn declares its own, because the turns stand side by side in one
// scope rather than each in a block of its own — two turns declaring `'part'`
// would be one name declared twice. `$` is not a word character, so nothing a
// reader writes can collide.
std::string turnCalled(unsigned which) {
  return "part$" + std::to_string(which);
}

void renameTheTurn(Expr *e, const std::string &from, const std::string &to);

void renameTheTurn(ValueList &list, const std::string &from, const std::string &to) {
  for (Value &value : list.values)
    for (ExprPtr &item : value.items)
      renameTheTurn(item.get(), from, to);
}

void renameTheTurn(Expr *e, const std::string &from, const std::string &to) {
  if (!e)
    return;
  if (e->kind == ExprKind::Name && e->text == from)
    e->text = to;
  for (ExprPtr &child : e->children)
    renameTheTurn(child.get(), from, to);
  renameTheTurn(e->args, from, to);
}

void renameTheTurn(Block &block, const std::string &from, const std::string &to);

void renameTheTurn(Stmt &s, const std::string &from, const std::string &to) {
  if (s.name == from)
    s.name = to;
  renameTheTurn(s.index.get(), from, to);
  renameTheTurn(s.value, from, to);
  renameTheTurn(s.condition.get(), from, to);
  renameTheTurn(s.call.get(), from, to);
  for (Branch &branch : s.branches) {
    renameTheTurn(branch.condition.get(), from, to);
    renameTheTurn(branch.body, from, to);
  }
  renameTheTurn(s.body, from, to);
}

void renameTheTurn(Block &block, const std::string &from, const std::string &to) {
  for (StmtPtr &st : block.stmts)
    if (st)
      renameTheTurn(*st, from, to);
}

ChainSegment word(Span at, std::string text) {
  return ChainSegment{at, std::move(text), false};
}

// What a turn of this field looks like: `[str 'name', loan.T 'value']`.
//
// `value` is lent rather than held, because the thing being walked keeps its
// places — taking one out would leave a hole, and there is no taking anything
// out of something only borrowed in the first place.
std::string partStructFor(const Program &program, std::vector<Item> &making,
                          const std::string &shapeName, const Param &field, Span at) {
  // `$` between, not a dot: a type is spelled with dots, and a blank filled in
  // with one of these is written back into a chain by splitting on them. A turn
  // called `part$point.x` came back as `part$point` holding an `x`.
  const std::string name = "part$" + shapeName + "$" + field.name;
  for (const Item &item : program.items)
    if (item.name == name)
      return name; // One per field, however many walks reach it.
  for (const Item &item : making)
    if (item.name == name)
      return name;

  Item made;
  made.kind = ItemKind::Struct;
  made.span = at;
  made.nameSpan = at;
  made.name = name;

  Param called;
  called.span = at;
  called.nameSpan = at;
  called.name = "name";
  called.chain.span = at;
  called.chain.segments.push_back(word(at, "str"));
  made.params.push_back(std::move(called));

  Param holds;
  holds.span = at;
  holds.nameSpan = at;
  holds.name = "value";
  holds.chain.span = at;
  bool alreadyLent = false;
  for (const ChainSegment &seg : field.chain.segments)
    alreadyLent = alreadyLent || (!seg.isName && (seg.text == "loan" ||
                                                  seg.text == "loanmut"));
  if (!alreadyLent)
    holds.chain.segments.push_back(word(at, "loan"));
  for (const ChainSegment &seg : field.chain.segments)
    holds.chain.segments.push_back(ChainSegment{at, seg.text, seg.isName});
  made.params.push_back(std::move(holds));

  making.push_back(std::move(made));
  return name;
}

// `var.<that struct> 'part$N' = [str:*field* loan <what is walked>.<field>];`
StmtPtr declareTheTurn(const std::string &partStruct, const std::string &called,
                       const std::string &field, const ExprPtr &walking, Span at) {
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Declare;
  s->span = at;
  s->nameSpan = at;
  s->name = called;
  s->chain.span = at;
  s->chain.segments.push_back(word(at, "var"));
  s->chain.segments.push_back(word(at, partStruct));

  auto written = std::make_unique<Expr>();
  written->kind = ExprKind::Written;
  written->span = at;
  written->text = field;
  auto asText = std::make_unique<Expr>();
  asText->kind = ExprKind::Typed;
  asText->span = at;
  asText->text = "str";
  asText->children.push_back(std::move(written));

  auto reach = std::make_unique<Expr>();
  reach->kind = ExprKind::Field;
  reach->span = at;
  reach->text = field;
  reach->children.push_back(clone(walking));
  auto lend = std::make_unique<Expr>();
  lend->kind = ExprKind::Borrow;
  lend->span = at;
  lend->text = "loan";
  lend->children.push_back(std::move(reach));

  Value one;
  one.span = at;
  one.items.push_back(std::move(asText));
  one.items.push_back(std::move(lend));
  s->value.span = at;
  s->value.values.push_back(std::move(one));
  return s;
}

void unrollBlock(const Program &program, std::vector<Item> &making, Block &block,
                 const CheckResult &checked, unsigned &done);

void unrollStmt(const Program &program, std::vector<Item> &making, Stmt &s,
                const CheckResult &checked, unsigned &done) {
  for (Branch &branch : s.branches)
    unrollBlock(program, making, branch.body, checked, done);
  unrollBlock(program, making, s.body, checked, done);
}

// The fields of a struct as they were written, which is where the type of each
// one is spelled. The checker's own record holds types rather than chains, and a
// chain is what has to be written into a turn's declaration.
const Item *structWritten(const Program &program, const std::string &name) {
  for (const Item &item : program.items)
    if (item.kind == ItemKind::Struct && item.name == name)
      return &item;
  return nullptr;
}

void unrollBlock(const Program &program, std::vector<Item> &making, Block &block,
                 const CheckResult &checked, unsigned &done) {
  std::vector<StmtPtr> kept;
  kept.reserve(block.stmts.size());
  for (StmtPtr &s : block.stmts) {
    if (!s)
      continue;
    if (s->kind != StmtKind::LoopParts) {
      unrollStmt(program, making, *s, checked, done);
      kept.push_back(std::move(s));
      continue;
    }
    const auto walks = checked.walksParts.find(s.get());
    if (walks == checked.walksParts.end() || walks->second >= checked.shapes.size() ||
        s->value.values.empty() || s->value.values[0].items.empty()) {
      kept.push_back(std::move(s)); // Never reached, so never worked out.
      continue;
    }
    const Item *written = structWritten(program, checked.shapes[walks->second].name);
    if (!written) {
      kept.push_back(std::move(s));
      continue;
    }
    // Taken out whole before anything else is written. Each turn writes its own
    // struct into this same list, and a reference into a list that grows is a
    // reference to wherever it used to be.
    const std::string shapeName = written->name;
    std::vector<Param> fields;
    for (const Param &field : written->params) {
      Param one;
      one.span = field.span;
      one.nameSpan = field.nameSpan;
      one.name = field.name;
      one.chain = field.chain;
      fields.push_back(std::move(one));
    }
    const ExprPtr &walking = s->value.values[0].items[0];
    // One copy of the body per field, in the order the struct was written in.
    // Not a scope of its own: the turns stand where the statement stood, the
    // same way a `whichever`'s arm does, because this is not a loop and there
    // is nothing to be inside. Which is exactly why each turn's own name for the
    // field has to differ from the last one's — they stand side by side in one
    // scope, and one name declared twice is one name declared twice.
    for (const Param &field : fields) {
      const std::string partStruct = partStructFor(program, making, shapeName, field, s->span);
      const std::string called = turnCalled(done);
      Block turn = clone(s->body);
      renameTheTurn(turn, s->name, called);
      kept.push_back(declareTheTurn(partStruct, called, field.name, walking, s->span));
      for (StmtPtr &inner : turn.stmts)
        if (inner)
          kept.push_back(std::move(inner));
      ++done;
    }
  }
  block.stmts = std::move(kept);
}

} // namespace

unsigned unroll(Program &program, const CheckResult &checked) {
  unsigned done = 0;
  // The turns' own structs are gathered aside and written in at the end. Adding
  // them as they were found grew the very list being walked, and the block being
  // walked was a reference into it — so a struct with two fields wrote the first
  // turn out and then read freed memory looking for the second.
  std::vector<Item> making;
  for (Item &item : program.items)
    unrollBlock(program, making, item.body, checked, done);
  for (Item &made : making)
    program.items.push_back(std::move(made));
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
