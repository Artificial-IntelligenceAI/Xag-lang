#include "xag/Units.h"

#include "xag/Ast.h"
#include "xag/Check.h"
#include "xag/Parser.h"

#include <dirent.h>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <sys/stat.h>
#include <unordered_map>
#include <algorithm>
#include <unordered_set>

namespace xag {
namespace {

// ---- a manifest, read

// As much TOML as a manifest uses: `[section]`, `key = "text"`, and
// `key = ["text", "text"]`. Written by hand rather than pulled in, because the
// shape is three forms and a reader who meets a fourth should be told exactly
// where — which a library would say in its own words, about its own model.
struct Said {
  std::string section;
  std::string key;
  std::vector<std::string> values; // one for a string, several for a list
  Span span;                       // the whole line
  Span valueSpan;                  // just what was said
};

struct Manifest {
  std::shared_ptr<Source> source;
  std::vector<Said> said;
  std::vector<Diagnostic> trouble;
};

std::string trimmed(std::string t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
    t.erase(t.begin());
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r'))
    t.pop_back();
  return t;
}

Manifest readManifest(const std::string &path) {
  Manifest out;
  std::ifstream in(path);
  std::stringstream whole;
  whole << in.rdbuf();
  out.source = std::make_shared<Source>(path, whole.str());

  const std::string_view text = out.source->text();
  std::string section;
  unsigned at = 0;
  while (at < text.size()) {
    const unsigned lineBegin = at;
    unsigned lineEnd = at;
    while (lineEnd < text.size() && text[lineEnd] != '\n')
      ++lineEnd;
    at = lineEnd + 1;

    std::string line(text.substr(lineBegin, lineEnd - lineBegin));
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos)
      line = line.substr(0, hash);
    line = trimmed(line);
    if (line.empty())
      continue;

    if (line.front() == '[' && line.back() == ']') {
      section = trimmed(line.substr(1, line.size() - 2));
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      out.trouble.push_back(Diagnostic{
          Span{lineBegin, lineEnd}, "E0601",
          "this line of the manifest is not `key = value`.", "here",
          {"a manifest is sections of `key = value` lines"},
          {"`[unit]`, then `name = \"text\"`; `[uses]`, then `paths = [\"../text\"]`."}});
      continue;
    }
    Said one;
    one.section = section;
    one.key = trimmed(line.substr(0, equals));
    one.span = Span{lineBegin, lineEnd};
    std::string value = trimmed(line.substr(equals + 1));
    // Where the value starts in the source, for pointing at it.
    const std::size_t valueAt = text.find(value, lineBegin);
    one.valueSpan = Span{static_cast<unsigned>(valueAt),
                         static_cast<unsigned>(valueAt + value.size())};

    if (!value.empty() && value.front() == '[') {
      if (value.back() != ']') {
        out.trouble.push_back(Diagnostic{
            one.valueSpan, "E0601", "this list does not close.", "here",
            {"a list is `[` then values then `]`, on one line"}});
        continue;
      }
      std::string inside = value.substr(1, value.size() - 2);
      std::size_t from = 0;
      while (from < inside.size()) {
        std::size_t comma = inside.find(',', from);
        if (comma == std::string::npos)
          comma = inside.size();
        std::string item = trimmed(inside.substr(from, comma - from));
        if (item.size() >= 2 && item.front() == '"' && item.back() == '"')
          one.values.push_back(item.substr(1, item.size() - 2));
        else if (!item.empty())
          out.trouble.push_back(Diagnostic{
              one.valueSpan, "E0601", "`" + item + "` is not a quoted value.", "here",
              {"a manifest's values are written in double quotes"}});
        from = comma + 1;
      }
    } else if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      one.values.push_back(value.substr(1, value.size() - 2));
    } else {
      out.trouble.push_back(Diagnostic{
          one.valueSpan, "E0601", "`" + value + "` is not a quoted value or a list.",
          "here", {"a manifest's values are written in double quotes"}});
      continue;
    }
    out.said.push_back(std::move(one));
  }
  return out;
}

const Said *find(const Manifest &m, const std::string &section, const std::string &key) {
  for (const Said &one : m.said)
    if (one.section == section && one.key == key)
      return &one;
  return nullptr;
}

// ---- the filesystem, as little of it as needed

std::string directoryOf(const std::string &path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string upFrom(const std::string &directory) {
  if (directory == "/" || directory == ".")
    return {};
  const std::size_t slash = directory.find_last_of('/');
  if (slash == std::string::npos)
    return ".";
  const std::string up = directory.substr(0, slash);
  return up.empty() ? "/" : up;
}

bool exists(const std::string &path) {
  struct stat info {};
  return ::stat(path.c_str(), &info) == 0;
}

// The manifest beside a source, or the nearest one above it.
std::string manifestFor(const std::string &sourcePath) {
  std::string directory = directoryOf(sourcePath);
  for (unsigned up = 0; up < 32 && !directory.empty(); ++up) {
    const std::string candidate = directory + "/Xag-Config.toml";
    if (exists(candidate))
      return candidate;
    directory = upFrom(directory);
  }
  return {};
}

// Every `.xag` in a directory, in name order so that a build is the same build
// twice. Not below it: a unit is one directory, and a directory under it is
// somebody else's.
std::vector<std::string> xagFilesIn(const std::string &directory) {
  std::vector<std::string> out;
  if (DIR *dir = ::opendir(directory.c_str())) {
    while (const dirent *entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".xag") == 0)
        out.push_back(directory + "/" + name);
    }
    ::closedir(dir);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// One spelling per directory, so that `a/../b/../a` and `a` are the same place
// — which they are, and which a loop is detected by. Left as written where the
// place does not exist, so a diagnostic about it can still name what was said.
std::string canonical(const std::string &path) {
  char resolved[4096];
  if (::realpath(path.c_str(), resolved))
    return resolved;
  return path;
}

// A path in a manifest is written against the manifest's own directory.
std::string joined(const std::string &directory, const std::string &path) {
  if (!path.empty() && path.front() == '/')
    return canonical(path);
  return canonical(directory + "/" + path);
}

// Whether a word can stand as a name or a prefix in Xag: the characters a word
// may be made of, and not one of the words a chain reads.
bool spellable(const std::string &word) {
  if (word.empty())
    return false;
  for (char c : word)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_'))
      return false;
  return true;
}

// ---- reading a unit, and everything it reaches

class Reader {
public:
  UnitsResult run(const std::string &sourcePath) {
    const std::string manifest = manifestFor(sourcePath);
    if (manifest.empty()) {
      // No manifest is a unit with nothing to say: no name, nothing used. Most
      // programs are this, and it is not a mistake.
      result_.self.directory = directoryOf(sourcePath);
      return std::move(result_);
    }
    // The entry's own unit is read the way every library is, and then walked
    // from. A cycle back to it is a cycle like any other.
    Unit self;
    if (!readUnit(manifest, self, /*mustBeNamed=*/false))
      return std::move(result_);
    result_.self = self;
    std::vector<std::string> path{canonical(directoryOf(manifest))};
    follow(self, path);
    return std::move(result_);
  }

private:
  UnitsResult result_;
  // Libraries already read, by directory, so a library two programs' paths
  // both reach is read once and appears once.
  std::unordered_map<std::string, unsigned> seen_;

  void complain(const std::shared_ptr<Source> &in, Diagnostic d) {
    result_.diagnostics.push_back(std::move(d));
    result_.about.push_back(in);
  }

  // `[defaults]`: the settings that change what a program answers. Each has
  // exactly the values the manifest documents, and a value that is none of
  // them is refused rather than read as the default — a manifest that says
  // `logic = "short-circuit"` meant something, and quietly giving it the other
  // thing is how a program comes to run under a language it did not ask for.
  bool readDefaults(const Manifest &m, Unit &out) {
    struct Knob {
      const char *key;
      const char *yes;   // the value that sets the flag
      const char *no;    // the default
      bool Settings::*flag;
    };
    static const Knob knobs[] = {
        {"logic", "asks-both", "stops-early", &Settings::asksBoth},
        {"division", "floored", "truncated", &Settings::floored},
        {"characters", "letters", "clusters", &Settings::letters},
        {"no-number", "stops", "carries-on", &Settings::noNumberStops},
    };
    for (const Knob &knob : knobs) {
      const Said *said = find(m, "defaults", knob.key);
      if (!said)
        continue;
      const std::string value = said->values.size() == 1 ? said->values.front() : "";
      if (value == knob.yes) {
        out.settings.*knob.flag = true;
      } else if (value != knob.no) {
        complain(m.source,
                 Diagnostic{said->valueSpan, "E0609",
                            "`" + std::string(knob.key) + "` cannot be `" + value + "`.", "here",
                            {"a setting has the values its manifest names, and no others"},
                            {"`" + std::string(knob.key) + "` is `\"" + knob.no + "\"` or `\"" +
                             knob.yes + "\"`."}});
        return false;
      }
    }
    return true;
  }

  bool readUnit(const std::string &manifestPath, Unit &out, bool mustBeNamed) {
    Manifest m = readManifest(manifestPath);
    for (Diagnostic &d : m.trouble)
      complain(m.source, std::move(d));
    if (!m.trouble.empty())
      return false;

    out.manifest = m.source;
    out.directory = canonical(directoryOf(manifestPath));
    out.files = xagFilesIn(out.directory);

    if (const Said *name = find(m, "unit", "name")) {
      out.name = name->values.empty() ? std::string() : name->values.front();
      out.nameSpan = name->valueSpan;
    }
    if (const Said *called = find(m, "unit", "called")) {
      out.called = called->values.empty() ? std::string() : called->values.front();
      out.calledSpan = called->valueSpan;
    }
    if (const Said *uses = find(m, "uses", "paths"))
      out.uses = uses->values;
    if (!readDefaults(m, out))
      return false;

    if (mustBeNamed) {
      if (out.name.empty()) {
        complain(m.source, Diagnostic{Span{0, 0}, "E0602",
                                      "this library's manifest gives it no name.", "here",
                                      {"a library says what it is called, both ways"},
                                      {"`[unit]`, then `name = \"text\"` for `import "
                                       "'text';` and `called = \"t\"` for `t.thing[…]`."}});
        return false;
      }
      if (out.called.empty()) {
        complain(m.source, Diagnostic{out.nameSpan, "E0602",
                                      "`" + out.name + "` says no name for its use sites.",
                                      "here", {"a library says what it is called, both ways"},
                                      {"`called = \"t\"` is what a use site writes: "
                                       "`t.thing[…]`. Bare names are the language's own, so "
                                       "every library has one."}});
        return false;
      }
    }
    for (const auto &[which, span] : {std::pair{&out.name, out.nameSpan},
                                      std::pair{&out.called, out.calledSpan}}) {
      if (which->empty())
        continue;
      if (!spellable(*which)) {
        complain(m.source, Diagnostic{span, "E0602",
                                      "`" + *which + "` cannot be spelled as a name.", "here",
                                      {"a name is letters, digits, `-` and `_`"}});
        return false;
      }
      if (isChainWord(*which) || typeNamed(*which) != Type::Unknown) {
        complain(m.source, Diagnostic{span, "E0602",
                                      "`" + *which + "` is a word a chain already reads.",
                                      "here",
                                      {"a unit's name cannot be a word a chain asks its "
                                       "questions with"},
                                      {"`var.t.point 'p'` reads `t` as answering a question "
                                       "unless nothing asks one by that word. Pick another."}});
        return false;
      }
    }
    return true;
  }

  // Walks `[uses]` from a unit, reading each library once and refusing a loop.
  // `path` is the chain of directories being walked, for saying where the loop
  // closes.
  void follow(const Unit &from, std::vector<std::string> &path) {
    for (const std::string &use : from.uses) {
      const std::string directory = joined(from.directory, use);
      const std::string manifest = directory + "/Xag-Config.toml";
      if (!exists(manifest)) {
        complain(from.manifest,
                 Diagnostic{spanOfUse(from, use), "E0603",
                            "there is no library at `" + use + "`.", "here",
                            {"a path in `[uses]` is a directory holding a "
                             "`Xag-Config.toml` with a `[unit]` in it"},
                            {"the path is written against this manifest's own directory, "
                             "which is `" + from.directory + "`."}});
        continue;
      }
      // A directory already on the path being walked is a loop.
      for (const std::string &walking : path)
        if (walking == directory) {
          std::string around;
          for (const std::string &step : path)
            around += (around.empty() ? "" : " uses ") + step;
          complain(from.manifest,
                   Diagnostic{spanOfUse(from, use), "E0604",
                              "`" + use + "` is already being used by something it uses.",
                              "here", {"a unit does not use itself, however far round"},
                              {"the loop is: " + around + " uses " + directory +
                               ". Two things that need each other are one thing, or "
                               "there is a third thing inside them that both use."}});
          return;
        }
      if (seen_.count(directory))
        continue; // read already, by another route, and it goes in once
      Unit library;
      if (!readUnit(manifest, library, /*mustBeNamed=*/true))
        continue;
      // Two libraries may not answer to one import name: `import 'text';`
      // has to mean one thing.
      for (const Unit &other : result_.libraries)
        if (other.name == library.name) {
          complain(library.manifest,
                   Diagnostic{library.nameSpan, "E0605",
                              "two libraries are both called `" + library.name + "`.",
                              "here", {"an import name means one library"},
                              {"the other is at `" + other.directory + "`."}});
          return;
        }
      path.push_back(directory);
      follow(library, path);
      path.pop_back();
      // After what it uses, so the list reads in an order where nothing comes
      // before what it depends on.
      seen_[directory] = static_cast<unsigned>(result_.libraries.size());
      result_.libraries.push_back(std::move(library));
    }
  }

  static Span spanOfUse(const Unit &in, const std::string &use) {
    if (!in.manifest)
      return Span{};
    const std::size_t at = in.manifest->text().find("\"" + use + "\"");
    if (at == std::string::npos)
      return Span{};
    return Span{static_cast<unsigned>(at), static_cast<unsigned>(at + use.size() + 2)};
  }
};

} // namespace

UnitsResult unitsFor(const std::string &sourcePath) { return Reader().run(sourcePath); }

namespace {

// ---- writing a library's call name onto what it declares

struct Renames {
  std::unordered_map<std::string, std::string> types;     // struct, one-of
  std::unordered_map<std::string, std::string> functions; // fn
  std::unordered_map<std::string, std::string> constants; // const
};

std::string renamed(const std::unordered_map<std::string, std::string> &map,
                    const std::string &name) {
  auto found = map.find(name);
  return found == map.end() ? name : found->second;
}

void qualifyChain(Chain &chain, const Renames &r) {
  if (chain.segments.empty())
    return;
  ChainSegment &type = chain.segments.back();
  if (!type.isName)
    type.text = renamed(r.types, type.text);
}

void qualifyBlock(Block &block, const Renames &r);
void qualifyValues(ValueList &list, const Renames &r);

void qualifyExpr(Expr *e, const Renames &r) {
  if (!e)
    return;
  switch (e->kind) {
  case ExprKind::Call:
    // `twice[…]` and `point[…]` — a function called, or a struct made where it
    // stands. Both are a word before a bracket, and both are renamed the same
    // way. A dotted callee is a built-in — `print.stdout` — and is left alone.
    if (e->path.size() == 1) {
      const std::string was = e->path.front();
      e->path.front() = renamed(r.functions, was);
      if (e->path.front() == was)
        e->path.front() = renamed(r.types, was);
    }
    break;
  case ExprKind::Typed:
    // `answer:*7*` names a type; `ok:*7*` names a case, and a case is left as
    // it was — it belongs to its type, and is reached through the type.
    e->text = renamed(r.types, e->text);
    break;
  case ExprKind::Name:
    e->text = renamed(r.constants, e->text);
    break;
  default:
    break;
  }
  for (ExprPtr &child : e->children)
    qualifyExpr(child.get(), r);
  qualifyValues(e->args, r);
}

void qualifyValues(ValueList &list, const Renames &r) {
  for (Value &value : list.values)
    for (ExprPtr &item : value.items)
      qualifyExpr(item.get(), r);
}

void qualifyStmt(Stmt &s, const Renames &r) {
  qualifyChain(s.chain, r);
  qualifyValues(s.value, r);
  qualifyExpr(s.index.get(), r);
  qualifyExpr(s.condition.get(), r);
  qualifyExpr(s.call.get(), r);
  for (Branch &branch : s.branches) {
    qualifyExpr(branch.condition.get(), r);
    qualifyBlock(branch.body, r);
  }
  qualifyBlock(s.body, r);
}

void qualifyBlock(Block &block, const Renames &r) {
  for (StmtPtr &s : block.stmts)
    if (s)
      qualifyStmt(*s, r);
}

// ---- what a file reaches for

// Every prefix a file's code names, with where it named it — the first place
// each, since one complaint per missing import is what a reader wants.
struct Reached {
  std::vector<std::pair<std::string, Span>> prefixes;
  void note(const std::string &prefix, Span at) {
    for (const auto &[had, where] : prefixes)
      if (had == prefix)
        return;
    prefixes.emplace_back(prefix, at);
  }
};

void reachedInChain(const Chain &chain, Reached &r) {
  if (chain.segments.empty())
    return;
  const ChainSegment &type = chain.segments.back();
  const std::size_t dot = type.text.find('.');
  if (!type.isName && dot != std::string::npos)
    r.note(type.text.substr(0, dot), type.span);
}

void reachedInBlock(const Block &block, Reached &r);
void reachedInValues(const ValueList &list, Reached &r);

void reachedInExpr(const Expr *e, Reached &r) {
  if (!e)
    return;
  if (e->kind == ExprKind::Call && e->path.size() == 2)
    r.note(e->path.front(), e->span);
  if (e->kind == ExprKind::Typed) {
    const std::size_t dot = e->text.find('.');
    if (dot != std::string::npos)
      r.note(e->text.substr(0, dot), e->span);
  }
  for (const ExprPtr &child : e->children)
    reachedInExpr(child.get(), r);
  reachedInValues(e->args, r);
}

void reachedInValues(const ValueList &list, Reached &r) {
  for (const Value &value : list.values)
    for (const ExprPtr &item : value.items)
      reachedInExpr(item.get(), r);
}

void reachedInStmt(const Stmt &s, Reached &r) {
  reachedInChain(s.chain, r);
  reachedInValues(s.value, r);
  reachedInExpr(s.index.get(), r);
  reachedInExpr(s.condition.get(), r);
  reachedInExpr(s.call.get(), r);
  for (const Branch &branch : s.branches) {
    reachedInExpr(branch.condition.get(), r);
    reachedInBlock(branch.body, r);
  }
  reachedInBlock(s.body, r);
}

void reachedInBlock(const Block &block, Reached &r) {
  for (const StmtPtr &s : block.stmts)
    if (s)
      reachedInStmt(*s, r);
}

} // namespace

void applySettings(Program &file, const Unit &unit) {
  for (Item &item : file.items)
    item.settings = unit.settings;
}

std::vector<Diagnostic> importsCover(const Program &file, const UnitsResult &units) {
  Reached reached;
  std::vector<std::string> imported;
  for (const Item &item : file.items) {
    if (item.kind == ItemKind::Import) {
      imported.push_back(item.name);
      continue;
    }
    reachedInChain(item.chain, reached);
    for (const Param &param : item.params)
      reachedInChain(param.chain, reached);
    reachedInValues(item.value, reached);
    reachedInBlock(item.body, reached);
  }

  std::vector<Diagnostic> out;
  for (const auto &[prefix, where] : reached.prefixes) {
    // Which library this prefix is. A dotted callee that is no library's call
    // name — `print.stdout` — is a built-in and is nobody's to import.
    const Unit *library = nullptr;
    for (const Unit &one : units.libraries)
      if (one.called == prefix)
        library = &one;
    if (!library)
      continue;
    bool covered = false;
    for (const std::string &name : imported)
      covered = covered || name == library->name;
    if (covered)
      continue;
    out.push_back(Diagnostic{
        where, "E0608",
        "this file reaches into `" + prefix + "` without importing it.", "here",
        {"a file uses what it imports"},
        {"`import '" + library->name + "';` in this file's `PREP` or `LIBRARY` says so. "
         "The manifest says what the program could use; `import` says what this file "
         "does, so a reader sees where a name came from without leaving the file."}});
  }
  return out;
}

// Who may see a declaration: this file, the unit, or everyone.
enum class Sees { File, Program, Export };

Sees visibilityOf(const Chain &chain) {
  for (const ChainSegment &seg : chain.segments) {
    if (seg.isName)
      continue;
    if (seg.text == "export")
      return Sees::Export;
    if (seg.text == "program")
      return Sees::Program;
  }
  return Sees::File;
}

void qualify(std::vector<Program> &files, const Unit &unit) {
  // Names the whole unit shares, and the names each file keeps to itself. A
  // reference is looked up in its own file's first, then the unit's, so a file
  // that declares its own `helper` gets its own and one that does not gets
  // nothing — never another file's.
  Renames shared;
  std::vector<Renames> own(files.size());
  for (std::size_t f = 0; f < files.size(); ++f) {
    for (const Item &item : files[f].items) {
      std::unordered_map<std::string, std::string> *shareMap = nullptr;
      std::unordered_map<std::string, std::string> *ownMap = nullptr;
      switch (item.kind) {
      case ItemKind::Struct:
      case ItemKind::OneOf:
        shareMap = &shared.types;
        ownMap = &own[f].types;
        break;
      case ItemKind::Function:
        shareMap = &shared.functions;
        ownMap = &own[f].functions;
        break;
      case ItemKind::Const:
        shareMap = &shared.constants;
        ownMap = &own[f].constants;
        break;
      default:
        continue;
      }
      switch (visibilityOf(item.chain)) {
      case Sees::Export:
        (*shareMap)[item.name] = unit.called + "." + item.name;
        break;
      case Sees::Program:
        (*shareMap)[item.name] = unit.called + "$" + item.name;
        break;
      case Sees::File:
        (*ownMap)[item.name] = unit.called + "$" + std::to_string(f) + "$" + item.name;
        break;
      }
    }
  }

  for (std::size_t f = 0; f < files.size(); ++f) {
    // This file's view: its own names in front of the unit's.
    Renames r = shared;
    for (const auto &[from, to] : own[f].types)
      r.types[from] = to;
    for (const auto &[from, to] : own[f].functions)
      r.functions[from] = to;
    for (const auto &[from, to] : own[f].constants)
      r.constants[from] = to;

    for (Item &item : files[f].items) {
      switch (item.kind) {
      case ItemKind::Struct:
      case ItemKind::OneOf:
        item.name = renamed(r.types, item.name);
        break;
      case ItemKind::Function:
        item.name = renamed(r.functions, item.name);
        break;
      case ItemKind::Const:
        item.name = renamed(r.constants, item.name);
        break;
      default:
        break;
      }
      qualifyChain(item.chain, r);
      for (Param &param : item.params)
        qualifyChain(param.chain, r);
      qualifyValues(item.value, r);
      qualifyBlock(item.body, r);
    }
  }
}

const Unit *unitNamed(const UnitsResult &units, const std::string &name) {
  for (const Unit &one : units.libraries)
    if (one.name == name)
      return &one;
  return nullptr;
}

} // namespace xag
