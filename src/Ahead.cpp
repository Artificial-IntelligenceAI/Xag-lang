#include "xag/Ahead.h"

#include "xag/Interpret.h"
#include "xag/Loops.h"
#include "xag_runtime.h"

#include <cstdio>

namespace xag {
namespace {

// A program that reads has an answer that depends on what it is given, so there
// is nothing to find out here. Asked of every body, because a function called
// from the loop reads just as the loop does.
bool readsInput(const Mir &mir) {
  for (const Body &body : mir.bodies)
    for (const BasicBlock &block : body.blocks)
      for (const Statement &s : block.statements)
        if (s.value.kind == RValueKind::Call && s.value.callee == "read.stdin")
          return true;
  return false;
}

// Everything a temporary file was written with, read back.
std::string drain(std::FILE *file) {
  std::fflush(file);
  std::rewind(file);
  std::string out;
  char buffer[4096];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    out.append(buffer, got);
  return out;
}

// Everything up to the first place two pieces of text stop being the same, so
// the report can say where they parted rather than printing both whole.
std::string upToTheDifference(const std::string &a, const std::string &b) {
  size_t at = 0;
  while (at < a.size() && at < b.size() && a[at] == b[at])
    ++at;
  return a.substr(0, at);
}

std::string firstLineFrom(const std::string &text, size_t at) {
  const size_t stop = text.find('\n', at);
  return text.substr(at, stop == std::string::npos ? std::string::npos : stop - at);
}

// Whether any of these places sits within this one. A bound points at the whole
// `set`, and the sum happens in a temporary inside it — `_6 = _1 + _5` — whose
// span is the part of the line that adds rather than the line. So the two are
// never equal, and containment is the question worth asking. Which way round it
// is asked matters: asked backwards, every real overflow went unreported and
// the tests said so.
bool inside(Span place, const std::vector<Span> &ones) {
  for (const Span &one : ones)
    if (one.begin >= place.begin && one.begin < place.end)
      return true;
  return false;
}

// Whether this place sits within any of those.
bool inside2(Span one, const std::vector<Span> &places) {
  for (const Span &place : places)
    if (one.begin >= place.begin && one.begin <= place.end)
      return true;
  return false;
}

std::vector<Span> spansOf(const std::vector<Diagnostic> &diagnostics) {
  std::vector<Span> out;
  out.reserve(diagnostics.size());
  for (const Diagnostic &one : diagnostics)
    out.push_back(one.span);
  return out;
}

bool holds(const std::vector<Span> &places, Span one) {
  for (const Span &place : places)
    if (place.begin == one.begin)
      return true;
  return false;
}

// The same places, whatever order they were reached in. A built program may
// arrive at them differently and still agree about which they are.
bool samePlaces(const std::vector<Span> &a, const std::vector<Span> &b) {
  for (const Span &one : a)
    if (!holds(b, one))
      return false;
  for (const Span &one : b)
    if (!holds(a, one))
      return false;
  return true;
}

bool hasStart(const Mir &mir) {
  for (const Body &body : mir.bodies)
    if (body.name == "START")
      return true;
  return false;
}

// What to say about a program that stopped.
//
// Both engines ran it, so both have an opinion, and only what they agree about
// is worth putting to the reader. Agreeing here means stopping for the same
// reason in the same place after writing the same thing.
Diagnostic stopping(const InterpretResult &reading, const Compiled &running,
                    const std::string &said) {
  const bool same = running.stopped && running.why == reading.trouble &&
                    running.said == said &&
                    running.stoppedAt.begin == reading.stoppedAt.begin;
  if (!same) {
    std::vector<std::string> both;
    both.push_back("Reading it, it stopped: " + reading.trouble);
    both.push_back(running.stopped ? "Built and started, it stopped: " + running.why
                   : running.ran  ? std::string("Built and started, it did not stop.")
                                  : "Built and started: " + running.trouble);
    return Diagnostic{Span{}, "", "the two ways I have of running this do not agree.",
                      "here", both, {}, {}, Severity::Mine};
  }
  return Diagnostic{
      reading.stoppedAt, "E0538", "this stops the program: " + reading.trouble, "here",
      {"a program that cannot go on is not one worth building"},
      {"nothing worked this out. I ran the program both ways I have of running it, "
       "and both stopped here for this reason."}};
}

// The compiler contradicting itself. No code, because a code names a rule the
// reader's code broke and no rule was broken; what stands in its place is the
// two answers, which is the thing worth having in the report.
Diagnostic disagreed(const std::string &interpreted, const Compiled &twice) {
  std::vector<std::string> both;
  if (!twice.ran) {
    both.push_back("Reading it, I ran it to the end. Built and started, it did not: " +
                   (twice.trouble.empty() ? std::string("it stopped.") : twice.trouble));
  } else {
    const size_t at = upToTheDifference(interpreted, twice.said).size();
    both.push_back("They agreed for " + std::to_string(at) +
                   " character(s), and then did not.");
    both.push_back("  reading it:  " + firstLineFrom(interpreted, at));
    both.push_back("  running it:  " + firstLineFrom(twice.said, at));
  }
  return Diagnostic{Span{}, "", "the two ways I have of running this do not agree.",
                    "here", both, {}, {}, Severity::Mine};
}

// They wrote the same thing and did not agree about where a sum came round.
// Only a watching interpreter and a checked build can see this at all, which is
// exactly why it is worth seeing: comparing output alone, it is silence.
Diagnostic disagreedAboutSums(const std::vector<Span> &reading,
                              const std::vector<Span> &running) {
  return Diagnostic{
      Span{}, "", "the two ways I have of running this do not agree.", "here",
      {"They wrote the same thing, and did not agree about where a sum came round.",
       "  reading it:  " + std::to_string(reading.size()) + " place(s)",
       "  running it:  " + std::to_string(running.size()) + " place(s)"},
      {}, {}, Severity::Mine};
}

} // namespace

// One run of one program, both ways, and whether the two agreed. `cameRound` is
// where they agreed a sum came round; it means nothing unless `agreed`.
struct Both {
  bool agreed = false;
  std::vector<Span> cameRound;
};

Both runBothWays(const Mir &mir, const Building &building) {
  Both out;
  std::FILE *sink = std::tmpfile();
  if (!sink)
    return out;
  xag_set_output(sink);
  const InterpretResult reading = interpretWatching(mir);
  xag_set_output(nullptr);
  const std::string said = drain(sink);
  std::fclose(sink);
  if (!reading.ran)
    return out;

  // Without a second engine there is nothing to agree with, and one engine may
  // not be believed about anything it would refuse a program for.
  if (!building)
    return out;
  const Compiled twice = building(mir);
  if (!twice.asked || !twice.ran || twice.said != said ||
      !samePlaces(reading.cameRound, twice.cameRound))
    return out;

  out.agreed = true;
  out.cameRound = reading.cameRound;
  return out;
}

// Whether some loop, run on its own, showed that this bound was worrying about
// nothing. The bound points at a statement; the loop holding that statement is
// the one that answers for it.
// Whether this loop holds the statement a bound is about.
bool holdsTheStatement(const Lifted &loop, Span bound) {
  for (const Span &place : loop.places)
    if (place.begin >= bound.begin && place.begin < bound.end)
      return true;
  return false;
}

// Which bounds the loops of a program clear, all of them at once.
//
// Once per loop, not once per bound. Asked a bound at a time it lifted every
// loop again and built and started each one again, so a program with two bounds
// in two loops paid for four builds to answer two questions.
std::vector<Diagnostic> whatTheLoopsLeave(const Mir &mir,
                                          const std::vector<Diagnostic> &aboutSums,
                                          const Building &building, bool &any) {
  const std::vector<Lifted> loops = loopsThatStandAlone(mir);
  std::vector<Both> answered(loops.size());
  std::vector<bool> asked(loops.size(), false);

  std::vector<Diagnostic> left;
  for (const Diagnostic &bound : aboutSums) {
    bool cleared = false;
    for (unsigned i = 0; i < loops.size() && !cleared; ++i) {
      if (!holdsTheStatement(loops[i], bound.span))
        continue;
      if (!asked[i]) {
        answered[i] = runBothWays(loops[i].mir, building);
        asked[i] = true;
      }
      cleared = answered[i].agreed && !inside(bound.span, answered[i].cameRound);
    }
    if (cleared)
      any = true;
    else
      left.push_back(bound);
  }
  return left;
}

AheadResult ahead(const Source &, const Mir &mir,
                  const std::vector<Diagnostic> &aboutSums,
                  const std::vector<Span> &intoPlainNames, const Building &building) {
  AheadResult out;
  // Something to settle, or a second engine to settle it with. With neither,
  // running the program would answer a question nobody asked.
  if ((aboutSums.empty() && !building) || !hasStart(mir)) {
    out.diagnostics = aboutSums;
    return out;
  }

  // A program that reads cannot be run here, and the loops inside it still can.
  // What a loop is entered with is written down even where what the program is
  // given is not, so each one is taken out and run as a program of its own.
  //
  // Only to *drop* a bound. A lifted loop says what happens when the loop is
  // entered, and whether it is ever entered is a question about the program
  // around it — so it may clear a suspicion and may not raise one.
  if (readsInput(mir)) {
    out.diagnostics = whatTheLoopsLeave(mir, aboutSums, building, out.ran);
    return out;
  }

  // What the program writes while it is being compiled is not what anybody
  // asked to see. It is kept all the same, because it is the answer the second
  // run is compared against.
  std::FILE *sink = std::tmpfile();
  if (!sink) {
    out.diagnostics = aboutSums;
    return out;
  }
  xag_set_output(sink);
  const InterpretResult result = interpretWatching(mir);
  xag_set_output(nullptr);
  const std::string said = drain(sink);
  std::fclose(sink);

  // A run that stopped saw only part of the program, so every bound stands. But
  // *why* it stopped is worth having: a program that asks for a place a `many`
  // does not have, or divides by zero, is a program that breaks — and knowing
  // that before it is run is the whole point of running it.
  if (!result.ran) {
    out.diagnostics = aboutSums;
    if (result.theirFault && building) {
      const Compiled twice = building(mir);
      if (twice.asked)
        out.diagnostics.push_back(stopping(result, twice, said));
    }
    return out;
  }

  out.ran = true;

  // The second answer. Without one, only the interpreter has spoken, and one
  // engine may drop a bound but may not stand one up.
  const Compiled twice = building ? building(mir) : Compiled{};
  if (twice.asked) {
    if (!twice.ran || twice.said != said) {
      out.diagnostics.push_back(disagreed(said, twice));
      return out;
    }
    // Both wrote the same thing, and both were asked where a sum came round.
    // The second question is the one that matters: two engines writing nothing
    // is not two engines agreeing, and a sum coming round in a value nothing
    // prints leaves them both silent.
    if (!samePlaces(result.cameRound, twice.cameRound)) {
      out.diagnostics.push_back(disagreedAboutSums(result.cameRound, twice.cameRound));
      return out;
    }
    out.compared = true;
  }

  // What the run found that nothing suspected. Only once both engines have
  // agreed about it: this refuses a program, and one engine may not do that.
  if (out.compared) {
    for (const Span &came : result.cameRound) {
      // Only where the answer becomes a name that did not say `wrapping`. A
      // sum inside a comparison, or handed straight to something, has nowhere
      // for the reader to have written the word — so it is not their fault and
      // is not put to them.
      if (!inside2(came, intoPlainNames) || inside2(came, spansOf(aboutSums)))
        continue;
      out.diagnostics.push_back(Diagnostic{
          came, "E0537", "a sum comes round here.", "here",
          {"a sum that does not fit comes round, and that is rarely what was wanted"},
          {"nothing worked this out. I ran the program both ways I have of running "
           "it, watching, and both saw this one come round. `wrapping` on the "
           "declaration says it is meant to."}});
    }
  }

  for (const Diagnostic &bound : aboutSums) {
    const bool came = inside(bound.span, result.cameRound);
    // It came round after all, so the bound was right about this one and is
    // reported as it stands. Everywhere else the loop ran and nothing came
    // round, which is an answer rather than an estimate.
    if (came)
      out.diagnostics.push_back(bound);
  }
  return out;
}

} // namespace xag
