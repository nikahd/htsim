import csv, os
from collections import defaultdict
from .perlink import parse_paths

def parse_trace_csv_fct_ecn(trace_csv):
    per_flow = defaultdict(lambda: {"seen_seqs": set(),"seq_first_ts": {},"seq_last_ts": {},"ecn_marks": 0})
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

def print_and_write_summary(trace_csv, paths_file, idle_frac, goodput_bins, bin_us, trial_csv_path):
    csv_flow   = parse_trace_csv_fct_ecn(trace_csv)
    flow_ids   = sorted(csv_flow.keys())
    id_to_nodes, id_to_pretty = parse_paths(paths_file)

    def avg_gp(fid):
        tb = goodput_bins.get(fid, {})
        if not tb: return 0.0
        vals = [(b*8.0)/bin_us for b in tb.values()]
        return sum(vals)/len(vals)

    print("\nFlow Summary")
    print("------------")
    print("{:<22} {:>10} {:>10} {:>7} {:>10} {:>10} {:>9} {:>10}  {}".format(
        "Flow","Pkts","ECN","ECN%","CNP-sent","CNP-rcvd","Idle%","GpAvg","Route"))

    # CNPs are optionally filled by caller (kept compatible with earlier code)
    rows, fcts = [], []
    # the caller prints CNPs; here we only print zeros to maintain format if absent
    for fid in flow_ids:
        d = csv_flow[fid]
        pkts = d["pkts"]; ecn = d["ecn"]
        ecn_pct = (100.0 * ecn / pkts) if pkts > 0 else 0.0
        fct_us  = d.get("fct_us", None)
        route   = id_to_pretty.get(fid, "")
        idle    = 100.0 * float(idle_frac.get(fid, 0.0))
        gp_avg  = avg_gp(fid)
        rows.append({
            "Flow": fid, "Pkts": pkts, "ECN": ecn, "ECN_pct": ecn_pct,
            "CNP_sent": 0, "CNP_rcvd": 0,
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
