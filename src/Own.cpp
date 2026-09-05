#include "xag/Own.h"

#include "xag/Check.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace xag {
namespace {

// How a name holds what it names.
enum class Mode { Owned, Ref, RefMut };

// A number is small enough that handing one over costs nothing and leaves the
// original where it was, so numbers are never moved. `str` is.
//
// This asks the one place that knows what a type is, rather than comparing the
// spelling here as well — which is how `int64` arriving quietly broke it.
bool copyType(std::string_view type) {
  if (type == "bool")
    return true;
  const Type named = typeNamed(type);
  return isNumber(named);
}

// A `many` owns the places it holds, whatever sits in them, so it is handed
// over rather than copied even when every element would be.
bool holdsMany(const Chain &chain) {
  const std::size_t n = chain.segments.size();
  return n >= 2 && !chain.segments[n - 2].isName && chain.segments[n - 2].text == "many";
}

bool copyChain(const Chain &chain) {
  return !holdsMany(chain) && copyType(chain.type().text);
}

Mode modeOfChain(const Chain &chain) {
  for (const ChainSegment &seg : chain.segments) {
    if (seg.isName)
      continue;
    if (seg.text == "refmut")
      return Mode::RefMut;
    if (seg.text == "ref")
      return Mode::Ref;
  }
  return Mode::Owned;
}

std::string loanOfChain(const Chain &chain) {
  for (const ChainSegment &seg : chain.segments)
    if (seg.isName)
      return seg.text;
  return {};
}

const char *word(Mode mode) {
  switch (mode) {
  case Mode::Ref:    return "ref";
  case Mode::RefMut: return "refmut";
  case Mode::Owned:  return "move";
  }
  return "move";
}

struct Binding {
  Mode mode = Mode::Owned;
  bool copies = true;
  // What one of its places holds, when it is a `many`. An array never copies,
  // but writing `set 'xs'[*0*] = [*9*]` puts an element there, and whether that
  // needs a word spelled is the element's question rather than the array's.
  bool elementCopies = true;
  bool holds = false; // whether it is a `many` at all
  bool changes = false;
  Span span;
  bool moved = false;
  Span movedAt;
};

// `mut` on what a name owns, `refmut` on what it borrows. Either way the name
// may be written through, and nothing else may.
bool changeable(const Chain &chain) {
  for (const ChainSegment &seg : chain.segments)
    if (!seg.isName && (seg.text == "mut" || seg.text == "refmut"))
      return true;
  return false;
}

struct ParamInfo {
  Mode mode = Mode::Owned;
  bool copies = true;
};

struct FnInfo {
  std::vector<ParamInfo> params;
  Mode result = Mode::Owned;
  bool variadic = false;
};

// How a value is being taken: read where it stands, or taken away for good.
enum class Use { Read, Consume };

class Owner {
public:
  Owner(const Source &source, const Program &program)
      : source_(source), program_(program) {}

  OwnResult run() {
    collect();
    scopes_.emplace_back();
    for (const Item &item : program_.items)
      if (item.kind == ItemKind::Const)
        scopes_.back()[item.name] =
            Binding{Mode::Owned, copyChain(item.chain), copyType(item.chain.type().text),
                    holdsMany(item.chain), false, item.nameSpan, false, {}};
    for (const Item &item : program_.items)
      body(item);
    return std::move(result_);
  }

private:
  const Source &source_;
  const Program &program_;
  OwnResult result_;
  std::vector<std::unordered_map<std::string, Binding>> scopes_;
  std::unordered_map<std::string, FnInfo> functions_;
  Mode giving_ = Mode::Owned;
  bool givingCopies_ = true;

  void complain(Span span, std::string code, std::string message,
                std::vector<std::string> rules, std::vector<std::string> tips = {},
                std::string label = "here", std::vector<Note> notes = {}) {
    result_.diagnostics.push_back(Diagnostic{span, std::move(code), std::move(message),
                                             std::move(label), std::move(rules),
                                             std::move(tips), std::move(notes)});
  }

  Binding *lookup(const std::string &text) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      auto found = scope->find(text);
      if (found != scope->end())
        return &found->second;
    }
    return nullptr;
  }

  static std::string joined(const std::vector<std::string> &path) {
    std::string out;
    for (const std::string &part : path)
      out += (out.empty() ? "" : ".") + part;
    return out;
  }

  // ---- signatures, and the one lifetime rule a signature can answer alone

  void collect() {
    functions_["print.stdout"] = FnInfo{{}, Mode::Owned, true};
    functions_["count"] = FnInfo{{ParamInfo{Mode::Ref, false}}, Mode::Owned, false};

    for (const Item &item : program_.items) {
      if (item.kind != ItemKind::Function)
        continue;

      FnInfo info;
      info.result = modeOfChain(item.chain);
      unsigned borrowed = 0;
      const std::string resultLoan = loanOfChain(item.chain);
      bool loanIsLent = false;
      std::vector<Note> lent;
      for (const Param &param : item.params) {
        const Mode mode = modeOfChain(param.chain);
        info.params.push_back(ParamInfo{mode, copyChain(param.chain)});
        if (mode != Mode::Owned) {
          ++borrowed;
          lent.push_back(Note{param.span, "lent here"});
          if (!resultLoan.empty() && loanOfChain(param.chain) == resultLoan)
            loanIsLent = true;
        }
      }
      functions_[item.name] = std::move(info);

      if (modeOfChain(item.chain) == Mode::Owned)
        continue;

      // The answer is borrowed. With one borrowed parameter there is only one
      // loan it could be on; with more there is a choice, and the compiler does
      // not get to make it.
      if (resultLoan.empty() && borrowed != 1)
        complain(item.chain.span, "E0402",
                 borrowed == 0
                     ? "this answer is borrowed, and nothing was lent to borrow it from."
                     : "this answer is borrowed, and so are " + std::to_string(borrowed) +
                           " of the parameters.",
                 {"a borrow that is given back says which loan it belongs to"},
                 {"with one borrowed parameter there is only one loan the answer could "
                  "be on, so nothing is written; with more there is a choice."},
                 "this answer is borrowed", lent);
      else if (!resultLoan.empty() && !loanIsLent)
        complain(item.chain.span, "E0402",
                 "the answer is on the loan `'" + resultLoan +
                     "'`, and no parameter is lent on it.",
                 {"a borrow that is given back says which loan it belongs to"},
                 {"a loan is a name for what the caller lent, so something the caller "
                  "lent has to carry it."});
    }
  }

  // ---- expressions

  void read(const Expr &e) { use(e, Use::Read, Mode::Owned, false); }

  // `mode` and `copies` describe the place the value is going.
  void use(const Expr &e, Use how, Mode wanted, bool copies) {
    switch (e.kind) {
    case ExprKind::Name: {
      Binding *binding = lookup(e.text);
      if (!binding)
        return; // the checker has already said so
      if (binding->moved) {
        // Two places matter: where it is wanted, and where it went.
        complain(e.span, "E0403", "`'" + e.text + "'` was moved, and holds nothing now.",
                 {"a name holds its value until it is moved, and then holds nothing"},
                 {"what was moved is somewhere else now, and there is only ever one of it."},
                 "used here",
                 {Note{binding->movedAt, "but it was handed over here"}});
        return;
      }
      if (how == Use::Consume && !copies) {
        // Passing a loan along is not a transfer: the borrow travels, and the
        // name it came from still holds what it holds.
        if (wanted != Mode::Owned && binding->mode != Mode::Owned)
          return;
        complain(e.span, "E0406",
                 "`'" + e.text + "'` is handed over here, and nothing says so.",
                 {"a transfer is spelled where it happens"},
                 {"a declaration describes a thing, but this acts: `" +
                  std::string(word(wanted)) + "` is the word for what happens to `'" +
                  e.text + "'` next."});
      }
      return;
    }

    case ExprKind::Index: {
      // An element is a place inside the array, so reading one reads the array
      // and lending one lends the whole of it: which element `'xs'['i']` names
      // is not known until the program runs, and no loan can be narrower than
      // what the index is read out of.
      if (!e.children.empty())
        read(*e.children[0]);
      Binding *binding = lookup(e.text);
      if (binding && binding->moved) {
        complain(e.span, "E0403", "`'" + e.text + "'` was moved, and holds nothing now.",
                 {"a name holds its value until it is moved, and then holds nothing"},
                 {"what was moved is somewhere else now, and there is only ever one of it."},
                 "used here",
                 {Note{binding->movedAt, "but it was handed over here"}});
        return;
      }
      if (how == Use::Consume && wanted == Mode::Owned && !copies)
        complain(e.span, "E0412",
                 "taking this out would leave a hole where it was.",
                 {"a `many` holds a value in every place it has"},
                 {"an element is read, written and lent where it stands; nothing in "
                  "Xag holds a gap."});
      return;
    }

    case ExprKind::Borrow: {
      if (e.children.empty())
        return;
      const Expr &inner = *e.children[0];
      Binding *binding = (inner.kind == ExprKind::Name || inner.kind == ExprKind::Index)
                             ? lookup(inner.text)
                             : nullptr;

      if (e.text == "move") {
        if (inner.kind == ExprKind::Index) {
          complain(e.span, "E0412", "taking this out would leave a hole where it was.",
                   {"a `many` holds a value in every place it has"},
                   {"an element is read, written and lent where it stands; nothing in "
                    "Xag holds a gap."});
          return;
        }
        if (binding && binding->mode != Mode::Owned) {
          complain(e.span, "E0404", "this is borrowed, and a borrow is not yours to give away.",
                   {"what is lent goes back to whoever lent it"},
                   {"only an owner can hand a value over for good."});
          return;
        }
        if (binding && binding->copies) {
          complain(e.span, "E0405", "nothing is moved out of a value this small.",
                   {"a small value is handed over by being copied, and the original stays"},
                   {"`move` says a name stops holding what it held, and this one does not."});
          return;
        }
        read(inner);
        if (binding) {
          binding->moved = true;
          binding->movedAt = e.span;
        }
        return;
      }

      // `ref` / `refmut`. A loan gives away no more than the lender had, so a
      // name that does not change cannot be lent for writing — whether it does
      // not change because it owns something quietly, or because what it holds
      // was itself only lent for reading.
      if (binding && e.text == "refmut" && !binding->changes)
        complain(e.span, "E0407",
                 binding->mode == Mode::Ref
                     ? "this was lent for reading, and cannot be lent for writing."
                     : "`'" + inner.text + "'` does not change, and cannot be lent for "
                                           "writing.",
                 {"a loan gives away no more than the lender had"},
                 {"a chain says `mut` when a name may be written through, and this one "
                  "does not."});
      read(inner);
      return;
    }

    case ExprKind::Call: {
      const std::string path = joined(e.path);
      auto found = functions_.find(path);
      if (found == functions_.end()) {
        for (const Value &value : e.args.values)
          for (const ExprPtr &item : value.items)
            read(*item);
        return;
      }
      const FnInfo &info = found->second;
      for (unsigned i = 0; i < e.args.values.size(); ++i) {
        const Value &argument = e.args.values[i];
        const bool known = !info.variadic && i < info.params.size();
        const ParamInfo param = known ? info.params[i] : ParamInfo{Mode::Owned, true};
        if (argument.items.size() == 1)
          argue(*argument.items[0], param, path);
        else
          for (const ExprPtr &piece : argument.items)
            read(*piece);
      }
      return;
    }

    default:
      for (const ExprPtr &child : e.children)
        read(*child);
      if (e.kind == ExprKind::Call)
        return;
      for (const Value &value : e.args.values)
        for (const ExprPtr &item : value.items)
          read(*item);
      return;
    }
  }

  // One argument against what its parameter asked for.
  void argue(const Expr &e, ParamInfo param, const std::string &path) {
    if (e.kind == ExprKind::Borrow) {
      const char *wanted = word(param.mode);
      if (e.text != wanted && !(param.mode == Mode::Owned && param.copies))
        complain(e.span, "E0406",
                 "`" + path + "` asks for `" + wanted + "` here, and this says `" + e.text + "`.",
                 {"a transfer is spelled where it happens, and says which one it is"},
                 {"`ref` lends for reading, `refmut` lends for writing, and `move` hands "
                  "the value over for good."});
      use(e, Use::Consume, param.mode, param.copies);
      return;
    }
    use(e, Use::Consume, param.mode, param.copies);
  }

  // ---- statements

  std::vector<std::pair<Binding *, bool>> snapshot() {
    std::vector<std::pair<Binding *, bool>> out;
    for (auto &scope : scopes_)
      for (auto &[key, binding] : scope) {
        (void)key;
        out.emplace_back(&binding, binding.moved);
      }
    return out;
  }

  void restore(const std::vector<std::pair<Binding *, bool>> &saved) {
    for (const auto &[binding, moved] : saved)
      binding->moved = moved;
  }

  void block(const Block &b) {
    scopes_.emplace_back();
    for (const StmtPtr &s : b.stmts)
      statement(*s);
    scopes_.pop_back();
  }

  void consumeInto(const ValueList &list, Mode mode, bool copies, bool collects = false,
                   bool elementCopies = true) {
    for (const Value &value : list.values) {
      // Items side by side under a `many` each end up in a place of their own,
      // so each is handed over. Under anything else they are joined, and
      // joining reads its pieces and builds something new out of them.
      if (collects) {
        if (value.items.size() == 1) {
          const Expr &only = *value.items[0];
          const Binding *from =
              only.kind == ExprKind::Name ? lookup(only.text) : nullptr;
          const bool whole = from && from->holds;
          use(only, Use::Consume, mode, whole ? copies : elementCopies);
          continue;
        }
        for (const ExprPtr &item : value.items)
          use(*item, Use::Consume, Mode::Owned, elementCopies);
        continue;
      }
      if (value.items.size() == 1)
        use(*value.items[0], Use::Consume, mode, copies);
      else
        for (const ExprPtr &item : value.items)
          read(*item);
    }
  }

  void statement(const Stmt &s) {
    switch (s.kind) {
    case StmtKind::Declare: {
      const Mode mode = modeOfChain(s.chain);
      const bool copies = copyChain(s.chain);
      consumeInto(s.value, mode, copies, holdsMany(s.chain),
                  copyType(s.chain.type().text));
      scopes_.back()[s.name] =
          Binding{mode, copies, copyType(s.chain.type().text), holdsMany(s.chain),
                  changeable(s.chain), s.nameSpan, false, {}};
      break;
    }

    case StmtKind::Set: {
      Binding *binding = lookup(s.name);
      if (s.index) {
        // Writing one place reads the array to find it, and what goes in is an
        // element rather than the array, so the array's own mode says nothing
        // about what the value has to be.
        read(*s.index);
        if (binding && binding->moved)
          complain(s.nameSpan, "E0403",
                   "`'" + s.name + "'` was moved, and holds nothing now.",
                   {"a name holds its value until it is moved, and then holds nothing"},
                   {"what was moved is somewhere else now, and there is only ever one "
                    "of it."},
                   "used here", {Note{binding->movedAt, "but it was handed over here"}});
        consumeInto(s.value, Mode::Owned, binding ? binding->elementCopies : true);
        break;
      }
      consumeInto(s.value, binding ? binding->mode : Mode::Owned,
                  binding ? binding->copies : true, binding && binding->holds,
                  binding ? binding->elementCopies : true);
      if (binding)
        binding->moved = false; // it holds something again
      break;
    }

    case StmtKind::If: {
      // A name given away down any arm is gone afterwards, because the compiler
      // does not get to assume which arm ran.
      const auto before = snapshot();
      std::vector<std::pair<Binding *, Span>> movedSomewhere;
      for (const Branch &branch : s.branches) {
        restore(before);
        if (branch.condition)
          read(*branch.condition);
        block(branch.body);
        for (const auto &[binding, was] : before)
          if (!was && binding->moved)
            movedSomewhere.emplace_back(binding, binding->movedAt);
      }
      restore(before);
      for (const auto &[binding, where] : movedSomewhere) {
        binding->moved = true;
        binding->movedAt = where;
      }
      break;
    }

    case StmtKind::LoopRange:
    case StmtKind::LoopWhile: {
      const auto before = snapshot();
      if (s.condition)
        read(*s.condition);
      for (const Value &value : s.value.values)
        for (const ExprPtr &item : value.items)
          read(*item);
      scopes_.emplace_back();
      if (s.kind == StmtKind::LoopRange)
        scopes_.back()[s.name] =
            Binding{Mode::Owned, copyChain(s.chain), copyType(s.chain.type().text),
                    holdsMany(s.chain), false, s.nameSpan, false, {}};
      for (const StmtPtr &inner : s.body.stmts)
        statement(*inner);
      scopes_.pop_back();

      // Anything given away inside a loop is given away again next time round.
      for (const auto &[binding, was] : before)
        if (!was && binding->moved) {
          complain(binding->movedAt, "E0403",
                   "this is moved every time round the loop, and there is only one of it.",
                   {"a name holds its value until it is moved, and then holds nothing"},
                   {"the second pass would find nothing here to give away."});
          binding->moved = false;
        }
      break;
    }

    case StmtKind::Give:
      for (const Value &value : s.value.values) {
        if (value.items.size() != 1) {
          for (const ExprPtr &item : value.items)
            read(*item);
          continue;
        }
        const Expr &given = *value.items[0];
        if (giving_ != Mode::Owned) {
          givingBorrow(given);
          continue;
        }
        // `give` is the word. A call site has a choice between lending and
        // handing over, so it says which; `give` has no second reading, so
        // spelling `move` here would be a second way to write one thing.
        if (given.kind == ExprKind::Borrow && given.text == "move") {
          complain(given.span, "E0406", "`give` already hands the answer over.",
                   {"a word is written where there is a choice, and here there is none"},
                   {"a call site chooses between lending and handing over, so it says "
                    "which; there is nothing else `give` could mean."});
          continue;
        }
        read(given);
        if (!givingCopies_ && given.kind == ExprKind::Name)
          if (Binding *binding = lookup(given.text))
            binding->moved = true;
      }
      break;

    case StmtKind::Break:
      break;

    case StmtKind::Call:
      if (s.call)
        read(*s.call);
      break;
    }
  }

  // An answer that is borrowed has to be borrowed from something the caller lent.
  void givingBorrow(const Expr &given) {
    const Expr *inner = &given;
    if (given.kind == ExprKind::Borrow && !given.children.empty())
      inner = given.children[0].get();

    if (inner->kind == ExprKind::Name) {
      Binding *binding = lookup(inner->text);
      if (binding && binding->mode == Mode::Owned) {
        complain(given.span, "E0401",
                 "`'" + inner->text +
                     "'` stops existing when this function ends, and the answer would "
                     "outlive it.",
                 {"a borrow never outlasts what it borrows from"},
                 {"the value belongs to this function, so the only thing that can leave "
                  "here with it is the value itself."});
        return;
      }
    }
    read(given);
  }

  // ---- items

  void body(const Item &item) {
    if (item.kind == ItemKind::Const)
      return;

    scopes_.emplace_back();
    if (item.kind == ItemKind::Function) {
      giving_ = modeOfChain(item.chain);
      givingCopies_ = copyType(item.chain.type().text);
      for (const Param &param : item.params)
        scopes_.back()[param.name] =
            Binding{modeOfChain(param.chain), copyChain(param.chain),
                    copyType(param.chain.type().text), holdsMany(param.chain),
                    changeable(param.chain), param.nameSpan, false, {}};
    } else {
      giving_ = Mode::Owned;
      givingCopies_ = true;
    }
    for (const StmtPtr &s : item.body.stmts)
      statement(*s);
    scopes_.pop_back();
    (void)source_;
  }
};

} // namespace

OwnResult own(const Source &source, const Program &program) {
  return Owner(source, program).run();
}

} // namespace xag
