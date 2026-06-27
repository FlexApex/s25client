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
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

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
    """Resolve url/key/model for a tier: tier-specific alias first, then generic LLM_* fallback."""
    prefix = TIER_PREFIX.get(tier)
    if prefix:
        val, name = _lookup(env, [prefix + kind.upper()])
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
  "rationale": one-line in-character reasoning / taunt (<=160 chars)
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
  "diagnosis": one-line situation read (<=120 chars)
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
    Unknown enum value -> drop the key (the C++ side then keeps its prior value). Returns key=value text.
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

    # Free text: rationale/diagnosis both map onto chat (D7); strategyName gets its own line.
    name = plan.get("strategyName")
    if isinstance(name, str) and name.strip():
        lines.append("strategyName=" + _clean_text(name, 48))
    chat = plan.get("rationale") if kind == "plan" else plan.get("diagnosis")
    if chat is None:
        chat = plan.get("chat")
    if isinstance(chat, str) and chat.strip():
        lines.append("chat=" + _clean_text(chat, 160))

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

    return "\n".join(lines) + "\n"


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
        plan["rationale"] = (f"[stub] {plan['phase']}; needIron={need_iron} needStone={need_stone} "
                             f"target={primary}; mil {my_mil} vs {enemy_mil}.")
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
        plan["diagnosis"] = f"[stub] tick {minutes}m, mil {my_mil} vs {enemy_mil}, contained={contained}."

    return plan_to_kv(plan, current, kind)


# ---- expensive-tier budget --------------------------------------------------

class ExpensiveBudget:
    """Global (single-threaded sidecar) rate limiter for the expensive tier. monotonic-clock based."""

    def __init__(self, min_interval_s=8.0, hard_cap=0):
        self.min_interval_s = float(min_interval_s)
        self.hard_cap = int(hard_cap)  # 0 = no hard cap
        self._last = None
        self._count = 0

    def allowed(self):
        if self.hard_cap and self._count >= self.hard_cap:
            return False
        if self._last is None:
            return True
        return (time.monotonic() - self._last) >= self.min_interval_s

    def record(self):
        """Record a SUCCESSFUL expensive call (call only after the model actually answered)."""
        self._last = time.monotonic()
        self._count += 1


# ---- request handling -------------------------------------------------------

def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(tmp, path)


def _route_chain(req_tier, cfgs, budget):
    """Return the ordered list of (tier, cfg) to try. Cheap never escalates; expensive may fall back."""
    chain = []
    if req_tier == "expensive":
        if cfgs.get("expensive") and budget.allowed():
            chain.append(("expensive", cfgs["expensive"]))
        if cfgs.get("cheap"):  # degrade expensive -> cheap so the model still answers
            chain.append(("cheap", cfgs["cheap"]))
    else:
        if cfgs.get("cheap"):
            chain.append(("cheap", cfgs["cheap"]))
    return chain


def handle_request(req_path, cache, cfgs, budget, prompt_cache, stub):
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

    # Cache by the situational fields (drop volatile gf/minutes) so identical positions reuse a
    # decision; key includes tier+kind so plan/tick and cheap/expensive don't collide.
    sit = {k: v for k, v in snapshot.items() if k not in ("gf", "minutes")}
    cache_key = ":".join((
        req_tier, kind,
        hashlib.sha1(json.dumps(sit, sort_keys=True).encode()).hexdigest(),
    ))
    if cache_key in cache:
        write_atomic(resp_path, cache[cache_key])
        return

    # Stub mode: deterministic canned plan, no cfg/network. Runs before any routing/budget.
    if stub:
        kv = stub_plan(snapshot, kind, current)
        cache[cache_key] = kv
        write_atomic(resp_path, kv)
        sys.stderr.write(f"[sidecar] STUB {stem} ({req_tier}/{kind}): {kv.splitlines()[0] if kv.strip() else '(empty)'}\n")
        return

    chain = _route_chain(req_tier, cfgs, budget)
    if not chain:
        # No usable tier (config incomplete / budget exhausted): leave no response -> C++ heuristic.
        return

    system_prompt = get_system_prompt(snapshot, kind, prompt_cache)
    for used_tier, cfg in chain:
        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": json.dumps(snapshot)},
        ]
        try:
            content = call_llm(cfg, messages)
            plan = extract_json(content) or {}
        except (urllib.error.URLError, urllib.error.HTTPError, KeyError, ValueError, TimeoutError) as e:
            sys.stderr.write(f"[sidecar] {used_tier} call failed for {base}: {type(e).__name__}\n")
            continue  # fall back to the next tier in the chain
        if used_tier == "expensive":
            budget.record()
        kv = plan_to_kv(plan, current, kind)
        cache[cache_key] = kv
        write_atomic(resp_path, kv)
        first = kv.splitlines()[0] if kv.strip() else "(empty)"
        sys.stderr.write(f"[sidecar] {stem} ({used_tier}/{kind}): {first} ... ({len(plan)} keys)\n")
        return
    # Every tier failed: leave no response; C++ keeps playing on its heuristic and retries next tick.


def serve(cfgs, budget, spool, once, poll, stub):
    cache = {}
    prompt_cache = {}
    os.makedirs(spool, exist_ok=True)
    if stub:
        sys.stderr.write(f"[sidecar] serving {spool} in STUB mode (no network)\n")
    else:
        have = [t for t in TIERS if cfgs.get(t)]
        sys.stderr.write(f"[sidecar] serving {spool}; tiers available: {have or 'NONE (heuristic floor only)'}\n")
    while True:
        for req in sorted(glob.glob(os.path.join(spool, "req_*.json"))):
            handle_request(req, cache, cfgs, budget, prompt_cache, stub)
        if once:
            return
        time.sleep(poll)


def main():
    ap = argparse.ArgumentParser(description="LLM sidecar for the RttR llm AI")
    ap.add_argument("--spool", default=os.environ.get("RTTR_LLM_SPOOL", "/tmp/rttr_llm"))
    ap.add_argument("--poll", type=float, default=0.25, help="seconds between directory scans")
    ap.add_argument("--once", action="store_true", help="process current files once then exit")
    ap.add_argument("--selftest", action="store_true", help="make one tiny call and report")
    ap.add_argument("--stub", action="store_true", help="serve deterministic canned plans, no network")
    ap.add_argument("--min-interval", type=float, default=8.0,
                    help="min seconds between expensive-tier calls (global)")
    ap.add_argument("--hard-cap", type=int, default=0, help="max expensive-tier calls (0 = unlimited)")
    args = ap.parse_args()

    stub = args.stub or os.environ.get("LLM_STUB") in ("1", "true", "yes")

    if args.selftest:
        if stub:
            print("SELFTEST OK — stub mode (no network)")
            return
        env = load_env_files()
        cfg = build_cfg(env, "cheap", require_key=True) or build_cfg(env, "expensive", require_key=True)
        if cfg is None:
            print("SELFTEST FAILED: no usable LLM config (checked tier aliases + generic LLM_*)")
            sys.exit(1)
        try:
            reply = call_llm(cfg, [{"role": "user", "content": "Reply with exactly: OK"}])
            print("SELFTEST OK — model replied:", reply.strip()[:60])
        except Exception as e:  # noqa: BLE001 - report any failure plainly
            print("SELFTEST FAILED:", type(e).__name__, str(e)[:200])
            sys.exit(1)
        return

    budget = ExpensiveBudget(min_interval_s=args.min_interval, hard_cap=args.hard_cap)

    if stub:
        cfgs = {}
    else:
        env = load_env_files()
        cfgs = {t: build_cfg(env, t) for t in TIERS}
        cfgs = {t: c for t, c in cfgs.items() if c}
        for t in TIERS:
            c = cfgs.get(t)
            if c:
                sys.stderr.write(
                    f"[sidecar] {t}: url<-{c['url_from']}, key<-{c['key_from']}, "
                    f"model<-{c['model_from']} (model='{c['model']}')\n")
            else:
                sys.stderr.write(f"[sidecar] {t}: not configured (disabled)\n")
        if not cfgs:
            sys.stderr.write("[sidecar] WARNING: no tier configured; every request leaves no response "
                             "(the C++ AI plays its heuristic floor). Use --stub for offline testing.\n")

    serve(cfgs, budget, args.spool, args.once, args.poll, stub)


if __name__ == "__main__":
    main()
