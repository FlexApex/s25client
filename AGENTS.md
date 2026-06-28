# AGENTS.md

Guidance for AI agents working on **s25client** (Return to the Roots).

## Coding conventions

Full reference: <https://github.com/Return-To-The-Roots/s25client/wiki/Coding-conventions>.
The essentials to follow:

- **C++17, and code must compile warning-free** (builds use `-Werror`).
- **Run clang-format (v10) before finishing** — `make clangFormat`; CI rejects unformatted code. Static analysis: `tools/runClangTidy.sh`.
- **Naming:** types `UpperCamelCase`; functions/params/locals `lowerCamelCase`; **member variables end with `_`** (`foo_`). Setters `setX`, getters `isX`/`canX`. No variable shadowing.
- **No magic numbers** — use named constants/enums.
- **RAII:** prefer `std::unique_ptr` (then intrusive_ptr, then `shared_ptr`); `std::array`/`std::vector` over C-arrays; never raw `new`/`delete`.
- **Includes:** include-what-you-use, prefer forward declarations, one class per file.
- **Paths** live in `bfs::path` (boost::filesystem); strings are UTF-8 (convert only at I/O — see libutil `ucString.h`/`utf8.h`). "Filepath" = full path, "Filename" = name only.
- **Docs:** a one-line `///` (JavaDoc) summary in the header for non-trivial declarations.
- **Tests:** add/extend tests for each fix or feature; keep them small and descriptive; account for RNG variability.
- **Avoid** unnecessary `else` after `return`, and duplicated code (factor it out).

## AI testing (ai-battle / LLM AI)

When evaluating an AI's competitiveness, test on these maps (all under the same ruleset below):

- **FL-Macro2.SWD**
- **Macro144.SWD**
- **AtomicFL.SWD**

Across **multiple seeds and both map orientations**, under: **2v2, Hard, inexhaustible mines,
gold→granite** (no gold deposits, so military strength = soldier count). The bar is to stay
competitive with **AIJH-Hard** with **no plateau** over long (~4h, ~288k GF) games.

## Submodules

`external/s25edit` (the map editor) is a **separate repository with its own older style** — e.g. PascalCase methods and members without the trailing `_`. There, **match the surrounding file's style** rather than the rules above. It ships no `.clang-format` of its own and uses this repo's config when built in-tree, so `make clangFormat` still applies.
