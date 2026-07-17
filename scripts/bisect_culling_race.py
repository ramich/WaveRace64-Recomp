#!/usr/bin/env python3
"""Bisect RDRAM for the wave-grid/culling bounds using a REAL race.

Same idea as bisect_culling.py (scan for view-rect-shaped candidates, widen
subsets via the native poke harness, score framebuffer margins, bisect) but
each probe drives an actual Sunny Beach Time Trials race via
scripts/drive_to_race.ps1 instead of idling in the attract loop — needed
because the wave-mesh coverage boundary (the remaining ultrawide artifact,
see docs/RE-NOTES.md "Remaining empty margins") is a live-race behavior.

Scoring uses fb_dump2.bin (~30s into the process = mid-race with the
default settle/menu timing plus AccelerateSeconds >= 20).

Usage: python scripts/bisect_culling_race.py [--mode=1]
"""
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAND = os.path.join(REPO, "poke_candidates.txt")
FB = os.path.join(REPO, "fb_dump2.bin")
DRIVE = os.path.join(REPO, "scripts", "drive_to_race.ps1")


def run_race(extra_env, tag):
    env = os.environ.copy()
    env["WR64_BORDERS"] = "0"
    env["WR64_FB_DUMP"] = "1"
    env.update(extra_env)
    if os.path.exists(FB):
        os.remove(FB)
    out_dir = os.path.join(REPO, f"drive_bisect_{tag}")
    subprocess.call([
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", DRIVE,
        "-AccelerateSeconds", "22", "-OutDir", out_dir,
    ], cwd=REPO, env=env, stdout=subprocess.DEVNULL)
    time.sleep(3)


def score():
    if not os.path.exists(FB):
        return None
    out = subprocess.check_output(
        [sys.executable, os.path.join(REPO, "scripts", "score_margins.py"), FB],
        cwd=REPO, text=True,
    )
    return float(out.split()[0])


def main():
    scan_mode = "1"  # s16 view-rect pairs by default
    for arg in sys.argv[1:]:
        if arg.startswith("--mode="):
            scan_mode = arg.split("=")[1]

    # Phase 1: scan DURING a race (WR64_POKE_SCAN rescans every 600 updates;
    # the last scan before exit wins, which lands mid-race).
    if os.path.exists(CAND):
        os.remove(CAND)
    print(f"== scan run (mode {scan_mode}, in-race) ==", flush=True)
    run_race({"WR64_POKE_SCAN": scan_mode}, "scan")
    if not os.path.exists(CAND):
        print("scan produced no candidate file; aborting")
        return 1
    n = len(open(CAND).read().strip().splitlines())
    print(f"{n} candidates", flush=True)

    # Phase 2: baseline and all-poked scores.
    print("== baseline race (no pokes) ==", flush=True)
    run_race({}, "base")
    base = score()
    print(f"baseline score: {base}", flush=True)
    if base is None:
        print("no baseline framebuffer; aborting")
        return 1

    runs = 0

    def try_range(lo, hi):
        nonlocal runs
        runs += 1
        run_race({"WR64_POKE_FILE": CAND, "WR64_POKE_RANGE": f"{lo}:{hi}"},
                 f"r{runs}")
        s = score()
        print(f"  range [{lo},{hi}): score={s}", flush=True)
        return s if s is not None else -1e9

    print("== all-poked race ==", flush=True)
    s_all = try_range(0, n)
    threshold = base + max(8.0, abs(base) * 0.15)
    if s_all < threshold:
        print(f"poking everything didn't clear threshold {threshold:.2f} — "
              "the wave-grid bounds may not match this scan pattern. Aborting.")
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
        print("  neither half wins alone — required vars span the midpoint")
        break

    print(f"== winning range [{lo},{hi}) ==")
    lines = open(CAND).read().strip().splitlines()
    for i in range(lo, min(hi, n)):
        print(f"  cand[{i}]: {lines[i]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
