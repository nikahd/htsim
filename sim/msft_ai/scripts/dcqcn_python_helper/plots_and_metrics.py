# dcqcn_python_helper/plots_and_metrics.py
import os
import re
import csv
import json
import shutil
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt

from .constants import DCQCN_BASE_KNOBS, DCQCN_DEFAULTS
from .paths import base_noext

# ---------- I/O helpers for filenames ----------
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

def copy_as_best(fair_png, csv_path, stat_path, goodput_png=None, suffix=""):
    folder = os.path.dirname(stat_path)
    plots_dir = os.path.join(folder, "option_plots")
    os.makedirs(plots_dir, exist_ok=True)
    fair_dst = os.path.join(plots_dir, f"fairness_best{suffix}.png")
    shutil.copyfile(fair_png, fair_dst)
    if goodput_png and os.path.exists(goodput_png):
        gp_dst = os.path.join(plots_dir, f"goodput_best{suffix}.png")
        shutil.copyfile(goodput_png, gp_dst)
    shutil.copyfile(csv_path,  os.path.join(folder, f"dcqcn_flow_summary_best{suffix}.csv"))

def parse_paths_file_name_from_log():
    return "uec_entry.paths"

# ---------- Fairness / sending rate parsing ----------
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

def parse_current_rates(stat_path):
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

def draw_fairness_plot(sending_rate_dict, out_png, title):
    draw_rate_series(sending_rate_dict, out_png, title, "Sending Rate (Mbps)")

# ---------- Goodput (receiver-side) ----------
def _row_payload_bytes(row):
    for k in ("payload_bytes","bytes","len","size","pkt_bytes"):
        if k in row and row[k] not in (None, "",):
            try: return int(float(row[k]))
            except: pass
    return 1024  # fallback

def build_goodput_bins(trace_csv, bin_us=1000):
    gp_bins = defaultdict(lambda: defaultdict(int))
    first_seen = defaultdict(lambda: defaultdict(lambda: None))
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

# ---------- Idle fraction from CNP cooldown ----------
def knob_value(name, trial_knobs):
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

    intervals = defaultdict(list)
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
                s = t; e = min(sim_dur_us, t + cooldown_us)
                if s < e:
                    intervals[fid].append((s, e))
    except Exception:
        return {}

    idle_frac = {}
    for fid, ivals in intervals.items():
        if not ivals: continue
        ivals.sort()
        merged = []
        cs, ce = ivals[0]
        for s, e in ivals[1:]:
            if s <= ce: ce = max(ce, e)
            else: merged.append((cs, ce)); cs, ce = s, e
        merged.append((cs, ce))
        total_idle = sum(e - s for s, e in merged)
        idle_frac[fid] = max(0.0, min(1.0, total_idle / sim_dur_us))
    return idle_frac

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

def build_effective_sender_rate_from_idle(target_samples, idle_windows_by_flow):
    eff = {}
    for fid, samples in target_samples.items():
        try: fid_int = int(fid)
        except Exception: fid_int = fid
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
                out.append((t, 0.0))
            else:
                out.append((t, r))
        eff[fid] = out
    return eff

# ---------- Optional queue plot ----------
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

# ---------- Paths & link utilization ----------
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

# ---------- Flow summary & ECN/FCT ----------
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

def ecn_per_packet(csv_flow):
    ecn = sum(d["ecn"] for d in csv_flow.values())
    pkts = sum(d["pkts"] for d in csv_flow.values())
    return (ecn / pkts) if pkts > 0 else 0.0

# ---------- Fairness & Convergence ----------
def tail_mean_rates_from_sending(stat_path, tail_frac=0.2):
    sr = parse_sending_rates(stat_path)
    tail_means = {}
    for fid, arr in sr.items():
        if not arr:
            tail_means[fid] = 0.0
            continue
        arr = sorted(arr)
        t_end = arr[-1][0]
        t_cut = t_end * (1.0 - tail_frac)
        xs = [bps for (t, bps) in arr if t >= t_cut]
        tail_means[fid] = (sum(xs)/len(xs))/1e6 if xs else 0.0  # Mbps
    return tail_means

def jain_index(values):
    vals = [v for v in values if v > 0]
    if not vals:
        return 0.0
    s = sum(vals)
    s2 = sum(v*v for v in vals)
    return (s*s) / (len(vals) * s2)

def convergence_time_us(stat_path, eps_frac=0.05, window_us=2000):
    sr = parse_sending_rates(stat_path)
    if not sr:
        return 0.0
    tail = tail_mean_rates_from_sending(stat_path, tail_frac=0.2)
    tail_bps = {f: m*1e6 for f, m in tail.items()}
    all_ts = sorted(set(t for arr in sr.values() for (t, _) in arr))
    if not all_ts:
        return 0.0
    eps = {f: max(1.0, tail_bps.get(f, 0.0) * eps_frac) for f in tail_bps}

    idx = {f: 0 for f in sr}
    arrs = {f: sorted(sr[f]) for f in sr}

    def ok_window(t0):
        t1 = t0 + window_us
        for f, arr in arrs.items():
            i = idx[f]
            while i < len(arr) and arr[i][0] < t0:
                i += 1
            j = i
            while j < len(arr) and arr[j][0] <= t1:
                t, bps = arr[j]
                if abs(bps - tail_bps.get(f, 0.0)) > eps.get(f, 1.0):
                    return False
                j += 1
            if j == i:  # no samples
                return False
        return True

    for t in all_ts:
        if ok_window(t):
            return t
    return all_ts[-1]

# ---------- Composite score ----------
def composite_score(avg_ms, p95_ms, jain, ecn_ppkt, tconv_us, sim_end_us,
                    w_avg=1.0, w_p95=0.5):
    denom = w_avg*avg_ms + w_p95*p95_ms
    if denom <= 0:
        return 0.0
    pconv = 1.0 + (tconv_us / max(sim_end_us, 1.0))
    s = (1000.0 / denom) * jain * (1.0 / (1.0 + ecn_ppkt)) / pconv
    if jain < 0.90:  # soft guard-rail
        s *= 0.1
    return s

# ---------- Per-matrix summary printer ----------
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
    legacy_score = (1000.0 / avg_fct_ms) if avg_fct_ms > 0 else 0.0
    print("\nAverage FCT (ms): {:.3f}".format(avg_fct_ms))
    print("Score (1000 / Avg FCT ms): {:.3f}".format(legacy_score))

    with open(trial_csv_path, "w", newline='') as f:
        w = csv.writer(f)
        w.writerow(["Flow","Pkts","ECN","ECN_pct","CNP_sent","CNP_rcvd","Idle_pct","Goodput_avg_Mbps","Route","FCT_us"])
        for r in rows:
            w.writerow([r["Flow"], r["Pkts"], r["ECN"], "{:.2f}".format(r["ECN_pct"]),
                        r["CNP_sent"], r["CNP_rcvd"], "{:.2f}".format(r["Idle_pct"]),
                        "{:.1f}".format(r["Goodput_avg_Mbps"]), r["Route"],
                        int(r["FCT_us"]) if r["FCT_us"] is not None else ""])
        w.writerow([])
        w.writerow(["Average_FCT_ms", "{:.3f}".format(avg_fct_ms)])
        w.writerow(["Score_1000_over_avgFCTms", "{:.3f}".format(legacy_score)])
    print(f"Wrote {trial_csv_path}")
    return legacy_score, avg_fct_ms

# ---------- CNP events ----------
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
