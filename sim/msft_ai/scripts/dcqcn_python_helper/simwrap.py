import os, sys, subprocess
from pathlib import Path
from .constants import DCQCN_BASE_KNOBS
from .paths import find_matrix_path

def run_sim_with_knobs(matrix, knobs, folder_name, binary_path,
                       drop_rate="0", link_down=0, use_jitter=0, exp=1, seed=1):
    """
    One simulator run for a connection matrix with explicit knobs.
    Cleans stale CSVs, passes -seed, returns (out_file, stat_file).
    """
    os.makedirs(folder_name, exist_ok=True)

    # Clean CSVs so analysis never reads leftovers
    for f in ["trace_packets.csv", "cnp_events.csv", "fabric_breadcrumbs.csv", "queue_samples.csv"]:
        try: os.remove(f)
        except FileNotFoundError: pass

    conn_matrix = find_matrix_path(matrix)
    if not Path(conn_matrix).exists():
        print(f"ERROR: matrix file not found: {conn_matrix}", file=sys.stderr)
        sys.exit(1)

    base_noext  = os.path.splitext(os.path.basename(conn_matrix))[0]
    suffix      = f"_linkdown{link_down}_droprate{drop_rate}_usejitter{use_jitter}_exp{exp}"

    out_file  = os.path.join(folder_name, f"output_{base_noext}{suffix}.txt")
    stat_file = os.path.join(folder_name, f"statistics_{base_noext}{suffix}.txt")

    cmd = [
        binary_path,
        "-o","uec_entry",
        "-switch_latency","0",
        "-collect_data","1",
        "-strat","ecmp_host",
        "-tm", conn_matrix,
        "-noFi","-noQaInter","-noQaIntra","-noRto",
        "-drop-rate", str(drop_rate),
        "-use-jitter", str(use_jitter),
        "-interQSize","4000000",
        "-intraQSize","200000000",
        "-is-link-down", str(link_down),
        "-statistics-filename", stat_file,
        "-seed", str(seed),
    ]

    env = os.environ.copy()
    for k, v in DCQCN_BASE_KNOBS.items():
        env[k] = str(v)
    for k, v in knobs.items():
        env[str(k)] = str(v)

    print("Executing command:")
    print(" ".join(cmd))
    print(f"Seed: {seed}")
    print(f"Statistics will be written to: {stat_file}")
    print("----------------------------------------")

    with open(out_file, "w") as fout:
        proc = subprocess.run(cmd, stdout=fout, stderr=subprocess.STDOUT, env=env)

    if proc.returncode != 0:
        print("Command failed (see output).")
    else:
        print("Command completed successfully")
    print(f"Output: {out_file}")
    print("----------------------------------------")

    return out_file, stat_file
