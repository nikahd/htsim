import os, shutil, re
from pathlib import Path

def find_matrix_path(matrix: str) -> str:
    cand = Path(matrix)
    if cand.exists():
        return str(cand.resolve())

    env_dir = os.environ.get("MSFT_AI_CM_DIR")
    candidates = []
    if env_dir:
        candidates.append(Path(env_dir) / matrix)

    candidates += [
        Path("./scripts/msft_ai_connection_matrices") / matrix,
        Path("/htsim/scripts/msft_ai_connection_matrices") / matrix,
        Path("/htsim/scripts/msft_ai_dcqcn_knobs_matrices") / matrix,
        Path("sim/msft_ai/scripts/msft_ai_connection_matrices") / matrix,
        Path("msft_ai/scripts/msft_ai_connection_matrices") / matrix,
        Path("scripts") / "msft_ai_connection_matrices" / matrix,
    ]
    for p in candidates:
        if p.exists():
            return str(p.resolve())
    return str((Path("./scripts/msft_ai_connection_matrices") / matrix).resolve())

def stat_path_from_sim_out(sim_out_path):
    base = os.path.basename(sim_out_path)
    return os.path.join(os.path.dirname(sim_out_path),
                        base.replace("output_", "statistics_"))

def _plots_dir(stat_path):
    folder = os.path.dirname(stat_path)
    plots_dir = os.path.join(folder, "option_plots")
    os.makedirs(plots_dir, exist_ok=True)
    return plots_dir

def plot_path_for_trial(stat_path, tag, kind="fairness"):
    plots_dir = _plots_dir(stat_path)
    base = os.path.basename(stat_path).replace("statistics_", "").replace(".txt", "")
    return os.path.join(plots_dir, f"{kind}_{base}__{tag}.png")

def summary_csv_for_trial(stat_path, tag):
    folder = os.path.dirname(stat_path)
    return os.path.join(folder, f"dcqcn_flow_summary_{tag}.csv")

def copy_as_best(fair_png, csv_path, stat_path, goodput_png=None):
    plots_dir = _plots_dir(stat_path)
    shutil.copyfile(fair_png, os.path.join(plots_dir, "fairness_best.png"))
    if goodput_png and os.path.exists(goodput_png):
        shutil.copyfile(goodput_png, os.path.join(plots_dir, "goodput_best.png"))
    shutil.copyfile(csv_path,  os.path.join(os.path.dirname(stat_path), "dcqcn_flow_summary_best.csv"))

def parse_paths_file_name_from_log():
    return "uec_entry.paths"
