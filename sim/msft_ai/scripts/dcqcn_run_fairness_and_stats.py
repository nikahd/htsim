#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import sys
import csv
import argparse
import subprocess
from collections import defaultdict
from pathlib import Path
import matplotlib.pyplot as plt

# ============================================================
# DCQCN knob overrides (sent to the C++ binary via env vars)
# Leave any key out to use the binary's compiled default.
# ============================================================

DCQCN_KNOBS = {
    "DCQCN_CNP_INTERVAL_US": 173,                 # sink CNP pacing (us)
    "DCQCN_EPOCH_US": 5648,                       # control epoch (us)
    "DCQCN_ALPHA_INIT": 0.07376021746153359,      # initial alpha
    "DCQCN_G": 0.02822790251854281,               # alpha EWMA decay
    "DCQCN_B_BYTES": 68306285,                    # bytes per BC epoch
    "DCQCN_F_EPOCHS": 10,                         # HI eligibility epochs
    "DCQCN_MD_CAP": 0.6023534106230093,           # per-CNP MD cap
    "DCQCN_FLOOR_LINE_FRAC": 0.6251945643094746,  # floor >= frac*line
    "DCQCN_FLOOR_RT_FRAC": 0.9014808054671803,    # floor >= frac*prev RT
    "DCQCN_HI_COOLDOWN": 1,                       # epochs of AI-only after MD
    "DCQCN_HI_CAP_DIV": 668,                      # HI cap = link / DIV
}

# ============================================================
# Helpers: paths & plotting
# ============================================================

def find_matrix_path(matrix: str) -> str:
    """
    Accept absolute/relative paths, otherwise probe common repo locations.
    """
    cand = Path(matrix)
    if cand.exists():
        return str(cand.resolve())

    env_dir = os.environ.get("MSFT_AI_CM_DIR")
    candidates = []
    if env_dir:
        candidates.append(Path(env_dir) / matrix)

    candidates += [
        Path("./scripts/msft_ai_connection_matrices") / matrix,
        Path("sim/msft_ai/scripts/msft_ai_connection_matrices") / matrix,
        Path("msft_ai/scripts/msft_ai_connection_matrices") / matrix,
        Path("scripts") / "msft_ai_connection_matrices" / matrix,
    ]
    for p in candidates:
        if p.exists():
            return str(p.resolve())
    # fallback (for a nicer error message later)
    return str((Path("./scripts/msft_ai_connection_matrices") / matrix).resolve())

def plot_path_from_stat(stat_path: str) -> str:
    folder = os.path.dirname(stat_path)
    plots = os.path.join(folder, "option_plots")
    os.makedirs(plots, exist_ok=True)
    base = os.path.basename(stat_path)
    m = re.match(
        r"statistics_(.+?)_linkdown(\d+)_droprate([0-9a-zA-Z.]+)_usejitter(\d+)_exp(\d+)\.txt",
        base,
    )
    if not m:
        return os.path.join(plots, f"fairness_{base}.png")
    matrix, link, droprate, usejit, _exp = m.groups()
    return os.path.join(
        plots,
        f"fairness_{matrix}_link{link}_usejitter{usejit}_droprate{droprate}_initcwnd0.7.png",
    )

def parse_sending_rates(stat_path: str):
    """
    Read statistics_*.txt and extract (time_us, sending_rate_bps) per flow.
    Returns dict: { flow_id_str -> [(t_us, rate_bps), ...] }.
    """
    sending_rate_dict = defaultdict(list)
    if not os.path.exists(stat_path):
        print(f"WARNING: statistics file not found: {stat_path}")
        return sending_rate_dict
    print("sim_file_name:", stat_path)
    print(f"Processing {stat_path}...")
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
    try:
        if flow_id.startswith("DCQCN"):
            idx = int(flow_id[5:])
        else:
            idx = int(flow_id) % 1000
    except Exception:
        idx = hash(flow_id) % 1000
    return palette[idx % len(palette)]

def draw_fairness_plot(sending_rate_dict, out_png: str, title: str):
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
# Trace analysis (paths + ECN/CNP + FCT + score)
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
    De-duplicate by (flow, seq) using the real sequence numbers now logged by the sink.
    FCT per flow = max(last_ts) - min(first_ts) among unique seqs.
    Returns { flow_id(int) -> {"pkts": int, "ecn": int, "fct_us": float|None} }.
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
            if not reader.fieldnames:
                print(f"ERROR: empty or invalid CSV: {trace_csv}", file=sys.stderr)
                return {}
            flow_key = 'flow' if 'flow' in reader.fieldnames else ('flow_id' if 'flow_id' in reader.fieldnames else None)
            ts_key   = 'ts_us' if 'ts_us' in reader.fieldnames else None
            seq_key  = 'seq'   # our sink writes this
            ecn_key  = 'ecn_marked' if 'ecn_marked' in reader.fieldnames else None
            if not (flow_key and ts_key and seq_key):
                print("ERROR: trace_packets.csv must include ts_us, flow, and seq columns.", file=sys.stderr)
                return {}
            for row in reader:
                try:
                    fid = int(row[flow_key]); ts = float(row[ts_key]); seq = int(row.get(seq_key, 0))
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
    Parse cnp_events.csv if present.
    Returns (sent_counts, rcvd_counts) as dicts: flow_id -> count.
    """
    sent = defaultdict(int)
    rcvd = defaultdict(int)
    if not os.path.exists(cnp_csv_path):
        return sent, rcvd
    try:
        with open(cnp_csv_path, newline="") as f:
            reader = csv.DictReader(f)
            if not reader.fieldnames or "flow" not in reader.fieldnames or "event" not in reader.fieldnames:
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

def print_and_write_summary(trace_csv, paths_file):
    id_to_path = parse_paths(paths_file)
    csv_flow   = parse_trace_csv(trace_csv)
    flow_ids   = sorted(csv_flow.keys())
    cnp_sent, cnp_rcvd = parse_cnp_events("cnp_events.csv")

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

    out_csv = "dcqcn_flow_summary.csv"
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

# ============================================================
# Simulation runner (deterministic + stale CSV cleanup)
# ============================================================

def run_one_sim(matrix: str,
                folder_name="msft_ai_wan_single_dcqcn",
                binary_path="./build/msft_ai_wan_single_dcqcn",
                drop_rate="0",
                link_down=0,
                use_jitter=0,
                exp=1,
                seed=1):
    """
    Run the simulator once for `matrix`.
      - Deletes stale CSVs so analysis never reads leftovers.
      - Passes an explicit -seed to the binary for reproducibility.
    """
    os.makedirs(folder_name, exist_ok=True)

    # Remove stale CSVs produced by previous runs
    for f in ("trace_packets.csv", "cnp_events.csv", "fabric_breadcrumbs.csv"):
        try:
            os.remove(f)
        except FileNotFoundError:
            pass

    conn_matrix = find_matrix_path(matrix)
    if not Path(conn_matrix).exists():
        print(f"ERROR: matrix file not found: {conn_matrix}", file=sys.stderr)
        sys.exit(1)

    base_noext = os.path.splitext(os.path.basename(conn_matrix))[0]
    suffix = f"_linkdown{link_down}_droprate{drop_rate}_usejitter{use_jitter}_exp{exp}"

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
        "-seed", str(seed),   # <-- deterministic runs
    ]

    print("Executing command:")
    print(" ".join(cmd))
    print(f"Statistics will be written to: {stat_file}")
    print("----------------------------------------")

    # Prepare environment with DCQCN knobs
    env = os.environ.copy()
    for k, v in DCQCN_KNOBS.items():
        env[k] = str(v)

    # Echo applied knobs & seed for traceability
    print("Applied DCQCN_KNOBS:")
    for k in sorted(DCQCN_KNOBS):
        print(f"  {k}={DCQCN_KNOBS[k]}")
    print(f"Applied seed: {seed}")

    with open(out_file, "w") as fout:
        proc = subprocess.run(cmd, stdout=fout, stderr=subprocess.STDOUT, env=env)

    if proc.returncode != 0:
        print("Command failed (see output).")
    else:
        print("Command completed successfully")
    print(f"Output: {out_file}")
    print("----------------------------------------")

    return out_file, stat_file

# ============================================================
# CLI / Main
# ============================================================

def main():
    ap = argparse.ArgumentParser(
        description="Run simulator for ONE connection matrix and then plot + summarize (deterministic, with optional DCQCN knob overrides via env)."
    )
    ap.add_argument("--run-sim", metavar="MATRIX", required=True,
                    help="Connection matrix filename OR path, e.g. one_one_4_200MB.cm")
    ap.add_argument("--binary", default="./build/msft_ai_wan_single_dcqcn",
                    help="Path to simulator binary.")
    ap.add_argument("--trace-csv", default="trace_packets.csv",
                    help="Path to trace_packets.csv.")
    ap.add_argument("--paths-file", default="uec_entry.paths",
                    help="Path to uec_entry.paths.")
    # Optional run toggles (match your bash)
    ap.add_argument("--drop-rate", default="0")
    ap.add_argument("--link-down", type=int, default=0)
    ap.add_argument("--use-jitter", type=int, default=0)
    ap.add_argument("--exp", type=int, default=1)
    ap.add_argument("--seed", type=int, default=1, help="Deterministic RNG seed passed to the simulator.")
    args = ap.parse_args()

    out_file, stat_file = run_one_sim(
        matrix=args.run_sim,
        binary_path=args.binary,
        drop_rate=args.drop_rate,
        link_down=args.link_down,
        use_jitter=args.use_jitter,
        exp=args.exp,
        seed=args.seed,
    )

    # Fairness plot
    rates = parse_sending_rates(stat_file)
    plot_path = plot_path_from_stat(stat_file)
    title = f"Sending rates for {os.path.basename(plot_path).replace('fairness_','').replace('.png','')}"
    draw_fairness_plot(rates, plot_path, title)

    # Flow table + CSV (includes FCT/Avg/Score) — now de-duplicated by (flow, seq)
    print_and_write_summary(args.trace_csv, args.paths_file)

if __name__ == "__main__":
    main()
