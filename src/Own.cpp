#include "xag/Own.h"

#include "xag/Check.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace xag {
namespace {

// How a name holds what it names.
enum class Mode { Owned, Ref, RefMut };

// This pass used to keep a model of types of its own, built out of chains read
// as text: whether a word meant several, whether a type copied, what one of the
// things a struct holds is. Three bugs came out of it in one day, all the same
// shape — something asked what kind of thing it had and got its answer from a
// list written before that kind existed. `many-growing` was not in the list of
// words meaning several. A case of a `one-of` was not in the list of things
// that hand a value over. Nesting was not in the question at all.
//
// So it asks the checker now, through the typed tree, and the questions below
// are the whole of what it asks.

// A number is small enough that handing one over costs nothing and leaves the
// original where it was, so numbers are never moved. Text is not, and neither
// is anything that owns places or fields.
bool copies(Ty type) {
  if (type.holds() || type.isStruct() || type.kind == Type::OneOf)
    return false;
  return type.kind == Type::Bool || isNumber(type.kind);
}

// Whether what goes into one of its places is copied there. One place of a
// `many.many.int64` holds a whole `many.int64`, which owns its own places.
bool placeCopies(Ty type) { return copies(type.holds() ? elementOf(type) : type); }

Mode modeOf(Ty type) {
  switch (type.held) {
  case Held::Loan:    return Mode::Ref;
  case Held::LoanMut: return Mode::RefMut;
  case Held::Owned:   break;
  }
  return Mode::Owned;
}

const char *word(Mode mode) {
  switch (mode) {
  case Mode::Ref:    return "loan";
  case Mode::RefMut: return "loanmut";
  case Mode::Owned:  return "move";
  }
  return "move";
}

struct Binding {
  Mode mode = Mode::Owned;
  Ty type;
  bool changes = false;
  Span span;
  bool moved = false;
  Span movedAt;
  // Which of the things it holds have been handed over on their own. A field is
  // known where it is written, so this can be tracked one at a time — which is
  // the whole reason a struct's ownership is not the array's.
  std::vector<unsigned> partsMoved;
  std::vector<Span> partsMovedAt;

  bool copiesWhole() const { return copies(type); }
  bool holds() const { return type.holds(); }
  bool placeCopiesInto() const { return placeCopies(type); }
  bool fillsAStruct() const { return type.isStruct() && mode == Mode::Owned; }
};

// What `holds` lends: a borrow of what was there, so it is read where it stands
// and never handed over.
Binding heldBinding(Span where, Ty what) {
  Binding out;
  out.mode = Mode::Ref;
  out.type = what;
  out.type.held = Held::Loan;
  out.changes = false;
  out.span = where;
  return out;
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
  Owner(const Source &source, const TypedProgram &program)
      : source_(source), program_(program) {}

  OwnResult run() {
    collect();
    scopes_.emplace_back();
    for (const TypedItem &item : program_.items)
      if (item.kind == TypedItemKind::Const)
        scopes_.back()[item.name] =
            Binding{Mode::Owned, item.answers, false, item.nameSpan, false, {}, {}, {}};
    for (const TypedItem &item : program_.items)
      body(item);
    (void)source_;
    return std::move(result_);
  }

private:
  const Source &source_;
  const TypedProgram &program_;
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

  // What a struct holds, and what one of the things it holds is called. Both
  // come from the checker's own table now rather than from a copy of it.
  const Field *fieldOf(Ty of, unsigned which) const {
    if (!of.isStruct() || of.named >= program_.shapes.size())
      return nullptr;
    const Shape &shape = program_.shapes[of.named];
    return which < shape.fields.size() ? &shape.fields[which] : nullptr;
  }

  std::string fieldName(Ty of, unsigned which) const {
    const Field *field = fieldOf(of, which);
    return field ? field->name : std::string();
  }

  // ---- signatures, and the one lifetime rule a signature can answer alone

  void collect() {
    functions_["print.stdout"] = FnInfo{{}, Mode::Owned, true};
    functions_["print.stderr"] = FnInfo{{}, Mode::Owned, true};
    functions_["count"] = FnInfo{{ParamInfo{Mode::Ref, false}}, Mode::Owned, false};
    functions_["read.stdin"] = FnInfo{{}, Mode::Owned, false};
    functions_["arguments"] = FnInfo{{}, Mode::Owned, false};
    // Read where it stands, and the text goes on belonging to whoever had it.
    functions_["convert-to-number"] =
        FnInfo{{ParamInfo{Mode::Ref, false}}, Mode::Owned, false};

    for (const TypedItem &item : program_.items) {
      if (item.kind != TypedItemKind::Function)
        continue;

      FnInfo info;
      info.result = modeOf(item.answers);
      unsigned borrowed = 0;
      bool loanIsLent = false;
      std::vector<Note> lent;
      for (const TypedParam &param : item.params) {
        const Mode mode = modeOf(param.type);
        info.params.push_back(ParamInfo{mode, copies(param.type)});
        if (mode != Mode::Owned) {
          ++borrowed;
          lent.push_back(Note{param.span, "lent here"});
          if (!item.loan.empty() && param.loan == item.loan)
            loanIsLent = true;
        }
      }
      functions_[item.name] = std::move(info);

      if (modeOf(item.answers) == Mode::Owned)
        continue;

      // The answer is borrowed. With one borrowed parameter there is only one
      // loan it could be on; with more there is a choice, and the compiler does
      // not get to make it.
      if (item.loan.empty() && borrowed != 1)
        complain(item.answersSpan, "E0402",
                 borrowed == 0
                     ? "this answer is borrowed, and nothing was lent to borrow it from."
                     : "this answer is borrowed, and so are " + std::to_string(borrowed) +
                           " of the parameters.",
                 {"a borrow that is given back says which loan it belongs to"},
                 {"with one borrowed parameter there is only one loan the answer could "
                  "be on, so nothing is written; with more there is a choice."},
                 "this answer is borrowed", lent);
      else if (!item.loan.empty() && !loanIsLent)
        complain(item.answersSpan, "E0402",
                 "the answer is on the loan `'" + item.loan +
                     "'`, and no parameter is lent on it.",
                 {"a borrow that is given back says which loan it belongs to"},
                 {"a loan is a name for what the caller lent, so something the caller "
                  "lent has to carry it."});
    }
  }

  // ---- expressions

  void read(const TypedExpr &e) { use(e, Use::Read, Mode::Owned, false); }

  // The name a run of fields is reached out of, and which fields those were.
  static const TypedExpr *rootOf(const TypedExpr &e, std::vector<unsigned> &fields,
                                 std::vector<Ty> &of) {
    const TypedExpr *at = &e;
    while (at->kind == TypedKind::Field && !at->children.empty()) {
      fields.insert(fields.begin(), at->which);
      of.insert(of.begin(), at->children[0]->type);
      at = at->children[0].get();
    }
    return at->kind == TypedKind::Name ? at : nullptr;
  }

  // Whether this field, or the whole it is part of, has already gone.
  const Span *goneAlready(const Binding &binding,
                          const std::vector<unsigned> &fields) const {
    for (unsigned i = 0; i < binding.partsMoved.size(); ++i)
      if (fields.empty() || binding.partsMoved[i] == fields.front())
        return &binding.partsMovedAt[i];
    return nullptr;
  }

  // `mode` and `copies` describe the place the value is going.
  void use(const TypedExpr &e, Use how, Mode wanted, bool copiesThere) {
    switch (e.kind) {
    case TypedKind::Name: {
      Binding *binding = lookup(e.text);
      if (!binding)
        return; // the checker has already said so
      if (!binding->partsMoved.empty()) {
        const std::string gone = fieldName(binding->type, binding->partsMoved.front());
        complain(e.span, "E0414",
                 "`'" + e.text + "'` is not all here: `'" + e.text + "'." + gone +
                     "` was handed over.",
                 {"a struct holds each of its things until that one is moved"},
                 {"what is left of it can still be reached one field at a time; the "
                  "whole of it cannot, because part of the whole is somewhere else."},
                 "the whole of it is wanted here",
                 {Note{binding->partsMovedAt.front(), "and this went from it here"}});
        return;
      }
      if (binding->moved) {
        // Two places matter: where it is wanted, and where it went.
        complain(e.span, "E0403", "`'" + e.text + "'` was moved, and holds nothing now.",
                 {"a name holds its value until it is moved, and then holds nothing"},
                 {"what was moved is somewhere else now, and there is only ever one of it."},
                 "used here",
                 {Note{binding->movedAt, "but it was handed over here"}});
        return;
      }
      if (how == Use::Consume && !copiesThere) {
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

    case TypedKind::Field: {
      std::vector<unsigned> fields;
      std::vector<Ty> of;
      const TypedExpr *root = rootOf(e, fields, of);
      Binding *binding = root ? lookup(root->text) : nullptr;
      if (!binding) {
        for (const TypedPtr &child : e.children)
          read(*child);
        return;
      }
      if (binding->moved) {
        complain(e.span, "E0403",
                 "`'" + root->text + "'` was moved, and holds nothing now.",
                 {"a name holds its value until it is moved, and then holds nothing"},
                 {}, "used here",
                 {Note{binding->movedAt, "but it was handed over here"}});
        return;
      }
      if (const Span *gone = goneAlready(*binding, fields)) {
        complain(e.span, "E0413",
                 "`'" + root->text + "'." + fieldName(binding->type, fields.front()) +
                     "` was handed over, and is not there now.",
                 {"a struct holds each of its things until that one is moved"},
                 {"the rest of it is still there; this one is not."},
                 "used here", {Note{*gone, "but it was handed over here"}});
        return;
      }
      if (how == Use::Consume && wanted == Mode::Owned && !copiesThere) {
        // Taking one of them out, which leaves the rest where they are.
        binding->partsMoved.push_back(fields.front());
        binding->partsMovedAt.push_back(e.span);
      }
      return;
    }

    case TypedKind::Element: {
      // An element is a place inside the array, so reading one reads the array
      // and lending one lends the whole of it: which element `'xs'['i']` names
      // is not known until the program runs, and no loan can be narrower than
      // what the index is read out of.
      if (e.children.size() > 1)
        read(*e.children[1]);
      Binding *binding =
          !e.children.empty() && e.children[0]->kind == TypedKind::Name
              ? lookup(e.children[0]->text)
              : nullptr;
      if (binding && binding->moved) {
        complain(e.span, "E0403",
                 "`'" + e.children[0]->text + "'` was moved, and holds nothing now.",
                 {"a name holds its value until it is moved, and then holds nothing"},
                 {"what was moved is somewhere else now, and there is only ever one of it."},
                 "used here",
                 {Note{binding->movedAt, "but it was handed over here"}});
        return;
      }
      if (!binding && !e.children.empty())
        read(*e.children[0]);
      if (how == Use::Consume && wanted == Mode::Owned && !copiesThere)
        complain(e.span, "E0412",
                 "taking this out would leave a hole where it was.",
                 {"a `many` holds a value in every place it has"},
                 {"an element is read, written and lent where it stands; nothing in "
                  "Xag holds a gap."});
      return;
    }

    case TypedKind::Borrow: {
      if (e.children.empty())
        return;
      const TypedExpr &inner = *e.children[0];
      const std::string named =
          inner.kind == TypedKind::Name ? inner.text
          : inner.kind == TypedKind::Element && !inner.children.empty() &&
                    inner.children[0]->kind == TypedKind::Name
              ? inner.children[0]->text
              : std::string();
      Binding *binding = named.empty() ? nullptr : lookup(named);

      if (e.text == "move" && inner.kind == TypedKind::Field) {
        std::vector<unsigned> reached;
        std::vector<Ty> of;
        const TypedExpr *root = rootOf(inner, reached, of);
        // A struct may hold a borrow, and what is borrowed is not the struct's
        // to give away — no more than a borrowed name is. This was let through,
        // and the value went to whoever asked for it while still belonging to
        // whoever lent it.
        if (root && modeOf(inner.type) != Mode::Owned) {
          complain(e.span, "E0404",
                   "this is borrowed, and a borrow is not yours to give away.",
                   {"what is lent goes back to whoever lent it"},
                   {"the struct holding it borrowed it too, and only an owner can hand "
                    "a value over for good."});
          return;
        }
        use(inner, Use::Consume, Mode::Owned, false);
        return;
      }

      if (e.text == "move") {
        if (inner.kind == TypedKind::Element) {
          complain(e.span, "E0412", "taking this out would leave a hole where it was.",
                   {"a `many` holds a value in every place it has"},
                   {"an element is read, written and lent where it stands; nothing in "
                    "Xag holds a gap."});
          return;
        }
        if (binding && binding->mode != Mode::Owned) {
          complain(e.span, "E0404",
                   "this is borrowed, and a borrow is not yours to give away.",
                   {"what is lent goes back to whoever lent it"},
                   {"only an owner can hand a value over for good."});
          return;
        }
        if (binding && binding->copiesWhole()) {
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

      // `loan` / `loanmut`. A loan gives away no more than the lender had, so a
      // name that does not change cannot be lent for writing — whether it does
      // not change because it owns something quietly, or because what it holds
      // was itself only lent for reading.
      if (binding && e.text == "loanmut" && !binding->changes)
        complain(e.span, "E0407",
                 binding->mode == Mode::Ref
                     ? "this was lent for reading, and cannot be lent for writing."
                     : "`'" + named + "'` does not change, and cannot be lent for "
                                      "writing.",
                 {"a loan gives away no more than the lender had"},
                 {"a chain says `mut` when a name may be written through, and this one "
                  "does not."});
      read(inner);
      return;
    }

    case TypedKind::Call: {
      auto found = functions_.find(e.name);
      if (found == functions_.end()) {
        for (const std::vector<TypedPtr> &given : e.args)
          for (const TypedPtr &item : given)
            read(*item);
        return;
      }
      const FnInfo &info = found->second;
      for (unsigned i = 0; i < e.args.size(); ++i) {
        const std::vector<TypedPtr> &argument = e.args[i];
        const bool known = !info.variadic && i < info.params.size();
        const ParamInfo param = known ? info.params[i] : ParamInfo{Mode::Owned, true};
        if (argument.size() == 1)
          argue(*argument[0], param, e.name);
        else
          for (const TypedPtr &piece : argument)
            read(*piece);
      }
      return;
    }

    case TypedKind::Made: {
      // A struct made where it stands: each item goes into a place of its own,
      // so each is handed over. What the field holds says whether that costs a
      // word.
      fillWith(e.children, e.type);
      return;
    }

    case TypedKind::Case: {
      // `text:'s'` puts `'s'` into a `one-of`, which is a hand-over the same
      // way putting it in a struct is: the case holds it afterwards, and the
      // name it came from does not.
      if (!e.children.empty() && e.sum < program_.sums.size() &&
          e.which < program_.sums[e.sum].fields.size())
        use(*e.children[0], Use::Consume, Mode::Owned,
            copies(program_.sums[e.sum].fields[e.which].type));
      else
        for (const TypedPtr &child : e.children)
          read(*child);
      return;
    }

    default:
      for (const TypedPtr &child : e.children)
        read(*child);
      for (const std::vector<TypedPtr> &given : e.args)
        for (const TypedPtr &item : given)
          read(*item);
      return;
    }
  }

  // One argument against what its parameter asked for.
  void argue(const TypedExpr &e, ParamInfo param, const std::string &path) {
    if (e.kind == TypedKind::Borrow && e.text != "move") {
      const char *wanted = word(param.mode);
      if (e.text != wanted && !(param.mode == Mode::Owned && param.copies))
        complain(e.span, "E0406",
                 "`" + path + "` asks for `" + wanted + "` here, and this says `" +
                     e.text + "`.",
                 {"a transfer is spelled where it happens, and says which one it is"},
                 {"`loan` lends for reading, `loanmut` lends for writing, and `move` hands "
                  "the value over for good."});
      use(e, Use::Consume, param.mode, param.copies);
      return;
    }
    if (e.kind == TypedKind::Borrow) {
      const char *wanted = word(param.mode);
      if (std::string("move") != wanted && !(param.mode == Mode::Owned && param.copies))
        complain(e.span, "E0406",
                 "`" + path + "` asks for `" + wanted + "` here, and this says `move`.",
                 {"a transfer is spelled where it happens, and says which one it is"},
                 {"`loan` lends for reading, `loanmut` lends for writing, and `move` hands "
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

  void block(const TypedBlock &b) {
    scopes_.emplace_back();
    for (const TypedStmtPtr &s : b.stmts)
      statement(*s);
    scopes_.pop_back();
  }

  // What the path of a `set 'p'.a.b` arrives at. Each step is a number the
  // checker worked out where it walked the path itself; nothing here matches a
  // name against a struct's list, which is where this used to go wrong.
  Ty alongPath(Ty of, const std::vector<unsigned> &path) const {
    Ty here = of;
    for (const unsigned step : path) {
      const Field *field = fieldOf(here, step);
      if (!field)
        return Ty{}; // the checker has already said so
      here = field->type;
    }
    return here;
  }

  // One item for each of the things a struct holds. An item that is itself a
  // struct made where it stands fills that field's own shape the same way — the
  // brackets nest, and so does this.
  void fillWith(const std::vector<TypedPtr> &items, Ty fills) {
    if (!fills.isStruct() || fills.named >= program_.shapes.size()) {
      for (const TypedPtr &one : items)
        read(*one);
      return;
    }
    const Shape &shape = program_.shapes[fills.named];
    for (unsigned i = 0; i < items.size(); ++i) {
      const Ty inner = i < shape.fields.size() ? shape.fields[i].type : Ty{};
      if (items[i]->kind == TypedKind::Made) {
        fillWith(items[i]->children, items[i]->type);
        continue;
      }
      use(*items[i], Use::Consume, Mode::Owned,
          i < shape.fields.size() ? copies(inner) : true);
    }
  }

  // Whether one item standing alone is the whole struct rather than the first
  // of the things it holds. A lone item that is already the struct is the
  // struct — and a struct with exactly one field is where the two readings
  // meet, which is where this was getting it wrong: every one-field struct
  // asked for its value to be handed over, however little was in it.
  bool isTheWholeStruct(const TypedExpr &item, Ty fills) {
    const TypedExpr *at = &item;
    if (at->kind == TypedKind::Borrow && !at->children.empty())
      at = at->children[0].get();
    if (at->kind == TypedKind::Made)
      return at->type.isStruct() && at->type.named == fills.named;
    if (at->kind != TypedKind::Name)
      return false;
    const Binding *from = lookup(at->text);
    return from && from->type.isStruct() && from->type.named == fills.named;
  }

  void consumeInto(const std::vector<TypedPtr> &items, Mode mode, bool copiesThere,
                   bool collects = false, bool elementCopies = true, Ty fills = Ty{}) {
    const bool intoStruct = fills.isStruct() && fills.named < program_.shapes.size();
    // A struct's items each go into a place of their own, as a `many`'s do, so
    // each of them is handed over. Reading them as pieces of a joined value let
    // an owning one be put in and let go twice.
    if (intoStruct && !items.empty() &&
        !(items.size() == 1 && isTheWholeStruct(*items[0], fills))) {
      fillWith(items, fills);
      return;
    }
    // Items side by side under a `many` each end up in a place of their own, so
    // each is handed over. Under anything else they are joined, and joining
    // reads its pieces and builds something new out of them.
    if (collects) {
      if (items.size() == 1) {
        const TypedExpr &only = *items[0];
        const Binding *from =
            only.kind == TypedKind::Name ? lookup(only.text) : nullptr;
        const bool whole = from && from->holds();
        use(only, Use::Consume, mode, whole ? copiesThere : elementCopies);
        return;
      }
      for (const TypedPtr &item : items)
        use(*item, Use::Consume, Mode::Owned, elementCopies);
      return;
    }
    if (items.size() == 1)
      use(*items[0], Use::Consume, mode, copiesThere);
    else
      for (const TypedPtr &item : items)
        read(*item);
  }

  void statement(const TypedStmt &s) {
    switch (s.kind) {
    case TypedStmtKind::Declare: {
      const Mode mode = modeOf(s.type);
      const Ty fills = s.type.holds() || mode != Mode::Owned ? Ty{} : s.type;
      consumeInto(s.value, mode, copies(s.type), s.type.holds(), placeCopies(s.type),
                  fills);
      scopes_.back()[s.name] =
          Binding{mode, s.type, s.changeable, s.nameSpan, false, {}, {}, {}};
      break;
    }

    case TypedStmtKind::Add:
    case TypedStmtKind::Set: {
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
        // What one place holds decides how the value is read. A place of a
        // `many.str` holds one text, so a name put there is handed over. A
        // place of a `many.many.int64` holds a whole row, so what is written
        // there is *collected* into one — several items each going into a place
        // of the row, or one item that is already a row and is handed over.
        // Read as one value going in, a number written into a grid looked like
        // a hand-over of something that copies.
        const Ty place = binding && binding->holds() ? elementOf(binding->type) : Ty{};
        consumeInto(s.value, Mode::Owned, copies(place), place.holds(),
                    placeCopies(place));
        break;
      }
      // Writing one of the things it holds is that one's question: which of
      // them is meant is known where it is written, so what may go in is asked
      // of the field rather than of the struct around it.
      if (binding && !s.fields.empty()) {
        // A loan gives away no more than the lender had, and a field lent for
        // reading was being written through: the compiler took it, and then
        // every engine agreed that nothing happened. Silence three ways is the
        // one thing running a program twice cannot find.
        const Ty written = alongPath(binding->type, s.path);
        if (modeOf(written) == Mode::Ref)
          complain(s.nameSpan, "E0407",
                   "`'" + s.name + "'." + s.fields.front() +
                       "` was lent for reading, and cannot be written through.",
                   {"a loan gives away no more than the lender had"},
                   {"`loanmut` is the word for a borrow that may be written through, "
                    "and this field says `loan`."});
        consumeInto(s.value, Mode::Owned, s.path.empty() || copies(written));
        break;
      }
      // `add` puts one more value in a place of its own, exactly as writing a
      // place does, so it asks what a place holds rather than what the whole is.
      if (s.kind == TypedStmtKind::Add) {
        const Ty place = binding && binding->holds() ? elementOf(binding->type) : Ty{};
        consumeInto(s.value, Mode::Owned, copies(place), place.holds(),
                    placeCopies(place));
        break;
      }
      consumeInto(s.value, binding ? binding->mode : Mode::Owned,
                  binding ? binding->copiesWhole() : true,
                  binding && binding->holds(),
                  binding ? binding->placeCopiesInto() : true,
                  binding && binding->fillsAStruct() ? binding->type : Ty{});
      if (binding)
        binding->moved = false; // it holds something again
      break;
    }

    case TypedStmtKind::When:
    case TypedStmtKind::If: {
      // Each arm is a way the program could go, so a name given away down one
      // is gone after all of them — the compiler does not get to assume which
      // arm ran.
      if (s.kind == TypedStmtKind::When && s.condition)
        read(*s.condition);
      const auto before = snapshot();
      std::vector<std::pair<Binding *, Span>> movedSomewhere;
      for (const TypedArm &arm : s.arms) {
        restore(before);
        if (s.kind == TypedStmtKind::If && arm.condition)
          read(*arm.condition);
        scopes_.emplace_back();
        if (!arm.matchesNothing && !arm.binds.empty())
          scopes_.back()[arm.binds] = heldBinding(arm.bindsSpan, arm.bound);
        for (const TypedStmtPtr &inner : arm.body.stmts)
          statement(*inner);
        scopes_.pop_back();
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

    // The block changes nothing about what is inside it: what it grants is
    // asked for by name, and ownership is not one of the things it grants.
    case TypedStmtKind::Unsafe:
      for (const TypedStmtPtr &inner : s.body.stmts)
        statement(*inner);
      break;

    case TypedStmtKind::LoopRange:
    case TypedStmtKind::LoopWhile: {
      const auto before = snapshot();
      if (s.condition)
        read(*s.condition);
      if (s.from)
        read(*s.from);
      if (s.to)
        read(*s.to);
      scopes_.emplace_back();
      if (!s.binds.empty())
        scopes_.back()[s.binds] = heldBinding(s.bindsSpan, s.bound);
      if (s.kind == TypedStmtKind::LoopRange)
        scopes_.back()[s.name] =
            Binding{Mode::Owned, s.type, false, s.nameSpan, false, {}, {}, {}};
      for (const TypedStmtPtr &inner : s.body.stmts)
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

    case TypedStmtKind::Give:
      for (const TypedPtr &one : s.value) {
        if (s.value.size() != 1) {
          read(*one);
          continue;
        }
        const TypedExpr &given = *one;
        if (giving_ != Mode::Owned) {
          givingBorrow(given);
          continue;
        }
        // `give` is the word. A call site has a choice between lending and
        // handing over, so it says which; `give` has no second reading, so
        // spelling `move` here would be a second way to write one thing.
        if (given.kind == TypedKind::Borrow && given.text == "move") {
          complain(given.span, "E0406", "`give` already hands the answer over.",
                   {"a word is written where there is a choice, and here there is none"},
                   {"a call site chooses between lending and handing over, so it says "
                    "which; there is nothing else `give` could mean."});
          continue;
        }
        read(given);
        if (!givingCopies_ && given.kind == TypedKind::Name)
          if (Binding *binding = lookup(given.text))
            binding->moved = true;
      }
      break;

    case TypedStmtKind::Break:
      break;

    case TypedStmtKind::Call:
      if (s.call)
        read(*s.call);
      break;
    }
  }

  // An answer that is borrowed has to be borrowed from something the caller lent.
  void givingBorrow(const TypedExpr &given) {
    const TypedExpr *inner = &given;
    if (given.kind == TypedKind::Borrow && !given.children.empty())
      inner = given.children[0].get();

    if (inner->kind == TypedKind::Name) {
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

  // Whether a type still has a blank in it — `any`, written where a type goes.
  static bool hasBlank(Ty type) {
    return type.kind == Type::Blank || type.element == Type::Blank;
  }

  void body(const TypedItem &item) {
    if (item.kind == TypedItemKind::Const)
      return;

    // A generic's body is not read with the blank still in it, for the same
    // reason the checker will not: whether a thing copies or is handed over is
    // exactly what the blank has not said yet, and this pass asks that of
    // everything. The body is read once per type it is called with, after the
    // blank is filled.
    if (item.kind == TypedItemKind::Function) {
      if (hasBlank(item.answers))
        return;
      for (const TypedParam &param : item.params)
        if (hasBlank(param.type))
          return;
    }

    scopes_.emplace_back();
    if (item.kind == TypedItemKind::Function) {
      giving_ = modeOf(item.answers);
      givingCopies_ = copies(item.answers);
      for (const TypedParam &param : item.params)
        scopes_.back()[param.name] =
            Binding{modeOf(param.type), param.type,
                    modeOf(param.type) == Mode::RefMut, param.nameSpan, false, {}, {}, {}};
    } else {
      giving_ = Mode::Owned;
      givingCopies_ = true;
    }
    for (const TypedStmtPtr &s : item.body.stmts)
      statement(*s);
    scopes_.pop_back();
  }
};

} // namespace

OwnResult own(const Source &source, const TypedProgram &program) {
  return Owner(source, program).run();
}

OwnResult own(const Source &source, const Program &program, const CheckResult &checked) {
  const TypedResult tree = typedTree(source, program, checked);
  return own(source, tree.program);
}

} // namespace xag
