#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DCQCN Bayesian/Random Tuning Runner

- Launches the msft_ai_wan_single_dcqcn simulator with a given connection matrix
- Passes DCQCN "direct knobs" via environment variables
- Parses outputs to:
    * Plot sending-rate fairness (per flow) from statistics_*.txt
    * Build a Flow Summary from trace_packets.csv (dedup by (flow, seq))
      including ECN%, CNP-sent/rcvd (from cnp_events.csv), per-flow FCT,
      average FCT, and a "score" = 1000 / AvgFCT(ms)

Layout tweaks per your request:
- CSVs for *every trial* are written in the main sim folder (alongside output_*/statistics_*)
- Plots for *every trial* go under <folder>/option_plots/
- The best trial’s plot is also copied to <folder>/option_plots/fairness_best.png
- The best trial’s CSV is also copied to <folder>/dcqcn_flow_summary_best.csv

Usage:
  python3 sim/msft_ai/scripts/dcqcn_bayes_tune.py --run-sim one_one_4_200MB.cm --max-evals 5
"""

import os
import re
import sys
import csv
import json
import random
import argparse
import subprocess
import shutil
from collections import defaultdict

import matplotlib.pyplot as plt


# ============================================================
# Path helpers
# ============================================================

def stat_path_from_sim_out(sim_out_path: str) -> str:
    """Convert an output_*.txt path to its paired statistics_*.txt path."""
    base = os.path.basename(sim_out_path)
    return os.path.join(os.path.dirname(sim_out_path),
                        base.replace("output_", "statistics_"))

def plot_path_for_trial(stat_path: str, tag: str) -> str:
    """
    Build a per-trial plot path under <folder>/option_plots/.
    Example: fairness_one_one_4_200MB_linkdown0_...__trial_0.png
    """
    folder = os.path.dirname(stat_path)
    plots_dir = os.path.join(folder, "option_plots")
    os.makedirs(plots_dir, exist_ok=True)
    base = os.path.basename(stat_path)  # statistics_one_one_4_200MB_linkdown0_...
    name = base.replace("statistics_", "").replace(".txt", "")
    return os.path.join(plots_dir, f"fairness_{name}__{tag}.png")

def summary_csv_for_trial(stat_path: str, tag: str) -> str:
    """
    Build a per-trial flow-summary CSV path in the main sim folder (NOT option_plots).
    Example: dcqcn_flow_summary_trial_0.csv
    """
    folder = os.path.dirname(stat_path)
    return os.path.join(folder, f"dcqcn_flow_summary_{tag}.csv")

def copy_as_best(plot_path: str, csv_path: str, stat_path: str):
    """
    Copy best trial artifacts to standard names:
      - plot  -> <folder>/option_plots/fairness_best.png
      - CSV   -> <folder>/dcqcn_flow_summary_best.csv
    """
    folder = os.path.dirname(stat_path)
    plots_dir = os.path.join(folder, "option_plots")
    os.makedirs(plots_dir, exist_ok=True)
    shutil.copyfile(plot_path, os.path.join(plots_dir, "fairness_best.png"))
    shutil.copyfile(csv_path,  os.path.join(folder, "dcqcn_flow_summary_best.csv"))


# ============================================================
# Fairness plot (parse statistics_*.txt)
# ============================================================

def parse_sending_rates(stat_path: str):
    """
    Read statistics_*.txt and extract (time_us, sending_rate_bps) per flow.
    Returns dict: { flow_id_str -> [(t_us, rate_bps), ...] }.
    """
    sending_rate_dict = defaultdict(list)
    if not os.path.exists(stat_path):
        print(f"WARNING: statistics file not found: {stat_path}")
        return sending_rate_dict
    with open(stat_path, "r") as f:
        for raw in f:
            line = raw.strip()
            if not line or "Flow" not in line or "sending_rate" not in line:
                continue
            try:
                parts = line.split()
                flow_id = parts[1]
                t_us = float(parts[parts.index("time:")+1])
                r_bps = float(parts[parts.index("sending_rate:")+1])
                sending_rate_dict[flow_id].append((t_us, r_bps))
            except Exception:
                continue
    return sending_rate_dict

def _color_for(flow_id: str, palette):
    """Deterministic color selection based on flow id."""
    try:
        if flow_id.startswith("DCQCN"):
            idx = int(flow_id[5:])
        else:
            idx = int(flow_id) % 1000
    except Exception:
        idx = hash(flow_id) % 1000
    return palette[idx % len(palette)]

def draw_fairness_plot(sending_rate_dict, out_png: str, title: str):
    """
    Draw per-flow sending rates (Mbps) vs time (us).
    Saves to out_png.
    """
    if not sending_rate_dict:
        print("No sending-rate samples to plot.")
        return
    plt.rcParams.update({'font.size': 14})
    plt.figure(figsize=(28, 16))
    colors = ['skyblue','lightgreen','salmon','plum','lightcoral','lightgoldenrodyellow',
              'lightcyan','lavender','lightpink','lightseagreen','lightsalmon','lightsteelblue','lightyellow']
    for flow_id, samples in sorted(sending_rate_dict.items(), key=lambda kv: kv[0]):
        ts, rates = zip(*samples)
        mbps = [r / 1e6 for r in rates]
        plt.plot(ts, mbps, label=f"Flow {flow_id}", color=_color_for(flow_id, colors), linewidth=2)
    plt.title(title)
    plt.xlabel("Time (us)")
    plt.ylabel("Sending Rate (Mbps)")
    plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
    plt.tight_layout()
    plt.savefig(out_png)
    print(f"Plotting {out_png}")


# ============================================================
# Trace parsing (paths + ECN/CNP + FCT + score)
# ============================================================

def parse_paths(paths_file):
    """
    Parse <basename>.paths into { flow_id(int) -> human_readable_path }.
    Strips queue(...) and pipe(...) boilerplate for readability.
    """
    id_to_path = {}
    try:
        with open(paths_file, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or ':' not in line:
                    continue
                m = re.match(r'(\d+)\s*:\s*(.*)$', line)
                if not m:
                    continue
                fid = int(m.group(1))
                pth = m.group(2).strip()
                pth = re.sub(r'queue\([^)]+\)Queue--', '', pth)
                pth = pth.replace(' -> pipe(1us) -> ', ' -> ').replace('-> ->', '->')
                id_to_path[fid] = pth
    except FileNotFoundError:
        pass
    return id_to_path

def parse_trace_csv(trace_csv):
    """
    Read trace_packets.csv and build per-flow stats while deduplicating by (flow, seq):
      - unique packet count (pkts)
      - ECN-marked count
      - FCT_us measured between min(first_ts) and max(last_ts) across unique seqs

    Returns dict: { flow_id(int) -> {"pkts": int, "ecn": int, "fct_us": float|None} }.
    """
    per_flow = defaultdict(lambda: {
        "seen_seqs": set(),
        "seq_first_ts": {},
        "seq_last_ts": {},
        "ecn_marks": 0
    })
    try:
        with open(trace_csv, newline='') as f:
            reader = csv.DictReader(f)
            flow_key = 'flow' if 'flow' in reader.fieldnames else 'flow_id'
            ts_key   = 'ts_us'
            ecn_key  = 'ecn_marked' if 'ecn_marked' in reader.fieldnames else None
            seq_key  = 'seq'
            for row in reader:
                try:
                    fid = int(row[flow_key])
                    ts  = float(row[ts_key])
                    seq = int(row.get(seq_key, 0))
                except Exception:
                    continue
                d = per_flow[fid]
                if ecn_key and row.get(ecn_key, ''):
                    try:
                        if int(row[ecn_key]) == 1:
                            d["ecn_marks"] += 1
                    except:
                        pass
                if seq not in d["seen_seqs"]:
                    d["seen_seqs"].add(seq)
                    d["seq_first_ts"][seq] = ts
                    d["seq_last_ts"][seq]  = ts
                else:
                    d["seq_last_ts"][seq] = max(d["seq_last_ts"][seq], ts)
    except FileNotFoundError:
        print(f"WARNING: {trace_csv} not found; FCT and ECN% will be unavailable.", file=sys.stderr)
        return {}

    summary = {}
    for fid, d in per_flow.items():
        unique_pkts = len(d["seen_seqs"])
        ecn = d["ecn_marks"]
        if unique_pkts > 0:
            t_first = min(d["seq_first_ts"].values())
            t_last  = max(d["seq_last_ts"].values())
            fct_us  = max(0.0, t_last - t_first)
        else:
            fct_us = None
        summary[fid] = {"pkts": unique_pkts, "ecn": ecn, "fct_us": fct_us}
    return summary

def parse_cnp_events(cnp_csv_path="cnp_events.csv"):
    """
    Parse cnp_events.csv produced by DCQCN (if instrumented).
    Returns (sent_counts, rcvd_counts) as dicts: flow_id -> count.
    """
    sent = defaultdict(int)
    rcvd = defaultdict(int)
    if not os.path.exists(cnp_csv_path):
        return sent, rcvd
    try:
        with open(cnp_csv_path, newline="") as f:
            reader = csv.DictReader(f)
            if not {"flow", "event"}.issubset(reader.fieldnames or []):
                return sent, rcvd
            for row in reader:
                try:
                    fid = int(row["flow"])
                    ev  = row["event"].strip().upper()
                except Exception:
                    continue
                if ev == "SENT":
                    sent[fid] += 1
                elif ev in ("RCVD", "RECV", "RECEIVED"):
                    rcvd[fid] += 1
    except Exception:
        pass
    return sent, rcvd

def print_and_write_summary(trace_csv, paths_file, out_csv):
    """
    Build and print the Flow Summary table; also write a CSV (out_csv).
    Returns (score, avg_fct_ms).
    """
    id_to_path = parse_paths(paths_file)
    csv_flow   = parse_trace_csv(trace_csv)
    cnp_sent, cnp_rcvd = parse_cnp_events("cnp_events.csv")
    flow_ids   = sorted(csv_flow.keys())

    print("\nFlow Summary")
    print("------------")
    print("{:<22} {:>10} {:>10} {:>7} {:>10} {:>10}  {}  {:>10}".format(
        "Flow", "Pkts", "ECN", "ECN%", "CNP-sent", "CNP-rcvd", "Route", "FCT_us"))

    rows, fcts = [], []
    for fid in flow_ids:
        d = csv_flow[fid]
        pkts = d["pkts"]; ecn = d["ecn"]
        ecn_pct = (100.0 * ecn / pkts) if pkts > 0 else 0.0
        fct_us  = d.get("fct_us", None)
        route   = id_to_path.get(fid, "")
        rows.append({
            "Flow": fid, "Pkts": pkts, "ECN": ecn, "ECN_pct": ecn_pct,
            "CNP_sent": cnp_sent.get(fid, 0), "CNP_rcvd": cnp_rcvd.get(fid, 0),
            "Route": route, "FCT_us": fct_us
        })
        if fct_us is not None and pkts > 0:
            fcts.append(fct_us)

    for r in rows:
        print("{:<22} {:>10} {:>10} {:>7.2f} {:>10} {:>10}  {}  {:>10}".format(
            r["Flow"], r["Pkts"], r["ECN"], r["ECN_pct"],
            r["CNP_sent"], r["CNP_rcvd"], r["Route"],
            int(r["FCT_us"]) if r["FCT_us"] is not None else 0))

    avg_fct_ms = (sum(fcts) / len(fcts) / 1000.0) if fcts else 0.0
    score = (1000.0 / avg_fct_ms) if avg_fct_ms > 0 else 0.0
    print("\nAverage FCT (ms): {:.3f}".format(avg_fct_ms))
    print("Score (1000 / Avg FCT ms): {:.3f}".format(score))

    with open(out_csv, "w", newline='') as f:
        w = csv.writer(f)
        w.writerow(["Flow","Pkts","ECN","ECN_pct","CNP_sent","CNP_rcvd","Route","FCT_us"])
        for r in rows:
            w.writerow([r["Flow"], r["Pkts"], r["ECN"], "{:.2f}".format(r["ECN_pct"]),
                        r["CNP_sent"], r["CNP_rcvd"], r["Route"],
                        int(r["FCT_us"]) if r["FCT_us"] is not None else ""])
        w.writerow([])
        w.writerow(["Average_FCT_ms", "{:.3f}".format(avg_fct_ms)])
        w.writerow(["Score_1000_over_avgFCTms", "{:.3f}".format(score)])
    print(f"\nWrote {out_csv}")
    return score, avg_fct_ms


# ============================================================
# DCQCN knobs / sim runner
# ============================================================

DEFAULT_KNOBS_SPACE = {
    # Int/floats; ranges define search space (and types)
    "DCQCN_CNP_INTERVAL_US": (10, 200),       # int
    "DCQCN_EPOCH_US":        (2000, 30000),   # int
    "DCQCN_ALPHA_INIT":      (0.05, 1.0),     # float
    "DCQCN_G":               (0.001, 0.1),    # float
    "DCQCN_B_BYTES":         (8*1024*1024, 256*1024*1024),  # int
    "DCQCN_F_EPOCHS":        (2, 64),         # int
    "DCQCN_MD_CAP":          (0.05, 0.8),     # float
    "DCQCN_FLOOR_LINE_FRAC": (0.05, 0.9),     # float
    "DCQCN_FLOOR_RT_FRAC":   (0.5, 0.95),     # float
    "DCQCN_HI_COOLDOWN":     (0, 8),          # int
    "DCQCN_HI_CAP_DIV":      (64, 1024),      # int
}

def run_sim_with_knobs(matrix: str, knobs: dict, folder_name: str, binary_path: str,
                       drop_rate="0", link_down=0, use_jitter=0, exp=1):
    """
    Invoke the simulator for one connection matrix with environment-provided DCQCN knobs.
    Returns (out_file, stat_file).
    """
    os.makedirs(folder_name, exist_ok=True)

    conn_matrix = os.path.join("./scripts/msft_ai_connection_matrices", matrix)
    base_noext  = os.path.splitext(os.path.basename(conn_matrix))[0]
    suffix      = f"_linkdown{link_down}_droprate{drop_rate}_usejitter{use_jitter}_exp{exp}"

    out_file  = os.path.join(folder_name, f"output_{base_noext}{suffix}.txt")
    stat_file = os.path.join(folder_name, f"statistics_{base_noext}{suffix}.txt")

    cmd = [
        binary_path,
        "-o","uec_entry",
        "-switch_latency","0",
        "-collect_data","1",
        "-strat","ecmp_host",
        "-tm", conn_matrix,
        "-noFi","-noQaInter","-noQaIntra","-noRto",
        "-drop-rate", str(drop_rate),
        "-use-jitter", str(use_jitter),
        "-interQSize","4000000",
        "-intraQSize","200000000",
        "-is-link-down", str(link_down),
        "-statistics-filename", stat_file,
    ]

    # Pass knobs to the binary via env
    env = os.environ.copy()
    for k, v in knobs.items():
        env[str(k)] = str(v)

    print("Executing command:")
    print(" ".join(cmd))
    print(f"Statistics will be written to: {stat_file}")
    print("----------------------------------------")

    with open(out_file, "w") as fout:
        proc = subprocess.run(cmd, stdout=fout, stderr=subprocess.STDOUT, env=env)

    if proc.returncode != 0:
        print("Command failed (see output).")
    else:
        print("Command completed successfully")
    print(f"Output: {out_file}")
    print("----------------------------------------")

    return out_file, stat_file

def parse_paths_file_name_from_log():
    """Simulator convention: -o uec_entry -> writes uec_entry.paths in CWD."""
    return "uec_entry.paths"


# ============================================================
# Objective evaluation (one trial)
# ============================================================

def evaluate(matrix, trial_knobs, args, tag):
    """
    Run a single trial:
      1) Run the simulator with trial_knobs
      2) Plot fairness under option_plots/
      3) Build Flow Summary CSV in the main folder
      4) Print score line: [trial <tag>] score=... | avg_fct_ms=...
    Returns (score, avg_fct_ms, out_file, stat_file, trial_plot, trial_csv).
    """
    print(f"\n=== Trial {tag} knobs ===")
    for k in sorted(trial_knobs.keys()):
        print(f"  {k} = {trial_knobs[k]}")

    out_file, stat_file = run_sim_with_knobs(
        matrix=matrix, knobs=trial_knobs,
        folder_name=args.folder, binary_path=args.binary,
        drop_rate=args.drop_rate, link_down=args.link_down,
        use_jitter=args.use_jitter, exp=args.exp
    )

    # Per-trial fairness plot (in option_plots/)
    trial_plot = plot_path_for_trial(stat_file, tag)
    rates = parse_sending_rates(stat_file)
    title = f"{tag}: {os.path.basename(trial_plot)}"
    draw_fairness_plot(rates, trial_plot, title)

    # Per-trial Flow Summary CSV (in main folder)
    trial_csv = summary_csv_for_trial(stat_file, tag)
    paths_file = args.paths_file or parse_paths_file_name_from_log()
    score, avg_fct_ms = print_and_write_summary(args.trace_csv, paths_file, out_csv=trial_csv)

    print(f"[trial {tag}] score={score:.3f} | avg_fct_ms={avg_fct_ms:.3f}")
    return score, avg_fct_ms, out_file, stat_file, trial_plot, trial_csv


# ============================================================
# Search strategies
# ============================================================

def rand_point(space: dict) -> dict:
    """Sample one random point from the space (int ranges -> ints; float ranges -> floats)."""
    trial = {}
    for k, rng in space.items():
        lo, hi = rng
        if isinstance(lo, int) and isinstance(hi, int):
            trial[k] = random.randint(int(lo), int(hi))
        else:
            v = random.random() * (float(hi) - float(lo)) + float(lo)
            trial[k] = float(v)
    return trial

def get_optimizer(space):
    """
    Try to construct a scikit-optimize Optimizer; otherwise return a "random" sentinel.
    Returns (kind, optimizer_or_None, knob_names_list).
    """
    try:
        from skopt import Optimizer
        from skopt.space import Integer, Real
        dims = []
        names = []
        for k, (lo, hi) in space.items():
            names.append(k)
            if isinstance(lo, int) and isinstance(hi, int):
                dims.append(Integer(lo, hi, name=k))
            else:
                prior = "log-uniform" if (float(lo) > 0 and float(hi) > 0) else "uniform"
                dims.append(Real(float(lo), float(hi), prior=prior, name=k))
        opt = Optimizer(dimensions=dims, base_estimator="GP", acq_func="EI")
        return ("skopt", opt, names)
    except Exception:
        print("scikit-optimize not found; falling back to random search.")
        return ("random", None, list(space.keys()))


# ============================================================
# CLI / Main
# ============================================================

def main():
    ap = argparse.ArgumentParser(
        description="Bayesian (or random) search to tune DCQCN knobs; runs sim, plots, prints summary."
    )
    ap.add_argument("--run-sim", metavar="MATRIX", required=True,
                    help="Connection matrix filename, e.g. one_one_4_200MB.cm")
    ap.add_argument("--binary", default="./build/msft_ai_wan_single_dcqcn",
                    help="Path to simulator binary.")
    ap.add_argument("--folder", default="msft_ai_wan_single_dcqcn",
                    help="Output folder used by the binary.")
    ap.add_argument("--trace-csv", default="trace_packets.csv",
                    help="Path to trace_packets.csv.")
    ap.add_argument("--paths-file", default="uec_entry.paths",
                    help="Path to uec_entry.paths (if you want to override).")
    ap.add_argument("--drop-rate", default="0")
    ap.add_argument("--link-down", type=int, default=0)
    ap.add_argument("--use-jitter", type=int, default=0)
    ap.add_argument("--exp", type=int, default=1)
    ap.add_argument("--max-evals", type=int, default=10)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    random.seed(args.seed)

    search_kind, opt, names = get_optimizer(DEFAULT_KNOBS_SPACE)

    best_score   = -1.0
    best_summary = None  # (trial_idx, knobs, score, avg_fct_ms, stat_file, plot, csv)

    for i in range(args.max_evals):
        tag = f"trial_{i}"

        if search_kind == "skopt":
            x = opt.ask()
            trial_knobs = {names[j]: x[j] for j in range(len(names))}
        else:
            trial_knobs = rand_point(DEFAULT_KNOBS_SPACE)

        # Coerce types based on space definition (int ranges => int)
        coerced = {}
        for k, v in trial_knobs.items():
            lo, hi = DEFAULT_KNOBS_SPACE[k]
            coerced[k] = int(round(v)) if isinstance(lo, int) and isinstance(hi, int) else float(v)
        trial_knobs = coerced

        score, avg_fct_ms, out_file, stat_file, trial_plot, trial_csv = evaluate(
            args.run_sim, trial_knobs, args, tag
        )

        if search_kind == "skopt":
            # Maximize score by minimizing negative score
            opt.tell(list(trial_knobs.values()), -score)

        if score > best_score:
            best_score = score
            best_summary = (i, trial_knobs, score, avg_fct_ms, stat_file, trial_plot, trial_csv)
            copy_as_best(trial_plot, trial_csv, stat_file)

    print("\n==================== Best configuration ====================")
    if best_summary is None:
        print("No successful trials.")
        return
    i, knobs, score, avg_fct_ms, stat_file, trial_plot, trial_csv = best_summary
    print(f"trial #{i}: score={score:.3f} | avg_fct_ms={avg_fct_ms:.3f}")
    print(json.dumps(knobs, indent=2, sort_keys=True))
    print("Artifacts:")
    print(f"  Trial plots: {os.path.join(os.path.dirname(stat_file), 'option_plots')}/")
    print(f"  Trial CSVs:  {os.path.dirname(stat_file)}/dcqcn_flow_summary_trial_*.csv")
    print(f"  Best plot:   {os.path.join(os.path.dirname(stat_file), 'option_plots', 'fairness_best.png')}")
    print(f"  Best CSV:    {os.path.join(os.path.dirname(stat_file), 'dcqcn_flow_summary_best.csv')}")

if __name__ == "__main__":
    main()
