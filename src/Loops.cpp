#include "xag/Loops.h"

#include "xag/Interpret.h"

#include <algorithm>
#include <set>

namespace xag {
namespace {

// Every block a terminator can go to.
std::vector<unsigned> goesTo(const BasicBlock &block) {
  const Terminator &end = block.terminator;
  if (end.kind == TerminatorKind::Return)
    return {};
  return end.targets;
}

// A loop, found the way the shape of the middle layer allows: a block that
// jumps back to one at or before itself. Everything `MirBuild` writes is
// structured, so a back edge is exactly that and nothing else is.
struct Circle {
  unsigned header = 0;
  std::set<unsigned> blocks;
};

// The blocks that can reach the latch without going back through the header,
// plus the header. Walked backwards from the latch, which is the definition
// rather than a guess about how the blocks happen to be numbered.
std::set<unsigned> around(const Body &body, unsigned header, unsigned latch) {
  std::vector<std::vector<unsigned>> before(body.blocks.size());
  for (const BasicBlock &block : body.blocks)
    for (unsigned to : goesTo(block))
      if (to < before.size())
        before[to].push_back(block.id);

  std::set<unsigned> inside{header, latch};
  std::vector<unsigned> asking{latch};
  while (!asking.empty()) {
    const unsigned at = asking.back();
    asking.pop_back();
    if (at == header)
      continue;
    for (unsigned from : before[at])
      if (inside.insert(from).second)
        asking.push_back(from);
  }
  return inside;
}

// Whether the header can still get to the block that jumps back to it. A jump
// back is not a loop on its own: once a loop has been answered and its header
// sends everything straight past it, the blocks left behind still jump back to
// a header that no longer reaches them, and taking that for a loop had this
// pass answering the same dead loop again every time it was asked.
bool reaches(const Body &body, unsigned from, unsigned to) {
  std::vector<bool> seen(body.blocks.size(), false);
  std::vector<unsigned> asking{from};
  while (!asking.empty()) {
    const unsigned at = asking.back();
    asking.pop_back();
    if (at >= body.blocks.size() || seen[at])
      continue;
    seen[at] = true;
    for (unsigned next : goesTo(body.blocks[at])) {
      if (next == to)
        return true;
      asking.push_back(next);
    }
  }
  return false;
}

std::vector<Circle> circlesIn(const Body &body) {
  std::vector<Circle> found;
  for (const BasicBlock &block : body.blocks)
    for (unsigned to : goesTo(block))
      // A loop told not to be run while compiling is not taken out either.
      // Taking it out and running it on its own is the same time spent.
      if (to <= block.id && !body.blocks[to].noItmt && reaches(body, to, block.id)) {
        Circle one;
        one.header = to;
        one.blocks = around(body, to, block.id);
        found.push_back(std::move(one));
      }
  return found;
}

// Whether an assignment can be made again somewhere else, out of what is
// written in it and nothing else.
//
// This is what decides whether a loop can be taken out, and it is about the
// *value* rather than the type. A `many` of written numbers can be built again
// — the loop then owns a copy of its own and shares nothing with the program it
// came from, which was the whole worry. A `many` that was read, or worked out,
// or handed over from somewhere cannot.
bool canBeMadeAgain(const Statement &s) {
  if (s.kind != StatementKind::Assign || !s.parts.empty())
    return false;
  const RValue &value = s.value;
  const bool shapes = value.kind == RValueKind::Use || value.kind == RValueKind::Collect ||
                      value.kind == RValueKind::Group || value.kind == RValueKind::Fill;
  if (!shapes)
    return false;
  if (value.kind == RValueKind::Use && value.operands.size() != 1)
    return false;
  for (const Operand &one : value.operands)
    if (one.kind != OperandKind::Written)
      return false;
  return !value.operands.empty();
}

void readsOf(const Operand &operand, std::vector<unsigned> &out) {
  if (operand.kind != OperandKind::Written)
    out.push_back(operand.local);
}

void readsOf(const RValue &value, std::vector<unsigned> &out) {
  if (value.kind == RValueKind::Ref)
    out.push_back(value.local);
  for (const Operand &one : value.operands)
    readsOf(one, out);
}

// Whether a statement could change what a local holds.
//
// Three ways, and only the first is an assignment. Writing one place of a
// `many` is a `Store` and leaves the name alone. Lending a name out for writing
// hands the changing to somebody else, and what comes back is not what went in.
// Counting only assignments meant both of those were invisible: a `many` filled
// in before a loop was taken out as it was first written, and a loop writing
// through a loan was answered as though it had written nothing.
bool couldChange(const Statement &s, unsigned id) {
  if ((s.kind == StatementKind::Assign || s.kind == StatementKind::Store) &&
      s.place == id)
    return true;
  return s.value.kind == RValueKind::Ref && s.value.op == "loanmut" &&
         s.value.local == id;
}

// The one change to a local outside the loop, and how many there were. More
// than one and no single value reaches the loop; none and there is nothing to
// put back.
const Statement *onlyOneOutside(const Body &body, const std::set<unsigned> &loop,
                                unsigned id, unsigned &howMany) {
  const Statement *only = nullptr;
  howMany = 0;
  for (const BasicBlock &block : body.blocks) {
    if (loop.count(block.id))
      continue;
    for (const Statement &s : block.statements)
      if (couldChange(s, id)) {
        ++howMany;
        only = &s;
      }
  }
  return only;
}

// The statement that would put a local back the way the loop finds it.
//
// Usually the assignment itself. Sometimes the thing was built into a temporary
// and handed over — `_6 = fill(*10*, *4*)` and then `_5'xs' = move _6`, which is
// how `fill` and every other list is written down — and then it is the
// statement that built it, aimed at where it was going.
bool putBack(const Body &body, const std::set<unsigned> &loop, unsigned id,
             Statement &out) {
  unsigned howMany = 0;
  const Statement *only = onlyOneOutside(body, loop, id, howMany);
  if (howMany != 1 || !only)
    return false;
  if (canBeMadeAgain(*only)) {
    out = *only;
    return true;
  }
  if (only->parts.empty() && only->value.kind == RValueKind::Use &&
      only->value.operands.size() == 1 &&
      only->value.operands[0].kind != OperandKind::Written) {
    unsigned again = 0;
    const Statement *built =
        onlyOneOutside(body, loop, only->value.operands[0].local, again);
    if (again == 1 && built && canBeMadeAgain(*built)) {
      out = *built;
      out.place = id;
      return true;
    }
  }
  return false;
}

} // namespace

std::vector<Lifted> loopsThatStandAlone(const Mir &mir) {
  std::vector<Lifted> out;
  for (const Body &body : mir.bodies) {
    for (const Circle &circle : circlesIn(body)) {
      // Everything the loop touches, and everything it is entered with.
      std::set<unsigned> touched;
      std::set<unsigned> needed;
      bool plain = true;
      for (unsigned id : circle.blocks) {
        std::set<unsigned> writtenHere;
        const BasicBlock &block = body.blocks[id];
        for (const Statement &s : block.statements) {
          if (s.kind == StatementKind::Assign && s.value.kind == RValueKind::Call)
            plain = false;
          std::vector<unsigned> read;
          // Only what this kind of statement actually has. An operand nobody
          // filled in still says `copy _0`, and reading those had every loop
          // touching the answer slot — which is a `nothing`, so nothing ever
          // stood alone.
          if (s.kind == StatementKind::Assign)
            readsOf(s.value, read);
          if (s.kind == StatementKind::Store) {
            readsOf(s.value, read);
            readsOf(s.at, read);
          }
          if (s.conditional)
            read.push_back(s.flag);
          if (s.kind == StatementKind::Drop || s.kind == StatementKind::Store)
            read.push_back(s.place);
          for (unsigned one : read) {
            touched.insert(one);
            if (!writtenHere.count(one))
              needed.insert(one);
          }
          if (s.kind == StatementKind::Assign) {
            touched.insert(s.place);
            writtenHere.insert(s.place);
          }
        }
        std::vector<unsigned> read;
        if (block.terminator.kind == TerminatorKind::Switch)
          readsOf(block.terminator.condition, read);
        if (block.terminator.kind == TerminatorKind::Return && block.terminator.answers)
          readsOf(block.terminator.answer, read);
        for (unsigned one : read) {
          touched.insert(one);
          if (!writtenHere.count(one))
            needed.insert(one);
        }
      }
      if (!plain)
        continue;

      // What each of those is entered with: one assignment outside the loop,
      // and that assignment one that can be made again out of what is written
      // in it. One assignment outside means no other value can reach the loop,
      // which settles it without working out which paths run.
      std::vector<Statement> entering;
      bool known = true;
      for (unsigned id : needed) {
        Statement again;
        if (!putBack(body, circle.blocks, id, again)) {
          known = false;
          break;
        }
        entering.push_back(std::move(again));
      }
      if (!known || entering.empty())
        continue;

      // The loop as a program: what it is entered with, then the loop itself,
      // then an end for every way out of it.
      Lifted lifted;
      lifted.mir = mir;
      for (Body &had : lifted.mir.bodies)
        if (had.name == "START")
          had.name = "START-as-written";

      Body alone;
      alone.name = "START";
      alone.parameters = 0;
      alone.result = body.result;
      alone.locals = body.locals;
      alone.types = body.types;
      alone.typed = body.typed;

      std::vector<unsigned> renumbered(body.blocks.size(), 0);
      unsigned next = 1;
      for (unsigned id : circle.blocks)
        renumbered[id] = next++;
      const unsigned leaves = next;

      BasicBlock enters;
      enters.id = 0;
      enters.statements = entering;
      enters.terminator.kind = TerminatorKind::Goto;
      enters.terminator.targets = {renumbered[circle.header]};
      alone.blocks.push_back(std::move(enters));

      for (unsigned id : circle.blocks) {
        BasicBlock copy = body.blocks[id];
        copy.id = renumbered[id];
        for (unsigned &to : copy.terminator.targets)
          to = circle.blocks.count(to) ? renumbered[to] : leaves;
        alone.blocks.push_back(std::move(copy));
        lifted.places.push_back(body.blocks[id].statements.empty()
                                    ? Span{}
                                    : body.blocks[id].statements.front().span);
        for (const Statement &s : body.blocks[id].statements)
          lifted.places.push_back(s.span);
      }

      BasicBlock ends;
      ends.id = leaves;
      ends.terminator.kind = TerminatorKind::Return;
      ends.terminator.answers = false;
      alone.blocks.push_back(std::move(ends));

      lifted.mir.bodies.push_back(std::move(alone));

      // Where it came from, and what it leaves behind: everything it assigns
      // that anything outside it mentions. A temporary the loop made for itself
      // is nobody's business once the loop is a number.
      lifted.body = static_cast<unsigned>(&body - mir.bodies.data());
      lifted.header = circle.header;
      for (unsigned to : goesTo(body.blocks[circle.header]))
        if (!circle.blocks.count(to))
          lifted.leaves = to;
      for (unsigned id : touched) {
        // Everything the loop changes, and writing one place of a `many` is
        // changing it. Counting only whole assignments meant a loop that filled
        // an array in was one whose answer looked like nothing had happened —
        // which would have been safe only for as long as no such loop could be
        // taken out at all.
        bool changedInside = false;
        for (unsigned at : circle.blocks)
          for (const Statement &s : body.blocks[at].statements)
            if (couldChange(s, id))
              changedInside = true;
        if (!changedInside)
          continue;
        bool seenOutside = false;
        for (const BasicBlock &block : body.blocks) {
          if (circle.blocks.count(block.id))
            continue;
          std::vector<unsigned> read;
          for (const Statement &s : block.statements) {
            if (s.kind == StatementKind::Assign)
              readsOf(s.value, read);
            if (s.kind == StatementKind::Store) {
              readsOf(s.value, read);
              readsOf(s.at, read);
              read.push_back(s.place);
            }
            if (s.kind == StatementKind::Drop)
              read.push_back(s.place);
          }
          if (block.terminator.kind == TerminatorKind::Switch)
            readsOf(block.terminator.condition, read);
          if (block.terminator.kind == TerminatorKind::Return && block.terminator.answers)
            readsOf(block.terminator.answer, read);
          for (unsigned one : read)
            if (one == id)
              seenOutside = true;
        }
        if (seenOutside)
          lifted.liveOut.push_back(id);
      }

      out.push_back(std::move(lifted));
    }
  }
  return out;
}

unsigned writeInWhatTheLoopsAnswer(Mir &mir) {
  unsigned written = 0;
  for (const Lifted &loop : loopsThatStandAlone(mir)) {
    if (loop.liveOut.empty())
      continue;
    const InterpretResult answered = interpretForTheAnswer(loop.mir);
    if (!answered.ran)
      continue;

    // Every one of them has to have an answer, or the loop cannot go: what is
    // left behind has to be all of it.
    std::vector<Statement> instead;
    Body &body = mir.bodies[loop.body];
    bool all = true;
    for (unsigned id : loop.liveOut) {
      if (id >= answered.endedHolding.size() || answered.endedHolding[id].empty()) {
        all = false;
        break;
      }
      Statement s;
      s.kind = StatementKind::Assign;
      s.span = body.blocks[loop.header].statements.empty()
                   ? Span{}
                   : body.blocks[loop.header].statements.front().span;
      s.place = id;
      s.value.kind = RValueKind::Use;
      s.value.type = body.locals[id].type;
      Operand said;
      said.kind = OperandKind::Written;
      said.written = answered.endedHolding[id];
      said.type = body.locals[id].type;
      s.value.operands.push_back(said);
      instead.push_back(std::move(s));
    }
    if (!all)
      continue;

    // The header goes straight to the way out, holding what the loop would have
    // left. Its blocks are then reachable from nothing and LLVM drops them.
    BasicBlock &header = body.blocks[loop.header];
    header.statements = std::move(instead);
    header.terminator = Terminator{};
    header.terminator.kind = TerminatorKind::Goto;
    header.terminator.targets = {loop.leaves};
    ++written;
  }
  return written;
}

} // namespace xag
