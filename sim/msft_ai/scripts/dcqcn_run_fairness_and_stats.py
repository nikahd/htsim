#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import sys
import csv
import argparse
import subprocess
from collections import defaultdict, Counter
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

# Reasonable fallbacks if neither env nor above are set
DCQCN_DEFAULTS = {
    "DCQCN_EPOCH_US": 15000,
    "DCQCN_HI_COOLDOWN": 2,
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

def goodput_plot_path_from_stat(stat_path: str) -> str:
    folder = os.path.dirname(stat_path)
    plots = os.path.join(folder, "option_plots")
    os.makedirs(plots, exist_ok=True)
    base = os.path.basename(stat_path).replace("statistics_", "").replace(".txt", "")
    return os.path.join(plots, f"goodput_{base}.png")

def queue_plot_path_from_stat(stat_path: str) -> str:
    folder = os.path.dirname(stat_path)
    plots = os.path.join(folder, "option_plots")
    os.makedirs(plots, exist_ok=True)
    base = os.path.basename(stat_path).replace("statistics_", "").replace(".txt", "")
    return os.path.join(plots, f"queues_{base}.png")

def _color_for(flow_id: str, palette):
    try:
        if str(flow_id).startswith("DCQCN"):
            idx = int(str(flow_id)[5:])
        else:
            idx = int(flow_id) % 1000
    except Exception:
        idx = hash(str(flow_id)) % 1000
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
# Parsers used by multiple metrics
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

def parse_paths(paths_file):
    """
    Parse <basename>.paths into { flow_id(int) -> [node names ...] } plus a pretty string.
    """
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
                # strip queue(...) boilerplate and pipes, then split into nodes
                cleaned = re.sub(r'queue\([^)]+\)Queue--', '', pth)
                cleaned = cleaned.replace(' -> pipe(1us) -> ', ' -> ').replace('-> ->','->')
                nodes = [x.strip() for x in cleaned.split("->") if x.strip()]
                id_to_nodes[fid] = nodes
    except FileNotFoundError:
        pass
    return id_to_nodes, id_to_pretty

# ============================================================
# Goodput (no C++ changes needed)
# ============================================================

def _row_payload_bytes(row) -> int:
    # Try common names; fallback to ~1024 if unknown (your MTU is ~1024 in many tests)
    for k in ("payload_bytes","bytes","len","size","pkt_bytes"):
        if k in row and row[k] not in (None, "",):
            try: return int(float(row[k]))
            except: pass
    return 1024

def build_goodput_bins(trace_csv: str, bin_us: int = 1000):
    """
    Returns:
      gp_bins: {flow -> {bin_idx -> bytes_delivered_in_bin}}
      sim_dur_us: total duration inferred from trace
      flow_total_bytes: {flow -> total bytes delivered (dedup by seq)}
    """
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

def plot_goodput_bins(gp_bins, out_png: str, bin_us: int):
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
        # Convert bytes per bin to Mbps: (bytes*8)/(bin_us*1e-6)/1e6 = bytes*8/(bin_us)
        ys = [ (tb[t]*8.0)/bin_us for t in xs ]
        ts = [ t*bin_us for t in xs ]
        plt.plot(ts, ys, label=f"Flow {fid}", color=_color_for(fid, colors), linewidth=2)
    plt.xlabel("Time (us)")
    plt.ylabel(f"Goodput (Mbps)  [bin={bin_us}us]")
    plt.title("Per-flow Goodput")
    plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
    plt.tight_layout()
    plt.savefig(out_png)
    print(f"Plotting {out_png}")

# ============================================================
# Idle fraction (reconstruct from CNPs + knobs)
# ============================================================

def knob_value(name: str) -> float:
    # priority: environment -> DCQCN_KNOBS constant -> DCQCN_DEFAULTS
    if name in os.environ:
        try: return float(os.environ[name])
        except: pass
    if name in DCQCN_KNOBS:
        try: return float(DCQCN_KNOBS[name])
        except: pass
    return float(DCQCN_DEFAULTS.get(name, 0.0))

def compute_idle_fraction(cnp_csv: str, sim_dur_us: float) -> dict:
    """
    Reconstruct per-flow idle regions:
      each RCVD event -> cooldown of HI_COOLDOWN * EPOCH_US (us).
    Overlapping cooldowns are merged. Returns {flow -> idle_fraction[0..1]}.
    """
    epoch_us = knob_value("DCQCN_EPOCH_US")
    cooldown_epochs = int(round(knob_value("DCQCN_HI_COOLDOWN")))
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

    # merge overlaps per flow and compute total idle
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

# ============================================================
# Optional: Queue length plot (requires queue_samples.csv)
# Expect CSV columns: time_us, link (or queue), q_bytes
# ============================================================

def maybe_plot_queues(stat_path: str, queue_csv: str = "queue_samples.csv", topk: int = 5):
    if not os.path.exists(queue_csv):
        return
    by_link = defaultdict(list)  # link -> [(t, qbytes)]
    try:
        with open(queue_csv, newline="") as f:
            rdr = csv.DictReader(f)
            if not rdr.fieldnames:
                return
            tkey = 'time_us' if 'time_us' in rdr.fieldnames else ('ts_us' if 'ts_us' in rdr.fieldnames else None)
            lkey = 'link' if 'link' in rdr.fieldnames else ('queue' if 'queue' in rdr.fieldnames else None)
            qkey = 'q_bytes' if 'q_bytes' in rdr.fieldnames else ('queue_bytes' if 'queue_bytes' in rdr.fieldnames else None)
            if not (tkey and lkey and qkey):
                return
            for row in rdr:
                try:
                    t = float(row[tkey]); L = row[lkey]; q = float(row[qkey])
                except:
                    continue
                by_link[L].append((t, q))
    except Exception:
        return
    # choose top-k by max queue
    scored = []
    for L, arr in by_link.items():
        if not arr: continue
        mx = max(q for _, q in arr)
        scored.append((mx, L))
    scored.sort(reverse=True)
    keep = [L for _, L in scored[:topk]]
    if not keep:
        return
    plt.figure(figsize=(28, 10))
    for L in keep:
        arr = sorted(by_link[L])
        ts = [t for t,_ in arr]
        qs = [q/1024.0 for _,q in arr]  # KB
        plt.plot(ts, qs, label=L)
    plt.xlabel("Time (us)")
    plt.ylabel("Queue size (KB)")
    plt.title("Top queue occupancies")
    plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
    plt.tight_layout()
    out_png = queue_plot_path_from_stat(stat_path)
    plt.savefig(out_png)
    print(f"Plotting {out_png}")

# ============================================================
# Per-link utilization estimate (from paths + bytes)
# ============================================================

def estimate_link_util(paths_file: str, flow_total_bytes: dict, sim_dur_us: float, out_csv="link_util_est.csv"):
    """
    Build edges from consecutive node names in uec_entry.paths (after cleaning).
    Utilization estimate (bytes/s) per edge = sum(flow_bytes for flows using it) / (sim_dur_us * 1e-6).
    Writes CSV with columns: edge, flows, est_bytes_per_sec.
    """
    if sim_dur_us <= 0:
        return
    id_to_nodes, _pretty = parse_paths(paths_file)
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
# Flow summary (incl. FCT, ECN%, CNPs, Idle, Goodput avg)
# ============================================================

def parse_trace_csv_fct_ecn(trace_csv):
    """
    Build FCT and ECN counts, dedup by (flow, seq).
    Returns {fid: {"pkts":int,"ecn":int,"fct_us":float|None}}
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
            seq_key  = 'seq'
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

def print_and_write_summary(trace_csv, paths_file, idle_frac: dict, goodput_bins, bin_us, sim_dur_us):
    csv_flow   = parse_trace_csv_fct_ecn(trace_csv)
    flow_ids   = sorted(csv_flow.keys())

    # derive avg goodput per flow as average over non-empty bins
    avg_gp_mbps = {}
    for fid in flow_ids:
        tb = goodput_bins.get(fid, {})
        if not tb:
            avg_gp_mbps[fid] = 0.0
        else:
            # bytes/bin -> Mbps
            vals = [(b*8.0)/bin_us for b in tb.values()]
            avg_gp_mbps[fid] = sum(vals)/len(vals)

    # Paths (pretty string for display)
    _nodes, id_to_pretty = parse_paths(paths_file)

    # CNP counts for table
    cnp_sent, cnp_rcvd = parse_cnp_events("cnp_events.csv")

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

    out_csv = "dcqcn_flow_summary.csv"
    with open(out_csv, "w", newline='') as f:
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
    print(f"\nWrote {out_csv}")

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
            fk = 'flow' if 'flow' in reader.fieldnames else None
            ek = 'event' if 'event' in reader.fieldnames else None
            if not fk or not ek:
                return sent, rcvd
            for row in reader:
                try:
                    fid = int(row[fk]); ev = row[ek].strip().lower()
                except:
                    continue
                if ev == 'sent':
                    sent[fid] += 1
                elif ev in ('rcvd','recv','received'):
                    rcvd[fid] += 1
    except Exception:
        pass
    return sent, rcvd

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
    for f in ("trace_packets.csv", "cnp_events.csv", "fabric_breadcrumbs.csv", "queue_samples.csv"):
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
    ap.add_argument("--goodput-bin-us", type=int, default=1000, help="Bin size for goodput curve.")
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

    # Goodput bins/plot (no C++ change needed)
    gp_bins, sim_dur_us, flow_total_bytes = build_goodput_bins(args.trace_csv, bin_us=args.goodput_bin_us)
    gp_png = goodput_plot_path_from_stat(stat_file)
    plot_goodput_bins(gp_bins, gp_png, args.goodput_bin_us)

    # Idle fraction per flow (reconstructed)
    idle_frac = compute_idle_fraction("cnp_events.csv", sim_dur_us)

    # Optional: plot queues if queue_samples.csv exists
    maybe_plot_queues(stat_path=stat_file, queue_csv="queue_samples.csv")

    # Per-link utilization estimate CSV
    estimate_link_util(args.paths_file, flow_total_bytes, sim_dur_us, out_csv="link_util_est.csv")

    # Flow table + CSV (includes FCT/Avg/Score + Idle% + GoodputAvg)
    print_and_write_summary(args.trace_csv, args.paths_file, idle_frac, gp_bins, args.goodput_bin_us, sim_dur_us)

if __name__ == "__main__":
    main()
