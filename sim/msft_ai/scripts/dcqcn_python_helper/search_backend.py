# dcqcn_python_helper/search_backend.py
import random

def rand_point(space, rng=None):
    rng = rng or random
    trial = {}
    for k, (lo, hi) in space.items():
        if isinstance(lo, int) and isinstance(hi, int):
            trial[k] = rng.randint(int(lo), int(hi))
        else:
            v = rng.random() * (float(hi) - float(lo)) + float(lo)
            trial[k] = float(v)
    return trial

def get_optimizer(space, seed=42, force="auto", n_initial_points=8):
    if force == "random":
        print("Using random search (forced).")
        return ("random", None, list(space.keys()))
    try:
        from skopt import Optimizer
        from skopt.space import Integer, Real
    except Exception:
        if force == "skopt":
            raise
        print("scikit-optimize not found; falling back to random search.")
        return ("random", None, list(space.keys()))

    dims, names = [], []
    for k, (lo, hi) in space.items():
        names.append(k)
        if isinstance(lo, int) and isinstance(hi, int):
            dims.append(Integer(int(lo), int(hi), name=k))
        else:
            prior = "log-uniform" if (float(lo) > 0 and float(hi) > 0) else "uniform"
            dims.append(Real(float(lo), float(hi), prior=prior, name=k))

    opt = Optimizer(
        dimensions=dims,
        base_estimator="GP",
        acq_func="EI",
        acq_optimizer="auto",
        random_state=seed,
        n_initial_points=max(1, int(n_initial_points))
    )
    return ("skopt", opt, names)
