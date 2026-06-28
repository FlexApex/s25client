# ai-battle-runs — generated run data

Everything the headless **ai-battle** runner and the **ai-eval** harness produce is written here, so
all run artifacts live in one predictable, inspectable place. **The contents are git-ignored** (only this
README and the `.gitignore` are tracked) — delete anything in here freely; it is all reproducible.

```
ai-battle-runs/
├── eval/     # one folder per ai-eval run  (tools/ai-eval/eval.py writes here by default)
│   └── <timestamp>_<challenger>_vs_<baseline>[_<label>]/
│       ├── summary.json                  # machine-readable totals: per-map W/L/D, win-share, 95% CI, verdict
│       ├── <map>_seed<N>_o<0|1>.log      # full ai-battle console output for each game (incl. the RESULT block)
│       └── <map>_seed<N>_o<0|1>.csv      # per-player trajectory (only with eval.py --stats)
│
└── games/    # one folder per single ad-hoc game  (tools/ai-eval/run-game.sh writes here)
    └── <timestamp>_<map>/
        ├── game.log     # console output (incl. the final RESULT block)
        ├── stats.csv    # per-player trajectory over time
        ├── replay.rpl   # replay (re-watchable / re-verifiable)
        └── save.sav     # final savegame
```

## How data gets here

- **Eval suite:** `tools/ai-eval/eval.py [...]` → `eval/<run-id>/` (override with `--out-dir`).
- **Single game:** `tools/ai-eval/run-game.sh --map <m> --ai <a> --ai <b> [ai-battle args...]` → `games/<run-id>/`.
- **Raw ai-battle:** running `build/bin/ai-battle` directly only writes files for the paths you pass
  (`--stats FILE`, `--replay FILE`, `--save FILE`); point them in here, e.g.
  `--stats ai-battle-runs/games/manual/stats.csv`, if you want them collected with the rest.

Note: ai-battle does **not** create files outside the paths you give it. The RTTR engine's own logs (if
any) go to `<RTTR_USERDATA>/LOGS` (`~/.s25rttr/LOGS` on Linux), which is outside this repo.

## Inspecting

- `summary.json` is the quickest read for an eval run's outcome.
- The `*.log` files contain each game's `RESULT` / `RESULT_PLAYER` block and the live progress table.
- The `*.csv` files (one row per player per `statsInterval` frames) are easy to plot or diff to see how a
  game developed (land, military, buildings, soldiers, the production chain, …).
