"""Sweep the solver's knobs and keep only the runs that are DRC-perfect.

Each point runs the whole chain the way a user would - solve, write the board
with the KiCad shim, ask KiCad to check it - and is kept only if KiCad reports
no courtyard overlap, no short and no mask bridge. Everything is written to the
CSV, rejections included: knowing which knob breaks the DRC is half the answer.

    uv run python scripts/tune_weights.py --scene /tmp/r10.bin --board r10.kicad_pcb \
        --extract r10/.dodplace/extract.json --pass 1 --jobs 4
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PYTHON_DIR = ROOT / "python"

# The three violations that invalidate a run outright.
GATES = ("courtyards_overlap", "shorting_items", "solder_mask_bridge")

BENCH = re.compile(r"hpwl=([\d.]+).*?overlaps_movable=(\d+).*?seconds=([\d.]+)")


def run_point(point: dict, cfg: dict) -> dict:
    """One point: solve, apply, DRC. Returns the row to record."""
    row = {**point, "valid": False, "courtyards": -1, "shorts": -1, "bridges": -1,
           "hpwl": float("nan"), "engine_overlaps": -1, "seconds": float("nan"),
           "note": ""}
    start = time.time()
    work = Path(tempfile.mkdtemp(prefix="dodplace-tune-"))
    try:
        placement = work / "p.bin"
        board_out = work / "p.kicad_pcb"
        # The DRC reads the project's rules from beside the board: without them
        # it falls back to defaults and the gate means nothing.
        for name in ("kicad_pro", "kicad_dru", "fp-lib-table"):
            src = cfg["board"].parent / (cfg["board"].stem + "." + name) \
                if name != "fp-lib-table" else cfg["board"].parent / name
            if src.is_file():
                dst = board_out.with_suffix("." + name)
                shutil.copyfile(src, dst)

        command = [cfg["solver"], cfg["scene"], "--solve", "--bench", "-o", str(placement)]
        for key, value in point.items():
            if key.startswith("--"):
                command += [key, str(value)]
        solved = subprocess.run(command, capture_output=True, text=True, timeout=600)
        bench = BENCH.search(solved.stdout + solved.stderr)
        if bench is None:
            row["note"] = "the solver produced nothing"
            return row
        row["hpwl"] = float(bench.group(1))
        row["engine_overlaps"] = int(bench.group(2))
        row["seconds"] = float(bench.group(3))

        applied = subprocess.run(
            ["uv", "run", "dodplace", "apply", str(cfg["board"]), "--scene", cfg["scene"],
             "--placement", str(placement), "--extract", str(cfg["extract"]),
             "-o", str(board_out)],
            capture_output=True, text=True, timeout=900, cwd=PYTHON_DIR)
        if not board_out.is_file():
            row["note"] = "the shim failed: " + applied.stderr.strip().splitlines()[-1:][0] \
                if applied.stderr.strip() else "the shim failed"
            return row

        drc_txt = work / "drc.txt"
        subprocess.run(["kicad-cli", "pcb", "drc", "--exit-code-violations",
                        "-o", str(drc_txt), str(board_out)],
                       capture_output=True, text=True, timeout=900)
        report = drc_txt.read_text(errors="replace") if drc_txt.is_file() else ""
        for gate in GATES:
            row[gate.replace("courtyards_overlap", "courtyards")
                       .replace("shorting_items", "shorts")
                       .replace("solder_mask_bridge", "bridges")] = \
                report.count("[" + gate + "]")
        row["valid"] = all(row[k] == 0 for k in ("courtyards", "shorts", "bridges"))
        return row
    except subprocess.TimeoutExpired:
        row["note"] = "timeout"
        return row
    finally:
        row["wall_seconds"] = round(time.time() - start, 1)
        shutil.rmtree(work, ignore_errors=True)


SEEDS = (1, 42, 1337)


def points_for(pass_number: int) -> list[dict]:
    """The grid, one entry per (configuration, seed)."""
    if pass_number == 1:
        grid = [(0.005, 1e-3), (0.010, 1e-3), (0.020, 1e-3), (0.050, 1e-3),
                (0.010, 1e-4), (0.010, 5e-3), (0.005, 1e-4), (0.050, 5e-3)]
        return [{"--anneal-t-start-ratio": a, "--anneal-t-end-ratio": b, "seed": 1}
                for a, b in grid]
    # Pass 2: the knobs that showed sensitivity, on three seeds, because a
    # single lucky exploration must not be mistaken for a better optimum.
    points = []
    for moves in (4000, 8000, 16000):
        for clearance in (0.46, 0.48, 0.50):
            for crossings in (5.0, 2.5):
                cfg_id = f"m{moves}-c{clearance:.2f}-x{crossings:.1f}"
                for seed in SEEDS:
                    points.append({
                        "--moves": moves,
                        "--pad-clearance": clearance,
                        "--w-crossings": crossings,
                        "seed": seed,
                        "config": cfg_id,
                    })
    return points


REQUIRED_SIDECARS = ("extra.toml", "dodplace-parts.csv")
"""The reference board's enrichment: the physical rules and the part catalogue.

Without them a scene still loads - and quietly means something else: no masses,
no ceiling, none of the thermal rules, and a third of the decoupling pairs. A
number measured on that scene is not the board's number, so the harness refuses
to run rather than report one.
"""

FORBIDDEN_DEGRADED = ("mass-unknown", "ceiling-unknown", "no-thermal-data")


def require_enriched(cfg: dict) -> str | None:
    """None when the harness can certify, else why it cannot."""
    board = Path(cfg["board"])
    missing = [name for name in REQUIRED_SIDECARS if not (board.parent / name).is_file()]
    if missing:
        return (f"the reference harness needs the board's enrichment beside it: "
                f"{', '.join(missing)} missing in {board.parent}")
    probe = subprocess.run([cfg["solver"], cfg["scene"], "--identity", "--quiet",
                            "-o", os.devnull], capture_output=True, text=True)
    text = probe.stdout + probe.stderr
    if probe.returncode != 0:
        return f"the scene did not load: {text.strip().splitlines()[-1] if text else 'no output'}"
    bad = [name for name in FORBIDDEN_DEGRADED if name in text]
    if bad:
        return (f"the scene was built without that enrichment ({', '.join(bad)} in its "
                f"report): re-run `dodplace ingest` with the files beside the board")
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scene", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--extract", required=True)
    parser.add_argument("--solver", default=str(ROOT / "build/gcc-release/dodplace-solve"))
    parser.add_argument("--pass", dest="pass_number", type=int, default=1)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()

    cfg = {"scene": args.scene, "board": Path(args.board), "extract": args.extract,
           "solver": args.solver}
    refused = require_enriched(cfg)
    if refused is not None:
        print(f"error: {refused}", file=sys.stderr)
        return 2
    print(f"harness: {cfg['board'].name} is enriched (rules, catalogue, no degraded "
          f"mass/ceiling/thermal)")
    points = points_for(args.pass_number)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = ROOT / "scripts" / f"tune_{stamp}.csv"
    rows: list[dict] = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for row in pool.map(run_point, points, [cfg] * len(points)):
            rows.append(row)
            mark = "VALID " if row["valid"] else "rejet "
            label = str(row.get("config") or
                        f"t0={row.get('--anneal-t-start-ratio')}/{row.get('--anneal-t-end-ratio')}")
            print(f"{mark} {label:22s} graine {row.get('seed')} hpwl={row['hpwl']:8.1f} "
                  f"drc={row['courtyards']}/{row['shorts']}/{row['bridges']} "
                  f"({row['wall_seconds']}s) {row['note']}")

    columns = sorted({k for row in rows for k in row})
    with open(out, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)

    print(f"\n{sum(1 for r in rows if r['valid'])}/{len(rows)} runs conformes -> {out}")

    # A configuration counts only when EVERY seed is DRC-perfect.
    groups: dict[str, list[dict]] = {}
    for row in rows:
        groups.setdefault(str(row.get("config", "pass1")), []).append(row)
    scored = []
    for cfg_id, runs in groups.items():
        ok = [r for r in runs if r["valid"]]
        if len(ok) == len(runs):
            mean = sum(r["hpwl"] for r in ok) / len(ok)
            spread = (max(r["hpwl"] for r in ok) - min(r["hpwl"] for r in ok)) if len(ok) > 1 else 0.0
            scored.append((mean, spread, cfg_id, len(ok)))
    scored.sort()
    print("")
    if scored:
        for mean, spread, cfg_id, n in scored[:3]:
            print(f"CONFORME {cfg_id:22s} hpwl moyen {mean:8.1f} mm (etendue {spread:.1f}, {n} graines)")
        print(f"cible 33 000 mm : {'atteinte' if scored[0][0] <= 33000 else 'non atteinte'}")
    else:
        print("aucune configuration conforme sur toutes les graines")

    # The closest failure: fewest violations, then the best wirelength.
    near = sorted((r for r in rows if not r["valid"]),
                  key=lambda r: (r["courtyards"] + r["shorts"] + r["bridges"], r["hpwl"]))
    if near:
        r = near[0]
        print(f"au bord du seuil : {r.get('config')} graine {r.get('seed')} "
              f"hpwl {r['hpwl']:.1f} mm, DRC {r['courtyards']}/{r['shorts']}/{r['bridges']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
