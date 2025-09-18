# dcqcn_python_helper/sim_runner.py
import os
import sys
import subprocess
from pathlib import Path

from .constants import DCQCN_BASE_KNOBS
from .paths import find_matrix_path, base_noext

def run_sim_with_knobs(matrix,
                       knobs,
                       folder_name,
                       binary_path,
                       drop_rate="0",
                       link_down=0,
                       use_jitter=0,
                       exp=1,
                       seed=1):
    """
    Run ONE simulator job for a given connection matrix.
    Returns (out_file, stat_file).
    """
    os.makedirs(folder_name, exist_ok=True)

    # Clean CSVs so analysis never reads leftovers between runs
    for f in ["trace_packets.csv", "cnp_events.csv", "fabric_breadcrumbs.csv", "queue_samples.csv"]:
        try:
            os.remove(f)
        except FileNotFoundError:
            pass

    conn_matrix = find_matrix_path(matrix)
    if not Path(conn_matrix).exists():
        print(f"ERROR: matrix file not found: {conn_matrix}", file=sys.stderr)
        sys.exit(1)

    base = base_noext(conn_matrix)
    suffix = f"_linkdown{link_down}_droprate{drop_rate}_usejitter{use_jitter}_exp{exp}"

    out_file  = os.path.join(folder_name, f"output_{base}{suffix}.txt")
    stat_file = os.path.join(folder_name, f"statistics_{base}{suffix}.txt")

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

    # Compose env with base + per-trial knobs
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
