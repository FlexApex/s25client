# AGENTS.md

Guidance for AI agents working on **s25client** (Return to the Roots).

## Coding conventions

- Never add a Co-Authored By line or otherwise mention the agent in a commit message
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

## AI testing (ai-eval / ai-battle / LLM AI)

Evaluate an AI's competitiveness with the **ai-eval** harness (`tools/ai-eval`). It plays many headless
1v1 games against a baseline (default AIJH) over a fixed set of symmetric maps and seeds in **both start
orientations**, then reports a win share with a 95% confidence interval and a **PASS/FAIL** verdict
(PASS = significantly stronger than the baseline). The bar is to stay competitive with **AIJH-Hard**.

```sh
# build the runner, then evaluate ApexAI vs AIJH over 8 seeds (48 games)
cmake --build build --target ai-battle -j
python3 tools/ai-eval/eval.py --challenger apex --baseline aijh --num-seeds 8
```

Maps, ruleset (inexhaustible mines + gold→granite), start positions, early-abort and scoring are all
defined by the harness — see `tools/ai-eval/README.md`. Run from the repo root; when testing a build
tree other than `build/`, pass `--bin <dir>/bin/ai-battle` and `--mapdir <dir>/share/s25rttr/RTTR/MAPS/NEW`
(and set `RTTR_RTTR_DIR="$PWD/data/RTTR"`).

### Underlying tools

- **`ai-battle`** — the headless game runner ai-eval drives. AI names: `aijh`, `apex`/`apexai`, `llm`,
  `dummy`. Also useful standalone for one-off games: replays (`--replay`), savegames (`--save`; pass a
  `.sav` as `-m` to continue), trajectory stats (`--stats`), and `RTTR_ANALYZE=1` economy dumps.
- **`replay-verify <game.rpl>`** — headless replay desync checker (exit 0 in-sync, 2 desync). See
  `extras/replay-verify/README.md`.
- **LLM AI** — `extras/ai-battle/llm_sidecar.py` is the runtime companion for the `llm` AI (spool dir via
  `RTTR_LLM_SPOOL`); `extras/ai-battle/run_llm_game.sh <map>` starts the sidecar + a game and cleans up.

Run the engine from a build tree with `RTTR_RTTR_DIR="$PWD/data/RTTR" RTTR_USERDATA_DIR="$HOME/.s25rttr"`.

## Submodules

`external/s25edit` (the map editor) is a **separate repository with its own older style** — e.g. PascalCase methods and members without the trailing `_`. There, **match the surrounding file's style** rather than the rules above. It ships no `.clang-format` of its own and uses this repo's config when built in-tree, so `make clangFormat` still applies.
