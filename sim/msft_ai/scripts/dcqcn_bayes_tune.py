#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os, sys, json, argparse, random
from pathlib import Path

from dcqcn_python_helper.constants import DCQCN_BASE_KNOBS, DCQCN_DEFAULTS, DEFAULT_KNOBS_SPACE
from dcqcn_python_helper.paths import plot_path_for_trial, summary_csv_for_trial, copy_as_best, parse_paths_file_name_from_log
from dcqcn_python_helper.plots import draw_time_series
from dcqcn_python_helper.goodput import build_goodput_bins, plot_goodput_bins
from dcqcn_python_helper.idle import compute_idle_fraction, compute_idle_windows, build_effective_sender_rate_from_idle
from dcqcn_python_helper.queues import maybe_plot_queues
from dcqcn_python_helper.perlink import estimate_link_util
from dcqcn_python_helper.flow_summary import parse_trace_csv_fct_ecn, print_and_write_summary
from dcqcn_python_helper.cnp import parse_cnp_events
from dcqcn_python_helper.simwrap import run_sim_with_knobs
from dcqcn_python_helper.scoring import (
    parse_sending_rates, parse_current_rates,
    tail_mean_rates_from_sending, jain_index,
    convergence_time_us, composite_score
)

# -------------------- Search strategies --------------------

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

# ---------------------- Evaluate (one trial) ----------------------

def evaluate(matrix, trial_knobs, args, tag, trial_seed):
    print(f"\n=== Trial {tag} knobs ===")
    for k in sorted(trial_knobs.keys()):
        print(f"  {k} = {trial_knobs[k]}")

    out_file, stat_file = run_sim_with_knobs(
        matrix=matrix, knobs=trial_knobs,
        folder_name=args.folder, binary_path=args.binary,
        drop_rate=args.drop_rate, link_down=args.link_down,
        use_jitter=args.use_jitter, exp=args.exp, seed=trial_seed
    )

    # Sender TARGET rate
    fairness_png = plot_path_for_trial(stat_file, tag, kind="fairness")
    rates = parse_sending_rates(stat_file)
    draw_time_series(rates, fairness_png, f"{tag}: {os.path.basename(fairness_png)}", "Sending Rate (Mbps)")

    # Goodput + sim duration + per-link util estimate
    gp_bins, sim_dur_us, flow_total_bytes = build_goodput_bins(args.trace_csv, bin_us=args.goodput_bin_us)
    goodput_png = plot_path_for_trial(stat_file, tag, kind="goodput")
    plot_goodput_bins(gp_bins, goodput_png, args.goodput_bin_us)

    # Idle fraction + sender CURRENT (parse if present; otherwise mask)
    idle_frac = compute_idle_fraction("cnp_events.csv", sim_dur_us, trial_knobs)
    idle_w    = compute_idle_windows("cnp_events.csv", sim_dur_us, trial_knobs)
    current_rates = parse_current_rates(stat_file) or build_effective_sender_rate_from_idle(rates, idle_w)
    sender_cur_png = plot_path_for_trial(stat_file, f"{tag}_sender_current", kind="sender_current")
    draw_time_series(current_rates, sender_cur_png, f"{tag}: Sender CURRENT rate", "Current Rate (Mbps)")

    # Queues (optional)
    maybe_plot_queues(stat_file, "queue_samples.csv", tag)

    # Per-link util estimate
    estimate_link_util(args.paths_file or parse_paths_file_name_from_log(),
                       flow_total_bytes, sim_dur_us,
                       out_csv=f"link_util_est_{tag}.csv")

    # Composite-score metrics
    csv_flow = parse_trace_csv_fct_ecn(args.trace_csv)
    fcts_us = [d["fct_us"] for d in csv_flow.values() if d.get("fct_us") is not None]
    if fcts_us:
        fcts_us.sort()
        avg_fct_ms_calc = (sum(fcts_us) / len(fcts_us)) / 1000.0
        p95_fct_ms = fcts_us[int(0.95 * (len(fcts_us) - 1))] / 1000.0
    else:
        avg_fct_ms_calc = 0.0
        p95_fct_ms = 0.0

    tail_means_mbps = tail_mean_rates_from_sending(stat_file, tail_frac=getattr(args, "fairness_tail_frac", 0.2))
    j_tail = jain_index(tail_means_mbps.values())

    # ECN per packet (across flows)
    pkts = sum(d.get("pkts", 0) for d in csv_flow.values())
    ecn  = sum(d.get("ecn", 0)  for d in csv_flow.values())
    ecn_ppkt = (ecn / pkts) if pkts > 0 else 0.0

    tconv_us = convergence_time_us(stat_file, eps_frac=getattr(args, "conv_eps", 0.05),
                                   window_us=getattr(args, "conv_window_us", 2000))

    S = composite_score(avg_fct_ms_calc, p95_fct_ms, j_tail, ecn_ppkt, tconv_us, sim_dur_us,
                        w_avg=getattr(args, "w_avg", 1.0),
                        w_p95=getattr(args, "w_p95", 0.5))

    # Legacy CSV/score (kept for continuity)
    trial_csv = summary_csv_for_trial(stat_file, tag)
    legacy_score, legacy_avg_ms = print_and_write_summary(
        args.trace_csv,
        args.paths_file or parse_paths_file_name_from_log(),
        idle_frac,
        gp_bins,
        args.goodput_bin_us,
        trial_csv_path=trial_csv
    )

    classic = (1000.0 / avg_fct_ms_calc) if avg_fct_ms_calc > 0 else 0.0
    print(f"[metrics] avg_ms={avg_fct_ms_calc:.3f} p95_ms={p95_fct_ms:.3f} "
          f"jain={j_tail:.3f} ecn/packet={ecn_ppkt:.3f} "
          f"t*={tconv_us:.0f}us S={S:.3f} (legacy={classic:.3f})")

    print(f"[{tag}] score={S:.3f} | avg_fct_ms={avg_fct_ms_calc:.3f}")
    return S, avg_fct_ms_calc, out_file, stat_file, fairness_png, goodput_png, trial_csv

# ---------------------- CLI / Main ----------------------

def parse_inline_knobs(kv_list):
    out = {}
    if not kv_list: return out
    for kv in kv_list:
        if "=" not in kv: continue
        k, v = kv.split("=", 1); k = k.strip(); v = v.strip()
        try:
            if v.lower().startswith("0x"): out[k] = int(v, 16)
            elif "." in v: out[k] = float(v)
            else: out[k] = int(v)
        except Exception:
            try: out[k] = float(v)
            except Exception: out[k] = v
    return out

def main():
    ap = argparse.ArgumentParser(description="DCQCN search or fixed evaluation with enhanced plots/metrics.")
    ap.add_argument("--mode", choices=["search","fixed"], default="search")
    ap.add_argument("--run-sim", metavar="MATRIX", required=True)
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

    # Search
    ap.add_argument("--max-evals", type=int, default=10)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--force-search", choices=["auto","random","skopt"], default="auto")
    ap.add_argument("--n-initial-points", type=int, default=8)

    # Fixed-mode knobs
    ap.add_argument("--knobs-json", default=None)
    ap.add_argument("--knob", action="append", default=None)
    ap.add_argument("--default-knobs", default="/htsim/scripts/msft_ai_dcqcn_knobs_matrices/tuned_knobs.json")

    args = ap.parse_args()
    random.seed(args.seed)

    if args.mode == "fixed":
        fixed = {}
        if args.knobs_json:
            try:
                with open(args.knobs_json, "r") as f:
                    data = json.load(f)
                    if isinstance(data, dict): fixed.update(data)
                    else: print("WARN: knobs-json is not a dict; ignoring.", file=sys.stderr)
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
                        else:
                            print(f"WARN: default knobs at {auto_knobs} is not a dict", file=sys.stderr)
                except Exception as e:
                    print(f"WARN: failed to read default knobs {auto_knobs}: {e}", file=sys.stderr)
            else:
                print(f"WARN: default knobs file not found: {auto_knobs}", file=sys.stderr)

        fixed.update(parse_inline_knobs(args.knob))
        tag = "fixed"
        score, avg_fct_ms, out_file, stat_file, fairness_png, gp_png, trial_csv = evaluate(
            args.run_sim, fixed, args, tag, trial_seed=args.seed
        )
        copy_as_best(fairness_png, trial_csv, stat_file, goodput_png=gp_png)
        print("\n==================== Fixed run results ====================")
        print(f"score={score:.3f} | avg_fct_ms={avg_fct_ms:.3f}")
        print("Applied knobs:"); print(json.dumps(fixed, indent=2, sort_keys=True))
        return

    search_kind, opt, names = get_optimizer(
        DEFAULT_KNOBS_SPACE, seed=args.seed,
        force=args.force_search, n_initial_points=args.n_initial_points
    )

    best_score   = -1.0
    best_summary = None

    for i in range(args.max_evals):
        tag = f"trial_{i}"
        if search_kind == "skopt":
            x = opt.ask()
            trial_knobs = {names[j]: x[j] for j in range(len(names))}
        else:
            trial_knobs = rand_point(DEFAULT_KNOBS_SPACE, rng=random)

        # coerce numeric types per space
        coerced = {}
        for k, v in trial_knobs.items():
            lo, hi = DEFAULT_KNOBS_SPACE[k]
            coerced[k] = int(round(v)) if isinstance(lo, int) and isinstance(hi, int) else float(v)
        trial_knobs = coerced

        score, avg_fct_ms, out_file, stat_file, fairness_png, goodput_png, trial_csv = evaluate(
            args.run_sim, trial_knobs, args, tag, trial_seed=args.seed + i
        )

        if search_kind == "skopt":
            opt.tell(list(trial_knobs.values()), -score)

        if score > best_score:
            best_score = score
            best_summary = (i, trial_knobs, score, avg_fct_ms, stat_file, fairness_png, goodput_png, trial_csv)
            copy_as_best(fairness_png, trial_csv, stat_file, goodput_png)

    print("\n==================== Best configuration ====================")
    if best_summary is None:
        print("No successful trials."); return
    i, knobs, score, avg_fct_ms, stat_file, fairness_png, goodput_png, trial_csv = best_summary
    print(f"trial #{i}: score={score:.3f} | avg_fct_ms={avg_fct_ms:.3f}")
    print(json.dumps(knobs, indent=2, sort_keys=True))
    folder = os.path.dirname(stat_file)
    print("Artifacts:")
    print(f"  Fairness plots: {os.path.join(folder, 'option_plots')}/fairness_*__trial_*.png")
    print(f"  Goodput plots:  {os.path.join(folder, 'option_plots')}/goodput_*__trial_*.png")
    print(f"  Trial CSVs:     {folder}/dcqcn_flow_summary_trial_*.csv")
    print(f"  Best fairness:  {os.path.join(folder, 'option_plots', 'fairness_best.png')}")
    print(f"  Best goodput:   {os.path.join(folder, 'option_plots', 'goodput_best.png')}")
    print(f"  Best CSV:       {os.path.join(folder, 'dcqcn_flow_summary_best.csv')}")

if __name__ == "__main__":
    main()


"""
usage: dcqcn_bayes_tune.py [-h] [--mode {search,fixed}] --run-sim MATRIX [--binary BINARY] [--folder FOLDER] [--trace-csv TRACE_CSV] [--paths-file PATHS_FILE] [--drop-rate DROP_RATE] [--link-down LINK_DOWN] [--use-jitter USE_JITTER] [--exp EXP] [--goodput-bin-us GOODPUT_BIN_US]
                           [--w-avg W_AVG] [--w-p95 W_P95] [--conv-eps CONV_EPS] [--conv-window-us CONV_WINDOW_US] [--fairness-tail-frac FAIRNESS_TAIL_FRAC] [--max-evals MAX_EVALS] [--seed SEED] [--force-search {auto,random,skopt}] [--n-initial-points N_INITIAL_POINTS]
                           [--knobs-json KNOBS_JSON] [--knob KNOB] [--default-knobs DEFAULT_KNOBS]
"""

