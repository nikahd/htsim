# dcqcn_python_helper/metrics_primitives.py
from .plots import parse_sending_rates, parse_current_rates  # if you kept parsers in plots.py move them here
# If your project originally had these in other modules, keep imports aligned.

# If you do NOT already have the parsers, paste these:

def parse_sending_rates(stat_path):
    from collections import defaultdict
    import os
    d = defaultdict(list)
    if not os.path.exists(stat_path): return d
    with open(stat_path,"r") as f:
        for raw in f:
            line = raw.strip()
            if not line or "Flow" not in line or "sending_rate" not in line: continue
            try:
                parts = line.split()
                flow_id = parts[1]
                t_us = float(parts[parts.index("time:")+1])
                r_bps = float(parts[parts.index("sending_rate:")+1])
                d[flow_id].append((t_us, r_bps))
            except: pass
    return d

def parse_current_rates(stat_path):
    from collections import defaultdict
    import os
    d = defaultdict(list)
    if not os.path.exists(stat_path): return d
    with open(stat_path,"r") as f:
        for raw in f:
            line = raw.strip()
            if not line or "Flow" not in line or "current_rate" not in line: continue
            try:
                parts = line.split()
                flow_id = parts[1]
                t_us = float(parts[parts.index("time:")+1])
                r_bps = float(parts[parts.index("current_rate:")+1])
                d[flow_id].append((t_us, r_bps))
            except: pass
    return d

# fairness + convergence primitives
def tail_mean_rates_from_sending(stat_path, tail_frac=0.2):
    sr = parse_sending_rates(stat_path)
    tail_means = {}
    for fid, arr in sr.items():
        if not arr:
            tail_means[fid] = 0.0; continue
        arr = sorted(arr)
        t_end = arr[-1][0]
        t_cut = t_end * (1.0 - tail_frac)
        xs = [bps for (t, bps) in arr if t >= t_cut]
        tail_means[fid] = (sum(xs)/len(xs))/1e6 if xs else 0.0
    return tail_means

def jain_index(values):
    vals = [v for v in values if v > 0]
    if not vals: return 0.0
    s = sum(vals); s2 = sum(v*v for v in vals)
    return (s*s)/(len(vals)*s2)

def convergence_time_us(stat_path, eps_frac=0.05, window_us=2000):
    sr = parse_sending_rates(stat_path)
    if not sr: return 0.0
    tail = tail_mean_rates_from_sending(stat_path, tail_frac=0.2)
    tail_bps = {f: m*1e6 for f, m in tail.items()}
    all_ts = sorted(set(t for arr in sr.values() for (t, _) in arr))
    if not all_ts: return 0.0
    eps = {f: max(1.0, tail_bps.get(f, 0.0) * eps_frac) for f in tail_bps}
    idx = {f: 0 for f in sr}
    arrs = {f: sorted(sr[f]) for f in sr}
    def ok_window(t0):
        t1 = t0 + window_us
        for f, arr in arrs.items():
            i = idx[f]
            while i < len(arr) and arr[i][0] < t0: i += 1
            j = i
            while j < len(arr) and arr[j][0] <= t1:
                t, bps = arr[j]
                if abs(bps - tail_bps.get(f, 0.0)) > eps.get(f, 1.0):
                    return False
                j += 1
            if j == i: return False
        return True
    for t in all_ts:
        if ok_window(t): return t
    return all_ts[-1]

def ecn_per_packet(csv_flow):
    ecn = sum(d.get("ecn",0) for d in csv_flow.values())
    pkts = sum(d.get("pkts",0) for d in csv_flow.values())
    return (ecn / pkts) if pkts > 0 else 0.0
