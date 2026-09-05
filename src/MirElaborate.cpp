#include "xag/Mir.h"

#include <algorithm>
#include <string>
#include <vector>

namespace xag {
namespace {

// Whether a local still holds something, tracked forwards over the graph. Two
// answers are wanted at once: whether it *might* hold something on some path in,
// and whether it *certainly* does on every path in.
struct Held {
  std::vector<char> maybe;
  std::vector<char> must;

  explicit Held(std::size_t locals) : maybe(locals, 0), must(locals, 0) {}

  void set(unsigned local, bool holding) {
    maybe[local] = holding ? 1 : 0;
    must[local] = holding ? 1 : 0;
  }
  bool operator==(const Held &other) const {
    return maybe == other.maybe && must == other.must;
  }
};

class Elaborator {
public:
  explicit Elaborator(Body &body) : body_(body) {}

  void run() {
    if (body_.blocks.empty())
      return;
    droppable();
    if (droppable_.empty())
      return;
    predecessors();
    settle();
    rewrite();
  }

private:
  Body &body_;
  std::vector<unsigned> droppable_;
  std::vector<std::vector<unsigned>> from_;
  std::vector<Held> entry_;
  std::vector<Held> exit_;

  std::size_t locals() const { return body_.locals.size(); }

  void droppable() {
    for (const BasicBlock &block : body_.blocks)
      for (const Statement &s : block.statements)
        if (s.kind == StatementKind::Drop &&
            std::find(droppable_.begin(), droppable_.end(), s.place) == droppable_.end())
          droppable_.push_back(s.place);
  }

  void predecessors() {
    from_.assign(body_.blocks.size(), {});
    for (const BasicBlock &block : body_.blocks)
      for (unsigned target : block.terminator.targets)
        if (target < from_.size())
          from_[target].push_back(block.id);
  }

  // Reading an operand may take what it names; assigning gives a place something.
  static void step(const Statement &s, Held &held) {
    for (const Operand &operand : s.value.operands)
      if (operand.kind == OperandKind::Move)
        held.set(operand.local, false);
    if (s.kind == StatementKind::Drop)
      held.set(s.place, false);
    else
      held.set(s.place, true);
  }

  static void step(const Terminator &t, Held &held) {
    if (t.condition.kind == OperandKind::Move)
      held.set(t.condition.local, false);
    if (t.answers && t.answer.kind == OperandKind::Move)
      held.set(t.answer.local, false);
  }

  void settle() {
    entry_.assign(body_.blocks.size(), Held(locals()));
    exit_.assign(body_.blocks.size(), Held(locals()));

    // A parameter arrives holding something; everything else starts empty.
    for (unsigned i = 1; i <= body_.parameters && i < locals(); ++i)
      entry_[0].set(i, true);

    bool moved = true;
    while (moved) {
      moved = false;
      for (unsigned id = 0; id < body_.blocks.size(); ++id) {
        Held in(locals());
        if (id == 0) {
          in = entry_[0];
        } else if (!from_[id].empty()) {
          for (unsigned i = 0; i < locals(); ++i) {
            char anyHolds = 0, allHold = 1;
            for (unsigned predecessor : from_[id]) {
              anyHolds |= exit_[predecessor].maybe[i];
              allHold &= exit_[predecessor].must[i];
            }
            in.maybe[i] = anyHolds;
            in.must[i] = allHold;
          }
        }
        if (!(in == entry_[id])) {
          entry_[id] = in;
          moved = true;
        }

        Held out = in;
        for (const Statement &s : body_.blocks[id].statements)
          step(s, out);
        step(body_.blocks[id].terminator, out);
        if (!(out == exit_[id])) {
          exit_[id] = out;
          moved = true;
        }
      }
    }
  }

  unsigned flagFor(unsigned local, std::vector<unsigned> &flags) {
    if (flags[local] != 0)
      return flags[local];
    const unsigned id = static_cast<unsigned>(body_.locals.size());
    // Every body carries a `bool` in its table by the time a flag is wanted.
    unsigned boolType = 0;
    for (unsigned i = 0; i < body_.types.size(); ++i)
      if (body_.types[i] == "bool")
        boolType = i;
    if (boolType == 0 && (body_.types.empty() || body_.types[0] != "bool")) {
      body_.types.push_back("bool");
      boolType = static_cast<unsigned>(body_.types.size() - 1);
    }
    body_.locals.push_back(
        Local{id, TypeRef{boolType}, "holds" + std::to_string(local), true});
    flags[local] = id;
    return id;
  }

  static Statement setFlag(unsigned flag, bool value, Span span, TypeRef type) {
    Statement s;
    s.kind = StatementKind::Assign;
    s.span = span;
    s.place = flag;
    s.value = RValue{RValueKind::Use, {}, {}, 0,
                     {Operand{OperandKind::Written, 0, value ? "true" : "false", type}},
                     type};
    return s;
  }

  void rewrite() {
    std::vector<unsigned> flags(locals(), 0);

    // Which drops need a flag: those where the paths in disagree.
    for (unsigned id = 0; id < body_.blocks.size(); ++id) {
      Held held = entry_[id];
      for (const Statement &s : body_.blocks[id].statements) {
        if (s.kind == StatementKind::Drop && held.maybe[s.place] && !held.must[s.place])
          flagFor(s.place, flags);
        step(s, held);
      }
    }

    const bool anyFlags =
        std::any_of(flags.begin(), flags.end(), [](unsigned f) { return f != 0; });

    for (unsigned id = 0; id < body_.blocks.size(); ++id) {
      BasicBlock &block = body_.blocks[id];
      Held held = entry_[id];
      std::vector<Statement> kept;
      kept.reserve(block.statements.size());

      // A flag starts false, and a parameter's starts true.
      if (id == 0 && anyFlags)
        for (unsigned local = 0; local < flags.size(); ++local)
          if (flags[local] != 0)
            kept.push_back(setFlag(flags[local], local <= body_.parameters && local != 0,
                                   Span{}, body_.locals[flags[local]].type));

      for (Statement &s : block.statements) {
        const Held before = held;
        step(s, held);

        if (s.kind == StatementKind::Drop) {
          if (!before.maybe[s.place])
            continue; // certainly moved already, so there is nothing here to end
          if (!before.must[s.place]) {
            s.conditional = true;
            s.flag = flags[s.place];
          }
          kept.push_back(s);
          if (s.flag != 0)
            kept.push_back(setFlag(s.flag, false, s.span, body_.locals[s.flag].type));
          continue;
        }

        kept.push_back(s);
        if (anyFlags) {
          // Whatever this statement settled about a flagged local, say so.
          for (const Operand &operand : s.value.operands)
            if (operand.kind == OperandKind::Move && flags[operand.local] != 0)
              kept.push_back(setFlag(flags[operand.local], false, s.span,
                                     body_.locals[flags[operand.local]].type));
          if (s.place < flags.size() && flags[s.place] != 0)
            kept.push_back(
                setFlag(flags[s.place], true, s.span, body_.locals[flags[s.place]].type));
        }
      }
      block.statements = std::move(kept);
    }
  }
};

} // namespace

void elaborate(Mir &mir) {
  for (Body &body : mir.bodies)
    Elaborator(body).run();
}

} // namespace xag
