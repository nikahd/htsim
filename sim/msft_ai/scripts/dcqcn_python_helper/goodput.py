import csv
import matplotlib.pyplot as plt
from collections import defaultdict
from .plots import _color_for

def _row_payload_bytes(row):
    for k in ("payload_bytes","bytes","len","size","pkt_bytes"):
        if k in row and row[k] not in (None, "",):
            try: return int(float(row[k]))
            except: pass
    return 1024

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
