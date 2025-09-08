#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import csv
import sys
import re
from collections import defaultdict, OrderedDict

"""
Usage:
  python3 analyze_dcqcn_traces.py <sim_out.txt> <trace_packets.csv> <paths_file>

What it prints:
  - Flow table with: Flow, Pkts, ECN, ECN%, CNP-sent, CNP-rcvd, Route, FCT_us
  - Average FCT (ms) across flows that carried packets
  - Score = 1000 / Average_FCT_ms
Also writes: dcqcn_flow_summary.csv
"""

def parse_paths(paths_file):
    """
    Parse lines like:
      1000000001: srcPath  ||  dstPath
    Returns map: flow_id -> "srcPath  ||  dstPath"
    Also a friendlier short path name if present; otherwise uses raw.
    """
    id_to_path = {}
    try:
        with open(paths_file, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or ':' not in line:
                    continue
                # flow_id: path...
                m = re.match(r'(\d+)\s*:\s*(.*)$', line)
                if not m:
                    continue
                fid = int(m.group(1))
                pth = m.group(2).strip()

                # Compact the queue/pipe prefixes if they exist (keep switch names)
                # e.g., "queue(100000Mb/s,...)Queue--" -> "", " -> pipe(1us) -> " -> " -> "
                pth_clean = re.sub(r'queue\([^)]+\)Queue--', '', pth)
                pth_clean = pth_clean.replace(' -> pipe(1us) -> ', ' -> ')
                id_to_path[fid] = pth_clean
    except FileNotFoundError:
        pass
    return id_to_path


def parse_sim_out(sim_out_file):
    """
    Parse sim output (the .txt) for per-flow counters if present.
    We’ll look for lines of the form in your current summary printer:
      Flow <id> Pkts <n> ECN <n> CNP-sent <n> CNP-rcvd <n>
    If not found, we’ll fall back to counting from the packet CSV.
    """
    data = {}
    try:
        with open(sim_out_file, 'r') as f:
            for line in f:
                # flexible extract of integers keyed by tokens
                # Not strictly required if your analyzer derives these from CSV,
                # but we keep this to stay compatible with your current file.
                pass
    except FileNotFoundError:
        pass
    return data  # possibly empty; we’ll fill from CSV


def parse_trace_csv(trace_csv):
    """
    Read trace_packets.csv (columns: ts_us,flow,seq,ecn_marked,src_id,sink_id)
    Returns:
      - counters per flow: pkts, ecn
      - FCT_us per flow: min(ts_us) .. max(ts_us)
    """
    per_flow = defaultdict(lambda: {
        "pkts": 0,
        "ecn": 0,
        "t_first": None,
        "t_last": None
    })
    try:
        with open(trace_csv, newline='') as f:
            reader = csv.DictReader(f)
            # handle both 'flow' and 'flow_id' naming defensively
            flow_key = 'flow' if 'flow' in reader.fieldnames else ('flow_id' if 'flow_id' in reader.fieldnames else None)
            ts_key   = 'ts_us' if 'ts_us' in reader.fieldnames else None
            ecn_key  = 'ecn_marked' if 'ecn_marked' in reader.fieldnames else None

            if not flow_key or not ts_key:
                print("ERROR: trace_packets.csv must include ts_us and flow columns.", file=sys.stderr)
                return {}

            for row in reader:
                try:
                    fid = int(row[flow_key])
                except:
                    # skip bad rows
                    continue
                try:
                    ts_us = float(row[ts_key])
                except:
                    continue

                ecn = 0
                if ecn_key and row.get(ecn_key, '') != '':
                    try:
                        ecn = int(row[ecn_key])
                    except:
                        ecn = 0

                per_flow[fid]["pkts"] += 1
                per_flow[fid]["ecn"]  += (1 if ecn else 0)
                tf = per_flow[fid]["t_first"]
                tl = per_flow[fid]["t_last"]
                per_flow[fid]["t_first"] = ts_us if tf is None else min(tf, ts_us)
                per_flow[fid]["t_last"]  = ts_us if tl is None else max(tl, ts_us)
    except FileNotFoundError:
        print(f"WARNING: {trace_csv} not found; FCT and ECN% will be unavailable.", file=sys.stderr)
        return {}

    # compute FCT
    for fid, d in per_flow.items():
        if d["t_first"] is not None and d["t_last"] is not None:
            d["fct_us"] = max(0.0, d["t_last"] - d["t_first"])
        else:
            d["fct_us"] = None
    return per_flow


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    sim_out_file = sys.argv[1]
    trace_csv     = sys.argv[2]
    paths_file    = sys.argv[3]

    id_to_path = parse_paths(paths_file)

    # Counters and FCT from packet CSV
    csv_flow = parse_trace_csv(trace_csv)

    # Prepare rows ordered by flow id
    flow_ids = sorted(csv_flow.keys())

    # Header
    print("\nFlow Summary")
    print("------------")
    print("{:<12} {:>10} {:>10} {:>7} {:>10} {:>10}  {}  {:>10}".format(
        "Flow", "Pkts", "ECN", "ECN%", "CNP-sent", "CNP-rcvd", "Route", "FCT_us"))

    # We don’t have CNP-sent/rcvd in the CSV—keep zeros unless you’re feeding them here.
    # If you later write total CNPs into the stats file per flow, you can plug them in below.
    rows = []
    fcts = []

    for fid in flow_ids:
        d = csv_flow[fid]
        pkts = d["pkts"]
        ecn  = d["ecn"]
        ecn_pct = (100.0 * ecn / pkts) if pkts > 0 else 0.0
        fct_us = d.get("fct_us", None)

        route = id_to_path.get(fid, "")
        cnps_sent = 0
        cnps_rcvd = 0  # if you instrumented DCQCNSrc to count per-flow CNPs, load from a file and fill here.

        rows.append({
            "Flow": fid,
            "Pkts": pkts,
            "ECN": ecn,
            "ECN_pct": ecn_pct,
            "CNP_sent": cnps_sent,
            "CNP_rcvd": cnps_rcvd,
            "Route": route,
            "FCT_us": fct_us
        })

        if fct_us is not None and pkts > 0:
            fcts.append(fct_us)

    # Print table
    for r in rows:
        print("{:<12} {:>10} {:>10} {:>7.2f} {:>10} {:>10}  {}  {:>10}".format(
            r["Flow"], r["Pkts"], r["ECN"], r["ECN_pct"],
            r["CNP_sent"], r["CNP_rcvd"], r["Route"], int(r["FCT_us"]) if r["FCT_us"] is not None else 0))

    # Average FCT and Score
    avg_fct_ms = (sum(fcts) / len(fcts) / 1000.0) if fcts else 0.0
    score = (1000.0 / avg_fct_ms) if avg_fct_ms > 0 else 0.0

    print("\nAverage FCT (ms): {:.3f}".format(avg_fct_ms))
    print("Score (1000 / Avg FCT ms): {:.3f}".format(score))

    # Write CSV
    out_csv = "dcqcn_flow_summary.csv"
    with open(out_csv, "w", newline='') as f:
        w = csv.writer(f)
        w.writerow(["Flow", "Pkts", "ECN", "ECN_pct", "CNP_sent", "CNP_rcvd", "Route", "FCT_us"])
        for r in rows:
            w.writerow([r["Flow"], r["Pkts"], r["ECN"], "{:.2f}".format(r["ECN_pct"]),
                        r["CNP_sent"], r["CNP_rcvd"], r["Route"], int(r["FCT_us"]) if r["FCT_us"] is not None else ""])
        # Footer lines for averages & score (handy when you open in a sheet)
        w.writerow([])
        w.writerow(["Average_FCT_ms", "{:.3f}".format(avg_fct_ms)])
        w.writerow(["Score_1000_over_avgFCTms", "{:.3f}".format(score)])

    print(f"\nWrote {out_csv}")

if __name__ == "__main__":
    main()
