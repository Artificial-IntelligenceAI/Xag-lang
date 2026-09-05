# Xag coding language

A programming language built around high performance, helpful error messages,
and Rust-style memory management. Programs are compiled ahead of time,
either to native code or to a form run by an AOT interpreter.

Early work in progress — nothing here is stable yet.

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

Source files are `.xag`. `xagc lex <file>` reads one and prints its tokens; `xagc llvm-smoke`
proves the LLVM backend is reachable. Run the tests with `ctest --test-dir build`.
Only the lexer exists so far — there is no parser, no checker, and no code
generation yet.

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
