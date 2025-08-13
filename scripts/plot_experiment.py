import argparse
import itertools
import json
import math
import os
import re
import textwrap
from collections import defaultdict
from dataclasses import dataclass, fields
from typing import Any, Callable, DefaultDict, List, Optional
from enum import Enum
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns
import scipy.stats
from common import NS_TO_US, TRAFFIC_GEN_CDF_FILES_ROOT, try_except_wrapper, SCRIPTS_PATH
from matplotlib.axis import Axis
from matplotlib.figure import Figure
from run_experiment import CM_DIR, Params
from scipy import stats
import matplotlib
matplotlib.use("Agg")

lss = ['solid', 'dashdot', 'dashed', 'dotted',] * 10
ms = ['o', 's', 'v', 'D', 'x', 'P', 'p', 'H', 'X', 'd', '1', '2', '3', '4', '8', 'h', 'H', '+', 'x', '|', '_'] * 10
linewidth = 3

def get_core_name(base_scheme: str):
    core_scheme = base_scheme.split('+')[1].split('(')[0].split(':')[0]
    if "no_trim" in base_scheme:
        if "pflr" in base_scheme:
            return f"{core_scheme}(No Trimming, w/ PFLR)"
        else:
            return f"{core_scheme}(No Trimming, w/ RTO)"
    return core_scheme

prettify_name = {"rss": "RSS",
                 "uss": "USS",
                 "reps": "REPS",
                 "oblivious": "RPS",
                 "flowbender": "PLB",
                 "ecmp": "ECMP",
                 "FrozenRSS": "Stable RSS",
                 "rss(No Trimming, w/ RTO)": "RSS(No Trimming, w/ RTO)",
                 "rss(No Trimming, w/ PFLR)": "RSS(No Trimming, w/ PFLR)",
                 "uss(No Trimming, w/ RTO)": "USS(No Trimming, w/ RTO)",
                 "uss(No Trimming, w/ PFLR)": "USS(No Trimming, w/ PFLR)"}

color_palette = sns.color_palette("hls", len(prettify_name))

standard_colors = {k: color_palette[i] for i, k in enumerate(prettify_name.keys())}

markers_list = ["v", "1", "8", "d", "p", "*", "+", "x", "|", "s", "P", "X"]
standard_markers = {k: markers_list[i] for i, k in enumerate(prettify_name.keys())}
matplotlib.rcParams.update({'font.size': 18})

class LoadBalancingAlgos(Enum):
    BITMAP = 0
    REPS = 1
    OBLIVIOUS = 2
    MIXED = 3
    RSS = 4
    ECMP = 5
    FLOWBENDER = 6
    USS = 7

@dataclass
class Experiment:
    dpath: str

    cli_params: Params  # passed to htsim
    sim_params_df: pd.DataFrame  # output by htsim
    flow_info_df: pd.DataFrame
    sink_stats_df: pd.DataFrame

    def __init__(self, dpath: str):
        assert dpath.endswith('.htsim_data')
        self.dpath = dpath

        cli_params_path = os.path.join(dpath, 'cliParams.json')
        with open(cli_params_path, 'r') as f:
            self.cli_params = Params(**json.load(f))

        sim_params_path = os.path.join(dpath, 'globalInfo.csv')
        self.sim_params_df = pd.read_csv(sim_params_path)

        flow_info_path = os.path.join(dpath, 'flowsInfo.csv')
        self.flow_info_df = pd.read_csv(flow_info_path)

        sink_stats_path = os.path.join(dpath, 'sinkStats.csv')
        self.sink_stats_df = pd.read_csv(sink_stats_path)

    @property
    def topo_file(self):
        tiers = f":tiers={self.cli_params.topology_tiers}" if self.cli_params.topology_tiers != 3 else ""
        return f'{SCRIPTS_PATH}/topologies/topo={self.cli_params.topo}:over_sub={self.cli_params.over_sub}:nodes={self.cli_params.nodes}:link_speed_gbps={self.cli_params.link_speed_gbps}{tiers}.topo'
    
    @property
    def min_rtt_ms_tiers(self):
        tier = -1
        latencies = []
        with open(self.topo_file) as f:
            for line in f:
                if f"Tier {tier + 1}\n" in line:
                    tier += 1
                if line.startswith("Downlink_Latency_ns"):
                    assert len(latencies) == tier
                    latencies.append(float(line.split()[1]) * NS_TO_US)
        return [2 * l for l in latencies]
    
    def __repr__(self) -> str:
        return self.dpath

@dataclass
class CCLB:
    cc: str
    lb: str
    rss_metric: str = None
    sf: int = None # Subflow count
    ui: float = None # Update interval in us 
    ft: float = None # Threshold for frozen RSS
    sr: int = None # Max number of skipped rounds
    no_trim: bool = False # Use packet drops instead of trimming
    pflr: bool = False # Use pflr

    def __post_init__(self):
        for field_info in fields(self):
            val = getattr(self, field_info.name)
            if val is not None:
                setattr(self, field_info.name, field_info.type(val))

def parse_experiments(input_dir) -> DefaultDict[str, List[Experiment]]:
    assert os.path.isdir(input_dir), f'Invalid input directory: {input_dir}'

    ret = defaultdict(list)
    for root, dirs, _ in os.walk(input_dir):
        for dir in dirs:
            if dir.endswith('.htsim_data'):
                dpath = os.path.join(root, dir)
                if not os.path.isfile(os.path.join(dpath, "globalInfo.csv")):
                    print(f"Skipped {dpath} because we could not find globalInfo.csv.")
                    continue
                try:
                    ret[root].append(Experiment(dpath))    
                except FileNotFoundError:
                    print(f"Skipped {dpath} because we could not find cliParams, flowInfo, or sinkStats.")
                    continue
    return ret


def filter_worse_rss_exps(sorted_exps: List[Experiment], xval: Callable[[Experiment], Any], uss=True) -> List[Experiment]:
    res = []
    best_fct = {"rss": np.inf, "SkippingRSS": np.inf, "FrozenRSS": np.inf}
    best_exp = {"rss": None, "SkippingRSS": None, "FrozenRSS": None}
    uss_params = 8
    for exp in sorted_exps:
        lb = exp.cli_params.scheme_str.split("+")[1].split("(")[0]
        if lb not in best_fct.keys():
            res.append(exp)
        else:
            if (not uss or exp.cli_params.rss_number_of_subflows == uss_params) and best_fct[lb] > (max_xp_fct := max(xval(exp.flow_info_df))):
                best_fct[lb] = max_xp_fct
                best_exp[lb] = exp
    res.extend(list(best_exp.values()))
    res = [r for r in res if r is not None]
    return res


def plot_cdf(
    experiments: List[Experiment],
    xval: Callable[[Experiment], Any],
    xlabel: str,
    fpath: str,
    extra: Callable[[Figure, Axis, Experiment], None] = lambda _fig, _ax, _exp: None,
    xlim=(None, None),
    title: str = "",
    simplify_plots=False
):
    fig, ax = plt.subplots()
    sorted_exps = sorted(experiments, key=lambda x: x.cli_params.scheme_str)
    if args.rss:
        sorted_exps = filter_worse_rss_exps(sorted_exps, xval)
    for exp in sorted_exps:
        scheme = exp.cli_params.scheme_str
        exp.flow_info_df[['src', 'dst', 'flow_id']] = exp.flow_info_df['srcNode_dstNode_flowId'].str.split('_', n=2, expand=True).apply(pd.to_numeric)
        # flow_info_df_fg = exp.flow_info_df[(1 - exp.flow_info_df["ecmp_flow"]).astype(bool)]
        flow_info_df_fg = exp.flow_info_df
        x_fg = xval(flow_info_df_fg)
        if len(x_fg) == 0:
            continue
        if exp.cli_params.disable_trim:
            trim_label = ""
        else:
            trim_label = f", {exp.sink_stats_df['trimmed'].sum()}, {exp.sink_stats_df['trimmed'].sum() / exp.sink_stats_df['received'].sum():.2E}"
        if simplify_plots:
            short_scheme = get_core_name(scheme)
            items = {"label": f"{prettify_name[short_scheme]}", "color": standard_colors[short_scheme], "linewidth": linewidth}
            title = ""
        else:
            items= {"label": f"{scheme} ({np.mean(x_fg):.2f}, {np.percentile(x_fg, 99):.2f}, {len(x_fg)}{trim_label})"}
        # items= {"label": f"{scheme} ({np.mean(x_fg):.2f}, {np.percentile(x_fg, 99):.2f})"}
        res = stats.ecdf(x_fg)
        res.cdf.plot(ax, **items)
    ax.set_xlabel(xlabel)
    ax.grid(True)
    ax.set_xlim(xlim)
    ax.set_title(title)
    extra(fig, ax, experiments[0])
    ax.set_xlim((lim * 1.1 for lim in ax.get_xlim()))
    
    ax.set_ylabel('CDF')
    # ax.legend(title=f"Scheme (mean FCT, p99 FCT, # flows{f'' if False else ', #trimmed, frac. trimmed'})", bbox_to_anchor=(1, 0), loc='lower left')
    ax.legend(title=f"Scheme (mean FCT, p99 FCT)", bbox_to_anchor=(1, 0), loc='lower left')
    # fig.tight_layout(pad=0.01)
    # ax.legend()
    # ax.legend(bbox_to_anchor=(1, 0), loc='lower left')
    fig.savefig(fpath, bbox_inches='tight', pad_inches=0.01, dpi=300)
    print(fpath)
    plt.close(fig)


def plot_slowdown_vs_msg_size(
    experiments: List[Experiment],
    fpath,
    title: str = ""
):
    fig, ax = plt.subplots()
    sorted_exps = sorted(experiments, key=lambda x: x.cli_params.scheme_str)
    for i, exp in enumerate(sorted_exps):
        p = exp.cli_params
        scheme = p.scheme_str

        df = exp.flow_info_df
        if len(df) == 0:
            continue

        # Compute slowdown
        df = df.sort_values(by='flowSizeBytes').reset_index(drop=True)
        df['theo_fct_us'] = df['flowSizeBytes'].apply(lambda x: get_single_fct_us(p, x))
        df['slowdown'] = df["fctNs"] * NS_TO_US / df['theo_fct_us']

        # Compute p99 slowdown for across msgs of the same size
        group_key = 'flowSizeBytes'
        tdf = df.groupby(group_key)['slowdown'].apply(lambda x: np.percentile(x, 99))
        df['slowdown_p99'] = df[group_key].apply(lambda x: tdf[x])

        x = df["slowdown"]
        label = f"{scheme} ({scipy.stats.gmean(x):.2f}, {np.percentile(x, 99):.2f}, {len(x)})"

        # Each msg gets one point on the x axis (helps reflect msg size
        # distribution)
        n = len(df)
        ax.step(range(n), df['slowdown_p99'], label=label, where='post', ls=lss[i])

    p = sorted_exps[0].cli_params
    ecdf = stats.ecdf(df['flowSizeBytes'])
    pkt_percentile = ecdf.cdf.evaluate(p.mss_bytes)
    bdp_percentile = ecdf.cdf.evaluate(p.bdp_transport_bytes)
    pkt_x = pkt_percentile * n
    bdp_x = bdp_percentile * n
    ax.axvline(x=pkt_x, color='gray', linestyle='--', label="MSS")
    ax.axvline(x=bdp_x, color='gray', linestyle='--', label="Payload BDP")

    xticks = ax.get_xticks()
    xticklabels = [int(df['flowSizeBytes'][t]) if t >= 0 and t < n else None for t in xticks]
    ax.set_xticklabels(xticklabels, rotation=45)

    ax.set_yscale('log')
    ax.set_xlabel('Message size (B)')
    ax.set_ylabel('p99 slowdown')
    ax.grid(True)
    ax.set_title(title)
    ax.legend(title="Scheme (geomean slowdown, p99 slowdown, # flows)", bbox_to_anchor=(1, 0), loc='lower left')

    fig.tight_layout(pad=0.01)
    fig.savefig(fpath, bbox_inches='tight', pad_inches=0.01, dpi=300)
    plt.close(fig)


def get_single_fct_us(
    p: Params,
    msg_size_bytes: Optional[int] = None,
    effective_gbps: Optional[float] = None,
):

    if msg_size_bytes is None:
        msg_size_bytes = p.msg_size_bytes

    if effective_gbps is None:
        effective_gbps = p.link_speed_gbps

    def get_small_pkt_fct_ns(payload_bytes):
        return (
            # Data tx
            p.one_way_hops * p.link_delay_ns
            + p.one_way_hops
            * p.get_tx_delay_ns(payload_bytes + p.header_bytes, effective_gbps)
            # ACK
            + p.one_way_hops * p.link_delay_ns
            + p.one_way_hops * p.get_tx_delay_ns(p.ack_wire_bytes, effective_gbps)
        )

    # When msg is longer than max payload size,
    # there is pipelining of tx delays.
    if msg_size_bytes > p.mss_bytes:
        n_pkts = math.floor(msg_size_bytes / p.mss_bytes)
        assert n_pkts >= 1
        return (
            get_small_pkt_fct_ns(p.mss_bytes)  # first pkt
            + p.get_tx_delay_ns(p.mtu_bytes, effective_gbps)
            * (n_pkts - 1)  # middle pkts
            + p.get_tx_delay_ns(
                msg_size_bytes % p.mss_bytes + p.header_bytes, effective_gbps
            )  # last pkt (if msg_size_bytes is not multiple of p.mss_bytes)
        ) * NS_TO_US

        # NOTE: Sometimes in sim, we get FCT better than this theoretical. When
        # message is not multiple of MSS, pkts smaller than MTU are sent. Due to
        # lower tx delays, these packets may arrive before older packets in the
        # flow. As a result, this packet arrives in parallel with others instead
        # of serially. This especially happens in incast with small messages.

    else:
        return get_small_pkt_fct_ns(msg_size_bytes) * NS_TO_US


def get_theo_best_fct(e: Experiment):
    p = e.cli_params
    if p.matrix in ["permutation", "incast", "oneWayTor", "oneWayPod"]:
        incast_degree = 1 if p.incast_degree is None else p.incast_degree
        effective_msg_size = p.msg_size_bytes * incast_degree * p.over_sub
        if p.asymmetry is None or p.asymmetry == "sym" or p.asymmetry == "bg":
            effective_link_rate = p.link_speed_gbps
        elif p.asymmetry == "single":
            if p.topology_tiers == 2:
                k = round(p.nodes ** (1/2))
                agg = k
            else:
                k = round((4 * p.nodes) ** (1/3))
                agg = k ** 2 / 2
            downlinks = k / 2
            effective_link_rate = ((agg * downlinks - 1) * p.link_speed_gbps + e.sim_params_df.asymmetricTopoSlowSwitchSpeed[0]) / agg / downlinks
        elif p.asymmetry == "all":
            if p.topology_tiers == 2:
                k = round(p.nodes ** (1/2))
                agg = k
            else:
                k = round((4 * p.nodes) ** (1/3))
                agg = k ** 2 / 2
            downlinks = k / 2
            if "asymmetricTopoSlowSwitchSpeed" in e.sim_params_df:
                effective_link_rate = ((agg - e.sim_params_df.asymmetricNumberOfSlowSwitches[0]) * (downlinks) * p.link_speed_gbps + e.sim_params_df.asymmetricNumberOfSlowSwitches[0] * downlinks * e.sim_params_df.asymmetricTopoSlowSwitchSpeed[0]) / agg / downlinks 
            else:
                effective_link_rate = ((agg - e.sim_params_df) * (downlinks) * p.link_speed_gbps + downlinks * p.link_speed_gbps / 2) / agg / downlinks
        return get_single_fct_us(p, effective_msg_size, effective_link_rate)
    else:
        return None


def add_theo_best_line_cdf(fig, ax, experiment: Experiment):
    theo_fct_us = get_theo_best_fct(experiment)
    if theo_fct_us is None:
        return
    ax.axvline(theo_fct_us, color='gray', linestyle='--', label=f"Theoretical best ({theo_fct_us:.2f})")


def parse_dc_cdf(p: Params):
    # Original empirical distribution
    cdf_file = p.matrix + ".txt"
    cdf_path = os.path.join(TRAFFIC_GEN_CDF_FILES_ROOT, cdf_file)
    df = pd.read_csv(cdf_path, sep=' ', header=None, names=["bytes", "percentile"])
    df["fct_us"] = df["bytes"].apply(lambda x: get_single_fct_us(p, x))

    # Sampled from original distribution
    cm_path = os.path.join(CM_DIR, p.get_cm_file())
    with open(cm_path, 'r') as file:
        samples = []
        for line in file:
            if "size" in line:
                sample = int(line.split(' ')[-1])
                sample_fct_us = get_single_fct_us(p, sample)
                samples.append(sample_fct_us)

    return df, samples


def add_dc_cdf(fig, ax, experiment: Experiment):
    p = experiment.cli_params
    fs_df, samples = parse_dc_cdf(p)

    ax.step(
        fs_df["fct_us"],
        fs_df["percentile"]/100.0,
        label="Flow size from CDF",
        ls='--',
        color="gray",
        where="post",
    )

    if len(samples) > 0:
        res = stats.ecdf(samples)
        x = samples
        label = f"Connection matrix ({np.mean(x):.2f}, {np.percentile(x, 99):.2f}, {len(x)})"
        res.cdf.plot(ax, label=label, ls='--', color="black")

    ax.set_xscale('log')
    ax.set_xlim((None, None))


def collate_fcts(experiment_dict: DefaultDict[str, List[Experiment]]):
    all_experiments = list(itertools.chain(*experiment_dict.values()))

    records = []
    for exp in all_experiments:
        p = exp.cli_params

        # Check if all flows completed
        cm_path = os.path.join(CM_DIR, p.get_cm_file())
        n_flows = 1
        with open(cm_path, 'r') as f:
            line = f.readlines()[1]  # second line
            assert line.split(' ')[0] == "Connections"
            n_flows = int(line.split(' ')[1])
        if len(exp.flow_info_df) < n_flows:
            print("Warning, not all flows completed for", exp.dpath)

        if len(exp.flow_info_df) == 0:
            continue

        theo_fct_us = get_theo_best_fct(exp)
        max_fct_us = exp.flow_info_df['fctNs'].max() * NS_TO_US
        records.append(
            {
                'scheme': p.scheme_str,
                'asymmetry': p.asymmetry,
                'msg_size_bytes': p.msg_size_bytes,
                'msg_size_kib': p.msg_size_bytes >> 10,
                'max_fct_us': max_fct_us,
                'theo_fct_us': theo_fct_us,
            }
        )

    return pd.DataFrame(records).sort_values(by=['msg_size_bytes', 'scheme'])


def plot_incast_summary(args, experiment_dict: DefaultDict[str, List[Experiment]]):
    df = collate_fcts(experiment_dict)
    print(df)
    asym_schemes = ["all", "sym"]
    asym_index = {k: i for i, k in enumerate(asym_schemes)}
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    i = 0
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", "scheme"])):
        if args.euroSys:
            scheme = get_core_name(group[1])
            label = prettify_name[get_core_name(group[1])]
            items = {
                "label": prettify_name[scheme],
                "marker": standard_markers[scheme],
                "color": standard_colors[scheme],
                "linewidth": linewidth
            }
        else:
            items = {
                "label": group[1].split("+")[1],
                "marker": ms[i],
                "ls": lss[i]
            }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["max_fct_us"] / gdf["theo_fct_us"], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        ax.set_ylabel('Normalized\nincast completion time')
        ax.set_xlabel("Message size (KiB)")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        if i == 0:
            ax.legend(loc='center left', bbox_to_anchor=(1, 0.5))

        fpath = os.path.join(args.input, f"incast_summary_{asym_schemes[i]}.svg")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])


def plot_single_flow_summary(args, experiment_dict: DefaultDict[str, List[Experiment]]):
    df = collate_fcts(experiment_dict)
    print(df)

    cols = {}
    fig, ax = plt.subplots()
    i = 0
    for group, gdf in df.groupby("scheme"):
        gdf = gdf.sort_values(by="msg_size_kib")
        cols[group] = np.array(gdf["max_fct_us"])
        ax.plot(
            gdf["msg_size_kib"],
            gdf["max_fct_us"],
            label=group,
            ls=lss[i],
            marker=ms[i],
        )
        i += 1

    p = list(experiment_dict.values())[0][0].cli_params
    msg_size_bytes_list = np.array(sorted(df["msg_size_bytes"].unique())).astype(int)
    cols["theo_fct_us"] = np.array([get_single_fct_us(p, x) for x in msg_size_bytes_list])
    cols["msg_size_kib"] = msg_size_bytes_list >> 10
    df = pd.DataFrame(cols)
    print(df)
    ax.plot(
        df["msg_size_kib"],
        df["theo_fct_us"],
        label="theoretical",
        color="black",
        linestyle="--",
        marker="o",
    )

    ax.set_ylabel("Flow completion time (us)")
    ax.set_xlabel("Message size (KiB)")
    ax.set_xscale('log', base=2)
    ax.grid(True)
    ax.legend()

    fig.tight_layout(pad=0.01)
    fpath = os.path.join(args.input, "single_flow_summary.svg")
    fig.savefig(fpath, bbox_inches='tight', pad_inches=0.01, dpi=300)
    plt.close(fig)


def extract_rss_info_from_dpath(dpath: str) -> CCLB:
    cclb = dpath.split("/")[-1].split(".htsim_data")[0]
    params = CCLB(**{item.split("=")[0]: item.split("=")[1] if "=" in item else True for item in cclb.replace("-", ":").split(":")})
    return params


def plot_rss_sweep(args, experiment_dict: DefaultDict[str, List[Experiment]]):
    for expr_dir, network in experiment_dict.items():
        rtt_min = None
        xps_metadata = defaultdict(lambda: defaultdict(list))
        for experiment in network:
            if rtt_min is None:
                rtt_min = 2 * sum(experiment.min_rtt_ms_tiers)
            xp_metadata = extract_rss_info_from_dpath(experiment.dpath)
            if "rss" not in xp_metadata.lb.lower():
                continue
            xps_metadata[xp_metadata.lb][xp_metadata.rss_metric].append((xp_metadata, experiment))
        if len(xps_metadata) > 1:
            fig, axs = plt.subplots(1, 3, figsize=(12, 4))
            indexx = {"rss": 0, "SkippingRSS": 1, "FrozenRSS": 2}
            indexy = {"mean_rtt": 0}#, "worse_rtt": 1, "ecn": 2}
            for rss_version in xps_metadata:
                for metric in xps_metadata[rss_version]:
                    subflow_count_index = set()
                    update_interval_index = set()
                    for metadata in xps_metadata[rss_version][metric]:
                        subflow_count_index.add(metadata[0].sf)
                        update_interval_index.add(metadata[0].ui)
                    subflow_count_index_dict = {k: v for v, k in enumerate(sorted(subflow_count_index))}
                    update_interval_index_dict = {k: v for v, k in enumerate(sorted(update_interval_index))}
                    canvas = np.zeros((len(subflow_count_index_dict), len(update_interval_index_dict)))
                    for metadata in xps_metadata[rss_version][metric]: # We could have a single loop but I don't think it matters here
                        canvas[subflow_count_index_dict[metadata[0].sf], update_interval_index_dict[metadata[0].ui]] = (metadata[1].flow_info_df["fctNs"] * NS_TO_US).max()
                    xticks = [round(interval / rtt_min, 1) for interval in update_interval_index_dict.keys()]
                    heatmap = sns.heatmap(canvas, ax=axs[indexx[rss_version]], xticklabels=xticks, yticklabels=list(subflow_count_index_dict.keys()))
                    colorbar = heatmap.collections[0].colorbar
                    if indexx[rss_version] == 0:
                        axs[indexx[rss_version]].set_ylabel(f"Number of subflows")
                    if indexy[metric] == 0:
                        axs[indexx[rss_version]].set_title(f"{rss_version}")
                    if indexy[metric] == len(indexy) - 1:
                        axs[indexx[rss_version]].set_xlabel("Update Interval (# of min RTT for longest path)")
                    if indexx[rss_version] == len(indexy) - 1:
                        colorbar.set_label('FCT($\mu$s)')
        else:
            fig, ax = plt.subplots(figsize=(5, 4))
            subflow_count_index = set()
            update_interval_index = set()
            for metadata in xps_metadata["rss"]["mean_rtt"]:
                subflow_count_index.add(metadata[0].sf)
                update_interval_index.add(metadata[0].ui)
            subflow_count_index_dict = {k: v for v, k in enumerate(sorted(subflow_count_index))}
            update_interval_index_dict = {k: v for v, k in enumerate(sorted(update_interval_index))}
            canvas = np.zeros((len(subflow_count_index_dict), len(update_interval_index_dict)))
            for metadata in xps_metadata["rss"]["mean_rtt"]: # We could have a single loop but I don't think it matters here
                canvas[subflow_count_index_dict[metadata[0].sf], update_interval_index_dict[metadata[0].ui]] = (metadata[1].flow_info_df["fctNs"] * NS_TO_US).max()
            xticks = [round(interval / rtt_min, 1) for interval in update_interval_index_dict.keys()]
            heatmap = sns.heatmap(canvas, ax=ax, xticklabels=xticks, yticklabels=list(subflow_count_index_dict.keys()))
            colorbar = heatmap.collections[0].colorbar
            ax.set_ylabel(f"Number of subflows")
            ax.set_title(f"RSS")
            ax.set_xlabel("Update Interval (# of min RTT for longest path)")
            colorbar.set_label('FCT($\mu$s)')
                
        sym = network[0].cli_params.asymmetry
        fig.tight_layout()
        fig.savefig(os.path.join(expr_dir, f"rss_sweep_map_{sym}.png"), dpi=600)
        plt.close(fig)

def plot_frozen_rss_comparison(args, experiment_dict: DefaultDict[str, List[Experiment]]):
    for expr_dir, network in experiment_dict.items():
        x = []
        y_mean = []
        y_max = []
        y_99 = []
        for experiment in network:
            if "FrozenRSS" in experiment.dpath:
                xp_metadata = extract_rss_info_from_dpath(experiment.dpath)
                x.append(xp_metadata.ft)
                y_mean.append((experiment.flow_info_df["fctNs"] * NS_TO_US).mean())
                y_max.append((experiment.flow_info_df["fctNs"] * NS_TO_US).max())
                y_99.append((experiment.flow_info_df["fctNs"] * NS_TO_US).quantile(0.99))
            elif "rss" in experiment.dpath:
                xp_metadata = extract_rss_info_from_dpath(experiment.dpath)
                x.append(0)
                y_mean.append((experiment.flow_info_df["fctNs"] * NS_TO_US).mean())
                y_max.append((experiment.flow_info_df["fctNs"] * NS_TO_US).max())
                y_99.append((experiment.flow_info_df["fctNs"] * NS_TO_US).quantile(0.99))
        if len(x) == 0:
            continue
        x, y_mean = zip(*sorted(zip(x, y_mean), key=lambda x: x[0]))
        x, y_max = zip(*sorted(zip(x, y_max), key=lambda x: x[0]))
        x, y_99 = zip(*sorted(zip(x, y_99), key=lambda x: x[0]))
        sym = network[0].cli_params.asymmetry
        plt.scatter(x, y_mean, label="mean")
        plt.scatter(x, y_max, label="max")
        plt.scatter(x, y_99, label="99th \%-ile")
        plt.xlabel("Frozen RSS Threshold")
        plt.ylabel("FCT($\mu$s)")
        plt.legend()
        plt.tight_layout()
        plt.savefig(os.path.join(expr_dir, f"frozen_comparison_{sym}.png"), dpi=600)
        plt.close()


def plot_websearch_load(args, experiment_dict: DefaultDict[str, List[Experiment]]):
    data_dict = defaultdict(lambda: defaultdict(list))
    for expr_dir, network in experiment_dict.items():
        for experiment in network:
            load = experiment.cli_params.load
            asym = experiment.cli_params.asymmetry
            xp_metadata = extract_rss_info_from_dpath(experiment.dpath)
            xp_tag = f"{xp_metadata.lb}{'(' if xp_metadata.no_trim or xp_metadata.pflr else ''}{'No Trimming' if xp_metadata.no_trim else ''}{', w/ PFLR' if xp_metadata.pflr else ', w/ RTO'}{')' if xp_metadata.no_trim or xp_metadata.pflr else ''}"
            data_dict[asym][xp_tag].append((load, experiment.flow_info_df.fctNs.mean() * NS_TO_US))
    root = Path(expr_dir).parent
    symmetries = list(data_dict.keys())
    figs, axes = [], []
    for s in symmetries:
        fig, ax = plt.subplots()
        figs.append(fig)
        axes.append(ax)
    for s in data_dict:
        idx = symmetries.index(s)
        for lb in data_dict[s]:
            axes[idx].plot(*zip(*sorted(data_dict[s][lb], key=lambda x: x[0])), label=lb, marker=standard_markers[lb], color=standard_colors[lb])
        axes[idx].set_xlabel("Network Load")
        axes[idx].set_ylabel("Average FCT ($\mu$s)")
        axes[idx].grid(True)
        figs[idx].tight_layout()
        # axes[idx].legend()
        figs[idx].savefig(root / f"load_graph_{s}.png", dpi=600)


def plot_fct_vs_msg_size(args, experiment_dict: DefaultDict[str, List[Experiment]], normalized_fct=True):
    df = collate_fcts(experiment_dict)
    df.to_csv(os.path.join(args.input, 'fct.csv'), index=False)
    print(df)
    asym_schemes = ["sym"]
    asym_index = {k: i for i, k in enumerate(asym_schemes)}
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", "scheme"])):
        # import pdb;pdb.set_trace()
        if args.euroSys:
            scheme = get_core_name(group[1])
            label = prettify_name[get_core_name(group[1])]
            items = {
                "label": prettify_name[scheme],
                "marker": standard_markers[scheme],
                "color": standard_colors[scheme],
                "linewidth": linewidth
            }
        else:
            segments = group[1].split('+')
            items = {
                # "label": group[1].split('+')[1],
                # "label": '+'.join([seg for seg in segments if ((seg not in ['uec_mprdma', 'no_trim', 'no_header_drop']) and ('probe' not in seg) and ('rand' not in seg))]),
                "label": group[1],
                "marker": ms[i],
                "ls": lss[i]
            }
        if normalized_fct:
            axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["max_fct_us"] / gdf["theo_fct_us"], **items)
        else:
            axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["max_fct_us"], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        if normalized_fct:
            ax.set_ylabel('Normalized\n last flow completion time')
        else:
            ax.set_ylabel('Last flow completion time (us)')
        ax.set_xlabel("Message size (KiB)")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))

        fpath = os.path.join(args.input, f"{'normalized_' if normalized_fct else ''}fct_vs_msg_size_{asym_schemes[i]}.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])

def collate_stats(experiment_dict: DefaultDict[str, List[Experiment]]):
    all_experiments = list(itertools.chain(*experiment_dict.values()))
    records = []
    for exp in all_experiments:
        p = exp.cli_params
        stat_path = os.path.join(exp.dpath, 'stat.json')
        with open(stat_path, 'r') as f:
            stat = json.load(f)
        records.append(
            {
                'scheme': p.scheme_str,
                'asymmetry': p.asymmetry,
                'msg_size_bytes': p.msg_size_bytes,
                'msg_size_kib': p.msg_size_bytes >> 10,
                'max_queue_util': max(stat['max_queue_util']-1,0),
                'total_data': stat['total_data'],
                'total_rtx': stat['total_rtx'],
                'total_data_probe': stat['total_data_probe'],
                'total_rtx_probe': stat['total_rtx_probe'],
                'total_ev_change_probe': stat['total_probe'],
                'rtx_to_data': stat['total_rtx']/stat['total_data'],
                'probe_to_data': (stat['total_data_probe'] + stat['total_rtx_probe'] + stat['total_probe']) \
                                / (stat['total_data']),
            }
        )
        # if (stat['total_probe'] > 0) and (p.msg_size_bytes==32768):
        #     import pdb;pdb.set_trace()

    return pd.DataFrame(records).sort_values(by=['msg_size_bytes', 'scheme'])

def plot_stat_vs_msg_size(args, experiment_dict: DefaultDict[str, List[Experiment]], normalized_fct=True):
    df = collate_stats(experiment_dict)
    print(df)
    df = df[df['scheme'].str.contains('pfld')]
    def simplify_stat_scheme(x):
        match = re.match(r'.*(probe_\d).*', x)
        if match:
            x_new = match.group(1).replace('_', ': ').replace('0', 'RTT')
        else:
            x_new = x
        return x_new
    df['scheme_simp'] = df['scheme'].apply(simplify_stat_scheme)
    label_scheme = 'scheme_simp'
    asym_schemes = ["sym"]
    asym_index = {k: i for i, k in enumerate(asym_schemes)}
    # plot 1:  queue max size
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", label_scheme])):
        items = {
            # "label": group[1].split("+")[1],
            "label": group[1],
            "marker": ms[i],
            "ls": lss[i]
        }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["max_queue_util"], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")
        # max_queue_util = df[df["asymmetry"] == asym_schemes[i]]['max_queue_util'].max()
        # plt.axhline(y=max_queue_util, color='r', linestyle='--')
        # plt.text(df[df["asymmetry"] == asym_schemes[i]]['msg_size_kib'].min(), max_queue_util, f'{max_queue_util}', color='black', ha='left', va='bottom')

        ax.set_xlabel("Message size (KiB)")
        ax.set_ylabel("Extra Headroom Needed")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))
        # ax.legend()

        fpath = os.path.join(args.input, f"max_queue_overhead.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])
    
    # plot 2: probe to data
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", label_scheme])):
        items = {
            # "label": group[1].split("+")[1],
            "label": group[1],
            "marker": ms[i],
            "ls": lss[i]
        }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["probe_to_data"], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        ax.set_xlabel("Message size (KiB)")
        ax.set_ylabel("Probe to Data")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))
        # ax.legend()

        fpath = os.path.join(args.input, f"probe_to_data.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])
    
    # plot 3: rtx to data
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", label_scheme])):
        items = {
            # "label": group[1].split("+")[1],
            "label": group[1],
            "marker": ms[i],
            "ls": lss[i]
        }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["rtx_to_data"], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        ax.set_xlabel("Message size (KiB)")
        ax.set_ylabel("Rtx to Data")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))
        # ax.legend()

        fpath = os.path.join(args.input, f"rtx_to_data.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])
    
    # plot 4.1: total data probe
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", label_scheme])):
        items = {
            # "label": group[1].split("+")[1],
            "label": group[1],
            "marker": ms[i],
            "ls": lss[i]
        }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["total_data_probe"], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        ax.set_xlabel("Message size (KiB)")
        ax.set_ylabel("Proactive Data Probe Count")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))
        # ax.legend()

        fpath = os.path.join(args.input, f"data_probe_count.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])
    
    # plot 4.2: total data probe to data
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", label_scheme])):
        items = {
            # "label": group[1].split("+")[1],
            "label": group[1],
            "marker": ms[i],
            "ls": lss[i]
        }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["total_data_probe"]/gdf['total_data'], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        ax.set_xlabel("Message size (KiB)")
        ax.set_ylabel("Proactive Data Probe to Data")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))
        # ax.legend()

        fpath = os.path.join(args.input, f"data_probe_to_data.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])
    
    # plot 5.1: total rtx probe
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", label_scheme])):
        items = {
            # "label": group[1].split("+")[1],
            "label": group[1],
            "marker": ms[i],
            "ls": lss[i]
        }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["total_rtx_probe"], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        ax.set_xlabel("Message size (KiB)")
        ax.set_ylabel("Proactive RTX Probe Count")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))
        # ax.legend()

        fpath = os.path.join(args.input, f"rtx_probe_count.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])
    
    # plot 5.2: total rtx probe to data
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", label_scheme])):
        items = {
            # "label": group[1].split("+")[1],
            "label": group[1],
            "marker": ms[i],
            "ls": lss[i]
        }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["total_rtx_probe"]/gdf['total_data'], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        ax.set_xlabel("Message size (KiB)")
        ax.set_ylabel("Proactive RTX Probe to Data")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))
        # ax.legend()

        fpath = os.path.join(args.input, f"rtx_probe_to_data.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])
    
    # plot 5.1: total ev change probe
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", label_scheme])):
        items = {
            # "label": group[1].split("+")[1],
            "label": group[1],
            "marker": ms[i],
            "ls": lss[i]
        }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["total_ev_change_probe"], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        ax.set_xlabel("Message size (KiB)")
        ax.set_ylabel("EV Change Probe Count")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))
        # ax.legend()

        fpath = os.path.join(args.input, f"ev_change_probe_count.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])
    
    # plot 5.2: total ev change probe to data
    figs, axes = [], []
    for _ in asym_index:
        f, a = plt.subplots()
        figs.append(f)
        axes.append(a)
    for i, (group, gdf) in enumerate(df.groupby(["asymmetry", label_scheme])):
        items = {
            # "label": group[1].split("+")[1],
            "label": group[1],
            "marker": ms[i],
            "ls": lss[i]
        }
        axes[asym_index[group[0]]].plot(gdf["msg_size_kib"], gdf["total_ev_change_probe"]/gdf['total_data'], **items)

    p = list(experiment_dict.values())[0][0].cli_params
    for i, ax in enumerate(axes):
        ax.axvline(x=int(p.bdp_transport_bytes) >> 10, color='gray', linestyle='--', label="Transport BDP")
        ax.axvline(x=int(p.mss_bytes) >> 10, color='gray', linestyle='dotted', label="MSS")

        ax.set_xlabel("Message size (KiB)")
        ax.set_ylabel("EV Change Probe to Data")
        ax.set_xscale('log', base=2)
        ax.grid(True)
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        # if i == 0:
        ax.legend(loc='lower left', bbox_to_anchor=(1, 0))
        # ax.legend()

        fpath = os.path.join(args.input, f"ev_change_probe_to_data.png")
        figs[i].savefig(fpath, dpi=600, bbox_inches='tight', pad_inches=0.01)
        plt.close(figs[i])


def main_plots(args, experiment_dict: DefaultDict[str, List[Experiment]]):
    for expr_dir, experiments in experiment_dict.items():
        print("Found:", expr_dir)

        extra = add_theo_best_line_cdf
        if args.datacenter:
            extra = add_dc_cdf

        exp = experiments[0]
        p = exp.cli_params
        # FCT CDF
        plot_cdf(
            experiments,
            lambda flow_info_df: flow_info_df["fctNs"] * NS_TO_US if len(flow_info_df) > 0 else [],
            "FCT (us)",
            os.path.join(expr_dir, f"{p.matrix}_{p.asymmetry}_fct_cdf.png"),
            extra,
            xlim=(0, None),
            title="\n".join(textwrap.wrap(p.get_fig_title(), width=80)),
            simplify_plots=args.euroSys
        )

        if args.datacenter:
            plot_slowdown_vs_msg_size(
                experiments,
                os.path.join(expr_dir, "slowdown_vs_msg_size.svg"),
                title="\n".join(textwrap.wrap(p.get_fig_title(), width=80)),
            )


# @try_except_wrapper
def main(args):
    experiment_dict = parse_experiments(args.input)
    main_plots(args, experiment_dict)

    if args.stat_vs_msg_size:
        plot_stat_vs_msg_size(args, experiment_dict)
        return
    if args.fct_vs_msg_size:
        plot_fct_vs_msg_size(args, experiment_dict, True)
        plot_fct_vs_msg_size(args, experiment_dict, False)

    if args.incast_summary:
        plot_incast_summary(args, experiment_dict)

    if args.single_flow_summary:
        plot_single_flow_summary(args, experiment_dict)

    if args.rss:
        plot_rss_sweep(args, experiment_dict)

    if args.frozen_exp:
        plot_frozen_rss_comparison(args, experiment_dict)

    if args.load:
        plot_websearch_load(args, experiment_dict)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-i', '--input', required=True,
        type=str, action='store',
        help='path input directory')
    parser.add_argument(
        '--incast-summary',
        action='store_true',
        help="Plot normalized FCT vs. msg size (incast experiment)")
    parser.add_argument(
        '--single-flow-summary',
        action='store_true',
        help="Plot FCT vs. msg size")
    parser.add_argument(
        '--datacenter',
        action='store_true',
        help="Show underlying flow sizes on the FCT CDF, and plot slowdown vs msg size.")
    parser.add_argument(
        "--rss",
        action="store_true",
        help="Plot RSS Parameter Sweep"
    )
    parser.add_argument(
        "--frozen-exp",
        action="store_true"
    )
    parser.add_argument(
        "--load",
        action="store_true",
        help="print mean fct as f(load)"
    )
    parser.add_argument(
        "--euroSys",
        action="store_true",
        help="Normalize plots legends etc..."
    )
    parser.add_argument(
        "--fct-vs-msg-size",
        action="store_true",
        help="Normalize plots legends etc..."
    )
    parser.add_argument(
        "--stat-vs-msg-size",
        action="store_true",
        help="Normalize plots legends etc..."
    )
    args = parser.parse_args()

    main(args)
