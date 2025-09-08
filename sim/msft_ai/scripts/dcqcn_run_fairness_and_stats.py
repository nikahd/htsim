#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import sys
import csv
import argparse
import subprocess
from collections import defaultdict
import matplotlib.pyplot as plt

# ---------- helpers for file names ----------

def stat_path_from_sim_out(sim_out_path: str) -> str:
    base = os.path.basename(sim_out_path)
    return os.path.join(os.path.dirname(sim_out_path),
                        base.replace("output_", "statistics_"))

def plot_path_from_stat(stat_path: str) -> str:
    folder = os.path.dirname(stat_path)
    os.makedirs(os.path.join(folder, "option_plots"), exist_ok=True)
    base = os.path.basename(stat_path)
    m = re.match(r"statistics_(.+?)_linkdown(\d+)_droprate([0-9a-zA-Z.]+)_usejitter(\d+)_exp(\d+)\.txt", base)
    if not m:
        return os.path.join(folder, "option_plots", f"fairness_{base}.png")
    matrix, link, droprate, usejit, _exp = m.groups()
    return os.path.join(folder, "option_plots",
                        f"fairness_{matrix}_link{link}_usejitter{usejit}_droprate{droprate}_initcwnd0.7.png")

# ---------- fairness plot (parse statistics_*.txt) ----------

def parse_sending_rates(stat_path: str):
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

# ---------- trace analysis (paths + ECN/CNP + FCT + score) ----------

def parse_paths(paths_file):
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
    per_flow = defaultdict(lambda: {"pkts":0, "ecn":0, "t_first":None, "t_last":None})
    try:
        with open(trace_csv, newline='') as f:
            reader = csv.DictReader(f)
            flow_key = 'flow' if 'flow' in reader.fieldnames else ('flow_id' if 'flow_id' in reader.fieldnames else None)
            ts_key   = 'ts_us' if 'ts_us' in reader.fieldnames else None
            ecn_key  = 'ecn_marked' if 'ecn_marked' in reader.fieldnames else None
            if not flow_key or not ts_key:
                print("ERROR: trace_packets.csv must include ts_us and flow columns.", file=sys.stderr)
                return {}
            for row in reader:
                try:
                    fid = int(row[flow_key])
                    ts = float(row[ts_key])
                except:
                    continue
                ecn = 0
                if ecn_key and row.get(ecn_key, '') != '':
                    try: ecn = int(row[ecn_key])
                    except: ecn = 0
                d = per_flow[fid]
                d["pkts"] += 1
                d["ecn"]  += (1 if ecn else 0)
                d["t_first"] = ts if d["t_first"] is None else min(d["t_first"], ts)
                d["t_last"]  = ts if d["t_last"]  is None else max(d["t_last"], ts)
    except FileNotFoundError:
        print(f"WARNING: {trace_csv} not found; FCT and ECN% will be unavailable.", file=sys.stderr)
        return {}
    for d in per_flow.values():
        if d["t_first"] is not None and d["t_last"] is not None:
            d["fct_us"] = max(0.0, d["t_last"] - d["t_first"])
        else:
            d["fct_us"] = None
    return per_flow

def print_and_write_summary(trace_csv, paths_file):
    id_to_path = parse_paths(paths_file)
    csv_flow = parse_trace_csv(trace_csv)
    flow_ids = sorted(csv_flow.keys())

    print("\nFlow Summary")
    print("------------")
    print("{:<12} {:>10} {:>10} {:>7} {:>10} {:>10}  {}  {:>10}".format(
        "Flow", "Pkts", "ECN", "ECN%", "CNP-sent", "CNP-rcvd", "Route", "FCT_us"))

    rows, fcts = [], []
    for fid in flow_ids:
        d = csv_flow[fid]
        pkts = d["pkts"]; ecn = d["ecn"]
        ecn_pct = (100.0 * ecn / pkts) if pkts > 0 else 0.0
        fct_us  = d.get("fct_us", None)
        route = id_to_path.get(fid, "")
        cnps_sent = 0
        cnps_rcvd = 0
        rows.append({
            "Flow": fid, "Pkts": pkts, "ECN": ecn, "ECN_pct": ecn_pct,
            "CNP_sent": cnps_sent, "CNP_rcvd": cnps_rcvd,
            "Route": route, "FCT_us": fct_us
        })
        if fct_us is not None and pkts > 0:
            fcts.append(fct_us)

    for r in rows:
        print("{:<12} {:>10} {:>10} {:>7.2f} {:>10} {:>10}  {}  {:>10}".format(
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

# ---------- simulation runner (bash content inline) ----------

def run_one_sim(matrix: str,
                folder_name="msft_ai_wan_single_dcqcn",
                binary_path="./build/msft_ai_wan_single_dcqcn",
                drop_rate="0",
                link_down=0,
                use_jitter=0,
                exp=1):
    os.makedirs(folder_name, exist_ok=True)

    conn_matrix = f"./scripts/msft_ai_connection_matrices/{matrix}"
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
    ]

    print("Executing command:")
    print(" ".join(cmd))
    print(f"Statistics will be written to: {stat_file}")
    print("----------------------------------------")

    with open(out_file, "w") as fout:
        proc = subprocess.run(cmd, stdout=fout, stderr=subprocess.STDOUT)

    if proc.returncode != 0:
        print("Command failed (see output).")
    else:
        print("Command completed successfully")
    print(f"Output: {out_file}")
    print("----------------------------------------")

    return out_file, stat_file

# ---------- main ----------

def main():
    ap = argparse.ArgumentParser(
        description="Run simulator for ONE connection matrix and then plot + summarize."
    )
    ap.add_argument("--run-sim", metavar="MATRIX", required=True,
                    help="Connection matrix filename, e.g. one_one_4_200MB.cm")
    ap.add_argument("--binary", default="./build/msft_ai_wan_single_dcqcn",
                    help="Path to simulator binary.")
    ap.add_argument("--trace-csv", default="trace_packets.csv",
                    help="Path to trace_packets.csv.")
    ap.add_argument("--paths-file", default="uec_entry.paths",
                    help="Path to uec_entry.paths.")
    # Optional overrides if needed later:
    ap.add_argument("--drop-rate", default="0")
    ap.add_argument("--link-down", type=int, default=0)
    ap.add_argument("--use-jitter", type=int, default=0)
    ap.add_argument("--exp", type=int, default=1)
    args = ap.parse_args()

    matrix = args.run_sim
    out_file, stat_file = run_one_sim(
        matrix=matrix,
        binary_path=args.binary,
        drop_rate=args.drop_rate,
        link_down=args.link_down,
        use_jitter=args.use_jitter,
        exp=args.exp
    )

    # Fairness plot
    rates = parse_sending_rates(stat_file)
    plot_path = plot_path_from_stat(stat_file)
    title = f"Sending rates for {os.path.basename(plot_path).replace('fairness_','').replace('.png','')}"
    draw_fairness_plot(rates, plot_path, title)

    # Flow table + CSV (includes FCT/Avg/Score)
    print_and_write_summary(args.trace_csv, args.paths_file)

if __name__ == "__main__":
    main()
