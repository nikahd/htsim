#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys
import json
import argparse
import random
from statistics import geometric_mean

from dcqcn_python_helper.constants import (
    DCQCN_BASE_KNOBS, DCQCN_DEFAULTS, DEFAULT_KNOBS_SPACE
)
from dcqcn_python_helper.paths import find_matrix_path, base_noext
from dcqcn_python_helper.sim_runner import run_sim_with_knobs
from dcqcn_python_helper.plots_and_metrics import (
    plot_path_for_trial, goodput_plot_path_for_trial, queue_plot_path_for_trial,
    summary_csv_for_trial, copy_as_best, parse_paths_file_name_from_log,
    parse_sending_rates, draw_fairness_plot,
    build_goodput_bins, plot_goodput_bins,
    compute_idle_fraction, compute_idle_windows, parse_current_rates,
    build_effective_sender_rate_from_idle, draw_rate_series, maybe_plot_queues,
    estimate_link_util, parse_trace_csv_fct_ecn,
    tail_mean_rates_from_sending, jain_index, convergence_time_us,
    composite_score, print_and_write_summary
)

# ---------------- Search strategies ----------------
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

# ---------------- CLI helpers ----------------
def parse_inline_knobs(kv_list):
    out = {}
    if not kv_list:
        return out
    for kv in kv_list:
        if "=" not in kv:
            continue
        k, v = kv.split("=", 1)
        k = k.strip(); v = v.strip()
        try:
            if v.lower().startswith("0x"):
                out[k] = int(v, 16)
            elif "." in v:
                out[k] = float(v)
            else:
                out[k] = int(v)
        except Exception:
            try:
                out[k] = float(v)
            except Exception:
                out[k] = v
    return out

def normalize_matrices(run_sim_args):
    """Support repeated --run-sim flags and/or comma-separated lists."""
    mats = []
    for item in run_sim_args:
        parts = [p.strip() for p in item.split(",") if p.strip()]
        mats.extend(parts)
    # de-dup while preserving order
    seen = set()
    out = []
    for m in mats:
        if m not in seen:
            out.append(m)
            seen.add(m)
    return out

# ---------------- Per-matrix evaluation ----------------
def evaluate_one_matrix(matrix, trial_knobs, args, tag, run_seed):
    """
    Run the simulator for ONE matrix, generate plots/CSVs, and compute composite metrics.
    Returns a dict with all the pieces needed for aggregation and logging.
    """
    # 1) Run sim
    out_file, stat_file = run_sim_with_knobs(
        matrix=matrix, knobs=trial_knobs,
        folder_name=args.folder, binary_path=args.binary,
        drop_rate=args.drop_rate, link_down=args.link_down,
        use_jitter=args.use_jitter, exp=args.exp, seed=run_seed
    )
    
    # cleanup giant raw sim output to save disk space ---
    try:
        if os.path.exists(out_file):
            os.remove(out_file)
            print(f"Deleted raw sim log: {out_file}")
    except Exception as e:
        print(f"WARNING: could not delete {out_file}: {e}")

    # 2) Parse receiver trace FIRST to learn sim_dur_us (align x-axes across plots)
    gp_bins, sim_dur_us, flow_total_bytes = build_goodput_bins(
        args.trace_csv, bin_us=args.goodput_bin_us
    )

    # Sender TARGET plot (optionally capped to sim_dur_us)
    fairness_png = plot_path_for_trial(stat_file, tag)
    rates = parse_sending_rates(stat_file)
    draw_fairness_plot(
        rates, fairness_png, f"{tag}: {os.path.basename(fairness_png)}",
        sim_dur_us=sim_dur_us
    )

    # Receiver goodput (same x-axis)
    goodput_png = goodput_plot_path_for_trial(stat_file, tag)
    plot_goodput_bins(gp_bins, goodput_png, args.goodput_bin_us, sim_dur_us=sim_dur_us)

    # Idle windows from CNP cooldown reconstruction
    idle_frac = compute_idle_fraction("cnp_events.csv", sim_dur_us, trial_knobs)
    idle_w    = compute_idle_windows("cnp_events.csv", sim_dur_us, trial_knobs)

    # Sender CURRENT (native if logged; else mask TARGET during idle windows)
    current_rates = parse_current_rates(stat_file)
    if not current_rates:
        current_rates = build_effective_sender_rate_from_idle(rates, idle_w)
    sender_cur_png = goodput_plot_path_for_trial(stat_file, f"{tag}_sender_current")
    draw_rate_series(
        current_rates, sender_cur_png, f"{tag}: Sender CURRENT rate",
        "Current Rate (Mbps)", sim_dur_us=sim_dur_us
    )

    # Optional queue plot
    maybe_plot_queues(stat_file, "queue_samples.csv", tag)

    # Per-link util (trial-specific CSV to avoid collisions)
    estimate_link_util(args.paths_file or parse_paths_file_name_from_log(),
                       flow_total_bytes, sim_dur_us,
                       out_csv=f"link_util_est_{tag}.csv")

    # 3) Composite-score metrics for this matrix
    csv_flow = parse_trace_csv_fct_ecn(args.trace_csv)
    fcts_us = [d["fct_us"] for d in csv_flow.values() if d.get("fct_us") is not None]
    if fcts_us:
        fcts_us_sorted = sorted(fcts_us)
        avg_fct_ms = (sum(fcts_us_sorted) / len(fcts_us_sorted)) / 1000.0
        p95_idx = int(0.95 * (len(fcts_us_sorted) - 1))
        p95_fct_ms = fcts_us_sorted[p95_idx] / 1000.0
    else:
        avg_fct_ms = 0.0
        p95_fct_ms = 0.0

    tail_means_mbps = tail_mean_rates_from_sending(stat_file, tail_frac=args.fairness_tail_frac)
    j_tail = jain_index(tail_means_mbps.values())

    # ECN per packet
    pkts = sum(d.get("pkts", 0) for d in csv_flow.values())
    ecn  = sum(d.get("ecn",  0) for d in csv_flow.values())
    ecn_ppkt = (ecn / pkts) if pkts > 0 else 0.0

    # Convergence time
    tconv_us = convergence_time_us(stat_file, eps_frac=args.conv_eps, window_us=args.conv_window_us)

    # Composite score for THIS matrix
    S = composite_score(avg_fct_ms, p95_fct_ms, j_tail, ecn_ppkt, tconv_us, sim_dur_us,
                        w_avg=args.w_avg, w_p95=args.w_p95)

    # Legacy per-matrix CSV (+ prints)
    trial_csv = summary_csv_for_trial(stat_file, tag)
    legacy_score, legacy_avg_ms = print_and_write_summary(
        args.trace_csv, args.paths_file or parse_paths_file_name_from_log(),
        idle_frac, gp_bins, args.goodput_bin_us, trial_csv_path=trial_csv
    )

    # Friendly line
    mat_base = base_noext(find_matrix_path(matrix))
    print(f"[{tag} | {mat_base}] avg_ms={avg_fct_ms:.3f} p95_ms={p95_fct_ms:.3f} "
          f"jain={j_tail:.3f} ecn/packet={ecn_ppkt:.3f} t*={tconv_us:.0f}us "
          f"S={S:.3f} (legacy={ (1000.0/avg_fct_ms) if avg_fct_ms>0 else 0.0 :.3f})")

    return {
        "matrix": matrix,
        "matrix_base": mat_base,
        "score_S": S,
        "avg_fct_ms": avg_fct_ms,
        "p95_fct_ms": p95_fct_ms,
        "jain": j_tail,
        "ecn_per_pkt": ecn_ppkt,
        "tconv_us": tconv_us,
        "sim_dur_us": sim_dur_us,
        "artifacts": {
            "out_file": out_file,
            "stat_file": stat_file,
            "fairness_png": fairness_png,
            "goodput_png": goodput_png,
            "sender_current_png": sender_cur_png,
            "trial_csv": trial_csv
        }
    }

# ---------------- Seed policy ----------------
def compute_run_seed(base_seed, trial_index, matrix_index, policy):
    """
    Return the seed to pass to the simulator according to policy.
    Policies:
      - 'fixed':                base
      - 'per-matrix':           base + matrix_index
      - 'per-trial':            base + trial_index
      - 'per-trial-and-matrix': base + trial_index + matrix_index
    """
    if policy == "fixed":
        return base_seed
    if policy == "per-matrix":
        return base_seed + matrix_index
    if policy == "per-trial":
        return base_seed + trial_index
    # default (legacy)
    return base_seed + trial_index + matrix_index

# ---------------- Multi-matrix evaluation (aggregate) ----------------
def aggregate_scores(per_matrix_results, mode="geomean", weights=None):
    scores = [r["score_S"] for r in per_matrix_results]
    if not scores:
        return 0.0
    if mode == "min":
        return min(scores)
    if mode == "mean":
        if weights and len(weights) == len(scores):
            s = sum(w*s for w,s in zip(weights, scores))
            wsum = sum(weights) if sum(weights) > 0 else 1.0
            return s / wsum
        return sum(scores) / len(scores)
    # default geomean
    try:
        return geometric_mean([max(s, 1e-9) for s in scores])
    except Exception:
        return sum(scores) / len(scores)

def evaluate_across_matrices(matrices, trial_knobs, args, tag, trial_index):
    per_matrix_results = []
    for m_idx, mat in enumerate(matrices):
        # Respect the selected seed policy
        run_seed = compute_run_seed(args.seed, trial_index, m_idx, args.seed_policy)
        res = evaluate_one_matrix(
            mat, trial_knobs, args,
            f"{tag}__{base_noext(find_matrix_path(mat))}",
            run_seed
        )
        per_matrix_results.append(res)

    weights = None
    if args.matrix_weights:
        try:
            weights = [float(x) for x in args.matrix_weights.split(",")]
        except Exception:
            weights = None
    agg_S = aggregate_scores(per_matrix_results, mode=args.aggregate, weights=weights)

    ms_vals = [r["avg_fct_ms"] for r in per_matrix_results if r["avg_fct_ms"] > 0]
    agg_avg_ms = (sum(ms_vals)/len(ms_vals)) if ms_vals else 0.0

    mats_str = ",".join(r["matrix_base"] for r in per_matrix_results)
    print(f"[{tag} | aggregate over {mats_str}] S={agg_S:.3f} | avg_ms≈{agg_avg_ms:.3f}")

    return agg_S, agg_avg_ms, per_matrix_results

# ---------------- Main ----------------
def main():
    ap = argparse.ArgumentParser(
        description="DCQCN search or fixed evaluation with multi-matrix support."
    )
    ap.add_argument("--mode", choices=["search","fixed"], default="search")
    ap.add_argument("--run-sim", action="append", required=True,
                    help="Connection matrix filename or path. May be repeated or comma-separated.")
    ap.add_argument("--binary", default="./build/msft_ai_wan_single_dcqcn")
    ap.add_argument("--folder", default="msft_ai_wan_single_dcqcn")
    ap.add_argument("--trace-csv", default="trace_packets.csv")
    ap.add_argument("--paths-file", default="uec_entry.paths")
    ap.add_argument("--drop-rate", default="0")
    ap.add_argument("--link-down", type=int, default=0)
    ap.add_argument("--use-jitter", type=int, default=0)
    ap.add_argument("--exp", type=int, default=1)
    ap.add_argument("--goodput-bin-us", type=int, default=1000)

    # Composite score params
    ap.add_argument("--w-avg", type=float, default=1.0, dest="w_avg")
    ap.add_argument("--w-p95", type=float, default=0.5, dest="w_p95")
    ap.add_argument("--conv-eps", type=float, default=0.05, dest="conv_eps")
    ap.add_argument("--conv-window-us", type=int, default=2000, dest="conv_window_us")
    ap.add_argument("--fairness-tail-frac", type=float, default=0.20, dest="fairness_tail_frac")

    # Aggregation across matrices
    ap.add_argument("--aggregate", choices=["mean","geomean","min"], default="geomean")
    ap.add_argument("--matrix-weights", default=None,
                    help="Comma-separated weights aligned with --run-sim matrices (used only with --aggregate=mean).")

    # Search settings
    ap.add_argument("--max-evals", type=int, default=10)
    ap.add_argument("--seed", type=int, default=42)

    # Seed policy
    ap.add_argument("--seed-policy",
                    choices=["fixed","per-matrix","per-trial","per-trial-and-matrix"],
                    default="per-matrix",
                    help=("How seeds change across matrices/trials. "
                          "'fixed' = same seed everywhere; "
                          "'per-matrix' = seed+matrix_idx (stable across trials); "
                          "'per-trial' = seed+trial_idx; "
                          "'per-trial-and-matrix' = seed+trial_idx+matrix_idx (legacy)."))

    ap.add_argument("--force-search", choices=["auto","random","skopt"], default="auto")
    ap.add_argument("--n-initial-points", type=int, default=8)

    # Fixed-mode knobs
    ap.add_argument("--knobs-json", default=None)
    ap.add_argument("--knob", action="append", default=None)
    ap.add_argument("--default-knobs", default="/htsim/scripts/msft_ai_dcqcn_knobs_matrices/tuned_knobs.json")

    args = ap.parse_args()

    # Normalize matrices
    matrices = normalize_matrices(args.run_sim)
    if not matrices:
        print("No connection matrices provided.", file=sys.stderr)
        sys.exit(1)
    print("Matrices to optimize over:", ", ".join(matrices))

    random.seed(args.seed)

    # -------- FIXED mode --------
    if args.mode == "fixed":
        fixed = {}
        if args.knobs_json:
            try:
                with open(args.knobs_json, "r") as f:
                    data = json.load(f)
                    if isinstance(data, dict):
                        fixed.update(data)
                    else:
                        print("WARN: knobs-json is not a dict; ignoring.", file=sys.stderr)
            except Exception as e:
                print(f"WARN: failed to read knobs-json: {e}", file=sys.stderr)
        else:
            auto_knobs = args.default_knobs
            if os.path.exists(auto_knobs):
                try:
                    with open(auto_knobs, "r") as f:
                        data = json.load(f)
                        if isinstance(data, dict):
                            print(f"Using default knobs from: {auto_knobs}")
                            fixed.update(data)
                except Exception as e:
                    print(f"WARN: failed to read default knobs {auto_knobs}: {e}", file=sys.stderr)

        fixed.update(parse_inline_knobs(args.knob))

        tag = "fixed"
        # trial_index = 0 so seed policy applies deterministically
        score, avg_fct_ms, per_matrix = evaluate_across_matrices(
            matrices, fixed, args, tag, trial_index=0
        )

        for res in per_matrix:
            suf = f"__{res['matrix_base']}"
            copy_as_best(
                res["artifacts"]["fairness_png"],
                res["artifacts"]["trial_csv"],
                res["artifacts"]["stat_file"],
                goodput_png=res["artifacts"]["goodput_png"],
                suffix=suf
            )

        print("\n==================== Fixed run results (aggregate) ====================")
        print(f"score={score:.3f} | avg_fct_ms≈{avg_fct_ms:.3f}")
        print("Applied knobs:")
        print(json.dumps(fixed, indent=2, sort_keys=True))
        return

    # -------- SEARCH mode --------
    search_kind, opt, names = get_optimizer(
        DEFAULT_KNOBS_SPACE, seed=args.seed, force=args.force_search, n_initial_points=args.n_initial_points
    )

    best_score = -1.0
    best_summary = None

    for i in range(args.max_evals):
        tag = f"trial_{i}"

        if search_kind == "skopt":
            x = opt.ask()
            trial_knobs = {names[j]: x[j] for j in range(len(names))}
        else:
            trial_knobs = rand_point(DEFAULT_KNOBS_SPACE, rng=random)

        # coerce types per space
        coerced = {}
        for k, v in trial_knobs.items():
            lo, hi = DEFAULT_KNOBS_SPACE[k]
            coerced[k] = int(round(v)) if isinstance(lo, int) and isinstance(hi, int) else float(v)
        trial_knobs = coerced

        # IMPORTANT: pass the trial index so seed policy can handle it
        score, avg_fct_ms, per_matrix = evaluate_across_matrices(
            matrices, trial_knobs, args, tag, trial_index=i
        )

        if search_kind == "skopt":
            opt.tell(list(trial_knobs.values()), -score)

        if score > best_score:
            best_score = score
            best_summary = (i, trial_knobs, score, avg_fct_ms, per_matrix)

            for res in per_matrix:
                suf = f"__{res['matrix_base']}"
                copy_as_best(
                    res["artifacts"]["fairness_png"],
                    res["artifacts"]["trial_csv"],
                    res["artifacts"]["stat_file"],
                    goodput_png=res["artifacts"]["goodput_png"],
                    suffix=suf
                )

    print("\n==================== Best configuration (aggregate) ====================")
    if best_summary is None:
        print("No successful trials.")
        return
    i, knobs, score, avg_fct_ms, per_matrix = best_summary
    print(f"trial #{i}: score={score:.3f} | avg_fct_ms≈{avg_fct_ms:.3f}")
    print(json.dumps(knobs, indent=2, sort_keys=True))
    print("Per-matrix scores:")
    for res in per_matrix:
        print(f"  {res['matrix_base']}: S={res['score_S']:.3f}, avg_ms={res['avg_fct_ms']:.3f}")
    print("Artifacts:")
    if per_matrix:
        folder = os.path.dirname(per_matrix[0]["artifacts"]["stat_file"])
        print(f"  Fairness plots: {os.path.join(folder, 'option_plots')}/fairness_*__trial_*.png")
        print(f"  Goodput plots:  {os.path.join(folder, 'option_plots')}/goodput_*__trial_*.png")
        print(f"  Trial CSVs:     {folder}/dcqcn_flow_summary_*.csv")
        print(f"  Best fairness:  {os.path.join(folder, 'option_plots', 'fairness_best__*.png')}")
        print(f"  Best goodput:   {os.path.join(folder, 'option_plots', 'goodput_best__*.png')}")
        print(f"  Best CSVs:      {folder}/dcqcn_flow_summary_best__*.csv")

if __name__ == "__main__":
    main()



"""
python3 sim/msft_ai/scripts/dcqcn_bayes_tune.py \
  --run-sim one_one_4_200MB.cm --run-sim one_one_2_200MB.cm \
  --seed 42 --seed-policy fixed \
  --max-evals 8

python3 sim/msft_ai/scripts/dcqcn_bayes_tune.py \
  --run-sim one_one_4_200MB.cm --run-sim one_one_2_200MB.cm \
  --seed 42 --seed-policy per-matrix \
  --max-evals 8

  --seed-policy per-trial-and-matrix
-----------------------------------------------------------------------------------------------------

python3 sim/msft_ai/scripts/dcqcn_bayes_tune.py \
  --run-sim one_one_8_200MB.cm --run-sim one_one_8_400MB.cm --run-sim one_one_8_800MB.cm \
  --seed 43 --seed-policy fixed --max-evals 20 \
  --conv-eps 10.0 --conv-window-us 1 \

"""