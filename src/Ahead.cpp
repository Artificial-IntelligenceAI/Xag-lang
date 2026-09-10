#include "xag/Ahead.h"

#include "xag/Interpret.h"
#include "xag/Loops.h"
#include "xag_runtime.h"

#include <cstdio>
#include <iostream>

namespace xag {
namespace {

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

// Whether running this program could find anything at all.
//
// Whether running this program could find anything at all.
//
// There are two things to find: a sum that comes round, and a stop. A sum needs
// `+`, `-` or `x` on whole numbers; a stop needs a divide, a remainder, a power,
// or reaching into a `many`. A program with none of those has nothing to learn
// about, and building and starting it costs half a second to find that out.
bool worthRunning(const Mir &mir) {
  for (const Body &body : mir.bodies)
    for (const BasicBlock &block : body.blocks) {
      for (const Statement &s : block.statements) {
        if (s.kind == StatementKind::Store)
          return true;
        // Which of the two streams a print goes to is a thing an engine can get
        // wrong, and getting it wrong is silent to anyone comparing output
        // alone — the two answers are the same text on different streams. So a
        // program that writes to standard error is worth running, where one
        // that only writes to standard output still is not.
        if (s.value.kind == RValueKind::Call && s.value.callee == "print.stderr")
          return true;
        if (s.value.kind == RValueKind::Element || s.value.kind == RValueKind::Fill)
          return true;
        if (s.value.kind != RValueKind::Binary)
          continue;
        const std::string &op = s.value.op;
        if (op == "/" || op == "mod" || op == "^")
          return true;
        if ((op == "+" || op == "-" || op == "x") &&
            isWhole(body.typed[s.value.type.index].held))
          return true;
      }
    }
  return false;
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

// A bound that was right, said as something certain rather than as an estimate.
// Only where both engines watched it happen.
Diagnostic confirmed(Diagnostic bound) {
  const std::string may = "` may reach past";
  const size_t at = bound.message.find(may);
  if (at != std::string::npos)
    bound.message.replace(at, may.size(), "` reaches past");
  bound.severity = Severity::Error;
  bound.tips = {"the loop's ends said it could get this far, and running it says it "
                "does — both ways I have of running it watched this come round. "
                "`wrapping` says coming round is meant."};
  return bound;
}

// The compiler contradicting itself. No code, because a code names a rule the
// reader's code broke and no rule was broken; what stands in its place is the
// two answers, which is the thing worth having in the report.
Diagnostic disagreed(const std::string &interpreted, const std::string &grumbled,
                     const Compiled &twice) {
  std::vector<std::string> both;
  if (!twice.ran) {
    both.push_back("Reading it, I ran it to the end. Built and started, it did not: " +
                   (twice.trouble.empty() ? std::string("it stopped.") : twice.trouble));
  } else {
    // Whichever stream they parted on. Saying "standard output" when what
    // differed was standard error would send a reader looking at the half that
    // matched.
    const bool onOutput = twice.said != interpreted;
    const std::string &mine = onOutput ? interpreted : grumbled;
    const std::string &theirs = onOutput ? twice.said : twice.complained;
    const size_t at = upToTheDifference(mine, theirs).size();
    both.push_back(std::string("They agreed for ") + std::to_string(at) +
                   " character(s) of standard " + (onOutput ? "output" : "error") +
                   ", and then did not.");
    both.push_back("  reading it:  " + firstLineFrom(mine, at));
    both.push_back("  running it:  " + firstLineFrom(theirs, at));
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
  std::FILE *grumbles = std::tmpfile();
  if (!sink || !grumbles) {
    if (sink)
      std::fclose(sink);
    if (grumbles)
      std::fclose(grumbles);
    return out;
  }
  xag_set_output(sink);
  xag_set_error(grumbles);
  const InterpretResult reading = interpretWatching(mir);
  xag_set_output(nullptr);
  xag_set_error(nullptr);
  const std::string said = drain(sink);
  const std::string complained = drain(grumbles);
  std::fclose(sink);
  std::fclose(grumbles);
  if (!reading.ran)
    return out;

  // Without a second engine there is nothing to agree with, and one engine may
  // not be believed about anything it would refuse a program for.
  if (!building)
    return out;
  const Compiled twice = building(mir);
  if (!twice.asked || !twice.ran || twice.said != said ||
      twice.complained != complained ||
      !samePlaces(reading.cameRound, twice.cameRound))
    return out;

  out.agreed = true;
  out.cameRound = reading.cameRound;
  return out;
}

// Whether some loop, run on its own, showed that this bound was worrying about
// nothing. The bound points at a statement; the loop holding that statement is
// the one that answers for it.
// What a loop taken out on its own can say: what happens when it runs, and
// nothing about whether it does. It says so.
Diagnostic whenItRuns(Diagnostic bound) {
  const std::string may = "` may reach past";
  const size_t at = bound.message.find(may);
  if (at != std::string::npos)
    bound.message.replace(at, may.size(), "` reaches past");
  bound.severity = Severity::Error;
  bound.tips = {"I took this loop out of the program and ran it on its own, both ways "
                "I have of running one, and both watched the sum come round — so it "
                "does, every time this loop runs. Whether it runs at all is a "
                "question about the program around it, which I did not run. "
                "`wrapping` says coming round is meant."};
  return bound;
}

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
    bool stood = false;
    for (unsigned i = 0; i < loops.size() && !cleared; ++i) {
      if (!holdsTheStatement(loops[i], bound.span))
        continue;
      if (!asked[i]) {
        answered[i] = runBothWays(loops[i].mir, building);
        asked[i] = true;
      }
      if (!answered[i].agreed)
        continue;
      if (inside(bound.span, answered[i].cameRound))
        stood = true;
      else
        cleared = true;
    }
    if (cleared) {
      any = true;
      continue;
    }
    // Both engines took the loop out, ran it, and watched the sum come round —
    // so it does, whenever the loop runs. Whether the loop runs at all is a
    // question about the program around it, which was not run, and the wording
    // says exactly that much and no more.
    left.push_back(stood ? whenItRuns(bound) : bound);
  }
  return left;
}

// Enough rounds to be worth mentioning. Below this a run is over before anybody
// wonders whether it is; above it, silence looks like being stuck.
constexpr long long kWorthMentioning = 1000000;

AheadResult ahead(const Source &source, const Mir &mir,
                  const std::vector<Diagnostic> &aboutSums,
                  const std::vector<Span> &intoPlainNames, const Building &building,
                  HowLong howLong) {
  AheadResult out;
  // Something to settle, or a second engine to settle it with. With neither,
  // running the program would answer a question nobody asked.
  if ((aboutSums.empty() && !building) || !hasStart(mir) ||
      (aboutSums.empty() && !worthRunning(mir))) {
    out.diagnostics = aboutSums;
    return out;
  }

  // Said before it starts rather than after, because after is no use to
  // somebody watching a build and wondering whether it has stopped. A run may
  // now take as long as the program does, so it can be a long quiet.
  if (howLong.rounds >= kWorthMentioning) {
    const Source::Position at = source.positionOf(howLong.where.begin);
    std::cerr << "xagc: about to run this program to find out what it does. A loop "
                 "at line "
              << at.line << " goes round " << howLong.rounds
              << " times, so this may take a moment.\n"
                 "      `no-itmt` on that loop, inside `UNSAFE`, says not to bother.\n";
  }

  // What the program writes while it is being compiled is not what anybody
  // asked to see. It is kept all the same, because it is the answer the second
  // run is compared against.
  std::FILE *sink = std::tmpfile();
  if (!sink) {
    out.diagnostics = aboutSums;
    return out;
  }
  std::FILE *grumbles = std::tmpfile();
  xag_set_output(sink);
  xag_set_error(grumbles);
  const InterpretResult result = interpretWatching(mir);
  xag_set_output(nullptr);
  xag_set_error(nullptr);
  const std::string said = drain(sink);
  const std::string complained = grumbles ? drain(grumbles) : std::string();
  std::fclose(sink);
  if (grumbles)
    std::fclose(grumbles);

  // A program that stopped is a program that breaks, and saying so before it is
  // ever run is the whole point of running it. Nothing else is settled: the run
  // saw only as far as the stop.
  if (!result.ran && !result.wouldRead) {
    out.diagnostics = aboutSums;
    if (result.theirFault && building) {
      const Compiled twice = building(mir);
      if (twice.asked)
        out.diagnostics.push_back(stopping(result, twice, said));
    }
    return out;
  }

  // It reached a read and stopped there. Everything before that happened, and
  // nothing after it is known — what a program does on what it was given is not
  // what it does on nothing.
  const bool partly = result.wouldRead || result.wouldTakeTime;
  out.ran = true;

  // The second answer. Without one, only the interpreter has spoken, and one
  // engine may drop a bound but may not stand one up.
  const Compiled twice = building ? building(mir) : Compiled{};
  if (twice.asked) {
    const bool bothStoppedTheSameWay = twice.wouldRead == result.wouldRead &&
                                      twice.wouldTakeTime == result.wouldTakeTime &&
                                      twice.ran == !partly;
    if (!bothStoppedTheSameWay || twice.said != said ||
        twice.complained != complained) {
      out.diagnostics.push_back(disagreed(said, complained, twice));
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

  // A bound the run answered is dropped. A run that went all the way answered
  // every one of them, including bounds on statements it never reached — a
  // statement a program with nothing to read never reaches is one that never
  // runs. A run that stopped at a read answered only what it got to.
  std::vector<Diagnostic> standing;
  for (const Diagnostic &bound : aboutSums) {
    const bool answered = !partly || inside(bound.span, result.reached);
    const bool came = inside(bound.span, result.cameRound);
    if (answered && !came)
      continue;
    // A bound is an estimate and estimates do not refuse anybody's program. It
    // becomes a refusal here and nowhere else: both engines ran the loop and
    // both watched the sum come round, which is not *at most* any more.
    if (came && out.compared && bound.code == "E0534")
      standing.push_back(confirmed(bound));
    else
      standing.push_back(bound);
  }

  // Whatever is left is about a loop the run never got to, and a loop can be
  // taken out and run on its own even when the program around it cannot.
  if (!standing.empty())
    standing = whatTheLoopsLeave(mir, standing, building, out.ran);
  out.diagnostics.insert(out.diagnostics.end(), standing.begin(), standing.end());
  return out;
}

} // namespace xag
