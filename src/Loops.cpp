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
      if (to <= block.id && reaches(body, to, block.id)) {
        Circle one;
        one.header = to;
        one.blocks = around(body, to, block.id);
        found.push_back(std::move(one));
      }
  return found;
}

// Whether a type is one a value can be written down as and handed back whole.
// Everything else has something behind it that the loop does not own.
bool plainlyANumber(const MirType &type) {
  if (type.isLoan() || type.orNothing || type.many)
    return false;
  return isNumber(type.held) || type.held == Type::Bool;
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

// Whether an assignment is a value written down in the source rather than
// worked out from something.
const std::string *writtenBy(const Statement &s) {
  if (s.kind != StatementKind::Assign || !s.parts.empty())
    return nullptr;
  if (s.value.kind != RValueKind::Use || s.value.operands.size() != 1)
    return nullptr;
  const Operand &only = s.value.operands[0];
  return only.kind == OperandKind::Written ? &only.written : nullptr;
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
      for (unsigned id : touched)
        if (id >= body.locals.size() ||
            !plainlyANumber(body.typed[body.locals[id].type.index]))
          plain = false;
      if (!plain)
        continue;

      // What each of those is entered with: one assignment outside the loop,
      // and that assignment a value written down.
      std::vector<Statement> entering;
      bool known = true;
      for (unsigned id : needed) {
        const Statement *only = nullptr;
        unsigned howMany = 0;
        for (const BasicBlock &block : body.blocks) {
          if (circle.blocks.count(block.id))
            continue;
          for (const Statement &s : block.statements)
            if (s.kind == StatementKind::Assign && s.place == id) {
              ++howMany;
              only = &s;
            }
        }
        if (howMany != 1 || !only || !writtenBy(*only)) {
          known = false;
          break;
        }
        entering.push_back(*only);
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
        bool assignedInside = false;
        for (unsigned at : circle.blocks)
          for (const Statement &s : body.blocks[at].statements)
            if (s.kind == StatementKind::Assign && s.place == id)
              assignedInside = true;
        if (!assignedInside)
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
