#include "xag/Ahead.h"

#include "xag/Interpret.h"
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

bool hasStart(const Mir &mir) {
  for (const Body &body : mir.bodies)
    if (body.name == "START")
      return true;
  return false;
}

} // namespace

AheadResult ahead(const Source &, const Mir &mir,
                  const std::vector<Diagnostic> &aboutSums) {
  AheadResult out;
  if (aboutSums.empty() || !hasStart(mir) || readsInput(mir)) {
    out.diagnostics = aboutSums;
    return out;
  }

  // What the program writes while it is being compiled is not what anybody
  // asked to see. It goes somewhere and is thrown away.
  std::FILE *sink = std::tmpfile();
  if (!sink) {
    out.diagnostics = aboutSums;
    return out;
  }
  xag_set_output(sink);
  const InterpretResult result = interpretWatching(mir);
  xag_set_output(nullptr);
  std::fclose(sink);

  // A run that stopped — out of steps, or on something the program does wrong —
  // saw only part of the program. What it did not reach, it cannot vouch for.
  if (!result.ran) {
    out.diagnostics = aboutSums;
    return out;
  }

  out.ran = true;
  for (const Diagnostic &bound : aboutSums) {
    // The bound points at the whole `set`, and the sum happens in a temporary
    // inside it — `_6 = _1 + _5` — whose span is the part of the line that adds
    // rather than the line. So the question is whether the place it came round
    // is inside the place the bound is about, not whether they are the same.
    bool came = false;
    for (const Span &span : result.cameRound)
      if (span.begin >= bound.span.begin && span.begin < bound.span.end)
        came = true;
    // It came round after all, so the bound was right about this one and is
    // reported as it stands. Everywhere else the loop ran and nothing came
    // round, which is an answer rather than an estimate.
    if (came)
      out.diagnostics.push_back(bound);
  }
  return out;
}

} // namespace xag
