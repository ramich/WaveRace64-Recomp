#!/usr/bin/env python3
"""Bisect RDRAM to find the game's view-culling bounds.

Repeatedly runs the game with the native poke harness widening a subset of
view-rect-shaped candidates (see rt64_render_context.cpp), scores the
framebuffer margins, and narrows the subset. Requires the scissor rewrite
active (WR64_BORDERS=0) so margins are visible.

Usage: python scripts/bisect_culling.py [--runs-budget N]
"""
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "WaveRace64Recomp.exe")
CAND = os.path.join(REPO, "poke_candidates.txt")
FB = os.path.join(REPO, "fb_dump.bin")

RUN_SECONDS = 25  # enough for the ~10s fb dump plus margin


def run_game(extra_env, seconds=RUN_SECONDS):
    env = os.environ.copy()
    env["WR64_BORDERS"] = "0"   # scissor rewrite ON so margins are visible
    env["WR64_FB_DUMP"] = "1"
    env.update(extra_env)
    if os.path.exists(FB):
        os.remove(FB)
    proc = subprocess.Popen(
        [EXE], cwd=REPO,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
    )
    time.sleep(seconds)
    crashed = proc.poll() is not None
    if not crashed:
        proc.kill()
        proc.wait()
    return crashed


def score():
    if not os.path.exists(FB):
        return None
    out = subprocess.check_output(
        [sys.executable, os.path.join(REPO, "scripts", "score_margins.py"), FB],
        cwd=REPO, text=True,
    )
    return float(out.split()[0])


def main():
    scan_mode = "2"  # 1 = s16 view-rect pairs, 2 = f32 clip-plane quads
    for arg in sys.argv[1:]:
        if arg.startswith("--mode="):
            scan_mode = arg.split("=")[1]

    # Phase 1: scan for candidates (always fresh — heap addresses shift).
    if os.path.exists(CAND):
        os.remove(CAND)
    print(f"== scan run (mode {scan_mode}) ==", flush=True)
    run_game({"WR64_POKE_SCAN": scan_mode}, seconds=15)
    if not os.path.exists(CAND):
        print("scan produced no candidate file; aborting")
        return 1
    n = len(open(CAND).read().strip().splitlines())
    print(f"{n} candidates", flush=True)

    # Phase 2: baseline and all-poked scores.
    print("== baseline run (no pokes) ==", flush=True)
    crashed = run_game({})
    base = score()
    print(f"baseline score: {base} crashed={crashed}", flush=True)
    if base is None:
        print("no baseline framebuffer; aborting")
        return 1

    def try_range(lo, hi):
        crashed = run_game({"WR64_POKE_FILE": CAND, "WR64_POKE_RANGE": f"{lo}:{hi}"})
        s = score()
        print(f"  range [{lo},{hi}): score={s} crashed={crashed}", flush=True)
        return s if s is not None else -1e9

    print("== all-poked run ==", flush=True)
    s_all = try_range(0, n)
    threshold = base + max(8.0, abs(base) * 0.15)
    if s_all < threshold:
        print(f"poking everything didn't clear threshold {threshold:.2f} — "
              "culling vars may not match the scan patterns. Aborting.")
        return 1

    # Phase 3: bisect to a minimal winning range.
    lo, hi = 0, n
    while hi - lo > 2:
        mid = (lo + hi) // 2
        s_left = try_range(lo, mid)
        if s_left >= threshold:
            hi = mid
            continue
        s_right = try_range(mid, hi)
        if s_right >= threshold:
            lo = mid
            continue
        # Neither half alone wins: multiple variables split across halves.
        # Keep the full range but shave edges to converge slowly.
        print("  neither half wins alone — required vars span the midpoint")
        break

    print(f"== winning range [{lo},{hi}) ==")
    lines = open(CAND).read().strip().splitlines()
    for i in range(lo, min(hi, n)):
        print(f"  cand[{i}]: {lines[i]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
