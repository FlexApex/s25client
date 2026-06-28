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

Two model tiers (role-based, never host-based):
    expensive  strong/costly/rate-limited strategist; called rarely (opening, deep-think,
               escalation). Produces a long-lived GamePlan (requestKind=plan).
    cheap      fast/unlimited tactician; called every strategist tick (requestKind=tick).
A request rides "tier" and "requestKind" inside the JSON (schema 2). Schema-1 requests (no
tier/requestKind) are accepted and default to the cheap/tick path, so the sidecar works with
both the current and the upgraded C++ wire.

Config (read from .env, falling back to .env.nvidia-input, then the environment). Each tier
has its own optional alias prefix; a tier with incomplete config is simply disabled (the other
tier, or the C++ heuristic floor, takes over). The generic LLM_* keys serve as the default for
both tiers:
    expensive: LLM_EXPENSIVE_URL / _APIKEY / _MODEL  (else generic LLM_URL / LLM_APIKEY / LLM_MODEL)
    cheap:     LLM_CHEAP_URL / _APIKEY / _MODEL       (else generic LLM_URL / LLM_APIKEY / LLM_MODEL)
  optional per tier (else generic, else built-in default):
    LLM[_<TIER>]_TEMPERATURE, LLM[_<TIER>]_MAX_TOKENS, LLM[_<TIER>]_TIMEOUT

Usage:
    python3 llm_sidecar.py --spool /tmp/rttr_llm          # serve
    python3 llm_sidecar.py --spool /tmp/rttr_llm --stub   # serve canned plans, NO network
    python3 llm_sidecar.py --selftest                     # one tiny call, prints OK/FAIL
    python3 llm_sidecar.py --spool DIR --once             # process current files once and exit

The API key is never printed. --selftest reports only which variable NAMES were found.
"""
import argparse
import glob
import datetime
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

# ---- logging ----------------------------------------------------------------
# Leveled, greppable logging to stderr. Tail it on a second monitor while gaming:
#   ... 2>llm.log ; tail -f llm.log                         # everything
#   tail -f llm.log | grep 'explanation:'                   # the model's one-liner reasoning
#   tail -f llm.log | grep -E ' (WARN|ERROR) '              # problems (HTTP codes, fallbacks)
#   tail -f llm.log | grep -E ' (REQ|RESP) '                # request/response trace
# Set LLM_LOG_LEVEL=DEBUG to also emit full request/response JSON (REQ-FULL / RESP-FULL).
_LEVELS = {"DEBUG": 10, "INFO": 20, "WARN": 30, "ERROR": 40}
_LOG_LEVEL = _LEVELS.get(os.environ.get("LLM_LOG_LEVEL", "INFO").upper(), 20)


def log(level, msg):
    if _LEVELS.get(level, 20) < _LOG_LEVEL:
        return
    ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    sys.stderr.write(f"{ts} {level:<5} {msg}\n")
    sys.stderr.flush()


# ---- config -----------------------------------------------------------------

# Generic aliases (the default for either tier). Tier-specific keys (LLM_EXPENSIVE_* /
# LLM_CHEAP_*) are tried first via TIER_PREFIX before falling back to these.
GENERIC = {
    "url": ["LLM_URL", "OPENAI_BASE_URL", "OPENAI_API_BASE", "NVIDIA_BASE_URL", "BASE_URL"],
    "key": ["LLM_APIKEY", "LLM_API_KEY", "OPENAI_API_KEY", "NVIDIA_API_KEY", "API_KEY", "NGC_API_KEY"],
    "model": ["LLM_MODEL", "OPENAI_MODEL", "MODEL", "MODEL_ID", "MODEL_NAME"],
}
TIER_PREFIX = {"expensive": "LLM_EXPENSIVE_", "cheap": "LLM_CHEAP_"}
TIERS = ("expensive", "cheap")

# Per-tier generation defaults (overridable via env). Expensive: creative/long; cheap: tight/short.
TIER_DEFAULTS = {
    "expensive": {"temperature": 1.0, "max_tokens": 1100, "timeout": 90.0},
    "cheap": {"temperature": 0.3, "max_tokens": 500, "timeout": 60.0},
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


def _lookup(env, names):
    for name in names:
        if env.get(name):
            return env[name], name
        if os.environ.get(name):
            return os.environ[name], name
    return None, None


def resolve(env, kind, tier):
    """Resolve url/key/model for a tier: tier-specific alias first, then generic LLM_* fallback.
    Tier aliases are the generic LLM_* names with the LLM_ prefix swapped for the tier prefix
    (LLM_APIKEY -> LLM_CHEAP_APIKEY, LLM_URL -> LLM_CHEAP_URL, ...), so the KEY suffix matches the
    generic convention (APIKEY) instead of the wrong "KEY" that kind.upper() produced."""
    prefix = TIER_PREFIX.get(tier)
    if prefix:
        tier_names = [prefix + n[len("LLM_"):] for n in GENERIC[kind] if n.startswith("LLM_")]
        val, name = _lookup(env, tier_names)
        if val:
            return val, name
    return _lookup(env, GENERIC[kind])


def _tier_setting(env, tier, suffix, default):
    """Read a numeric per-tier override (LLM_<TIER>_<SUFFIX>) then generic (LLM_<SUFFIX>)."""
    prefix = TIER_PREFIX.get(tier)
    names = ([prefix + suffix] if prefix else []) + ["LLM_" + suffix]
    val, _ = _lookup(env, names)
    return val if val is not None else default


def build_cfg(env, tier, require_key=False):
    """Return a config dict for a tier, or None if its config is incomplete (never sys.exit)."""
    url, un = resolve(env, "url", tier)
    key, kn = resolve(env, "key", tier)
    model, mn = resolve(env, "model", tier)
    needs = url and model and (key or not require_key)
    if not needs:
        return None
    d = TIER_DEFAULTS[tier]
    try:
        temperature = float(_tier_setting(env, tier, "TEMPERATURE", d["temperature"]))
    except (TypeError, ValueError):
        temperature = d["temperature"]
    try:
        max_tokens = int(_tier_setting(env, tier, "MAX_TOKENS", d["max_tokens"]))
    except (TypeError, ValueError):
        max_tokens = d["max_tokens"]
    try:
        timeout = float(_tier_setting(env, tier, "TIMEOUT", d["timeout"]))
    except (TypeError, ValueError):
        timeout = d["timeout"]
    return {
        "tier": tier, "url": url, "key": key or "", "model": model,
        "url_from": un, "key_from": kn, "model_from": mn,
        "temperature": temperature, "max_tokens": max_tokens, "timeout": timeout,
    }


# ---- validation tables (PINNED to Strategy.h Section 1.1 wire tokens) --------

PERSONAS = {"Balanced", "Rusher", "Boomer", "Turtle", "Expander"}
KEYS_INT = {
    "expansionAggression": (0, 10),
    "economyFocus": (0, 10),
    "militaryFocus": (0, 10),
    "attackAggression": (0, 10),
    "recruitRatio": (0, 10),
    "frontierFill": (0, 8),
}
# Phase: Auto is internal-only (never a wire token).
PLAN_PHASES = {"Open", "Expand", "Consolidate", "Push", "Defend"}
# SectorRole tokens the model may meaningfully choose.
SECTOR_ROLES = {"MilitaryPush", "ExpandEconomy", "FarmBelt", "MiningOutpost", "Hold", "Ignore"}
# Tokens valid to EMIT on the wire: the above plus "Auto", the explicit no-op used to pad/coerce
# missing or junk slots (A5). The C++ parser maps "Auto" -> SectorRole::Auto (no spatial override).
SECTOR_ROLES_EMIT = SECTOR_ROLES | {"Auto"}
FOCUS_TAGS = {"SecureIron", "SecureCoal", "SecureStone", "ExpandFront", "BoomEconomy",
              "AttackEnemy", "Defend", "Raid", "None"}
# AttackIntent: Auto is internal-only.
ATTACK_INTENT = {"Hold", "Probe", "Commit", "AllIn"}


# ---- dynamic addon-aware system prompt --------------------------------------

BASE_PROMPT = """You are the strategic brain of a computer player in "The Settlers II" (Return To The Roots).
A fast C++ executor handles all micro (placing buildings, roads, recruiting, attacks). YOUR job is to
set a handful of high-level knobs and a longer-lived plan, reacting to the situation. Variety and
adaptation are valued: play the persona, exploit weakness, react to threats, and DO NOT plateau.

Hard-won facts about this game:
- TERRITORY is the master lever. Military buildings claim land; land gates farms -> food -> fed mines
  -> coal+iron -> swords/shields -> soldiers. When you have room, keep expanding.
- A pure economy build loses to timing pushes; a pure rush stalls. Balance, and adapt to the enemy:
  if behind militarily, build weapons + defenders and stop throwing away attacks; if clearly ahead,
  press the attack and expand into them.
- If you are CONTAINED (boxed in, can't expand), stop wasting effort on expansion: convert the idle
  surplus into more weapons production and press out with attacks.
- Recruiting early at full ratio drains workers; ramp it up as your territory grows.
"""

# Economy/ruleset notes appended ONLY when the matching addon/rule is active for this game.
RULE_SECTIONS = {
    "goldGranite": (
        "- GOLD is removed (converted to granite): there are no coins and no rank upgrades, so military\n"
        "  strength == soldier COUNT. Just field more soldiers (more coal+iron -> smelter -> armory, plus\n"
        "  food to keep miners fed). Granite mines on mountains are an inexhaustible STONE source here;\n"
        "  stone gates military buildings AND smelters, so secure mountain tiles early."
    ),
    "inexhaustibleMines": (
        "- Mines are INEXHAUSTIBLE: a mine never depletes, so a claimed deposit is a permanent engine.\n"
        "  Prioritise reaching and holding iron/coal deposits; one good mine pays off forever."
    ),
    "wine": "- WINE economy is enabled (vineyards/temples): plan for it as an extra production chain.",
    "leather": "- LEATHER economy is enabled (skinner/tannery/armory): an alternative armor/weapon chain.",
    "charburner": "- CHARBURNER is enabled: charcoal can substitute for coal when coal is scarce.",
    "seaAttack": "- SEA ATTACK is enabled: harbors+ships let you strike island/coastal enemies; watch your coast.",
    "halfCost": "- HALF-COST military equipment: weapons are cheaper, so militarising is faster than usual.",
    "maxRank": "- MAX RANK is limited: rank upgrades are capped, so leaning on soldier COUNT matters more.",
}

OUTPUT_PLAN = """You are the EXPENSIVE strategist producing a long-lived GAME PLAN. Reply with ONLY a JSON
object (no prose, no markdown) with these keys (all optional; omit a key to keep the current value):
  "strategyName": short name for this plan (<=48 chars)
  "persona": one of Balanced|Rusher|Boomer|Turtle|Expander
  "expansionAggression": 0-10, "economyFocus": 0-10, "militaryFocus": 0-10,
  "attackAggression": 0-10, "recruitRatio": 0-10, "frontierFill": 0-8, "wantExpand": true/false
  "phase": one of Open|Expand|Consolidate|Push|Defend
  "primaryTargetEnemy": enemy player id to focus, or -1 for none
  "feintTargetEnemy": enemy player id to feint at, or -1 for none
  "timingTrigger": when to commit, e.g. "army>=70" or "minute>=40" (string)
  "economicGambit": true/false (boom now, militarise at the trigger)
  "sectorRoles": list of 6 sector roles by direction [W,NW,NE,E,SE,SW], each one of
                 MilitaryPush|ExpandEconomy|FarmBelt|MiningOutpost|Hold|Ignore
  "explanation": REQUIRED. One concise, human-readable sentence (<=140 chars) on what this plan does
                 and why (e.g. "Booming economy while walling the south; will push P2 once army>=70.").
                 This is shown to the human watching the game - make it genuinely informative.
"""

OUTPUT_TICK = """You are the CHEAP tactician adapting the live plan THIS tick. Reply with ONLY a JSON object
(no prose, no markdown) with these keys (all optional; omit a key to keep the current value):
  "persona": one of Balanced|Rusher|Boomer|Turtle|Expander
  "expansionAggression": 0-10, "economyFocus": 0-10, "militaryFocus": 0-10,
  "attackAggression": 0-10, "recruitRatio": 0-10, "frontierFill": 0-8, "wantExpand": true/false
  "focusPrimary"/"focusSecondary": one of SecureIron|SecureCoal|SecureStone|ExpandFront|BoomEconomy|
                                   AttackEnemy|Defend|Raid|None
  "expandIntent": true/false (keep pushing land, or hold)
  "attackIntent": one of Hold|Probe|Commit|AllIn
  "defense": 0-10 (defensive stance: 0 loose .. 10 fortress)
  "requestReplan": true/false (ask the expensive strategist for a fresh plan now)
  "explanation": REQUIRED. One concise, human-readable sentence (<=120 chars) on your read of the
                 situation and what you're adjusting this tick. Shown to the human watching the game.
"""


def merged_addons(snap):
    """Combine the request's rules and any explicit addons block; addons wins on conflict."""
    merged = {}
    rules = snap.get("rules")
    if isinstance(rules, dict):
        merged.update(rules)
    addons = snap.get("addons")
    if isinstance(addons, dict):
        merged.update(addons)
    return merged


def build_system_prompt(addons, kind):
    """Assemble BASE + only-enabled-economy sections + the kind-specific output contract."""
    parts = [BASE_PROMPT]
    enabled = []
    # gold->granite: explicit hasGold==false (or changeGoldDeposits>=1) means no gold economy.
    has_gold = addons.get("hasGold")
    change_gold = addons.get("changeGoldDeposits")
    if has_gold is False or (isinstance(change_gold, (int, float)) and change_gold >= 1):
        enabled.append("goldGranite")
    if addons.get("inexhaustibleMines"):
        enabled.append("inexhaustibleMines")
    if addons.get("wine"):
        enabled.append("wine")
    if addons.get("leather"):
        enabled.append("leather")
    if addons.get("charburner"):
        enabled.append("charburner")
    if addons.get("seaAttack"):
        enabled.append("seaAttack")
    if addons.get("halfCostMilEquip"):
        enabled.append("halfCost")
    mr = addons.get("maxRank")
    if isinstance(mr, (int, float)) and mr >= 1:
        enabled.append("maxRank")
    if enabled:
        parts.append("\nRuleset notes for THIS game:\n" + "\n".join(RULE_SECTIONS[t] for t in enabled))
    parts.append("\n" + (OUTPUT_PLAN if kind == "plan" else OUTPUT_TICK))
    return "\n".join(parts)


def get_system_prompt(snap, kind, cache):
    """Cache the assembled prompt keyed by (enabled ruleset, kind) so it is built once per situation."""
    addons = merged_addons(snap)
    flags = (
        addons.get("hasGold"), addons.get("changeGoldDeposits"), bool(addons.get("inexhaustibleMines")),
        bool(addons.get("wine")), bool(addons.get("leather")), bool(addons.get("charburner")),
        bool(addons.get("seaAttack")), bool(addons.get("halfCostMilEquip")), addons.get("maxRank"),
    )
    key = (flags, kind)
    prompt = cache.get(key)
    if prompt is None:
        prompt = build_system_prompt(addons, kind)
        cache[key] = prompt
    return prompt


# ---- LLM call ---------------------------------------------------------------

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
    # Some endpoints sit behind Cloudflare, which bans the default Python-urllib UA by client
    # signature (HTTP 403, "error code: 1010"). Present a normal browser UA so the call gets through.
    req.add_header("User-Agent",
                   "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/124.0.0.0 Safari/537.36")
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


# ---- plan -> key=value flattening + validation ------------------------------

_TIMING_RE = re.compile(r"(army|minute)\s*>=?\s*(\d+)", re.IGNORECASE)


def _truthy(v):
    return v is True or str(v).strip().lower() in ("1", "true", "yes")


def _clean_text(v, limit):
    return str(v).replace("\n", " ").replace("\r", " ").strip()[:limit]


def _clamp_player(v):
    """Player ids clamp to -1..11; anything unparseable -> -1 (none)."""
    try:
        n = int(round(float(v)))
    except (TypeError, ValueError):
        return -1
    return max(-1, min(11, n))


def _parse_timing(plan):
    """Parse a "timingTrigger" string into (army, minute) ints. Unparseable -> (0, 0) = no gate."""
    army = minute = 0
    trig = plan.get("timingTrigger")
    if isinstance(trig, str):
        for kind, num in _TIMING_RE.findall(trig):
            n = int(num)
            if kind.lower() == "army":
                army = n
            else:
                minute = n
    # Allow explicit numeric overrides too.
    for key, setter in (("timingTriggerArmy", "army"), ("timingTriggerMinute", "minute")):
        if key in plan:
            try:
                val = max(0, int(round(float(plan[key]))))
            except (TypeError, ValueError):
                continue
            if setter == "army":
                army = val
            else:
                minute = val
    return max(0, min(5000, army)), max(0, min(600, minute))


def _sector_roles_csv(plan):
    """Build the CSV[6] sectorRoles line: each token validated, missing/junk -> Auto. None on absent.

    A5: pad and coerce to "Auto" (the structural no-op), NOT "Ignore". Ignore actively suppresses a
    sector (-200 to every building there), so padding a short/partly-garbled list with Ignore would
    silently turn OFF the sectors the model didn't speak to, soft-freezing placement. Auto restores the
    "absent/garbage => no-op" invariant and matches the C++ parser (short CSV leaves slots Auto)."""
    raw = plan.get("sectorRoles")
    if raw is None:
        return None
    if isinstance(raw, str):
        items = [t.strip() for t in raw.split(",")]
    elif isinstance(raw, (list, tuple)):
        items = [str(t).strip() for t in raw]
    else:
        return None
    items = (items + ["Auto"] * 6)[:6]
    toks = [t if t in SECTOR_ROLES_EMIT else "Auto" for t in items]
    return ",".join(toks)


def plan_to_kv(plan, current, kind):
    """Validate/clamp the model's plan against the PINNED tables, falling back to current values.

    Emits the shared knobs always; plan-only keys when kind=="plan"; tick-only keys when kind=="tick".
    Unknown enum value -> drop the key (the C++ side then keeps its prior value).
    Returns (key=value text, explanation one-liner) - the explanation is logged for human tracing and
    also shown in-game via the chat= line.
    """
    lines = []

    # --- shared knobs (valid in both plan and tick) ---
    p = plan.get("persona")
    if p in PERSONAS:
        lines.append("persona=" + p)
    elif current.get("persona") in PERSONAS:
        lines.append("persona=" + current["persona"])
    for k, (lo, hi) in KEYS_INT.items():
        if k not in plan and k not in current:
            continue
        try:
            v = int(round(float(plan.get(k, current.get(k, lo)))))
        except (TypeError, ValueError):
            v = int(current.get(k, lo))
        lines.append(f"{k}={max(lo, min(hi, v))}")
    if "wantExpand" in plan or "wantExpand" in current:
        we = plan.get("wantExpand", current.get("wantExpand", True))
        lines.append("wantExpand=" + ("1" if _truthy(we) else "0"))

    # Free text. The model's `explanation` (a concise human-readable one-liner on what it's doing and
    # why) is the primary source; rationale/diagnosis/chat are accepted fallbacks. It is logged with an
    # "explanation:" prefix for human tracing AND shown in-game via the chat= line. strategyName its own.
    name = plan.get("strategyName")
    if isinstance(name, str) and name.strip():
        lines.append("strategyName=" + _clean_text(name, 48))
    explanation = plan.get("explanation")
    if not (isinstance(explanation, str) and explanation.strip()):
        explanation = plan.get("rationale") if kind == "plan" else plan.get("diagnosis")
    if not (isinstance(explanation, str) and explanation.strip()):
        explanation = plan.get("chat")
    explanation = _clean_text(explanation, 160) if isinstance(explanation, str) else ""
    if explanation:
        lines.append("chat=" + explanation)

    # --- plan-only keys ---
    if kind == "plan":
        ph = plan.get("phase")
        if ph in PLAN_PHASES:
            lines.append("phase=" + ph)
        if "primaryTargetEnemy" in plan:
            lines.append("primaryTargetEnemy=" + str(_clamp_player(plan["primaryTargetEnemy"])))
        if "feintTargetEnemy" in plan:
            lines.append("feintTargetEnemy=" + str(_clamp_player(plan["feintTargetEnemy"])))
        army, minute = _parse_timing(plan)
        if "timingTrigger" in plan or "timingTriggerArmy" in plan or "timingTriggerMinute" in plan:
            lines.append("timingTriggerArmy=" + str(army))
            lines.append("timingTriggerMinute=" + str(minute))
        if "economicGambit" in plan:
            lines.append("economicGambit=" + ("1" if _truthy(plan["economicGambit"]) else "0"))
        csv = _sector_roles_csv(plan)
        if csv is not None:
            lines.append("sectorRoles=" + csv)

    # --- tick-only keys ---
    if kind == "tick":
        for fk in ("focusPrimary", "focusSecondary"):
            fv = plan.get(fk)
            if fv in FOCUS_TAGS:
                lines.append(f"{fk}={fv}")
        if "expandIntent" in plan:
            lines.append("expandIntent=" + ("1" if _truthy(plan["expandIntent"]) else "0"))
        ai = plan.get("attackIntent")
        if ai in ATTACK_INTENT:
            lines.append("attackIntent=" + ai)
        if "defense" in plan:
            try:
                d = int(round(float(plan["defense"])))
            except (TypeError, ValueError):
                d = 5
            lines.append("defense=" + str(max(0, min(10, d))))
        if "requestReplan" in plan:
            lines.append("requestReplan=" + ("1" if _truthy(plan["requestReplan"]) else "0"))

    return "\n".join(lines) + "\n", explanation


# ---- stub mode (deterministic canned plans, ZERO network) -------------------

def stub_plan(snap, kind, current):
    """Deterministic canned plan, seeded by player+gf bucket, reacting to the live situation.

    Routes by requestKind and exercises the full plan_to_kv validation path so the wire is tested
    end to end with no .env and no network.
    """
    player = int(snap.get("player", 0))
    gf = int(snap.get("gf", 0))
    minutes = int(snap.get("minutes", gf // 1200))
    contained = bool(snap.get("contained", False))
    self_blk = snap.get("self", {}) if isinstance(snap.get("self"), dict) else {}
    enemy = snap.get("enemy", {}) if isinstance(snap.get("enemy"), dict) else {}
    my_mil = int(self_blk.get("militaryStrength", 0) or 0)
    enemy_mil = int(enemy.get("bestMilitary", 0) or 0)
    behind = enemy_mil > my_mil + 5
    ahead = my_mil > enemy_mil + 5
    bucket = (gf // 1000 + player) % 4  # tiny rotation for variety, still deterministic

    plan = {}
    personas = ["Expander", "Boomer", "Balanced", "Rusher"]
    plan["persona"] = "Turtle" if behind else personas[bucket]

    if kind == "plan":
        # Digest-aware planning: read the self-hints, the 6-sector mapDigest and the opponents list so
        # the plan is RESOURCE-DIRECTED and MAP-SPECIFIC (a proxy for a competent model — exercises the
        # whole digest->plan->executor path the heuristic floor never touches).
        iron_mines = int(self_blk.get("ironMines", 0) or 0)
        smelters = int(self_blk.get("smelters", 0) or 0)
        granite = int(self_blk.get("graniteMines", self_blk.get("granite", 0)) or 0)
        stone_starved = bool(self_blk.get("stoneStarved", False))
        iron_chain_broken = bool(self_blk.get("ironChainBroken", False))
        need_iron = iron_mines < 3 or smelters < 2 or iron_chain_broken
        need_stone = stone_starved or granite < 2
        md = snap.get("mapDigest") if isinstance(snap.get("mapDigest"), dict) else {}
        sectors = md.get("sectors") if isinstance(md.get("sectors"), list) else []
        opps = snap.get("opponents") if isinstance(snap.get("opponents"), list) else []
        # Primary target = weakest attackable enemy (else none); attack only once we have an army.
        primary = -1
        if opps:
            weakest = min(opps, key=lambda o: int(o.get("military", 1 << 30) or (1 << 30)))
            primary = int(weakest.get("id", -1))

        if behind:
            plan["phase"] = "Defend"
        elif contained:
            plan["phase"] = "Consolidate"
        elif ahead:
            plan["phase"] = "Push"
        elif minutes < 25:
            plan["phase"] = "Open"
        else:
            plan["phase"] = "Expand"
        plan["strategyName"] = f"Stub-{plan['phase']}-{bucket}"
        plan["expansionAggression"] = 3 if contained else 7
        plan["economyFocus"] = 7 if plan["phase"] in ("Open", "Consolidate") else 5
        plan["militaryFocus"] = 8 if (need_iron or behind or ahead or contained) else 5
        plan["attackAggression"] = 7 if ahead else (2 if behind else 4)
        plan["wantExpand"] = not contained
        plan["primaryTargetEnemy"] = primary if (ahead or not behind) else -1
        plan["feintTargetEnemy"] = -1
        plan["timingTrigger"] = "army>=70" if not behind else "minute>=60"
        plan["economicGambit"] = (plan["phase"] in ("Open", "Expand")) and not behind and not need_iron

        # Build map-specific sectorRoles: claim our nearest NEEDED ore (MiningOutpost), push toward the
        # nearest enemy (MilitaryPush), grab open land (ExpandEconomy/FarmBelt); leave the rest Auto.
        roles = ["Auto"] * 6
        for s in sectors:
            try:
                idx = int(s.get("index", 0))
            except (TypeError, ValueError):
                continue
            if not 0 <= idx < 6:
                continue
            iron_d = int(s.get("iron", 255) or 255)
            gran_d = int(s.get("granite", 255) or 255)
            room = int(s.get("room", 0) or 0)
            enemy_dir = int(s.get("enemyDir", -1))
            enemy_dist = int(s.get("enemyDist", 255) or 255)
            if need_iron and iron_d <= 22:
                roles[idx] = "MiningOutpost"
            elif need_stone and gran_d <= 22:
                roles[idx] = "MiningOutpost"
            elif (ahead or plan["phase"] == "Push") and enemy_dir >= 0 and enemy_dist <= 70:
                roles[idx] = "MilitaryPush"
            elif room >= 25:
                roles[idx] = "ExpandEconomy" if (idx % 2 == 0) else "FarmBelt"
        if all(r == "Auto" for r in roles):  # fallback so the spatial layer still engages
            roles = ["MilitaryPush", "ExpandEconomy", "FarmBelt", "MiningOutpost", "Hold", "ExpandEconomy"]
        if contained:
            roles = ["MilitaryPush", "MilitaryPush", "Hold", "MiningOutpost", "Hold", "MilitaryPush"]
        plan["sectorRoles"] = roles
        goal = "secure iron" if need_iron else ("secure stone" if need_stone else "expand & grow")
        plan["explanation"] = (f"[stub] {plan['phase']} plan: {goal}"
                               + (f", press P{primary}" if (ahead and primary >= 0) else "")
                               + f" (mil {my_mil} vs {enemy_mil}).")
    else:  # tick
        plan["expansionAggression"] = 2 if contained else (8 if not behind else 4)
        plan["economyFocus"] = 8 if (minutes < 30 and not behind) else 5
        plan["militaryFocus"] = 8 if (behind or contained) else 6
        plan["attackAggression"] = 8 if ahead else (2 if behind else 4)
        plan["wantExpand"] = not contained
        if behind:
            plan["focusPrimary"], plan["focusSecondary"] = "Defend", "SecureIron"
        elif contained:
            plan["focusPrimary"], plan["focusSecondary"] = "SecureIron", "AttackEnemy"
        elif ahead:
            plan["focusPrimary"], plan["focusSecondary"] = "AttackEnemy", "ExpandFront"
        else:
            plan["focusPrimary"], plan["focusSecondary"] = "ExpandFront", "BoomEconomy"
        plan["expandIntent"] = not contained
        plan["attackIntent"] = "Commit" if ahead else ("Hold" if behind else "Probe")
        plan["defense"] = 9 if behind else (3 if ahead else 5)
        # Occasionally ask for a replan when the picture shifts hard (never latches: one-shot).
        plan["requestReplan"] = bool(behind and bucket == 0)
        stance = "defending (behind)" if behind else ("pressing (ahead)" if ahead else "steady")
        plan["explanation"] = (f"[stub] tick {minutes}m: {plan['focusPrimary']}+{plan['focusSecondary']}, "
                               f"{stance}; mil {my_mil} vs {enemy_mil}.")

    return plan_to_kv(plan, current, kind)


# ---- expensive-tier budget --------------------------------------------------

def http_err_info(e):
    """Classify an LLM-call exception -> (code|None, label, is_rate_limit). Logs the real HTTP code."""
    code = getattr(e, "code", None)
    if code is not None:
        # 429 Too Many Requests; 503/529 overloaded -> treat as transient rate-limit (cool down).
        return code, f"HTTP {code}", code in (429, 503, 529)
    if isinstance(e, urllib.error.URLError):
        return None, f"URLError: {getattr(e, 'reason', e)}", False
    return None, f"{type(e).__name__}: {e}", False


class ExpensiveTier:
    """Health/pacing for the expensive (strong, rate-limited) tier. On a rate-limit it 'cools down' and
    requests fall back to the cheap tier; after retry_after_s it retries expensive. State transitions
    produce an in-game notice (down/up) so the player sees what's happening. monotonic-clock based."""

    def __init__(self, min_interval_s=8.0, retry_after_s=60.0):
        self.min_interval_s = float(min_interval_s)   # spacing between expensive calls (budget)
        self.retry_after_s = float(retry_after_s)     # how long to avoid expensive after a rate-limit
        self._last = None                             # last successful expensive call
        self._cooldown_until = 0.0                    # monotonic; >now => rate-limited, use cheap
        self.cooling = False                          # current state (for transition detection)

    def usable(self):
        """True if an expensive call may be attempted now (not cooling, and min-interval elapsed)."""
        if time.monotonic() < self._cooldown_until:
            return False
        if self._last is not None and (time.monotonic() - self._last) < self.min_interval_s:
            return False
        return True

    def cooling_now(self):
        return time.monotonic() < self._cooldown_until

    def record_success(self):
        self._last = time.monotonic()

    def trip(self):
        """Record a rate-limit: cool down and route to cheap until retry_after_s elapses."""
        self._cooldown_until = time.monotonic() + self.retry_after_s


# ---- request handling -------------------------------------------------------

def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(tmp, path)


def _emit(resp_path, cache, cache_key, kv, notice):
    """Write the response (optionally with a one-shot in-game notice= line). Notices are transitional
    (fallback/recovery), so they are NOT cached - only the plain kv is reused for identical positions."""
    if notice:
        write_atomic(resp_path, kv + "notice=" + _clean_text(notice, 160) + "\n")
    else:
        cache[cache_key] = kv
        write_atomic(resp_path, kv)


def handle_request(req_path, cache, cfgs, expensive, prompt_cache, stub):
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

    # Schema 1 carries no tier/requestKind -> default to the cheap/tick path.
    req_tier = snapshot.get("tier", "cheap")
    if req_tier not in TIERS:
        req_tier = "cheap"
    kind = snapshot.get("requestKind", "tick")
    if kind not in ("plan", "tick"):
        kind = "tick"
    current = snapshot.get("currentStrategy", {})
    log("INFO", f"REQ  {req_tier:<9} {kind:<4} {stem}")
    log("DEBUG", f"REQ-FULL {stem} {json.dumps(snapshot)}")

    # Cache by the situational fields (drop volatile gf/minutes) so identical positions reuse a
    # decision; key includes tier+kind so plan/tick and cheap/expensive don't collide.
    sit = {k: v for k, v in snapshot.items() if k not in ("gf", "minutes")}
    cache_key = ":".join((
        req_tier, kind,
        hashlib.sha1(json.dumps(sit, sort_keys=True).encode()).hexdigest(),
    ))
    if cache_key in cache:
        write_atomic(resp_path, cache[cache_key])
        log("DEBUG", f"CACHE {stem} (reused prior decision)")
        return

    # Stub mode: deterministic canned plan, no cfg/network. Runs before any routing.
    if stub:
        kv, expl = stub_plan(snapshot, kind, current)
        cache[cache_key] = kv
        write_atomic(resp_path, kv)
        log("INFO", f"RESP {req_tier:<9} {kind:<4} {stem} (stub)")
        if expl:
            log("INFO", f"explanation: [{stem} {req_tier}] {expl}")
        return

    # Build the try-chain. ONLY fallback is expensive -> cheap (per design). Cheap requests use cheap.
    attempts = []
    if req_tier == "expensive":
        if cfgs.get("expensive") and expensive.usable():
            attempts.append(("expensive", cfgs["expensive"]))
        if cfgs.get("cheap"):
            attempts.append(("cheap", cfgs["cheap"]))
    elif cfgs.get("cheap"):
        attempts.append(("cheap", cfgs["cheap"]))
    if not attempts:
        log("WARN", f"NO-TIER {stem}: no usable model (config incomplete / cooling) -> heuristic floor")
        return

    system_prompt = get_system_prompt(snapshot, kind, prompt_cache)
    notice = None  # one-shot in-game message on a state transition (down/up)
    for used_tier, cfg in attempts:
        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": json.dumps(snapshot)},
        ]
        try:
            # Test hook: LLM_FAIL_EXPENSIVE=<code> injects a fake HTTP error on expensive calls so the
            # rate-limit fallback/recovery path can be exercised offline.
            inj = os.environ.get("LLM_FAIL_EXPENSIVE")
            if used_tier == "expensive" and inj:
                raise urllib.error.HTTPError(cfg["url"], int(inj), "injected", {}, None)
            content = call_llm(cfg, messages)
            plan = extract_json(content) or {}
        except Exception as e:  # noqa: BLE001 - any call/parse failure -> try next tier
            code, label, is_rl = http_err_info(e)
            log("ERROR", f"{used_tier} call failed for {stem}: {label}")
            if used_tier == "expensive" and is_rl:
                if not expensive.cooling:  # transition OK -> cooling: tell the player in game
                    notice = "Strong strategist model rate-limited - switching to the fast model."
                    expensive.cooling = True
                    log("WARN", f"expensive rate-limited ({label}); cooling {expensive.retry_after_s:.0f}s, "
                                f"routing to cheap")
                expensive.trip()
            continue  # fall back to the next tier (expensive -> cheap only)

        if used_tier == "expensive":
            if expensive.cooling:  # transition cooling -> OK: recovered, tell the player
                notice = "Strong strategist model back online."
                expensive.cooling = False
                log("INFO", "expensive tier recovered")
            expensive.record_success()
        kv, expl = plan_to_kv(plan, current, kind)
        _emit(resp_path, cache, cache_key, kv, notice)
        log("INFO", f"RESP {used_tier:<9} {kind:<4} {stem} ({len(plan)} keys)"
                    + (f" [fell back from expensive]" if (req_tier == "expensive" and used_tier == "cheap") else ""))
        if expl:
            log("INFO", f"explanation: [{stem} {used_tier}] {expl}")
        log("DEBUG", f"RESP-FULL {stem} {kv.strip()}")
        if notice:
            log("INFO", f"NOTICE (in-game) {stem}: {notice}")
        return
    # Every tier failed: leave no response; C++ keeps playing on its heuristic and retries next tick.
    log("WARN", f"ALL-TIERS-FAILED {stem} -> heuristic floor this tick")


def serve(cfgs, expensive, spool, once, poll, stub):
    cache = {}
    prompt_cache = {}
    os.makedirs(spool, exist_ok=True)
    if stub:
        log("INFO", f"serving {spool} in STUB mode (no network)")
    else:
        have = [t for t in TIERS if cfgs.get(t)]
        log("INFO", f"serving {spool}; tiers available: {have or 'NONE (heuristic floor only)'}")
    while True:
        for req in sorted(glob.glob(os.path.join(spool, "req_*.json"))):
            handle_request(req, cache, cfgs, expensive, prompt_cache, stub)
        if once:
            return
        time.sleep(poll)


def resolve_cfg(env, tier, overrides, require_key=False):
    """build_cfg for a tier, then apply CLI --<tier>-url/--<tier>-model overrides. Shared by the
    serving and --selftest paths so a preflight selftest probes the SAME endpoint the games will use
    (without this, --selftest ignored the CLI overrides and tested only .env)."""
    url_o, model_o = overrides.get(tier, (None, None))
    cfg = build_cfg(env, tier, require_key=require_key)
    if cfg:
        if url_o:
            cfg["url"], cfg["url_from"] = url_o, "--cli"
        if model_o:
            cfg["model"], cfg["model_from"] = model_o, "--cli"
        return cfg
    if url_o and model_o:  # tier absent from .env but fully specified on the CLI (e.g. keyless Ollama)
        d = TIER_DEFAULTS[tier]
        return {"tier": tier, "url": url_o, "key": "", "model": model_o, "url_from": "--cli",
                "key_from": "-", "model_from": "--cli", "temperature": d["temperature"],
                "max_tokens": d["max_tokens"], "timeout": d["timeout"]}
    return None


def main():
    ap = argparse.ArgumentParser(description="LLM sidecar for the RttR llm AI")
    ap.add_argument("--spool", default=os.environ.get("RTTR_LLM_SPOOL", "/tmp/rttr_llm"))
    ap.add_argument("--poll", type=float, default=0.25, help="seconds between directory scans")
    ap.add_argument("--once", action="store_true", help="process current files once then exit")
    ap.add_argument("--selftest", action="store_true", help="make one tiny call and report")
    ap.add_argument("--stub", action="store_true", help="serve deterministic canned plans, no network")
    ap.add_argument("--min-interval", type=float, default=8.0,
                    help="min seconds between expensive-tier calls (keeps cloud usage well under the cap)")
    ap.add_argument("--retry-after", type=float, default=60.0,
                    help="seconds to route around the expensive tier after a rate-limit before retrying")
    # CLI overrides (handy to point a tier at a model/endpoint without editing .env).
    ap.add_argument("--cheap-model"); ap.add_argument("--cheap-url")
    ap.add_argument("--expensive-model"); ap.add_argument("--expensive-url")
    args = ap.parse_args()
    _overrides = {"cheap": (args.cheap_url, args.cheap_model),
                  "expensive": (args.expensive_url, args.expensive_model)}

    stub = args.stub or os.environ.get("LLM_STUB") in ("1", "true", "yes")

    if args.selftest:
        if stub:
            print("SELFTEST OK — stub mode (no network)")
            return
        env = load_env_files()
        any_cfg = False
        any_ok = False
        for t in TIERS:
            cfg = resolve_cfg(env, t, _overrides, require_key=False)
            if cfg is None:
                print(f"  {t:<9}: not configured")
                continue
            any_cfg = True
            print(f"  {t:<9}: model='{cfg['model']}' url<-{cfg['url_from']} -> POST {cfg['url'].rstrip('/')}/chat/completions")
            try:
                reply = call_llm(cfg, [{"role": "user", "content": "Reply with exactly: OK"}])
                print(f"             OK — replied: {reply.strip()[:50]!r}")
                any_ok = True
            except Exception as e:  # noqa: BLE001 - report each tier plainly with HTTP code
                _, label, _ = http_err_info(e)
                hint = ""
                if getattr(e, "code", None) == 404:
                    hint = "  (HTTP 404 -> wrong path? Ollama needs the URL to end in /v1)"
                print(f"             FAILED: {label}{hint}")
        if not any_cfg:
            print("SELFTEST FAILED: no usable LLM config (checked tier aliases + generic LLM_*)")
            sys.exit(1)
        print("SELFTEST OK" if any_ok else "SELFTEST FAILED: no tier answered")
        sys.exit(0 if any_ok else 1)

    expensive = ExpensiveTier(min_interval_s=args.min_interval, retry_after_s=args.retry_after)

    if stub:
        cfgs = {}
    else:
        env = load_env_files()
        # CLI overrides (point a tier at a model/endpoint without editing .env, e.g. a keyless local
        # Ollama: --cheap-url http://host:11434/v1 --cheap-model qwen2.5:7b-instruct-q4_K_M).
        cfgs = {t: resolve_cfg(env, t, _overrides) for t in TIERS}
        cfgs = {t: c for t, c in cfgs.items() if c}
        for t in TIERS:
            c = cfgs.get(t)
            if c:
                log("INFO", f"tier {t}: url<-{c['url_from']}, key<-{c['key_from']}, "
                            f"model<-{c['model_from']} (model='{c['model']}')")
            else:
                log("WARN", f"tier {t}: not configured (disabled)")
        if cfgs.get("expensive") and cfgs.get("cheap") and cfgs["expensive"]["url"] == cfgs["cheap"]["url"] \
           and cfgs["expensive"]["model"] == cfgs["cheap"]["model"]:
            log("WARN", "expensive and cheap resolve to the SAME endpoint+model (only LLM_* set?). The "
                        "frequent cheap calls will hit the cloud rate limit; set LLM_CHEAP_* to a local model.")
        if not cfgs:
            log("WARN", "no tier configured; every request leaves no response (C++ plays its heuristic "
                        "floor). Use --stub for offline testing.")

    serve(cfgs, expensive, args.spool, args.once, args.poll, stub)


if __name__ == "__main__":
    main()
