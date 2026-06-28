/goal # Goal: a human-like, LLM-driven AI for Return To The Roots

## North star
A computer opponent for *The Settlers II* (RttR) whose **strategy and execution feel human** —
thoughtful, adaptive, structured, and different from game to game — with a language model genuinely
making the decisions from a real understanding of the situation. It should be a credible, interesting
challenge across long games and entirely its own player.

## What we want

### Play style
- **Genuinely LLM-driven** strategy: the model makes the real decisions, not a thin wrapper over fixed rules.
- **Human-like and ingenious**: unconventional build orders, timing pushes, feints, economic gambits,
  map-specific plans, and adaptive reactions to what the opponent actually does.
- **Varied between games** — different games should look different; there is no single "optimal build" we want.
- **Free of the old AI**: it need not, and should not be constrained to, resemble the existing AIJH
  heuristic AI in any way.

### Understanding
- Reason from the **full picture** — economy, military, opponents, and the **relevant parts of the map /
  terrain** — not just aggregate counts.
- Have a good understanding of the game, including recent addons like wine ecnomy and leather economy.
  This understanding is given to the LLM and is dynamically assembled so it matches the game's activated addons & settings.
  Your aim is to build a great prompt that gets the best game-understanding and gameplay out of the LLM, most efficiently.
- **Diagnose and solve its own strategic problems** the way a human would (e.g. "stuck without iron",
  "boxed in", "being out-expanded", "under pressure on one front") and take concrete corrective action.

### Execution & building placement
- **Human-like, structured execution**: organized, sensible building placement and road / territory
  layout — no haphazard or self-conflicting placements (e.g. farms crammed among foresters).
- Placement should express real **spatial intent** (districts, frontiers, supply lines), not greedy
  local guesses.

### Competitiveness — the definition of done
- Evaluated **against AIJH** and able to **compete at the Hard level**.
- Only considered done when it is genuinely competitive with AIJH-Hard across a **variety of maps** —
  specifically **FL-Macro2.SWD, Macro144.SWD, and AtomicFL.SWD** — judged over multiple seeds and
  both start orientations (single games are noise).
- **No plateau**: keeps developing across a full ~4-hour game and does not stall or get out-macroed
  late (the old AI's main failing — it was also too oppressive early).
- Evaluated under the real playing ruleset: **2v2, Hard, inexhaustible mines, gold→granite** (so
  military strength = soldier count).

### Resource use
- **Frugal, sustainable LLM cost**: keep the rate-limited cloud model (≈40 rpm cap) usage **well
  below** the limit and at a rate sustainable for an entire long game — assume the cloud model cannot
  be called heavily or continuously.
- **Use both compute tiers well**: a **local GPU (RTX 2060 Super, 8 GB)** can run a suitable local
  model for the frequent / cheaper thinking, with the stronger cloud model reserved for rare, hard,
  high-value decisions.

### Robustness & fit
- Works in the user's **real games** (local / single-player 2v2 in the GUI on large macro maps) and
  is replay-safe.
- **Degrades gracefully** when the model is slow, rate-limited, or unavailable — the AI keeps playing
  competently — and surfaces a clear message on persistent failure so the player can pause, save, fix
  the setup, and resume.

## Done when
The AI plays a recognizably **human-like, varied** game and **holds its own against AIJH-Hard** on
FL-Macro2, Macro144, and AtomicFL — **growing steadily across long games without plateauing** — while
keeping **cloud-LLM usage light and sustainable** (leaning on the local model), and remaining robust
enough to use in real GUI matches.
