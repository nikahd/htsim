import csv, re
from collections import defaultdict

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
