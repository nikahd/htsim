import os, csv
import matplotlib.pyplot as plt
from collections import defaultdict
from .paths import plot_path_for_trial

def maybe_plot_queues(stat_path, queue_csv, tag, topk=5):
    if not os.path.exists(queue_csv):
        return None
    by_link = defaultdict(list)
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

    out_png = plot_path_for_trial(stat_path, tag, kind="queues")
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
