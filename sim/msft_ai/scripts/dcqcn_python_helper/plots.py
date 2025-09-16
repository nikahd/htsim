import matplotlib.pyplot as plt
from collections import defaultdict

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

def draw_time_series(series_dict, out_png, title, ylabel):
    if not series_dict:
        print(f"No samples to plot: {title}")
        return
    plt.rcParams.update({'font.size': 14})
    plt.figure(figsize=(28, 16))
    colors = ['skyblue','lightgreen','salmon','plum','lightcoral','lightgoldenrodyellow',
              'lightcyan','lavender','lightpink','lightseagreen','lightsalmon','lightsteelblue','lightyellow']
    for flow_id, samples in sorted(series_dict.items(), key=lambda kv: kv[0]):
        ts, vals = zip(*samples)
        mbps = [v / 1e6 for v in vals]
        plt.plot(ts, mbps, label=f"Flow {flow_id}", color=_color_for(flow_id, colors), linewidth=2)
    plt.title(title)
    plt.xlabel("Time (us)")
    plt.ylabel(ylabel)
    plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
    plt.tight_layout()
    plt.savefig(out_png)
    print(f"Plotting {out_png}")
