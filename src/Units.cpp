#include "xag/Units.h"

#include "xag/Ast.h"
#include "xag/Check.h"
#include "xag/Lexer.h"
#include "xag/Parser.h"

#include <dirent.h>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <sys/stat.h>
#include <unordered_map>
#include <algorithm>
#include <cstring>
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

// ---- a library's account of itself, read off its `LIBRARY` line

// The tokens of a `.xaglib`, walked as far as its `LIBRARY` line goes. The
// parser reads the same line properly later, with every library's call name in
// hand; this is the reading that produces those call names, so it has to come
// first and cannot be the parser. It takes what it can and says nothing about
// what is malformed — the parser will, in its own words, in the same place.
struct Header {
  std::string name, called;
  std::vector<std::string> uses;
  std::vector<Span> usesSpans;
  Span nameSpan, calledSpan;
  Settings settings;
  bool found = false; // a `LIBRARY` line at all
};

Header readHeader(const Source &source) {
  Header out;
  const LexResult lexed = lex(source);
  const std::vector<Token> &t = lexed.tokens;
  std::size_t at = 0;
  while (at < t.size() && !(t[at].kind == TokenKind::Word && t[at].text == "LIBRARY"))
    ++at;
  if (at == t.size())
    return out;
  out.found = true;
  ++at;
  while (at + 1 < t.size() && t[at].kind == TokenKind::Dot && t[at + 1].kind == TokenKind::Word) {
    settingWord(t[at + 1].text, out.settings);
    at += 2;
  }
  if (at < t.size() && t[at].kind == TokenKind::Name) {
    out.name = t[at].text;
    out.nameSpan = t[at].span;
    ++at;
  }
  if (at + 1 < t.size() && t[at].kind == TokenKind::Word && t[at].text == "called" &&
      t[at + 1].kind == TokenKind::Name) {
    out.called = t[at + 1].text;
    out.calledSpan = t[at + 1].span;
    at += 2;
  }
  if (at + 1 < t.size() && t[at].kind == TokenKind::Word && t[at].text == "uses" &&
      t[at + 1].kind == TokenKind::LBracket) {
    at += 2;
    while (at < t.size() && t[at].kind != TokenKind::RBracket && t[at].kind != TokenKind::LBrace) {
      if (t[at].kind == TokenKind::Written) {
        out.uses.push_back(t[at].text);
        out.usesSpans.push_back(t[at].span);
      }
      ++at;
    }
  }
  return out;
}

bool endsWith(const std::string &text, const char *tail) {
  const std::size_t n = std::strlen(tail);
  return text.size() >= n && text.compare(text.size() - n, n, tail) == 0;
}

// `to`, written against `from`'s directory: what goes into a manifest so that
// the manifest reads the same wherever the project is moved to.
std::string relativeTo(const std::string &directory, const std::string &to) {
  std::vector<std::string> a, b;
  std::stringstream sa(canonical(directory)), sb(to);
  std::string piece;
  while (std::getline(sa, piece, '/'))
    if (!piece.empty())
      a.push_back(piece);
  while (std::getline(sb, piece, '/'))
    if (!piece.empty())
      b.push_back(piece);
  std::size_t same = 0;
  while (same < a.size() && same < b.size() && a[same] == b[same])
    ++same;
  std::string out;
  for (std::size_t i = same; i < a.size(); ++i)
    out += "../";
  for (std::size_t i = same; i < b.size(); ++i)
    out += (i > same ? "/" : "") + b[i];
  return out.empty() ? "." : out;
}

// ---- reading a unit, and everything it reaches

class Reader {
public:
  UnitsResult run(const std::string &sourcePath) {
    // A library named directly: built alone. It is its own unit, and what it
    // uses is followed from it.
    if (endsWith(sourcePath, ".xaglib")) {
      Unit self;
      if (!readLibrary(canonical(sourcePath), self))
        return std::move(result_);
      result_.self = self;
      std::vector<std::string> path{canonical(sourcePath)};
      follow(self, path);
      return std::move(result_);
    }

    const std::string manifest = manifestFor(sourcePath);
    if (manifest.empty()) {
      // No manifest is a unit with nothing to say: one file, no name, nothing
      // used. Most programs are this, and it is not a mistake.
      result_.self.directory = directoryOf(sourcePath);
      result_.self.files.push_back(sourcePath);
      return std::move(result_);
    }
    Unit self;
    if (!readProgram(manifest, sourcePath, self))
      return std::move(result_);
    result_.self = self;
    std::vector<std::string> path{canonical(manifest)};
    follow(self, path);
    if (result_.ok())
      writeWhatWasReached(manifest, self);
    return std::move(result_);
  }

private:
  UnitsResult result_;
  // Libraries already read, by file, so a library two paths both reach is
  // read once and appears once.
  std::unordered_map<std::string, unsigned> seen_;
  // Which libraries the program's own manifest listed, by canonical file.
  std::unordered_set<std::string> listed_;

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

  // The program: its manifest, and the file that was named.
  bool readProgram(const std::string &manifestPath, const std::string &named, Unit &out) {
    Manifest m = readManifest(manifestPath);
    for (Diagnostic &d : m.trouble)
      complain(m.source, std::move(d));
    if (!m.trouble.empty())
      return false;

    out.manifest = m.source;
    out.directory = canonical(directoryOf(manifestPath));
    if (const Said *uses = find(m, "uses", "paths")) {
      out.uses = uses->values;
      out.usesSpans.assign(uses->values.size(), uses->valueSpan);
    }
    if (!readDefaults(m, out))
      return false;

    // Which files. `main` and `files` name them; without `main`, the program
    // is the one file that was named, which is what every single-file
    // program is.
    const Said *main = find(m, "unit", "main");
    const Said *files = find(m, "unit", "files");
    if (!main) {
      if (files) {
        complain(m.source, Diagnostic{files->span, "E0618",
                                      "`files` without `main` says which files and not which "
                                      "one runs.",
                                      "here", {"a program of several files says which one is its door"},
                                      {"`main = \"main.xag\"` beside it — that file's `START` "
                                       "runs, and the others' must be empty."}});
        return false;
      }
      out.files.push_back(named);
      return true;
    }
    const std::string mainFile = main->values.size() == 1 ? main->values.front() : "";
    out.files.push_back(joined(out.directory, mainFile));
    if (files)
      for (const std::string &file : files->values)
        if (joined(out.directory, file) != out.files.front())
          out.files.push_back(joined(out.directory, file));
    for (std::size_t i = 0; i < out.files.size(); ++i) {
      const std::string &file = out.files[i];
      if (!exists(file) || !endsWith(file, ".xag")) {
        complain(m.source, Diagnostic{i == 0 ? main->valueSpan : files->valueSpan, "E0618",
                                      "there is no program file at `" +
                                          (i == 0 ? mainFile : files->values[i - 1]) + "`.",
                                      "here", {"a program's files are the `.xag` files its manifest names"},
                                      {"the path is written against this manifest's own "
                                       "directory, which is `" + out.directory + "`."}});
        return false;
      }
    }
    // The file that was named has to be one of them, or it is a file the
    // manifest says nothing about, in a directory the manifest speaks for.
    const std::string here = canonical(named);
    bool among = false;
    for (const std::string &file : out.files)
      among = among || file == here;
    if (!among) {
      complain(m.source, Diagnostic{main->span, "E0618",
                                    "`" + named + "` is not one of this program's files.",
                                    "here", {"a program is the files its manifest names"},
                                    {"`main` and `files` in this manifest say which files the "
                                     "program is. Add it, or take the file somewhere the "
                                     "manifest does not reach."}});
      return false;
    }
    return true;
  }

  // One `.xaglib`, read off its `LIBRARY` line.
  bool readLibrary(const std::string &file, Unit &out) {
    std::ifstream in(file);
    std::stringstream whole;
    whole << in.rdbuf();
    out.manifest = std::make_shared<Source>(file, whole.str());
    out.library = true;
    out.directory = directoryOf(file);
    out.files.push_back(file);
    const Header header = readHeader(*out.manifest);
    if (!header.found) {
      complain(out.manifest, Diagnostic{Span{0, 0}, "E0616",
                                        "this `.xaglib` has no `LIBRARY` line.", "here",
                                        {"a library is `READ_ME`, `LIBRARY` and `ITMT`"},
                                        {"`LIBRARY 'text' called 't' {` names it both ways."}});
      return false;
    }
    out.name = header.name;
    out.called = header.called;
    out.nameSpan = header.nameSpan;
    out.calledSpan = header.calledSpan;
    out.uses = header.uses;
    out.usesSpans = header.usesSpans;
    out.settings = header.settings;
    if (out.name.empty() || out.called.empty())
      return false; // the parser says what is missing, at the line itself
    for (const auto &[which, span] : {std::pair{&out.name, out.nameSpan},
                                      std::pair{&out.called, out.calledSpan}}) {
      if (!spellable(*which)) {
        complain(out.manifest, Diagnostic{span, "E0602",
                                          "`" + *which + "` cannot be spelled as a name.", "here",
                                          {"a name is letters, digits, `-` and `_`"}});
        return false;
      }
      if (isChainWord(*which) || typeNamed(*which) != Type::Unknown) {
        complain(out.manifest, Diagnostic{span, "E0602",
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

  // Walks what a unit uses, reading each library once and refusing a loop.
  // `path` is the chain of files being walked, for saying where the loop
  // closes.
  void follow(const Unit &from, std::vector<std::string> &path) {
    for (std::size_t i = 0; i < from.uses.size(); ++i) {
      const std::string &use = from.uses[i];
      const Span at = i < from.usesSpans.size() ? from.usesSpans[i] : Span{};
      const std::string file = joined(from.directory, use);
      if (!exists(file) || !endsWith(file, ".xaglib")) {
        complain(from.manifest,
                 Diagnostic{at, "E0603", "there is no library at `" + use + "`.", "here",
                            {"a library is one `.xaglib` file, and a path to one names it"},
                            {"the path is written against " +
                             std::string(from.library ? "the library's own directory"
                                                      : "this manifest's own directory") +
                             ", which is `" + from.directory + "`."}});
        continue;
      }
      if (!from.library)
        listed_.insert(file);
      // A file already on the path being walked is a loop.
      for (const std::string &walking : path)
        if (walking == file) {
          std::string around;
          for (const std::string &step : path)
            around += (around.empty() ? "" : " uses ") + step;
          complain(from.manifest,
                   Diagnostic{at, "E0604",
                              "`" + use + "` is already being used by something it uses.",
                              "here", {"a unit does not use itself, however far round"},
                              {"the loop is: " + around + " uses " + file +
                               ". Two things that need each other are one thing, or "
                               "there is a third thing inside them that both use."}});
          return;
        }
      if (seen_.count(file))
        continue; // read already, by another route, and it goes in once
      Unit library;
      if (!readLibrary(file, library))
        continue;
      // Two libraries may not answer to one import name: `import 'text';`
      // has to mean one thing.
      for (const Unit &other : result_.libraries)
        if (other.name == library.name) {
          complain(library.manifest,
                   Diagnostic{library.nameSpan, "E0605",
                              "two libraries are both called `" + library.name + "`.",
                              "here", {"an import name means one library"},
                              {"the other is at `" + other.files.front() + "`."}});
          return;
        }
      path.push_back(file);
      follow(library, path);
      path.pop_back();
      // After what it uses, so the list reads in an order where nothing comes
      // before what it depends on.
      seen_[file] = static_cast<unsigned>(result_.libraries.size());
      result_.libraries.push_back(std::move(library));
    }
  }

  // The program's manifest lists every library the program reaches. One
  // reached only through another library's `uses` is written in, and said —
  // the library carried the path, so there is nothing the reader would have
  // had to work out, and a manifest that lists everything is a manifest a
  // reader can trust.
  void writeWhatWasReached(const std::string &manifestPath, const Unit &self) {
    std::vector<std::string> missing;
    for (const Unit &library : result_.libraries)
      if (!listed_.count(library.files.front()))
        missing.push_back(relativeTo(self.directory, library.files.front()));
    if (missing.empty())
      return;
    std::string text(self.manifest->text());
    Manifest m = readManifest(manifestPath);
    std::string added;
    for (const std::string &one : missing)
      added += (added.empty() ? "" : ", ") + std::string("\"") + one + "\"";
    if (const Said *uses = find(m, "uses", "paths")) {
      // Into the list as written: before its closing bracket.
      const std::size_t close = text.rfind(']', uses->valueSpan.end);
      if (close != std::string::npos && close >= uses->valueSpan.begin)
        text.insert(close, (uses->values.empty() ? "" : ", ") + added);
    } else {
      if (!text.empty() && text.back() != '\n')
        text += '\n';
      text += "\n[uses]\npaths = [" + added + "]\n";
    }
    std::ofstream out(manifestPath);
    out << text;
    for (const std::string &one : missing)
      result_.notes.push_back("xagc: `" + one + "` was added to `" + manifestPath +
                              "` — a library the program uses uses it.");
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
  // `t.'LIMIT'` — a library's constant, which the parser hands on as the one
  // name `t.LIMIT`.
  if (e->kind == ExprKind::Name) {
    const std::size_t dot = e->text.find('.');
    if (dot != std::string::npos)
      r.note(e->text.substr(0, dot), e->span);
  }
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

void applySettings(Program &file, const Unit &unit, unsigned which) {
  for (Item &item : file.items) {
    item.settings = unit.settings;
    item.unit = unit.called;
    item.file = which;
  }
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
      // A program's own files: nothing is exported, because nothing imports a
      // program, so `export` and `program` both mean the whole program — and
      // the name stays as written. A `file`-visible name is still walled off
      // from the other files, with `$` and no call name in front.
      const bool program = unit.called.empty();
      switch (visibilityOf(item.chain)) {
      case Sees::Export:
        if (!program)
          (*shareMap)[item.name] = unit.called + "." + item.name;
        break;
      case Sees::Program:
        if (!program)
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
