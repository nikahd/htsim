# dcqcn_python_helper/paths.py
import os
from pathlib import Path

SEARCH_DIRS = [
    Path("./scripts/msft_ai_connection_matrices"),
    Path("/htsim/scripts/msft_ai_connection_matrices"),
    Path("/htsim/scripts/msft_ai_dcqcn_knobs_matrices"),
    Path("sim/msft_ai/scripts/msft_ai_connection_matrices"),
    Path("msft_ai/scripts/msft_ai_connection_matrices"),
    Path("scripts") / "msft_ai_connection_matrices",
]

def find_matrix_path(matrix: str) -> str:
    cand = Path(matrix)
    if cand.exists():
        return str(cand.resolve())

    env_dir = os.environ.get("MSFT_AI_CM_DIR")
    candidates = []
    if env_dir:
        candidates.append(Path(env_dir) / matrix)
    candidates += [d / matrix for d in SEARCH_DIRS]

    for p in candidates:
        if p.exists():
            return str(p.resolve())

    # default fallback path
    return str((Path("./scripts/msft_ai_connection_matrices") / matrix).resolve())

def base_noext(path: str) -> str:
    return Path(path).name.rsplit(".", 1)[0]
