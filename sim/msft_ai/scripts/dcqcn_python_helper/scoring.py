import os
from collections import defaultdict

def parse_sending_rates(stat_path):
    from collections import defaultdict
    d = defaultdict(list)
    if not os.path.exists(stat_path):
        return d
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
                d[flow_id].append((t_us, r_bps))
            except Exception:
                continue
    return d

def parse_current_rates(stat_path):
    from collections import defaultdict
    d = defaultdict(list)
    if not os.path.exists(stat_path):
        return d
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
                    d[flow_id].append((t_us, r_bps))
                except Exception:
                    continue
    return d

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

    arrs = {f: sorted(sr[f]) for f in sr}
    def ok_window(t0):
        t1 = t0 + window_us
        for f, arr in arrs.items():
            # find all samples in window
            ok = False
            for (t, bps) in arr:
                if t0 <= t <= t1:
                    ok = True
                    if abs(bps - tail_bps.get(f, 0.0)) > eps.get(f, 1.0):
                        return False
            if not ok:  # no samples for this flow in window
                return False
        return True

    for t in all_ts:
        if ok_window(t):
            return t
    return all_ts[-1]

def composite_score(avg_ms, p95_ms, jain, ecn_ppkt, tconv_us, sim_end_us,
                    w_avg=1.0, w_p95=0.5):
    denom = w_avg*avg_ms + w_p95*p95_ms
    if denom <= 0:
        return 0.0
    pconv = 1.0 + (tconv_us / max(sim_end_us, 1.0))
    s = (1000.0 / denom) * jain * (1.0 / (1.0 + ecn_ppkt)) / pconv
    if jain < 0.90:
        s *= 0.1
    return s
