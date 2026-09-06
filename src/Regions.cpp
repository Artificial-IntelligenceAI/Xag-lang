#include "xag/Regions.h"

#include <algorithm>
#include <string>
#include <vector>

namespace xag {
namespace {

// One loan: taken here, from there, and either for reading or for writing.
struct Loan {
  unsigned referent = 0;
  bool writes = false;
  Span span;
  unsigned block = 0;
  unsigned at = 0;
};

// Which loans a local may be holding. Small bodies, few loans, so a byte each
// is simpler than packing bits and costs nothing worth counting.
using Holds = std::vector<std::vector<char>>;

class Reader {
public:
  Reader(const Body &body, RegionResult &result) : body_(body), result_(result) {}

  void run() {
    if (body_.blocks.empty())
      return;
    findLoans();
    if (loans_.empty())
      return;
    findEdges();
    settleHolds();
    settleLiveness();
    look();
  }

private:
  const Body &body_;
  RegionResult &result_;
  std::vector<Loan> loans_;
  std::vector<std::vector<unsigned>> after_;  // successors
  std::vector<Holds> entryHolds_;
  std::vector<std::vector<char>> liveOut_;    // [block][local]

  std::size_t locals() const { return body_.locals.size(); }

  Holds emptyHolds() const {
    return Holds(locals(), std::vector<char>(loans_.size(), 0));
  }

  void complain(Span span, std::string code, std::string message,
                std::vector<std::string> rules, std::string label,
                std::vector<Note> notes) {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             std::move(label), std::move(rules), {},
                                             std::move(notes)});
  }

  std::string nameOf(unsigned local) const {
    if (local < body_.locals.size() && !body_.locals[local].name.empty())
      return "`'" + body_.locals[local].name + "'`";
    return "this";
  }

  // Reaching into something and keeping a pointer to what is inside is a loan
  // of the thing reached into, whatever the reaching was written as. A field, a
  // place of a `many`, what an `or-nothing` holds: each of them answers a
  // pointer where the thing inside has an owner, and that pointer is only good
  // for as long as what it points into stays where it is.
  //
  // Without this a loan of `'p'.name` was recorded against the nameless value
  // that carried it, which died at the end of the statement — so handing `'p'`
  // over afterwards was allowed and the loan pointed at nothing.
  static bool reachesInside(const RValue &value) {
    return value.kind == RValueKind::Part || value.kind == RValueKind::Element ||
           value.kind == RValueKind::Inside;
  }

  void findLoans() {
    for (const BasicBlock &block : body_.blocks)
      for (unsigned at = 0; at < block.statements.size(); ++at) {
        const Statement &s = block.statements[at];
        if (s.kind != StatementKind::Assign)
          continue;
        if (s.value.kind == RValueKind::Ref) {
          loans_.push_back(
              Loan{s.value.local, s.value.op == "refmut", s.span, block.id, at});
        } else if (reachesInside(s.value) && canHold(s.place) &&
                   !s.value.operands.empty() &&
                   s.value.operands[0].kind != OperandKind::Written) {
          // Which way it will be used is not known here, so it is read as the
          // gentler of the two: a loan for reading, which forbids handing the
          // thing over and changing it behind the loan's back.
          loans_.push_back(Loan{s.value.operands[0].local, false, s.span, block.id, at});
        }
      }
  }

  void findEdges() {
    after_.assign(body_.blocks.size(), {});
    for (const BasicBlock &block : body_.blocks)
      for (unsigned target : block.terminator.targets)
        if (target < body_.blocks.size())
          after_[block.id].push_back(target);
  }

  bool canHold(unsigned local) const {
    if (local >= body_.locals.size())
      return false;
    const TypeRef type = body_.locals[local].type;
    if (type.index >= body_.types.size())
      return false;
    const std::string &spelled = body_.types[type.index];
    return spelled.rfind("ref ", 0) == 0 || spelled.rfind("refmut ", 0) == 0;
  }

  // What a statement leaves each local holding.
  void step(const Statement &s, Holds &holds) const {
    // Writing one place changes what the `many` holds, not what the name does,
    // so nothing about which loans it carries is different afterwards.
    if (s.kind == StatementKind::Store)
      return;
    if (s.kind == StatementKind::Drop) {
      std::fill(holds[s.place].begin(), holds[s.place].end(), 0);
      return;
    }
    std::vector<char> gathered(loans_.size(), 0);
    // Only something that *is* a loan can be holding one. A number that came
    // out of `count[ref 'x']` is a number, and the loan it was worked out from
    // ended at the semicolon — reading it as still open made the pass refuse
    // programs that were perfectly good.
    if (!canHold(s.place)) {
      if (s.place < locals())
        holds[s.place] = gathered;
      return;
    }
    if (s.value.kind == RValueKind::Ref) {
      for (unsigned i = 0; i < loans_.size(); ++i)
        if (loans_[i].referent == s.value.local && loans_[i].span.begin == s.span.begin &&
            loans_[i].writes == (s.value.op == "refmut"))
          gathered[i] = 1;
      // Lending something that is itself holding a loan carries that loan along
      // with it: what the new one points at is only good while the old one is.
      if (s.value.local < locals())
        for (unsigned i = 0; i < loans_.size(); ++i)
          if (holds[s.value.local][i])
            gathered[i] = 1;
    } else if (reachesInside(s.value) && !s.value.operands.empty() &&
               s.value.operands[0].kind != OperandKind::Written) {
      // The pointer this answers is the loan `findLoans` wrote down, so this is
      // where something starts holding it.
      for (unsigned i = 0; i < loans_.size(); ++i)
        if (loans_[i].referent == s.value.operands[0].local &&
            loans_[i].span.begin == s.span.begin)
          gathered[i] = 1;
      if (s.value.operands[0].local < locals())
        for (unsigned i = 0; i < loans_.size(); ++i)
          if (holds[s.value.operands[0].local][i])
            gathered[i] = 1;
    } else {
      // Anything built out of something holding a loan may hold it too.
      for (const Operand &operand : s.value.operands)
        if (operand.kind != OperandKind::Written && operand.local < locals())
          for (unsigned i = 0; i < loans_.size(); ++i)
            if (holds[operand.local][i])
              gathered[i] = 1;
    }
    if (s.place < locals())
      holds[s.place] = gathered;
  }

  void settleHolds() {
    entryHolds_.assign(body_.blocks.size(), emptyHolds());
    // A borrowed parameter arrives holding a loan of itself, so that giving one
    // back is checked against the caller rather than against nothing.
    bool moved = true;
    while (moved) {
      moved = false;
      for (unsigned id = 0; id < body_.blocks.size(); ++id) {
        Holds out = entryHolds_[id];
        for (const Statement &s : body_.blocks[id].statements)
          step(s, out);
        for (unsigned next : after_[id]) {
          Holds &into = entryHolds_[next];
          for (unsigned local = 0; local < locals(); ++local)
            for (unsigned i = 0; i < loans_.size(); ++i)
              if (out[local][i] && !into[local][i]) {
                into[local][i] = 1;
                moved = true;
              }
        }
      }
    }
  }

  // Which locals are still wanted after each block.
  void settleLiveness() {
    liveOut_.assign(body_.blocks.size(), std::vector<char>(locals(), 0));
    bool moved = true;
    while (moved) {
      moved = false;
      for (unsigned id = 0; id < body_.blocks.size(); ++id) {
        std::vector<char> live = liveOut_[id];
        // Backwards through the block: a read makes it live, a write ends it.
        const BasicBlock &block = body_.blocks[id];
        readTerminator(block.terminator, live);
        for (auto s = block.statements.rbegin(); s != block.statements.rend(); ++s) {
          if (s->kind == StatementKind::Drop) {
            live[s->place] = 1; // ending something is looking at it
            continue;
          }
          if (s->place < locals())
            live[s->place] = 0;
          if (s->value.kind == RValueKind::Ref && s->value.local < locals())
            live[s->value.local] = 1;
          for (const Operand &operand : s->value.operands)
            if (operand.kind != OperandKind::Written && operand.local < locals())
              live[operand.local] = 1;
        }
        // Whatever the block wants on the way in, its predecessors must supply.
        for (unsigned other = 0; other < body_.blocks.size(); ++other)
          for (unsigned next : after_[other])
            if (next == id)
              for (unsigned local = 0; local < locals(); ++local)
                if (live[local] && !liveOut_[other][local]) {
                  liveOut_[other][local] = 1;
                  moved = true;
                }
      }
    }
  }

  static void readTerminator(const Terminator &end, std::vector<char> &live) {
    if (end.condition.kind != OperandKind::Written && end.condition.local < live.size())
      live[end.condition.local] = 1;
    if (end.answers && end.answer.kind != OperandKind::Written &&
        end.answer.local < live.size())
      live[end.answer.local] = 1;
  }

  // ---- and now the looking

  void look() {
    for (unsigned id = 0; id < body_.blocks.size(); ++id) {
      const BasicBlock &block = body_.blocks[id];

      // Liveness at each point, walked backwards from what leaves the block.
      std::vector<std::vector<char>> liveAfter(block.statements.size() + 1,
                                               std::vector<char>(locals(), 0));
      liveAfter.back() = liveOut_[id];
      readTerminator(block.terminator, liveAfter.back());
      for (int at = static_cast<int>(block.statements.size()) - 1; at >= 0; --at) {
        std::vector<char> live = liveAfter[at + 1];
        const Statement &s = block.statements[at];
        if (s.kind == StatementKind::Drop) {
          live[s.place] = 1;
        } else {
          if (s.place < locals())
            live[s.place] = 0;
          if (s.value.kind == RValueKind::Ref && s.value.local < locals())
            live[s.value.local] = 1;
          for (const Operand &operand : s.value.operands)
            if (operand.kind != OperandKind::Written && operand.local < locals())
              live[operand.local] = 1;
        }
        liveAfter[at] = live;
      }

      Holds holds = entryHolds_[id];
      for (unsigned at = 0; at < block.statements.size(); ++at) {
        const Statement &s = block.statements[at];
        check(s, holds, liveAfter[at + 1]);
        step(s, holds);
      }
    }
  }

  // A loan still wanted after this point, held by a local still wanted too.
  std::vector<unsigned> standing(const Holds &holds,
                                 const std::vector<char> &live) const {
    std::vector<unsigned> out;
    for (unsigned i = 0; i < loans_.size(); ++i)
      for (unsigned local = 0; local < locals(); ++local)
        if (holds[local][i] && live[local]) {
          out.push_back(i);
          break;
        }
    return out;
  }

  void check(const Statement &s, const Holds &holds, const std::vector<char> &live) {
    const std::vector<unsigned> open = standing(holds, live);
    if (open.empty())
      return;

    for (unsigned i : open) {
      const Loan &loan = loans_[i];

      // Taking it away while somebody is still holding it.
      for (const Operand &operand : s.value.operands)
        if (operand.kind == OperandKind::Move && operand.local == loan.referent)
          complain(s.span, "E0408",
                   nameOf(loan.referent) + " is handed over while it is still lent.",
                   {"what is lent stays where it is until the loan is done with"},
                   "handed over here",
                   {Note{loan.span, "and lent here, still in use after this"}});

      // Ending it while somebody is still holding it.
      if (s.kind == StatementKind::Drop && s.place == loan.referent)
        complain(s.span, "E0411", nameOf(loan.referent) + " ends while it is still lent.",
                 {"a borrow never outlasts what it borrows from"}, "ends here",
                 {Note{loan.span, "and lent here, still in use after this"}});

      // Writing to it directly, going round the loan rather than through it —
      // whether that is the whole of it or one of the places it holds, because
      // a loan of a `many` is a loan of every place in it.
      if ((s.kind == StatementKind::Store ||
           (s.kind == StatementKind::Assign && s.value.kind != RValueKind::Ref)) &&
          s.place == loan.referent)
        complain(s.span, "E0409",
                 nameOf(loan.referent) + " is changed while it is lent.",
                 {"what is lent is written through the loan, or not at all"},
                 "changed here",
                 {Note{loan.span, "and lent here, still in use after this"}});

      // Lending it again, when one of the two is for writing.
      if (s.kind == StatementKind::Assign && s.value.kind == RValueKind::Ref &&
          s.value.local == loan.referent && s.span.begin != loan.span.begin &&
          (loan.writes || s.value.op == "refmut"))
        complain(s.span, "E0410",
                 nameOf(loan.referent) + " is lent for writing while it is already lent.",
                 {"one loan for writing, or any number for reading, and never both"},
                 "lent again here",
                 {Note{loan.span, "and already lent here"}});
    }
  }
};

} // namespace

RegionResult regions(const Source &source, const Mir &mir) {
  (void)source;
  RegionResult result;
  for (const Body &body : mir.bodies)
    Reader(body, result).run();
  return result;
}

} // namespace xag
