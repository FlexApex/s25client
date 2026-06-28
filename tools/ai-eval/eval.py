#!/usr/bin/env python3
# Copyright (C) 2005 - 2026 Settlers Freaks <sf-team at siedler25.org>
# SPDX-License-Identifier: GPL-2.0-or-later
"""
ai-eval: evaluate one AI against another over a suite of maps with the ai-battle tool.

For every (map, seed, orientation) it runs ai-battle, parses the machine-readable RESULT block, and
reports a per-map and overall win/loss/draw record plus a Wilson 95% confidence interval on the
challenger's win share. Each (map, seed) is played from BOTH orientations (challenger seated at the
first vs. the second of the map's two fixed start positions) to cancel out any start-position/seating
advantage: two equally-strong AIs then score an exact 50%, so a win share whose CI lower bound is
above 50% is real evidence the challenger is stronger.

Examples (run from the repo root):
    tools/ai-eval/eval.py                              # sanity: aijh vs aijh -> ~50%
    tools/ai-eval/eval.py --challenger myai            # evaluate "myai" vs aijh
    tools/ai-eval/eval.py --challenger a --baseline b --num-seeds 8

The challenger/baseline names are whatever ai-battle's --ai accepts (e.g. "aijh", "dummy"); register
your own AI there to evaluate it. Exit code is 0 when the challenger passes (CI lower bound > 50%).
"""
import argparse
import concurrent.futures
import dataclasses
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = REPO_ROOT / "build" / "bin" / "ai-battle"
DEFAULT_MAPDIR = REPO_ROOT / "build" / "share" / "s25rttr" / "RTTR" / "MAPS" / "NEW"

# Symmetric eval maps. Only the first two start positions are used for a 1v1; both orientations seat the
# AIs at exactly these slots, so the SAME start positions are always used (important on maps with more
# slots than players). Override per run with --maps / --positions.
DEFAULT_MAPS = ["TueranTuer.SWD", "dreamland.swd", "Landstr.swd"]
DEFAULT_POSITIONS = [0, 1]

RESULT_RE = re.compile(r"^RESULT reason=(\S+) gf=(\d+) finished=(\d+) numPlayers=(\d+) winner=(-?\d+)")
PLAYER_RE = re.compile(
    r"^RESULT_PLAYER idx=(\d+) type=(\d+) typeName=(\S+) defeated=(\d+) country=(\d+) "
    r"military=(\d+) buildings=(\d+) inhabitants=(\d+) productivity=(\d+) soldiers=(\d+) milblds=(\d+)"
)


@dataclasses.dataclass
class PlayerResult:
    idx: int
    type_name: str
    defeated: bool
    country: int
    military: int
    buildings: int
    inhabitants: int
    soldiers: int


@dataclasses.dataclass
class GameResult:
    map_name: str
    seed: int
    orientation: int  # 0: challenger at the first position; 1: challenger at the second position
    challenger_slot: int
    baseline_slot: int
    reason: str
    gf: int
    finished: bool
    players: dict  # idx -> PlayerResult
    wall_secs: float
    ok: bool = True
    error: str = ""

    @property
    def challenger(self):
        return self.players.get(self.challenger_slot)

    @property
    def baseline(self):
        return self.players.get(self.baseline_slot)

    def verdict(self):
        """Return 'win' | 'loss' | 'draw' for the challenger, with a small draw band on land."""
        if not self.ok or self.challenger is None or self.baseline is None:
            return "error"
        c, b = self.challenger, self.baseline
        if c.defeated and not b.defeated:
            return "loss"
        if b.defeated and not c.defeated:
            return "win"
        hi = max(c.country, b.country, 1)
        if abs(c.country - b.country) / hi < 0.05:
            return "draw"
        return "win" if c.country > b.country else "loss"

    def land_margin(self):
        """Bounded relative land lead in [-1, 1]: +1 = baseline wiped out, -1 = challenger wiped out."""
        if self.challenger is None or self.baseline is None:
            return 0.0
        c, b = self.challenger.country, self.baseline.country
        return (c - b) / max(c + b, 1)


def wilson_interval(wins, n, z=1.96):
    """95% Wilson score interval for a binomial proportion (robust for small n / extreme p)."""
    if n == 0:
        return float("nan"), float("nan")
    p = wins / n
    denom = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    half = (z * ((p * (1 - p) / n + z * z / (4 * n * n)) ** 0.5)) / denom
    return centre - half, centre + half


def parse_output(text):
    players = {}
    reason = gf = finished = None
    for line in text.splitlines():
        m = RESULT_RE.match(line)
        if m:
            reason, gf, finished = m.group(1), int(m.group(2)), bool(int(m.group(3)))
            continue
        m = PLAYER_RE.match(line)
        if m:
            idx = int(m.group(1))
            players[idx] = PlayerResult(
                idx=idx,
                type_name=m.group(3),
                defeated=bool(int(m.group(4))),
                country=int(m.group(5)),
                military=int(m.group(6)),
                buildings=int(m.group(7)),
                inhabitants=int(m.group(8)),
                soldiers=int(m.group(10)),
            )
    if reason is None or not players:
        return None
    return reason, gf, finished, players


def run_game(args, map_name, positions, seed, orientation, out_dir):
    """orientation 0: [challenger, baseline] -> slots positions[0], positions[1]; orientation 1: swapped."""
    if orientation == 0:
        ais = [args.challenger, args.baseline]
        challenger_slot, baseline_slot = positions[0], positions[1]
    else:
        ais = [args.baseline, args.challenger]
        challenger_slot, baseline_slot = positions[1], positions[0]

    map_path = Path(args.mapdir) / map_name
    cmd = [str(args.bin), "--map", str(map_path)]
    for ai in ais:
        cmd += ["--ai", ai]
    cmd += ["--positions", ",".join(str(p) for p in positions)]
    cmd += ["--maxGF", str(args.max_gf), "--random_init", str(seed), "--random_ai_init", str(seed)]
    if args.inexhaustible_mines:
        cmd += ["--inexhaustibleMines"]
    cmd += ["--goldDeposits", str(args.gold_deposits)]
    if args.dominance_factor > 0:
        cmd += ["--minDominanceGF", str(args.min_dominance_gf), "--dominanceFactor", str(args.dominance_factor)]
    tag = f"{Path(map_name).stem}_seed{seed}_o{orientation}"
    if args.stats:
        cmd += ["--stats", str(out_dir / f"{tag}.csv"), "--statsInterval", str(args.stats_interval)]

    t0 = time.time()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
        out = proc.stdout + "\n" + proc.stderr
    except subprocess.TimeoutExpired as e:
        (out_dir / f"{tag}.log").write_text((e.stdout or "") + "\n[TIMEOUT]")
        return GameResult(map_name, seed, orientation, challenger_slot, baseline_slot, "timeout", 0, False, {},
                          time.time() - t0, ok=False, error="timeout")
    wall = time.time() - t0
    (out_dir / f"{tag}.log").write_text(out)

    parsed = parse_output(out)
    if parsed is None:
        return GameResult(map_name, seed, orientation, challenger_slot, baseline_slot, "parse-error", 0, False, {},
                          wall, ok=False, error="no RESULT block (crash?). See log: " + tag + ".log")
    reason, gf, finished, players = parsed
    res = GameResult(map_name, seed, orientation, challenger_slot, baseline_slot, reason, gf, finished, players, wall)
    if res.challenger is None or res.baseline is None:
        res.ok = False
        res.error = "challenger/baseline slot missing from RESULT"
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--challenger", default="aijh", help="AI under test (ai-battle --ai name; default: aijh)")
    ap.add_argument("--baseline", default="aijh", help="opponent AI (default: aijh)")
    ap.add_argument("--maps", nargs="+", default=DEFAULT_MAPS, help="map filenames under --mapdir")
    ap.add_argument("--mapdir", default=str(DEFAULT_MAPDIR))
    ap.add_argument("--positions", default=",".join(str(p) for p in DEFAULT_POSITIONS),
                    help="the two fixed start slots used on every map, e.g. \"0,1\" (default). Both orientations "
                         "seat the AIs at exactly these slots.")
    ap.add_argument("--seeds", type=int, nargs="+", default=[101, 202, 303])
    ap.add_argument("--num-seeds", type=int, default=None,
                    help="if set, use this many deterministic seeds (overrides --seeds). Use >=8 for a trustworthy "
                         "verdict: per-(map,seed) outcomes swing 2-0/1-1/0-2, so few seeds are noisy.")
    ap.add_argument("--max-gf", type=int, default=400000)
    ap.add_argument("--dominance-factor", type=float, default=3.0,
                    help="abort a game early once one side's land is this x the other's (0 disables)")
    ap.add_argument("--min-dominance-gf", type=int, default=60000,
                    help="only allow the dominance abort after this many GF")
    ap.add_argument("--inexhaustible-mines", action="store_true", default=True)
    ap.add_argument("--no-inexhaustible-mines", dest="inexhaustible_mines", action="store_false")
    ap.add_argument("--gold-deposits", type=int, default=4, help="CHANGE_GOLD_DEPOSITS (4=gold->granite, default)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--timeout", type=int, default=3600, help="per-game wall-clock timeout (s)")
    ap.add_argument("--bin", default=str(DEFAULT_BIN))
    ap.add_argument("--out-dir", default=None, help="logs/summary dir (default: ai-battle-runs/eval/<run-id>)")
    ap.add_argument("--stats", action="store_true", help="also write per-game trajectory CSVs")
    ap.add_argument("--stats-interval", type=int, default=4000)
    ap.add_argument("--label", default="", help="optional label folded into the output dir name")
    args = ap.parse_args()
    if args.num_seeds is not None:
        args.seeds = [1000 * (i + 1) + 7 for i in range(args.num_seeds)]
    positions = [int(p) for p in args.positions.split(",") if p != ""]
    if len(positions) != 2:
        sys.exit(f"--positions must list exactly two slots (got {args.positions!r})")

    if not Path(args.bin).exists():
        sys.exit(f"ai-battle binary not found: {args.bin}\nBuild it: (cd build && make ai-battle -j)")

    ts = time.strftime("%Y%m%d-%H%M%S")
    name = f"{ts}_{args.challenger}_vs_{args.baseline}" + (f"_{args.label}" if args.label else "")
    out_dir = Path(args.out_dir) if args.out_dir else (REPO_ROOT / "ai-battle-runs" / "eval" / name)
    out_dir.mkdir(parents=True, exist_ok=True)

    jobs = [(m, s, o) for m in args.maps for s in args.seeds for o in (0, 1)]

    print(f"ai-eval: {args.challenger} vs {args.baseline}")
    print(f"  maps={args.maps} positions={positions} seeds={args.seeds} maxGF={args.max_gf} "
          f"dominance={args.dominance_factor}x@{args.min_dominance_gf}gf jobs={args.jobs}")
    print(f"  -> {len(jobs)} games, results in {out_dir}\n")

    results = []
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_game, args, m, positions, s, o, out_dir): (m, s, o) for (m, s, o) in jobs}
        for fut in concurrent.futures.as_completed(futs):
            r = fut.result()
            results.append(r)
            done += 1
            v = r.verdict()
            extra = "" if r.ok else f"  !! {r.error}"
            margin = f"land {r.land_margin() * 100:+.0f}%" if r.ok else ""
            print(f"[{done}/{len(jobs)}] {Path(r.map_name).stem:<18} seed{r.seed} o{r.orientation} "
                  f"-> {v:<5} ({r.reason}, gf={r.gf}, {margin}, {r.wall_secs:.0f}s){extra}")

    summarize(args, results, out_dir)


def summarize(args, results, out_dir):
    by_map = {}
    for r in results:
        by_map.setdefault(r.map_name, []).append(r)

    print("\n" + "=" * 72)
    print(f"SUMMARY: {args.challenger} vs {args.baseline}")
    print("=" * 72)
    tot = {"win": 0, "loss": 0, "draw": 0, "error": 0}
    print(f"{'map':<20} {'W':>3} {'L':>3} {'D':>3} {'E':>3}  {'winshare':>9}  {'avg land margin':>16}")
    for m in sorted(by_map):
        c = {"win": 0, "loss": 0, "draw": 0, "error": 0}
        margins = []
        for r in by_map[m]:
            c[r.verdict()] += 1
            if r.ok:
                margins.append(r.land_margin())
        for k in tot:
            tot[k] += c[k]
        decisive = c["win"] + c["loss"]
        share = c["win"] / decisive if decisive else float("nan")
        am = sum(margins) / len(margins) if margins else float("nan")
        print(f"{Path(m).stem:<20} {c['win']:>3} {c['loss']:>3} {c['draw']:>3} {c['error']:>3}  "
              f"{share:>8.0%}  {am * 100:>15.0f}%")
    decisive = tot["win"] + tot["loss"]
    overall = tot["win"] / decisive if decisive else float("nan")
    print("-" * 72)
    print(f"{'TOTAL':<20} {tot['win']:>3} {tot['loss']:>3} {tot['draw']:>3} {tot['error']:>3}  {overall:>8.0%}")

    ci_low, ci_high = wilson_interval(tot["win"], decisive) if decisive else (float("nan"), float("nan"))
    print(f"{'95% CI':<20} [{ci_low:.0%}, {ci_high:.0%}]  over {decisive} decisive games "
          f"({tot['draw']} draws, {tot['error']} errors)")

    passed = decisive >= 30 and ci_low > 0.50 and tot["error"] == 0
    if decisive < 30:
        print(f"\nVERDICT: INCONCLUSIVE (only {decisive} decisive games; run >=30, e.g. --num-seeds 8)")
    else:
        print(f"\nVERDICT: {'PASS' if passed else 'FAIL'} "
              f"(PASS needs the 95%% CI lower bound above 50%%, i.e. significantly better than the baseline)")

    payload = {
        "challenger": args.challenger, "baseline": args.baseline, "maps": args.maps, "positions": args.positions,
        "seeds": args.seeds, "max_gf": args.max_gf, "totals": tot, "overall_winshare": overall,
        "decisive_games": decisive, "ci95": [ci_low, ci_high], "passed": passed,
        "games": [
            {
                "map": r.map_name, "seed": r.seed, "orientation": r.orientation, "verdict": r.verdict(),
                "reason": r.reason, "gf": r.gf, "finished": r.finished, "wall_secs": round(r.wall_secs, 1),
                "ok": r.ok, "error": r.error,
                "challenger": dataclasses.asdict(r.challenger) if r.ok and r.challenger else None,
                "baseline": dataclasses.asdict(r.baseline) if r.ok and r.baseline else None,
            }
            for r in results
        ],
    }
    (out_dir / "summary.json").write_text(json.dumps(payload, indent=2))
    print(f"\nWrote {out_dir / 'summary.json'}")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
