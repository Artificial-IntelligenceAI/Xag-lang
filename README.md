# Xag coding language

Official domains: `xag-lang.com`, and `xag-lang.org` **only**

A programming language built around high performance, helpful error messages,
and Rust-style memory management. A size is always written: there is no `int`
on its own, because there is no size to assume. Programs are compiled ahead of time,
either to native code or to a form run by an AOT interpreter.

**Early work in progress — nothing here is stable yet.**

The code written is mostly or 100% AI-made. Why? Because, I'm more of a designer, not a C++ stroke-inducing syntax reader 🤣. No offense.

"Why would anyone ever use Xag?" Honestly, I don't know. 😂

## Building

The compiler is written in C++20 against LLVM's native C++ API.

Requirements: **LLVM 23 or newer**, CMake, Ninja.

```sh
brew install llvm cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
./build/xagc
```

CMake finds Homebrew's keg-only LLVM on its own. Build with Apple's `clang++`
(the default) rather than Homebrew's — Homebrew's driver ships its own libc++
and collides with the macOS SDK headers once LLVM's include directory is added.

Source files are `.xag`. `xagc lex <file>` prints its tokens, `xagc parse <file>`
prints its tree, `xagc check <file>` checks it, `xagc mir <file>` prints the
mid-level IR, and `xagc llvm-smoke` proves the LLVM backend is reachable. Run the
tests with `ctest --test-dir build`.

`xagc run <file>` runs a program on the test interpreter — the engine built to be
obviously correct rather than fast, which walks the IR as written and does nothing
clever anywhere. It is the one to believe when the engines disagree.

`xagc fast <file>` runs it on the fast interpreter, which turns the graph into
flat code once and then runs it without looking anything up again. It shares
nothing with the test interpreter but the runtime, on purpose: two engines that
borrow from each other agree about what they borrowed, and a vote between them
proves nothing.

`xagc build <file>` compiles a program ahead of time through LLVM at `-O3` and
writes an executable beside it. `xagc ir <file>` prints the LLVM IR, and
`xagc ir <file> --raw` prints it before the optimiser sees it.

All three engines exist: the test interpreter, the fast interpreter, and AOT
native. Two engines can say that something is wrong; three can say which.

All three engines call one runtime, so none of them can disagree about what
joining or counting means. `Xag-Config.toml` holds what this project has decided
once for every file in it.

## The oracle

`generator/` writes Xag programs and asks every engine what they say — and when
they differ, which one is out of step with the other two.

```sh
cd generator && cargo build --release
./target/release/xag-oracle --xagc ../build/xagc --cases 100
```

It has no dependencies. The random numbers and the threading are a few lines
each, and a fuzzer is the last place to want a supply chain. Work is handed out
by an atomic counter rather than split in advance, because cases differ in cost
and a thread that finishes early should take the next one.

The thing that decides its speed is not compiling or running. macOS spends about
170ms scanning every freshly linked binary before it will run it once, which is
four times what everything else costs put together — and it is paid per *binary*,
not per statement. So `--size` matters more than `--cases`: bigger programs
amortise it. Past about 400 statements the reference interpreter, which is slow
on purpose, stops finishing inside the timeout and cases start being skipped
rather than tested.

## Unicode

`count` counts grapheme clusters, which is what a person counting characters
means. The rules are UAX #29 in full — Hangul syllables, Indic vowel signs and
conjuncts, prepends, zero-width-joiner sequences and flags — against tables
generated from the Unicode Character Database, version 17.0.0.

All 766 of Unicode's own conformance cases run as part of the test suite, so the
claim is checkable rather than asserted. Regenerate `runtime/xag_unicode.h` and
`tests/unicode_cases.h` from the UCD when moving to a new Unicode version.

## Reporting a problem

Wrong diagnostics, confusing ones, and anything Xag accepts that it should not:
<https://github.com/Artificial-IntelligenceAI/Xag-lang/issues>

A diagnostic that points at the wrong thing, or names a rule the program did not
break, is a bug of the same kind as miscompiling — the compiler is telling the
reader something untrue either way.

## License

Copyright 2026 Tankun Sriket

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or
  <https://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or <https://opensource.org/licenses/MIT>)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted for
inclusion in this project by you, as defined in the Apache-2.0 license, shall be
dual licensed as above, without any additional terms or conditions.
