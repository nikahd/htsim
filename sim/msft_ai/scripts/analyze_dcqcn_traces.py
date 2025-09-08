#!/usr/bin/env python3
import re, sys, csv, os
from collections import defaultdict

# ---------- helpers ----------

def clean_path(p: str) -> str:
    """Normalize a path string: drop queue()/pipe() decorations and compress whitespace."""
    # remove queue(...) and pipe(...) fragments
    p = re.sub(r'\bqueue\([^)]*\)\s*', '', p, flags=re.IGNORECASE)
    p = re.sub(r'\bpipe\([^)]*\)\s*',  '', p, flags=re.IGNORECASE)
    # optional "Queue--" prefixes left by some printers
    p = re.sub(r'\bQueue--', '', p)
    # collapse spaces
    p = re.sub(r'\s+', ' ', p).strip()
    return p

# ---------- stdout parsing (CNPs, optional mapping) ----------

def parse_stdout(stdout_path):
    """
    Return:
      - cnp_sent: dict[flow] -> count
      - cnp_rcvd: dict[flow] -> count
      - name_map: dict[flow_token] -> flow_token  (currently a pass-through; kept for future extensibility)
    """
    cnp_sent = defaultdict(int)
    cnp_rcvd = defaultdict(int)
    name_map = {}

    if not os.path.exists(stdout_path):
        return cnp_sent, cnp_rcvd, name_map

    with open(stdout_path, 'r', errors='ignore') as f:
        for line in f:
            if '[CNP-SENT]' in line:
                m = re.search(r'flow=([A-Za-z0-9_]+)', line)
                if m: cnp_sent[m.group(1)] += 1
            elif '[CNP-RCVD]' in line:
                m = re.search(r'flow=([A-Za-z0-9_]+)', line)
                if m: cnp_rcvd[m.group(1)] += 1

            # (optional) future mapping hooks could go here if the log prints both numeric and name together
            # e.g., "flow_id=1000000001 name=DCQCN0" -> name_map['1000000001']='DCQCN0' etc.

    return cnp_sent, cnp_rcvd, name_map

# ---------- sink CSV parsing (ECN marks / packet counts) ----------

def parse_sink_csv(csv_path):
    """
    Returns:
      - ecn_marks: dict[flow] -> ECN-marked packet count
      - pkts: dict[flow] -> packet count
    """
    ecn_marks = defaultdict(int)
    pkts = defaultdict(int)

    if not csv_path or csv_path == '-' or not os.path.exists(csv_path):
        return ecn_marks, pkts

    with open(csv_path, newline='') as f:
        rdr = csv.DictReader(f)
        # Expected columns we wrote from DCQCNSink: flow, ecn_marked, ...
        # Gracefully handle slight header variations by lowercasing keys.
        for r in rdr:
            row = {k.lower(): v for k, v in r.items()}
            flow = row.get('flow', '')
            if not flow:
                # fallback: a different header name
                for k in row:
                    if 'flow' in k:
                        flow = row[k]
                        break
            if not flow:
                continue
            pkts[flow] += 1
            if row.get('ecn_marked', '0') == '1':
                ecn_marks[flow] += 1
    return ecn_marks, pkts

# ---------- paths parsing ----------

def parse_paths(paths_path):
    """
    Supports lines like:
        1000000001: <SRC_PATH>  ||  <DST_PATH>
    Also keeps compatibility with older formats:
        Flow DCQCN2: A -> B -> C
    Returns:
      - routes: dict[flow_token] -> {'src': str, 'dst': str, 'raw': str}
    """
    routes = {}
    if not paths_path or not os.path.exists(paths_path):
        return routes

    with open(paths_path, 'r', errors='ignore') as f:
        for line in f:
            s = line.strip()
            if not s:
                continue

            # New format: "<id>: src  ||  dst"
            m_new = re.match(r'^([A-Za-z0-9_]+)\s*:\s*(.+)$', s)
            if m_new:
                flow = m_new.group(1)
                body = m_new.group(2)

                # split on the "||" separator if present
                parts = [p.strip() for p in body.split('||', 1)]
                if len(parts) == 2:
                    src = clean_path(parts[0])
                    dst = clean_path(parts[1])
                    routes[flow] = {'src': src, 'dst': dst, 'raw': s}
                else:
                    # single path (older cases)
                    single = clean_path(parts[0])
                    routes[flow] = {'src': single, 'dst': '', 'raw': s}
                continue

            # Older heuristic: "Flow NAME: route..."
            m_old = re.search(r'(?:^|\s)(?:Flow|flow)\s+([A-Za-z0-9_]+).*?:\s*(.*)$', s)
            if m_old:
                flow = m_old.group(1)
                route = clean_path(m_old.group(2))
                routes[flow] = {'src': route, 'dst': '', 'raw': s}
                continue

    return routes

# ---------- main ----------

def main():
    if len(sys.argv) < 3:
        print("Usage: analyze_dcqcn_traces.py <stdout_log.txt> <trace_packets.csv or -> [uec_entry.paths]")
        sys.exit(1)

    stdout_log = sys.argv[1]
    sink_csv   = sys.argv[2]
    paths_file = sys.argv[3] if len(sys.argv) > 3 else "uec_entry.paths"

    cnp_sent, cnp_rcvd, name_map = parse_stdout(stdout_log)
    ecn_marks, pkts = parse_sink_csv(sink_csv)
    routes = parse_paths(paths_file)

    # Union of all flow keys we’ve seen across sources
    flows = set().union(cnp_sent.keys(), cnp_rcvd.keys(), ecn_marks.keys(), pkts.keys(), routes.keys())

    # Pretty header
    print("\nFlow Summary")
    print("------------")
    print(f"{'Flow':10} {'Pkts':>10} {'ECN':>10} {'ECN%':>7} {'CNP-sent':>10} {'CNP-rcvd':>10}  Route")

    rows = []
    for flow in sorted(flows):
        p = pkts.get(flow, 0)
        e = ecn_marks.get(flow, 0)
        ecn_pct = (100.0 * e / p) if p > 0 else 0.0

        # Route string for display: prefer "src || dst" if both available
        rinfo = routes.get(flow, {})
        if rinfo:
            if rinfo.get('dst'):
                route_str = f"{rinfo.get('src','')}  ||  {rinfo.get('dst','')}"
            else:
                route_str = rinfo.get('src','')
        else:
            # Maybe the flow was recorded with a different token (e.g., a name)
            # If you have a name_map in the future, you can try mapping here.
            route_str = ''

        print(f"{flow:10} {p:10} {e:10} {ecn_pct:7.2f} {cnp_sent.get(flow,0):10} {cnp_rcvd.get(flow,0):10}  {route_str}")

        rows.append({
            'flow': flow,
            'pkts': p,
            'ecn': e,
            'ecn_rate_pct': f"{ecn_pct:.2f}",
            'cnp_sent': cnp_sent.get(flow, 0),
            'cnp_rcvd': cnp_rcvd.get(flow, 0),
            'route_src': rinfo.get('src', ''),
            'route_dst': rinfo.get('dst', ''),
            'route_raw': rinfo.get('raw', '')
        })

    out_csv = "dcqcn_flow_summary.csv"
    with open(out_csv, 'w', newline='') as f:
        w = csv.DictWriter(
            f,
            fieldnames=['flow','pkts','ecn','ecn_rate_pct','cnp_sent','cnp_rcvd','route_src','route_dst','route_raw']
        )
        w.writeheader()
        w.writerows(rows)
    print(f"\nWrote {out_csv}")

if __name__ == "__main__":
    main()
