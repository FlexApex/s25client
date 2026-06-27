#!/usr/bin/env python3
# Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
# SPDX-License-Identifier: GPL-2.0-or-later
"""
LLM sidecar for the Return To The Roots "llm" AI.

The C++ AI (ai/llm) drops a JSON world snapshot per decision into a spool directory
(req_p<player>_<gf>.json). This sidecar watches that directory, asks an OpenAI-compatible
chat endpoint for a high-level plan, and writes the plan back as resp_p<player>_<gf>.txt
(key=value lines) for the C++ side to read. The C++ side never needs network code, and any
model can be swapped purely via .env.

Config (read from .env, falling back to .env.nvidia-input, then the environment):
    LLM_URL     base URL of an OpenAI-compatible endpoint (…/v1)
    LLM_APIKEY  bearer token
    LLM_MODEL   model id (e.g. GLM-5.1 on NVIDIA)
  optional:
    LLM_TEMPERATURE (default 0.7), LLM_MAX_TOKENS (default 700), LLM_TIMEOUT (default 60)

Usage:
    python3 llm_sidecar.py --spool /tmp/rttr_llm          # serve
    python3 llm_sidecar.py --selftest                     # one tiny call, prints OK/FAIL
    python3 llm_sidecar.py --spool DIR --once             # process current files once and exit

The API key is never printed. --selftest reports only which variable NAMES were found.
"""
import argparse
import glob
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request

# ---- config -----------------------------------------------------------------

ALIASES = {
    "url": ["LLM_URL", "OPENAI_BASE_URL", "OPENAI_API_BASE", "NVIDIA_BASE_URL", "BASE_URL"],
    "key": ["LLM_APIKEY", "LLM_API_KEY", "OPENAI_API_KEY", "NVIDIA_API_KEY", "API_KEY", "NGC_API_KEY"],
    "model": ["LLM_MODEL", "OPENAI_MODEL", "MODEL", "MODEL_ID", "MODEL_NAME"],
}


def load_env_files():
    """Parse .env then .env.nvidia-input (without overriding already-set values)."""
    env = {}
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = []
    for base in (os.getcwd(), here, os.path.abspath(os.path.join(here, "..", ".."))):
        candidates += [os.path.join(base, ".env"), os.path.join(base, ".env.nvidia-input")]
    for path in candidates:
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as fh:
                for line in fh:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    k, v = line.split("=", 1)
                    k = k.strip().strip('"').strip("'")
                    v = v.strip()
                    if v.endswith(","):  # tolerate JSON-style trailing commas
                        v = v[:-1].strip()
                    v = v.strip('"').strip("'")
                    if k:
                        env.setdefault(k, v)
        except OSError:
            pass
    return env


def resolve(env, kind):
    for name in ALIASES[kind]:
        if env.get(name):
            return env[name], name
        if os.environ.get(name):
            return os.environ[name], name
    return None, None


# ---- prompt -----------------------------------------------------------------

SYSTEM_PROMPT = """You are the strategic brain of a computer player in "The Settlers II" (Return To The Roots).
A fast C++ executor handles all micro (placing buildings, roads, recruiting, attacks). YOUR job is to
set a handful of high-level knobs every ~50 game-seconds, reacting to the situation. Variety and
adaptation are valued: play the persona, exploit weakness, react to threats, and DO NOT plateau.

Hard-won facts about this game:
- TERRITORY is the master lever. Military buildings claim land; land gates farms -> food -> fed mines
  -> coal+iron -> swords/shields -> soldiers. When you have room, keep expanding.
- With no gold (gold removed/converted), military strength == soldier COUNT and there are no rank
  upgrades, so just field more soldiers (more coal+iron->smelter->armory + food to sustain miners).
- If you are CONTAINED (boxed in, can't expand), stop wasting effort on expansion: convert the idle
  surplus into more weapons production and press out with attacks.
- A pure economy build loses to timing pushes; a pure rush stalls. Balance, and adapt to the enemy:
  if behind militarily, build weapons + defenders and stop throwing away attacks; if clearly ahead,
  press the attack and expand into them.
- Recruiting early at full ratio drains workers; ramp it up as your territory grows.

You will receive a JSON snapshot of your empire and the strongest enemy. Reply with ONLY a JSON
object (no prose, no markdown) with these keys:
  "persona": one of Balanced|Rusher|Boomer|Turtle|Expander
  "expansionAggression": 0-10
  "economyFocus": 0-10
  "militaryFocus": 0-10
  "attackAggression": 0-10
  "recruitRatio": 0-10
  "frontierFill": 0-8
  "wantExpand": true/false
  "chat": a short in-character taunt/》narration (<=80 chars)
"""

KEYS_INT = {
    "expansionAggression": (0, 10),
    "economyFocus": (0, 10),
    "militaryFocus": (0, 10),
    "attackAggression": (0, 10),
    "recruitRatio": (0, 10),
    "frontierFill": (0, 8),
}
PERSONAS = {"Balanced", "Rusher", "Boomer", "Turtle", "Expander"}


def call_llm(cfg, messages):
    body = json.dumps({
        "model": cfg["model"],
        "messages": messages,
        "temperature": cfg["temperature"],
        "max_tokens": cfg["max_tokens"],
    }).encode("utf-8")
    url = cfg["url"].rstrip("/") + "/chat/completions"
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + cfg["key"])
    with urllib.request.urlopen(req, timeout=cfg["timeout"]) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data["choices"][0]["message"]["content"]


def extract_json(text):
    """Pull the last {...} JSON object out of a possibly chatty/reasoning response."""
    depth = 0
    start = -1
    best = None
    for i, c in enumerate(text):
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start >= 0:
                best = text[start:i + 1]
    if best is None:
        return None
    try:
        return json.loads(best)
    except json.JSONDecodeError:
        return None


def plan_to_kv(plan, current):
    """Validate/clamp the model's plan, falling back to current values, return key=value text."""
    out = {}
    p = plan.get("persona")
    out["persona"] = p if p in PERSONAS else current.get("persona", "Balanced")
    for k, (lo, hi) in KEYS_INT.items():
        try:
            v = int(round(float(plan.get(k, current.get(k, lo)))))
        except (TypeError, ValueError):
            v = int(current.get(k, lo))
        out[k] = max(lo, min(hi, v))
    we = plan.get("wantExpand", current.get("wantExpand", True))
    out["wantExpand"] = 1 if (we is True or str(we).lower() in ("1", "true", "yes")) else 0
    chat = str(plan.get("chat", "")).replace("\n", " ").replace("\r", " ")[:80]
    lines = [f"{k}={v}" for k, v in out.items()]
    if chat:
        lines.append("chat=" + chat)
    return "\n".join(lines) + "\n"


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(tmp, path)


def handle_request(cfg, req_path, cache):
    base = os.path.basename(req_path)              # req_p<P>_<gf>.json
    stem = base[len("req_"):-len(".json")]          # p<P>_<gf>
    resp_path = os.path.join(os.path.dirname(req_path), "resp_" + stem + ".txt")
    if os.path.exists(resp_path):
        return  # already answered, awaiting consumption
    try:
        with open(req_path, "r", encoding="utf-8") as fh:
            snapshot = json.load(fh)
    except (OSError, json.JSONDecodeError):
        return  # half-written or gone; try again next loop

    current = snapshot.get("currentStrategy", {})
    # Cache by the situational fields (drop volatile gf) so identical positions reuse a decision.
    cache_key = hashlib.sha1(
        json.dumps({k: v for k, v in snapshot.items() if k != "gf"}, sort_keys=True).encode()
    ).hexdigest()
    if cache_key in cache:
        write_atomic(resp_path, cache[cache_key])
        return

    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": json.dumps(snapshot)},
    ]
    try:
        content = call_llm(cfg, messages)
        plan = extract_json(content) or {}
    except (urllib.error.URLError, urllib.error.HTTPError, KeyError, ValueError, TimeoutError) as e:
        sys.stderr.write(f"[sidecar] LLM call failed for {base}: {type(e).__name__}\n")
        # Leave no response: the C++ side keeps playing on its heuristic and will retry next tick.
        return
    kv = plan_to_kv(plan, current)
    cache[cache_key] = kv
    write_atomic(resp_path, kv)
    sys.stderr.write(f"[sidecar] {stem}: {kv.splitlines()[0]} ... ({len(plan)} keys)\n")


def serve(cfg, spool, once, poll):
    cache = {}
    os.makedirs(spool, exist_ok=True)
    sys.stderr.write(f"[sidecar] serving {spool} via model '{cfg['model']}' at {cfg['url']}\n")
    while True:
        for req in sorted(glob.glob(os.path.join(spool, "req_*.json"))):
            handle_request(cfg, req, cache)
        if once:
            return
        time.sleep(poll)


def build_cfg(env, require_key=True):
    url, un = resolve(env, "url")
    key, kn = resolve(env, "key")
    model, mn = resolve(env, "model")
    missing = [kind for kind, val in (("LLM_URL", url), ("LLM_APIKEY", key), ("LLM_MODEL", model)) if not val]
    if require_key and missing:
        sys.stderr.write("[sidecar] missing config (checked .env, .env.nvidia-input, environment):\n")
        for kind in missing:
            sys.stderr.write(f"          {kind} (aliases tried: {', '.join(ALIASES[{'LLM_URL':'url','LLM_APIKEY':'key','LLM_MODEL':'model'}[kind]])})\n")
        sys.exit(2)
    sys.stderr.write(f"[sidecar] config: url<-{un}, key<-{kn}, model<-{mn} (model='{model}')\n")
    return {
        "url": url, "key": key, "model": model,
        "temperature": float(env.get("LLM_TEMPERATURE", os.environ.get("LLM_TEMPERATURE", "0.7"))),
        "max_tokens": int(env.get("LLM_MAX_TOKENS", os.environ.get("LLM_MAX_TOKENS", "700"))),
        "timeout": float(env.get("LLM_TIMEOUT", os.environ.get("LLM_TIMEOUT", "60"))),
    }


def main():
    ap = argparse.ArgumentParser(description="LLM sidecar for the RttR llm AI")
    ap.add_argument("--spool", default=os.environ.get("RTTR_LLM_SPOOL", "/tmp/rttr_llm"))
    ap.add_argument("--poll", type=float, default=0.25, help="seconds between directory scans")
    ap.add_argument("--once", action="store_true", help="process current files once then exit")
    ap.add_argument("--selftest", action="store_true", help="make one tiny call and report")
    args = ap.parse_args()

    env = load_env_files()
    cfg = build_cfg(env)

    if args.selftest:
        try:
            reply = call_llm(cfg, [{"role": "user", "content": "Reply with exactly: OK"}])
            print("SELFTEST OK — model replied:", reply.strip()[:60])
        except Exception as e:  # noqa: BLE001 - report any failure plainly
            print("SELFTEST FAILED:", type(e).__name__, str(e)[:200])
            sys.exit(1)
        return

    serve(cfg, args.spool, args.once, args.poll)


if __name__ == "__main__":
    main()
