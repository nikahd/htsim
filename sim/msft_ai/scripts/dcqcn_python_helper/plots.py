# dcqcn_python_helper/plots.py
import os
from collections import defaultdict
import matplotlib.pyplot as plt

# ---------- parsers (used by scoring + other helpers) ----------

def parse_sending_rates(stat_path):
    """Parse sender TARGET/pacing samples written as 'sending_rate:' lines."""
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
                t_us = float(parts[parts.index("time:") + 1])
                r_bps = float(parts[parts.index("sending_rate:") + 1])
                d[flow_id].append((t_us, r_bps))
            except Exception:
                pass
    return d

def parse_current_rates(stat_path):
    """Parse sender CURRENT/effective samples if the stats include 'current_rate:'."""
    d = defaultdict(list)
    if not os.path.exists(stat_path):
        return d
    with open(stat_path, "r") as f:
        for raw in f:
            line = raw.strip()
            if not line or "Flow" not in line or "current_rate" not in line:
                continue
            try:
                parts = line.split()
                flow_id = parts[1]
                t_us = float(parts[parts.index("time:") + 1])
                r_bps = float(parts[parts.index("current_rate:") + 1])
                d[flow_id].append((t_us, r_bps))
            except Exception:
                pass
    return d

# ---------- plotting utilities ----------

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

def draw_series(series_dict, out_png, title, ylabel):
    """
    Generic multi-series time plot.
    series_dict: {flow_id: [(t_us, rate_bps), ...]}
    """
    if not series_dict:
        print(f"No samples to plot: {title}")
        return
    plt.rcParams.update({'font.size': 14})
    plt.figure(figsize=(28, 16))
    colors = ['skyblue','lightgreen','salmon','plum','lightcoral',
              'lightgoldenrodyellow','lightcyan','lavender','lightpink',
              'lightseagreen','lightsalmon','lightsteelblue','lightyellow']
    for flow_id, samples in sorted(series_dict.items(), key=lambda kv: kv[0]):
        if not samples:
            continue
        ts, rates = zip(*sorted(samples))
        mbps = [r / 1e6 for r in rates]
        plt.plot(ts, mbps, label=f"Flow {flow_id}",
                 color=_color_for(flow_id, colors), linewidth=2)
    plt.title(title)
    plt.xlabel("Time (us)")
    plt.ylabel(ylabel)
    plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
    plt.tight_layout()
    plt.savefig(out_png)
    print(f"Plotting {out_png}")

def draw_fairness_plot(sending_rate_dict, out_png, title):
    """Thin wrapper kept for backwards compatibility."""
    draw_series(sending_rate_dict, out_png, title, "Sending Rate (Mbps)")
