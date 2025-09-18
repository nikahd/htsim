# dcqcn_python_helper/scoring.py
from statistics import mean

from .goodput import build_goodput_bins, plot_goodput_bins
from .idle import compute_idle_fraction, compute_idle_windows, build_effective_sender_rate_from_idle
from .plots import draw_series  # <-- draw_series lives here
from .flow_summary import parse_trace_csv_fct_ecn, print_and_write_summary
from .perlink import estimate_link_util
from .paths import (
    fairness_plot_path, goodput_plot_path, summary_csv_for_trial, parse_paths_file_name_from_log
)
from .metrics_primitives import (
    parse_sending_rates, parse_current_rates,
    tail_mean_rates_from_sending, jain_index,
    convergence_time_us, ecn_per_packet
)
from .simwrap import run_sim_with_knobs

# ---- composite score formula -------------------------------------------------

def composite_score(avg_ms, p95_ms, jain, ecn_ppkt, tconv_us, sim_end_us,
                    w_avg=1.0, w_p95=0.5):
    denom = w_avg*avg_ms + w_p95*p95_ms
    if denom <= 0:
        return 0.0
    pconv = 1.0 + (tconv_us / max(sim_end_us, 1.0))
    s = (1000.0 / denom) * jain * (1.0 / (1.0 + ecn_ppkt)) / pconv
    if jain < 0.90:
        s *= 0.1
    return s

# ---- evaluate ONE matrix (used by the aggregator) ---------------------------

def evaluate_one_matrix(matrix, trial_knobs, args, tag, trial_seed):
    out_file, stat_file = run_sim_with_knobs(
        matrix=matrix, knobs=trial_knobs,
        folder_name=args.folder, binary_path=args.binary,
        drop_rate=args.drop_rate, link_down=args.link_down,
        use_jitter=args.use_jitter, exp=args.exp, seed=trial_seed
    )

    # Fairness (sender TARGET)
    fair_png = fairness_plot_path(stat_file, tag)
    target_rates = parse_sending_rates(stat_file)
    draw_series(target_rates, fair_png, f"{tag}: fairness", "Sending Rate (Mbps)")

    # Goodput + duration (+ per-link util CSV)
    gp_bins, sim_dur_us, flow_total_bytes = build_goodput_bins(args.trace_csv, bin_us=args.goodput_bin_us)
    gp_png = goodput_plot_path(stat_file, tag)
    plot_goodput_bins(gp_bins, gp_png, args.goodput_bin_us)

    estimate_link_util(args.paths_file or parse_paths_file_name_from_log(),
                       flow_total_bytes, sim_dur_us,
                       out_csv=f"link_util_est_{tag}.csv")

    # Idle → CURRENT / effective sender rate plot
    idle_frac = compute_idle_fraction("cnp_events.csv", sim_dur_us, trial_knobs)
    idle_w    = compute_idle_windows("cnp_events.csv", sim_dur_us, trial_knobs)
    cur_rates = parse_current_rates(stat_file) or \
                build_effective_sender_rate_from_idle(target_rates, idle_w)
    # Reuse goodput filename pattern for the current-rate plot if you want a separate one

    # Composite components
    csv_flow = parse_trace_csv_fct_ecn(args.trace_csv)
    fcts = [d["fct_us"] for d in csv_flow.values() if d.get("fct_us") is not None]
    if fcts:
        fcts.sort()
        avg_ms = (sum(fcts)/len(fcts))/1000.0
        p95_ms = fcts[int(0.95*(len(fcts)-1))] / 1000.0
    else:
        avg_ms = p95_ms = 0.0

    jain = jain_index(tail_mean_rates_from_sending(stat_file, tail_frac=args.fairness_tail_frac).values())
    ecn_ppkt = ecn_per_packet(csv_flow)
    tconv_us = convergence_time_us(stat_file, eps_frac=args.conv_eps, window_us=args.conv_window_us)

    # legacy per-matrix CSV
    trial_csv = summary_csv_for_trial(stat_file, tag)
    print_and_write_summary(
        args.trace_csv,
        args.paths_file or parse_paths_file_name_from_log(),
        idle_frac,
        gp_bins,
        args.goodput_bin_us,
        trial_csv_path=trial_csv
    )

    return {
        "avg_ms": avg_ms,
        "p95_ms": p95_ms,
        "jain": jain,
        "ecn_ppkt": ecn_ppkt,
        "tconv_us": tconv_us,
        "sim_end_us": sim_dur_us,
        "stat_file": stat_file,
        "fair_png": fair_png,
        "goodput_png": gp_png,
        "trial_csv": trial_csv,
    }

# ---- evaluate ACROSS matrices (what main() calls) ---------------------------

def evaluate_across_matrices(matrices, trial_knobs, args, tag, trial_seed):
    per = []
    for i, mat in enumerate(matrices):
        subtag = f"{tag}_{i}"
        comp = evaluate_one_matrix(mat, trial_knobs, args, subtag, trial_seed + i)
        per.append(comp)
        # quick line for visibility
        classic = (1000.0 / comp["avg_ms"]) if comp["avg_ms"] > 0 else 0.0
        print(f"[{subtag}] avg_ms={comp['avg_ms']:.3f} p95_ms={comp['p95_ms']:.3f} "
              f"jain={comp['jain']:.3f} ecn/packet={comp['ecn_ppkt']:.3f} "
              f"t*={comp['tconv_us']:.0f}us classic={classic:.3f}")

    # aggregate score (mean of per-matrix scores)
    scores = [
        composite_score(c["avg_ms"], c["p95_ms"], c["jain"], c["ecn_ppkt"],
                        c["tconv_us"], c["sim_end_us"],
                        w_avg=args.w_avg, w_p95=args.w_p95)
        for c in per
    ]
    S = mean(scores) if scores else 0.0
    avg_ms_mean = mean([c["avg_ms"] for c in per]) if per else 0.0

    last = per[-1] if per else {}
    print(f"[{tag}] S_mean={S:.3f} | avg_fct_ms_mean={avg_ms_mean:.3f}")
    return S, avg_ms_mean, last.get("stat_file"), last.get("fair_png"), last.get("goodput_png"), last.get("trial_csv")
