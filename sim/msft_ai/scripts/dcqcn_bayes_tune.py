#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DCQCN Tuning & Evaluation (search OR fixed)

Modes
-----
1) Search (default): Bayesian if scikit-optimize is present, otherwise random.
   -Renders fairness & goodput plots per trial
   -Optionally plots queue sizes (if queue_samples.csv exists)
   -Estimates per-link utilization from paths + delivered bytes
   -Writes a per-trial flow summary CSV with Idle% and GoodputAvg
   -Score = 1000 / AvgFCT(ms)
   -Deterministic across machines via --seed (each trial uses seed + idx)

   Example:
   python3 sim/msft_ai/scripts/dcqcn_bayes_tune.py \
       --run-sim one_one_4_200MB.cm --max-evals 12 --seed 42

2) Fixed: Run ONE trial with a fixed set of knobs (no search).
   -Provide knobs via --knobs-json (file) and/or repeated --knob K=V
   -Generates all the same outputs/plots as search mode
   -Writes best artifacts too (best == the only run)

   Example:
   python3 sim/msft_ai/scripts/dcqcn_bayes_tune.py \
       --run-sim one_one_4_200MB.cm --mode fixed \
       --knobs-json tuned_knobs.json --seed 7

   or with inline overrides:
   python3 sim/msft_ai/scripts/dcqcn_bayes_tune.py \
       --run-sim one_one_4_200MB.cm --mode fixed \
       --knob DCQCN_EPOCH_US=8000 --knob DCQCN_MD_CAP=0.4
"""

import os
import re
import sys
import csv
import json
import argparse
import random
import subprocess
import shutil
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


# ============================================================
# Baseline knob env (applied to all trials, unless you override)
# ============================================================

DCQCN_BASE_KNOBS = {
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

# Defaults only used if neither env nor knobs provide them
DCQCN_DEFAULTS = {
    "DCQCN_EPOCH_US": 15000,
    "DCQCN_HI_COOLDOWN": 2,
}


# ============================================================
# Search space
# ============================================================

DEFAULT_KNOBS_SPACE = {
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


# ============================================================
# Path helpers
# ============================================================

def find_matrix_path(matrix):
    cand = Path(matrix)
    if cand.exists():
        return str(cand.resolve())

    env_dir = os.environ.get("MSFT_AI_CM_DIR")
    candidates = []
    if env_dir:
        candidates.append(Path(env_dir) / matrix)

    # extended search paths (added /htsim/... and new knobs/matrices folder)
    candidates += [
        Path("./scripts/msft_ai_connection_matrices") / matrix,
        Path("/htsim/scripts/msft_ai_connection_matrices") / matrix,
        Path("/htsim/scripts/msft_ai_dcqcn_knobs_matrices") / matrix,
        Path("sim/msft_ai/scripts/msft_ai_connection_matrices") / matrix,
        Path("msft_ai/scripts/msft_ai_connection_matrices") / matrix,
        Path("scripts") / "msft_ai_connection_matrices" / matrix,
    ]
    for p in candidates:
        if p.exists():
            return str(p.resolve())
    return str((Path("./scripts/msft_ai_connection_matrices") / matrix).resolve())

def stat_path_from_sim_out(sim_out_path):
    base = os.path.basename(sim_out_path)
    return os.path.join(os.path.dirname(sim_out_path),
                        base.replace("output_", "statistics_"))

def plot_path_for_trial(stat_path, tag):
    folder = os.path.dirname(stat_path)
    plots_dir = os.path.join(folder, "option_plots")
    os.makedirs(plots_dir, exist_ok=True)
    base = os.path.basename(stat_path)  # statistics_*.txt
    name = base.replace("statistics_", "").replace(".txt", "")
    return os.path.join(plots_dir, f"fairness_{name}__{tag}.png")

def goodput_plot_path_for_trial(stat_path, tag):
    folder = os.path.dirname(stat_path)
    plots_dir = os.path.join(folder, "option_plots")
    os.makedirs(plots_dir, exist_ok=True)
    base = os.path.basename(stat_path).replace("statistics_", "").replace(".txt", "")
    return os.path.join(plots_dir, f"goodput_{base}__{tag}.png")

def queue_plot_path_for_trial(stat_path, tag):
    folder = os.path.dirname(stat_path)
    plots_dir = os.path.join(folder, "option_plots")
    os.makedirs(plots_dir, exist_ok=True)
    base = os.path.basename(stat_path).replace("statistics_", "").replace(".txt", "")
    return os.path.join(plots_dir, f"queues_{base}__{tag}.png")

def summary_csv_for_trial(stat_path, tag):
    folder = os.path.dirname(stat_path)
    return os.path.join(folder, f"dcqcn_flow_summary_{tag}.csv")

def copy_as_best(fair_png, csv_path, stat_path, goodput_png=None):
    folder = os.path.dirname(stat_path)
    plots_dir = os.path.join(folder, "option_plots")
    os.makedirs(plots_dir, exist_ok=True)
    shutil.copyfile(fair_png, os.path.join(plots_dir, "fairness_best.png"))
    if goodput_png and os.path.exists(goodput_png):
        shutil.copyfile(goodput_png, os.path.join(plots_dir, "goodput_best.png"))
    shutil.copyfile(csv_path,  os.path.join(folder, "dcqcn_flow_summary_best.csv"))

def parse_paths_file_name_from_log():
    return "uec_entry.paths"


# ============================================================
# Fairness plot (from statistics_*.txt)
# ============================================================

def parse_sending_rates(stat_path):
    sending_rate_dict = defaultdict(list)
    if not os.path.exists(stat_path):
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

# NEW: parse sender *current* rate, if the stats include it
def parse_current_rates(stat_path):
    """Parse sender current (effective) rate if the stats file contains 'current_rate:' samples."""
    current_rate = defaultdict(list)
    if not os.path.exists(stat_path):
        return current_rate
    with open(stat_path, "r") as f:
        for raw in f:
            line = raw.strip()
            if not line or "Flow" not in line:
                continue
            if "current_rate" in line:
                try:
                    parts = line.split()
                    flow_id = parts[1]
                    t_us = float(parts[parts.index("time:")+1])
                    r_bps = float(parts[parts.index("current_rate:")+1])
                    current_rate[flow_id].append((t_us, r_bps))
                except Exception:
                    continue
    return current_rate

def _color_for(flow_id, palette):
    try:
        s = str(flow_id)
        if s.startswith("DCQCN"):
            idx = int(s[5:])
        else:
            idx = int(s) % 1000
    except Exception:
        idx = hash(str(flow_id)) % 1000
    return palette[idx % len(palette)]

def draw_fairness_plot(sending_rate_dict, out_png, title):
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

# NEW: generic rate-series drawer (used for sender current rate)
def draw_rate_series(series_dict, out_png, title, ylabel):
    if not series_dict:
        print(f"No samples to plot: {title}")
        return
    plt.rcParams.update({'font.size': 14})
    plt.figure(figsize=(28, 16))
    colors = ['skyblue','lightgreen','salmon','plum','lightcoral','lightgoldenrodyellow',
              'lightcyan','lavender','lightpink','lightseagreen','lightsalmon','lightsteelblue','lightyellow']
    for flow_id, samples in sorted(series_dict.items(), key=lambda kv: kv[0]):
        ts, rates = zip(*samples)
        mbps = [r / 1e6 for r in rates]
        plt.plot(ts, mbps, label=f"Flow {flow_id}", color=_color_for(flow_id, colors), linewidth=2)
    plt.title(title)
    plt.xlabel("Time (us)")
    plt.ylabel(ylabel)
    plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
    plt.tight_layout()
    plt.savefig(out_png)
    print(f"Plotting {out_png}")


# ============================================================
# Goodput (from trace_packets.csv, no C++ changes required)
# ============================================================

def _row_payload_bytes(row):
    for k in ("payload_bytes","bytes","len","size","pkt_bytes"):
        if k in row and row[k] not in (None, "",):
            try: return int(float(row[k]))
            except: pass
    return 1024  # fallback

def build_goodput_bins(trace_csv, bin_us=1000):
    gp_bins = defaultdict(lambda: defaultdict(int))
    first_seen = defaultdict(lambda: defaultdict(lambda: None))  # flow -> seq -> first_ts
    last_ts = 0.0
    flow_total_bytes = defaultdict(int)
    try:
        with open(trace_csv, newline='') as f:
            reader = csv.DictReader(f)
            if not reader.fieldnames:
                return {}, 0.0, {}
            flow_key = 'flow' if 'flow' in reader.fieldnames else ('flow_id' if 'flow_id' in reader.fieldnames else None)
            ts_key   = 'ts_us' if 'ts_us' in reader.fieldnames else None
            seq_key  = 'seq'
            if not (flow_key and ts_key and seq_key):
                return {}, 0.0, {}
            for row in reader:
                try:
                    fid = int(row[flow_key]); ts = float(row[ts_key]); seq = int(row.get(seq_key, 0))
                except Exception:
                    continue
                last_ts = max(last_ts, ts)
                if first_seen[fid][seq] is None:
                    first_seen[fid][seq] = ts
                    b = _row_payload_bytes(row)
                    flow_total_bytes[fid] += b
                    bin_idx = int(ts // bin_us)
                    gp_bins[fid][bin_idx] += b
    except FileNotFoundError:
        return {}, 0.0, {}
    return gp_bins, last_ts, flow_total_bytes

def plot_goodput_bins(gp_bins, out_png, bin_us):
    if not gp_bins:
        print("No goodput data to plot.")
        return
    plt.figure(figsize=(28, 16))
    plt.rcParams.update({'font.size': 14})
    colors = ['skyblue','lightgreen','salmon','plum','lightcoral','lightgoldenrodyellow',
              'lightcyan','lavender','lightpink','lightseagreen','lightsalmon','lightsteelblue','lightyellow']
    for fid in sorted(gp_bins.keys()):
        tb = gp_bins[fid]
        xs = sorted(tb.keys())
        ys = [ (tb[t]*8.0)/bin_us for t in xs ]  # Mbps
        ts = [ t*bin_us for t in xs ]
        plt.plot(ts, ys, label=f"Flow {fid}", color=_color_for(fid, colors), linewidth=2)
    plt.xlabel("Time (us)")
    plt.ylabel(f"Goodput (Mbps)  [bin={bin_us}us]")
    plt.title("Per-flow Goodput (Receiver)")
    plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
    plt.tight_layout()
    plt.savefig(out_png)
    print(f"Plotting {out_png}")


# ============================================================
# Idle fraction (from CNPs + knobs)
# ============================================================

def knob_value(name, trial_knobs):
    # priority: explicit trial knobs -> env -> base knobs -> defaults
    if name in trial_knobs:
        try: return float(trial_knobs[name])
        except: pass
    if name in os.environ:
        try: return float(os.environ[name])
        except: pass
    if name in DCQCN_BASE_KNOBS:
        try: return float(DCQCN_BASE_KNOBS[name])
        except: pass
    return float(DCQCN_DEFAULTS.get(name, 0.0))

def compute_idle_fraction(cnp_csv, sim_dur_us, trial_knobs):
    epoch_us = knob_value("DCQCN_EPOCH_US", trial_knobs)
    cooldown_epochs = int(round(knob_value("DCQCN_HI_COOLDOWN", trial_knobs)))
    cooldown_us = max(0.0, epoch_us * cooldown_epochs)

    if sim_dur_us <= 0 or not os.path.exists(cnp_csv) or cooldown_us <= 0:
        return {}

    intervals = defaultdict(list)  # flow -> [(start, end), ...]
    try:
        with open(cnp_csv, newline="") as f:
            rdr = csv.DictReader(f)
            if not rdr.fieldnames or 'flow' not in rdr.fieldnames or 'ts_us' not in rdr.fieldnames or 'event' not in rdr.fieldnames:
                return {}
            for row in rdr:
                try:
                    if row['event'].strip().upper() not in ("RCVD","RECV","RECEIVED"):
                        continue
                    fid = int(row['flow'])
                    t  = float(row['ts_us'])
                except:
                    continue
                s = t
                e = min(sim_dur_us, t + cooldown_us)
                if s < e:
                    intervals[fid].append((s, e))
    except Exception:
        return {}

    idle_frac = {}
    for fid, ivals in intervals.items():
        if not ivals:
            continue
        ivals.sort()
        merged = []
        cur_s, cur_e = ivals[0]
        for s, e in ivals[1:]:
            if s <= cur_e:
                cur_e = max(cur_e, e)
            else:
                merged.append((cur_s, cur_e))
                cur_s, cur_e = s, e
        merged.append((cur_s, cur_e))
        total_idle = sum(e - s for s, e in merged)
        idle_frac[fid] = max(0.0, min(1.0, total_idle / sim_dur_us))
    return idle_frac

# NEW: return idle windows (merged) to mask sender-rate during CNP cooldowns
def compute_idle_windows(cnp_csv, sim_dur_us, trial_knobs):
    epoch_us = knob_value("DCQCN_EPOCH_US", trial_knobs)
    cooldown_epochs = int(round(knob_value("DCQCN_HI_COOLDOWN", trial_knobs)))
    cooldown_us = max(0.0, epoch_us * cooldown_epochs)
    out = defaultdict(list)
    if sim_dur_us <= 0 or not os.path.exists(cnp_csv) or cooldown_us <= 0:
        return out
    try:
        with open(cnp_csv, newline="") as f:
            rdr = csv.DictReader(f)
            if not rdr.fieldnames or 'flow' not in rdr.fieldnames or 'ts_us' not in rdr.fieldnames or 'event' not in rdr.fieldnames:
                return out
            tmp = defaultdict(list)
            for row in rdr:
                try:
                    if row['event'].strip().upper() not in ("RCVD","RECV","RECEIVED"):
                        continue
                    fid = int(row['flow']); t = float(row['ts_us'])
                except:
                    continue
                s = t; e = min(sim_dur_us, t + cooldown_us)
                if s < e: tmp[fid].append((s, e))
            for fid, ivals in tmp.items():
                if not ivals: 
                    continue
                ivals.sort()
                merged = []
                cs, ce = ivals[0]
                for s, e in ivals[1:]:
                    if s <= ce: ce = max(ce, e)
                    else: merged.append((cs, ce)); cs, ce = s, e
                merged.append((cs, ce))
                out[fid] = merged
    except Exception:
        return out
    return out

# NEW: mask target rate by idle windows to approximate sender current/effective rate
def build_effective_sender_rate_from_idle(target_samples, idle_windows_by_flow):
    eff = {}
    for fid, samples in target_samples.items():
        try:
            fid_int = int(fid)
        except Exception:
            fid_int = fid
        windows = sorted(idle_windows_by_flow.get(fid_int, []))
        if not windows:
            eff[fid] = samples
            continue
        out = []
        wi = 0
        for (t, r) in samples:
            while wi < len(windows) and t > windows[wi][1]:
                wi += 1
            if wi < len(windows) and windows[wi][0] <= t <= windows[wi][1]:
                out.append((t, 0.0))  # idle → zero
            else:
                out.append((t, r))
        eff[fid] = out
    return eff


# ============================================================
# Optional queue plot (requires queue_samples.csv from C++)
# ============================================================

def maybe_plot_queues(stat_path, queue_csv, tag, topk=5):
    if not os.path.exists(queue_csv):
        return None
    by_link = defaultdict(list)  # link -> [(t, qbytes)]
    try:
        with open(queue_csv, newline="") as f:
            rdr = csv.DictReader(f)
            if not rdr.fieldnames:
                return None
            tkey = 'time_us' if 'time_us' in rdr.fieldnames else ('ts_us' if 'ts_us' in rdr.fieldnames else None)
            lkey = 'link' if 'link' in rdr.fieldnames else ('queue' if 'queue' in rdr.fieldnames else None)
            qkey = 'q_bytes' if 'q_bytes' in rdr.fieldnames else ('queue_bytes' if 'queue_bytes' in rdr.fieldnames else None)
            if not (tkey and lkey and qkey):
                return None
            for row in rdr:
                try:
                    t = float(row[tkey]); L = row[lkey]; q = float(row[qkey])
                except:
                    continue
                by_link[L].append((t, q))
    except Exception:
        return None

    scored = []
    for L, arr in by_link.items():
        if not arr: continue
        mx = max(q for _, q in arr)
        scored.append((mx, L))
    scored.sort(reverse=True)
    keep = [L for _, L in scored[:topk]]
    if not keep:
        return None

    out_png = queue_plot_path_for_trial(stat_path, tag)
    plt.figure(figsize=(28, 10))
    for L in keep:
        arr = sorted(by_link[L])
        ts = [t for t,_ in arr]
        qs = [q/1024.0 for _,q in arr]  # KB
        plt.plot(ts, qs, label=L)
    plt.xlabel("Time (us)")
    plt.ylabel("Queue size (KB)")
    plt.title(f"Top queue occupancies ({tag})")
    plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
    plt.tight_layout()
    plt.savefig(out_png)
    print(f"Plotting {out_png}")
    return out_png


# ============================================================
# Per-link utilization estimate (paths + bytes)
# ============================================================

def parse_paths(paths_file):
    id_to_nodes = {}
    id_to_pretty = {}
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
                id_to_pretty[fid] = pth
                cleaned = re.sub(r'queue\([^)]+\)Queue--', '', pth)
                cleaned = cleaned.replace(' -> pipe(1us) -> ', ' -> ').replace('-> ->','->')
                nodes = [x.strip() for x in cleaned.split("->") if x.strip()]
                id_to_nodes[fid] = nodes
    except FileNotFoundError:
        pass
    return id_to_nodes, id_to_pretty

def estimate_link_util(paths_file, flow_total_bytes, sim_dur_us, out_csv):
    if sim_dur_us <= 0:
        return
    id_to_nodes, _ = parse_paths(paths_file)
    edge_bytes = defaultdict(int)
    edge_flows = defaultdict(set)

    for fid, nodes in id_to_nodes.items():
        if len(nodes) < 2: continue
        b = int(flow_total_bytes.get(fid, 0))
        for i in range(len(nodes)-1):
            edge = f"{nodes[i]} -> {nodes[i+1]}"
            edge_bytes[edge] += b
            edge_flows[edge].add(fid)

    if not edge_bytes:
        return

    denom = sim_dur_us * 1e-6  # seconds
    rows = []
    for edge in sorted(edge_bytes.keys()):
        est_bps = edge_bytes[edge] / denom  # bytes/s
        rows.append([edge, len(edge_flows[edge]), int(est_bps)])

    with open(out_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["edge","num_flows","est_bytes_per_sec"])
        w.writerows(rows)
    print(f"Wrote {out_csv}")


# ============================================================
# Flow summary (FCT/ECN + new metrics)
# ============================================================

def parse_trace_csv_fct_ecn(trace_csv):
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
                return {}
            flow_key = 'flow' if 'flow' in reader.fieldnames else ('flow_id' if 'flow_id' in reader.fieldnames else None)
            ts_key   = 'ts_us' if 'ts_us' in reader.fieldnames else None
            seq_key  = 'seq'
            ecn_key  = 'ecn_marked' if 'ecn_marked' in reader.fieldnames else None
            if not (flow_key and ts_key and seq_key):
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

def print_and_write_summary(trace_csv, paths_file, idle_frac, goodput_bins, bin_us, trial_csv_path):
    csv_flow   = parse_trace_csv_fct_ecn(trace_csv)
    flow_ids   = sorted(csv_flow.keys())
    id_to_nodes, id_to_pretty = parse_paths(paths_file)
    cnp_sent, cnp_rcvd = parse_cnp_events("cnp_events.csv")

    # avg goodput per flow across non-empty bins
    avg_gp_mbps = {}
    for fid in flow_ids:
        tb = goodput_bins.get(fid, {})
        if not tb:
            avg_gp_mbps[fid] = 0.0
        else:
            vals = [(b*8.0)/bin_us for b in tb.values()]
            avg_gp_mbps[fid] = sum(vals)/len(vals)

    print("\nFlow Summary")
    print("------------")
    print("{:<22} {:>10} {:>10} {:>7} {:>10} {:>10} {:>9} {:>10}  {}".format(
        "Flow","Pkts","ECN","ECN%","CNP-sent","CNP-rcvd","Idle%","GpAvg","Route"))

    rows, fcts = [], []
    for fid in flow_ids:
        d = csv_flow[fid]
        pkts = d["pkts"]; ecn = d["ecn"]
        ecn_pct = (100.0 * ecn / pkts) if pkts > 0 else 0.0
        fct_us  = d.get("fct_us", None)
        route   = id_to_pretty.get(fid, "")
        idle    = 100.0 * float(idle_frac.get(fid, 0.0))
        gp_avg  = avg_gp_mbps.get(fid, 0.0)
        rows.append({
            "Flow": fid, "Pkts": pkts, "ECN": ecn, "ECN_pct": ecn_pct,
            "CNP_sent": cnp_sent.get(fid, 0), "CNP_rcvd": cnp_rcvd.get(fid, 0),
            "Idle_pct": idle, "Goodput_avg_Mbps": gp_avg,
            "Route": route, "FCT_us": fct_us
        })
        if fct_us is not None and pkts > 0:
            fcts.append(fct_us)

    for r in rows:
        print("{:<22} {:>10} {:>10} {:>7.2f} {:>10} {:>10} {:>8.2f} {:>10.1f}  {}".format(
            r["Flow"], r["Pkts"], r["ECN"], r["ECN_pct"],
            r["CNP_sent"], r["CNP_rcvd"], r["Idle_pct"], r["Goodput_avg_Mbps"], r["Route"]))

    avg_fct_ms = (sum(fcts) / len(fcts) / 1000.0) if fcts else 0.0
    score = (1000.0 / avg_fct_ms) if avg_fct_ms > 0 else 0.0
    print("\nAverage FCT (ms): {:.3f}".format(avg_fct_ms))
    print("Score (1000 / Avg FCT ms): {:.3f}".format(score))

    with open(trial_csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Flow","Pkts","ECN","ECN_pct","CNP_sent","CNP_rcvd","Idle_pct","Goodput_avg_Mbps","Route","FCT_us"])
        for r in rows:
            w.writerow([r["Flow"], r["Pkts"], r["ECN"], "{:.2f}".format(r["ECN_pct"]),
                        r["CNP_sent"], r["CNP_rcvd"], "{:.2f}".format(r["Idle_pct"]),
                        "{:.1f}".format(r["Goodput_avg_Mbps"]), r["Route"],
                        int(r["FCT_us"]) if r["FCT_us"] is not None else ""])
        w.writerow([])
        w.writerow(["Average_FCT_ms", "{:.3f}".format(avg_fct_ms)])
        w.writerow(["Score_1000_over_avgFCTms", "{:.3f}".format(score)])
    print(f"Wrote {trial_csv_path}")
    return score, avg_fct_ms


# ============================================================
# CNP events (shared)
# ============================================================

def parse_cnp_events(cnp_csv_path="cnp_events.csv"):
    sent = defaultdict(int)
    rcvd = defaultdict(int)
    if not os.path.exists(cnp_csv_path):
        return sent, rcvd
    try:
        with open(cnp_csv_path, newline="") as f:
            reader = csv.DictReader(f)
            if not reader.fieldnames:
                return sent, rcvd
            if "flow" not in reader.fieldnames or "event" not in reader.fieldnames:
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


# ============================================================
# Simulator wrapper
# ============================================================

def run_sim_with_knobs(matrix,
                       knobs,
                       folder_name,
                       binary_path,
                       drop_rate="0",
                       link_down=0,
                       use_jitter=0,
                       exp=1,
                       seed=1):
    """
    One simulator run for a connection matrix with explicit knobs.
    Cleans stale CSVs, passes -seed, returns (out_file, stat_file).
    """
    os.makedirs(folder_name, exist_ok=True)

    # Clean CSVs so analysis never reads leftovers
    for f in ["trace_packets.csv", "cnp_events.csv", "fabric_breadcrumbs.csv", "queue_samples.csv"]:
        try:
            os.remove(f)
        except FileNotFoundError:
            pass

    conn_matrix = find_matrix_path(matrix)
    if not Path(conn_matrix).exists():
        print(f"ERROR: matrix file not found: {conn_matrix}", file=sys.stderr)
        sys.exit(1)

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
        "-seed", str(seed),
    ]

    # Compose env with base + per-trial knobs
    env = os.environ.copy()
    for k, v in DCQCN_BASE_KNOBS.items():
        env[k] = str(v)
    for k, v in knobs.items():
        env[str(k)] = str(v)

    print("Executing command:")
    print(" ".join(cmd))
    # (avoid duplicate knob printing; knobs already printed in evaluate())
    print(f"Seed: {seed}")
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


# ============================================================
# Trial evaluation (run + plots + CSV + score)
# ============================================================

def evaluate(matrix, trial_knobs, args, tag, trial_seed):
    """
    Returns (score, avg_fct_ms, out_file, stat_file, fairness_png, goodput_png, trial_csv).
    """
    print(f"\n=== Trial {tag} knobs ===")
    for k in sorted(trial_knobs.keys()):
        print(f"  {k} = {trial_knobs[k]}")

    out_file, stat_file = run_sim_with_knobs(
        matrix=matrix, knobs=trial_knobs,
        folder_name=args.folder, binary_path=args.binary,
        drop_rate=args.drop_rate, link_down=args.link_down,
        use_jitter=args.use_jitter, exp=args.exp, seed=trial_seed
    )

    # Sender TARGET rate (fairness plot)
    fairness_png = plot_path_for_trial(stat_file, tag)
    rates = parse_sending_rates(stat_file)
    title = f"{tag}: {os.path.basename(fairness_png)}"
    draw_fairness_plot(rates, fairness_png, title)

    # Goodput + sim duration + per-link util estimate
    gp_bins, sim_dur_us, flow_total_bytes = build_goodput_bins(args.trace_csv, bin_us=args.goodput_bin_us)
    goodput_png = goodput_plot_path_for_trial(stat_file, tag)
    plot_goodput_bins(gp_bins, goodput_png, args.goodput_bin_us)

    # Idle fraction (needs knobs for cooldown reconstruction)
    idle_frac = compute_idle_fraction("cnp_events.csv", sim_dur_us, trial_knobs)
    idle_w    = compute_idle_windows("cnp_events.csv", sim_dur_us, trial_knobs)

    # Sender CURRENT/effective rate:
    # 1) try to parse if stats have 'current_rate:' samples; else
    # 2) approximate by masking target rate during idle windows
    current_rates = parse_current_rates(stat_file)
    if not current_rates:
        current_rates = build_effective_sender_rate_from_idle(rates, idle_w)
    sender_cur_png = goodput_plot_path_for_trial(stat_file, f"{tag}_sender_current")
    draw_rate_series(current_rates, sender_cur_png, f"{tag}: Sender CURRENT rate", "Current Rate (Mbps)")

    # Optional queue plot (if CSV present)
    maybe_plot_queues(stat_file, "queue_samples.csv", tag)

    # Per-link util estimate CSV (trial-specific file to avoid collisions)
    estimate_link_util(args.paths_file or parse_paths_file_name_from_log(),
                       flow_total_bytes, sim_dur_us,
                       out_csv=f"link_util_est_{tag}.csv")

    # Flow summary CSV + score
    trial_csv = summary_csv_for_trial(stat_file, tag)
    score, avg_fct_ms = print_and_write_summary(
        args.trace_csv,
        args.paths_file or parse_paths_file_name_from_log(),
        idle_frac,
        gp_bins,
        args.goodput_bin_us,
        trial_csv_path=trial_csv
    )

    print(f"[{tag}] score={score:.3f} | avg_fct_ms={avg_fct_ms:.3f}")
    return score, avg_fct_ms, out_file, stat_file, fairness_png, goodput_png, trial_csv


# ============================================================
# Search strategies
# ============================================================

def rand_point(space):
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

def parse_inline_knobs(kv_list):
    """
    Parse repeated --knob KEY=VAL into a dict with int/float inference.
    """
    out = {}
    if not kv_list:
        return out
    for kv in kv_list:
        if "=" not in kv:
            continue
        k, v = kv.split("=", 1)
        k = k.strip()
        v = v.strip()
        # type inference
        try:
            if v.lower().startswith("0x"):
                out[k] = int(v, 16)
            elif "." in v:
                out[k] = float(v)
            else:
                out[k] = int(v)
        except Exception:
            try:
                out[k] = float(v)
            except Exception:
                out[k] = v
    return out

def main():
    ap = argparse.ArgumentParser(
        description="DCQCN search or fixed evaluation with enhanced plots/metrics."
    )
    ap.add_argument("--mode", choices=["search","fixed"], default="search",
                    help="search (default) = Bayesian/Random; fixed = run one trial with provided knobs")
    ap.add_argument("--run-sim", metavar="MATRIX", required=True,
                    help="Connection matrix filename or path, e.g. one_one_4_200MB.cm")
    ap.add_argument("--binary", default="./build/msft_ai_wan_single_dcqcn",
                    help="Path to simulator binary.")
    ap.add_argument("--folder", default="msft_ai_wan_single_dcqcn",
                    help="Output folder used by the binary.")
    ap.add_argument("--trace-csv", default="trace_packets.csv",
                    help="Path to trace_packets.csv.")
    ap.add_argument("--paths-file", default="uec_entry.paths",
                    help="Path to uec_entry.paths (default inferred).")
    ap.add_argument("--drop-rate", default="0")
    ap.add_argument("--link-down", type=int, default=0)
    ap.add_argument("--use-jitter", type=int, default=0)
    ap.add_argument("--exp", type=int, default=1)
    ap.add_argument("--goodput-bin-us", type=int, default=1000,
                    help="Bin size for goodput curve (us).")

    # Search settings
    ap.add_argument("--max-evals", type=int, default=10, help="Number of trials in search mode.")
    ap.add_argument("--seed", type=int, default=42, help="Base RNG seed; per-trial seed = seed + trial_index.")

    # Fixed-mode knobs:
    ap.add_argument("--knobs-json", default=None,
                    help="Path to JSON file with knobs (keys as env var names).")
    ap.add_argument("--knob", action="append", default=None,
                    help="Inline knob override K=V; can be repeated.")

    args = ap.parse_args()

    random.seed(args.seed)

    # -----------------------------------------
    # FIXED MODE
    # -----------------------------------------
    if args.mode == "fixed":
        fixed = {}
        # load from JSON if provided
        if args.knobs_json:
            try:
                with open(args.knobs_json, "r") as f:
                    data = json.load(f)
                    if isinstance(data, dict):
                        fixed.update(data)
                    else:
                        print("WARN: knobs-json is not a dict; ignoring.", file=sys.stderr)
            except Exception as e:
                print(f"WARN: failed to read knobs-json: {e}", file=sys.stderr)
        else:
            # Auto-discover default knobs at your new path
            auto_knobs = "/htsim/scripts/msft_ai_dcqcn_knobs_matrices/tuned_knobs.json"
            if os.path.exists(auto_knobs):
                try:
                    with open(auto_knobs, "r") as f:
                        data = json.load(f)
                        if isinstance(data, dict):
                            print(f"Using default knobs from: {auto_knobs}")
                            fixed.update(data)
                except Exception as e:
                    print(f"WARN: failed to read default knobs {auto_knobs}: {e}", file=sys.stderr)

        # inline overrides win last
        fixed.update(parse_inline_knobs(args.knob))

        tag = "fixed"
        score, avg_fct_ms, out_file, stat_file, fairness_png, gp_png, trial_csv = evaluate(
            args.run_sim, fixed, args, tag, trial_seed=args.seed
        )
        copy_as_best(fairness_png, trial_csv, stat_file, goodput_png=gp_png)

        print("\n==================== Fixed run results ====================")
        print(f"score={score:.3f} | avg_fct_ms={avg_fct_ms:.3f}")
        print("Applied knobs:")
        print(json.dumps(fixed, indent=2, sort_keys=True))
        return

    # -----------------------------------------
    # SEARCH MODE
    # -----------------------------------------
    search_kind, opt, names = get_optimizer(DEFAULT_KNOBS_SPACE)

    best_score   = -1.0
    best_summary = None  # (trial_idx, knobs, score, avg_fct_ms, stat_file, fairness_png, goodput_png, trial_csv)

    for i in range(args.max_evals):
        tag = f"trial_{i}"

        if search_kind == "skopt":
            x = opt.ask()
            trial_knobs = {names[j]: x[j] for j in range(len(names))}
        else:
            trial_knobs = rand_point(DEFAULT_KNOBS_SPACE)

        # Coerce types according to space definition
        coerced = {}
        for k, v in trial_knobs.items():
            lo, hi = DEFAULT_KNOBS_SPACE[k]
            coerced[k] = int(round(v)) if isinstance(lo, int) and isinstance(hi, int) else float(v)
        trial_knobs = coerced

        score, avg_fct_ms, out_file, stat_file, fairness_png, goodput_png, trial_csv = evaluate(
            args.run_sim, trial_knobs, args, tag, trial_seed=args.seed + i
        )

        if search_kind == "skopt":
            # maximize score by minimizing negative score
            opt.tell(list(trial_knobs.values()), -score)

        if score > best_score:
            best_score = score
            best_summary = (i, trial_knobs, score, avg_fct_ms, stat_file, fairness_png, goodput_png, trial_csv)
            copy_as_best(fairness_png, trial_csv, stat_file, goodput_png)

    print("\n==================== Best configuration ====================")
    if best_summary is None:
        print("No successful trials.")
        return
    i, knobs, score, avg_fct_ms, stat_file, fairness_png, goodput_png, trial_csv = best_summary
    print(f"trial #{i}: score={score:.3f} | avg_fct_ms={avg_fct_ms:.3f}")
    print(json.dumps(knobs, indent=2, sort_keys=True))
    print("Artifacts:")
    folder = os.path.dirname(stat_file)
    print(f"  Fairness plots: {os.path.join(folder, 'option_plots')}/fairness_*__trial_*.png")
    print(f"  Goodput plots:  {os.path.join(folder, 'option_plots')}/goodput_*__trial_*.png")
    print(f"  Trial CSVs:     {folder}/dcqcn_flow_summary_trial_*.csv")
    print(f"  Best fairness:  {os.path.join(folder, 'option_plots', 'fairness_best.png')}")
    print(f"  Best goodput:   {os.path.join(folder, 'option_plots', 'goodput_best.png')}")
    print(f"  Best CSV:       {os.path.join(folder, 'dcqcn_flow_summary_best.csv')}")

if __name__ == "__main__":
    main()
