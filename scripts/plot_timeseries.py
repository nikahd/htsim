import argparse
import multiprocessing
import os
import pprint
import textwrap
from dataclasses import dataclass
from typing import List
from collections import defaultdict
from numpy import int64
from math import ceil
import time

import pandas as pd
import plot_experiment
from plot_experiment import LoadBalancingAlgos
import plotly
import plotly.express as px
import plotly.graph_objects as go
import seaborn.objects as so
from common import BYTES_TO_BITS, NS_TO_MS, NS_TO_US, S_TO_MS, S_TO_US, US_TO_NS, US_TO_S, B_TO_KiB, Gb_TO_b, try_except_wrapper
from matplotlib import pyplot as plt
from plotly.subplots import make_subplots
import re
import numpy as np
from pathlib import Path
from tqdm import tqdm

colors = px.colors.qualitative.Plotly * 10

queue_fname_re = re.compile("(?P<side>queueDeq|queueEnq)_(?P<src_layer>[A-Z]+)(?P<src_number>[0-9]+)->(?P<dst_layer>[A-Z]+)(?P<dst_number>[0-9_]+)\([0-9]+\).csv")
all_experiments = ["main", "throughput", "rtt", "ecn", "queue", "rss_subflow_metrics", "rss_subflow_balls_bins"]

TPUT_BIN_US = 12
ECN_BIN_US = 5*TPUT_BIN_US
UTILIZATION_WINDOW_SIZE = int(20 * US_TO_NS)

@dataclass
class MyDataFrame:
    dpath: str
    fname: str
    df: pd.DataFrame


@dataclass
class PerformanceVariable:
    dpath: str
    prefix: str
    mdfs: List[MyDataFrame]


"""
These are the prefixes of the metric names collected using the DataCollector
class in htsim/sim/data_collector.cpp.
"""
PV_PREFIXES = [
    "ack", "queueDeq", "queueEnq", "ccEvent", "cwnd", "rssSubflow"
]


@dataclass
class Experiment(plot_experiment.Experiment):
    pvs: List[PerformanceVariable]

    def __init__(self, dpath: str):
        super().__init__(dpath)
        files = set(os.listdir(dpath))

        pvs = []
        for prefix in PV_PREFIXES:
            this_files = [f for f in files if f.startswith(prefix)]

            mdfs = []
            for f in this_files:
                files.remove(f)

                fpath = os.path.join(dpath, f)
                dtypes = defaultdict(int64)
                dtypes["entropies"] = str
                df = pd.read_csv(fpath, dtype=dtypes)
                mdfs.append(MyDataFrame(dpath, f, df))

            pvs.append(
                PerformanceVariable(
                    dpath=dpath,
                    prefix=prefix,
                    mdfs=mdfs
                )
            )

        self.pvs = pvs
        self.selected_servers, self.selected_queues = self._get_selected_servers_and_queues()
    
    def _get_selected_servers_and_queues(self):
        selected_queues = []
        selected_servers = []
        with open(self.topo_file) as f:
            found_tier_0 = False
            for line in f:
                if found_tier_0:
                    if "Radix_Down" in line:
                        selected_servers = list(range(int(line.split()[1])))
                    if "Radix_Up" in line:
                        selected_queues = [f"queueDeq_LS0->US{us_number}(0).csv" for us_number in range(int(line.split()[1]))]
                if "Tier 0" in line:
                    found_tier_0 = True
        return selected_servers, selected_queues


def compute_throughput_from_ack_df(_df: pd.DataFrame):
    df = _df.copy()

    # only count delivered bytes if ACK was not NACK.
    df["deliveredBytes"] =  (~df["isNack"].astype(bool)) * df["ackedBytes"]

    # compute throughput by windowing
    df.index = pd.to_timedelta(list(df["timeNs"]), unit="ns")
    df = df.resample(f"{TPUT_BIN_US}us").sum()
    df["throughputBps"] = df["deliveredBytes"] * BYTES_TO_BITS / (TPUT_BIN_US * US_TO_S)
    df["throughputGbps"] = df["throughputBps"] / Gb_TO_b

    return df


# @try_except_wrapper
def main_timeseries_plot(experiment: Experiment, split=False, also_plot_util_separately=False, single_switch=False):
    pvs = experiment.pvs
    pvs_dict = {pv.prefix: pv for pv in pvs}
    sdf = experiment.sim_params_df
    fdf = experiment.flow_info_df
    p = experiment.cli_params

    fname = os.path.basename(experiment.dpath).replace(".htsim_data", ".html")
    fpath = os.path.join(os.path.dirname(experiment.dpath), fname)
    print(fpath)
    print("")

    if split:
        fig = go.Figure()
        row_col = {}
    else:
        fig = make_subplots(rows=5, cols=1, shared_xaxes=True, vertical_spacing=0.02)
        row_col = {"row":1, "col":1}

    # RTT
    for mdf in pvs_dict['ack'].mdfs:
        if single_switch and int(mdf.fname.split("_")[1]) not in experiment.selected_servers:
            continue
        df = mdf.df
        fig.add_trace(
            go.Scatter(
                x=df["timeNs"] * NS_TO_US,
                y=df["rttNs"] * NS_TO_US,
                mode="lines",
                name=mdf.fname,
                line={"shape": "hv"},
            ),
            **row_col
        )

    base_rtt_us = fdf["baseRttNs"].max() * NS_TO_US
    target_rtt_us = fdf["targetRttNs"].max() * NS_TO_US
    max_time_us = max(map(lambda x: x.df["timeNs"].max(), pvs_dict['ack'].mdfs)) * NS_TO_US
    fig.add_hline(y=base_rtt_us, line_dash="dash", line_color="black", name="Base RTT", **row_col)
    fig.add_hline(y=target_rtt_us, line_dash="dash", line_color="black", name="Target RTT", **row_col)
    fig.add_annotation(x=max_time_us, y=base_rtt_us, text="Base RTT", showarrow=False, yanchor="bottom", **row_col)
    fig.add_annotation(x=max_time_us, y=target_rtt_us, text="Target RTT", showarrow=False, yanchor="bottom", **row_col)
    if split:
        fig.update_yaxes(title_text="RTT (us)")        
        fig.update_xaxes(title_text="Time (us)")
        plotly.offline.plot(fig, filename=fpath.replace(".html", "_rtt.html"), auto_open=False)
        fig = go.Figure()
    else:
        row_col["row"] = 2

    # Queue size
    if len(pvs_dict['queueDeq'].mdfs) > 0:
        df_by_src = defaultdict(list)
        for mdf in pvs_dict['queueDeq'].mdfs:
            if single_switch and mdf.fname not in experiment.selected_queues:
                continue
            df = mdf.df
            fig.add_trace(
                go.Scatter(
                    x=df["timeNs"] * NS_TO_US,
                    y=df["queueSizeBytes"],
                    mode="lines",
                    name=mdf.fname,
                    line={"shape": "hv"},
                ),
                **row_col
            )
            m = queue_fname_re.match(mdf.fname)
            if m is not None:
                df["dst"] = f"{m.group('dst_layer')}{m.group('dst_number')}"
            else:
                df["dst"] = ""
            df_by_src[f"{m.group('src_layer')}{m.group('src_number')}"].append(df)
        ecn_kmin_bytes = sdf["kMinBytes"].iloc[0]
        ecn_kmax_bytes = sdf["kMaxBytes"].iloc[0]
        target_bytes = (target_rtt_us-base_rtt_us) * US_TO_NS * sdf["linkSpeedGbps"].iloc[0] / BYTES_TO_BITS
        fig.add_hline(y=ecn_kmin_bytes, line_dash="dash", line_color="black", name="KMin", **row_col)
        fig.add_hline(y=ecn_kmax_bytes, line_dash="dash", line_color="black", name="KMax", **row_col)
        fig.add_hline(y=target_bytes, line_dash="dash", line_color="black", name="Target RTT - Base RTT", **row_col)
        fig.add_annotation(x=max_time_us, y=ecn_kmin_bytes, text="KMin", showarrow=False, **row_col, yanchor="bottom")
        fig.add_annotation(x=max_time_us, y=ecn_kmax_bytes, text="KMax", showarrow=False, **row_col, yanchor="bottom")
        fig.add_annotation(x=max_time_us, y=target_bytes, text="Target RTT - Base RTT", showarrow=False, **row_col, yanchor="bottom")

        if split:
            fig.update_yaxes(title_text="Queue (B)")        
            fig.update_xaxes(title_text="Time (us)")
            plotly.offline.plot(fig, filename=fpath.replace(".html", "_queueSize.html"), auto_open=False)
            fig = go.Figure()
        else:
            row_col["row"] = 3
        for src in df_by_src:
            plt.figure()
            df_by_src[src] = pd.concat(df_by_src[src])
            df_by_src[src].timeNs *= NS_TO_US
            fpath_qSize_stacked = fpath.replace(".html", f"_queueSize_{src}.png")
            sampled_df = (df_by_src[src].iloc[list(range(0, len(df.index), int(len(df.index) / 1000)))]).groupby("dst")
            for name, group in sampled_df:
                plt.plt(group["timeNs"], group["queueSizeBytes"], label=name)
            plt.xlabel("time ($\mu$s)")
            plt.ylabel("Queue Size")
            plt.tight_layout()
            plt.savefig(fpath_qSize_stacked, dpi=600)
            plt.close()

    # Queue utilization
    if len(pvs_dict['queueDeq'].mdfs) > 0:
        separate_fig, separate_ax = plt.subplots()
        for mdf in pvs_dict['queueDeq'].mdfs:
            if single_switch and mdf.fname not in experiment.selected_queues:
                continue
            df = mdf.df
            end_time = (df["timeNs"] + df["packetSizeBytes"] * 8 / 1E6 / experiment.cli_params.link_speed_mbps * S_TO_US * US_TO_NS).values
            end_time[:-1] = np.where(np.abs(df["timeNs"].values[1:] - end_time[:-1]) <= 1, df["timeNs"].values[1:], np.round(end_time[:-1]))
            bins = pd.IntervalIndex(pd.cut(df["timeNs"],
                                range(0, df["timeNs"].max() + UTILIZATION_WINDOW_SIZE, UTILIZATION_WINDOW_SIZE)))
            next_bin = np.maximum(0, end_time - bins.right.values)
            time_sending = end_time[1:-1] - df["timeNs"].values[1:-1] + next_bin[0:-2] - next_bin[1:-1]
            time_sending = np.insert(time_sending, 0, end_time[0] - df["timeNs"][0] - next_bin[0])
            time_sending = np.append(time_sending, end_time[0] - df["timeNs"][0] + next_bin[-1])
            df["utilization"] = time_sending
            df["bins"] = bins.left * NS_TO_US
            y = df.groupby("bins")["utilization"].sum() / UTILIZATION_WINDOW_SIZE
            fig.add_trace(
                go.Scatter(
                    y=y,
                    x=y.index.values,
                    mode="lines",
                    name=mdf.fname,
                    line={"shape": "hv"},
                ),
                **row_col
            )
            if also_plot_util_separately:
                separate_ax.plot(y.index.values,  y)
        if also_plot_util_separately:
            separate_ax.set_ylim(bottom=0)
            separate_ax.set_xlabel("Time (ms)")
            separate_ax.set_ylabel("Queue Utilization")
            separate_ax.grid(True)
            separate_fig.tight_layout(pad=0.01)
            separate_fig.savefig(fpath.replace(".html", "_queueUtilization.png"), dpi=300, bbox_inches='tight', pad_inches=0.01)
            plt.close(separate_fig)

        if split:
            fig.update_yaxes(title_text="Link utilization")        
            fig.update_xaxes(title_text="Time (us)")
            plotly.offline.plot(fig, filename=fpath.replace(".html", "_queueUtilization.html"), auto_open=False)
            fig = go.Figure()
        else:
            row_col["row"] = 4

    # cwnd
    for mdf in pvs_dict['cwnd'].mdfs:
        if single_switch and int(mdf.fname.split("_")[1]) not in experiment.selected_servers:
            continue
        df = mdf.df
        fig.add_trace(
            go.Scatter(
                x=df["timeNs"] * NS_TO_US,
                y=df["cwndBytes"],
                mode="lines",
                name=mdf.fname,
                line={"shape": "hv"},
            ),
            **row_col
        )

    max_transport_bdp_bytes = base_rtt_us = fdf["BdpBytes"].max()
    fig.add_hline(y=max_transport_bdp_bytes, line_dash="dash", line_color="black", name="BDP", **row_col)
    fig.add_annotation(x=max_time_us, y=max_transport_bdp_bytes, text="BDP", showarrow=False, **row_col, yanchor="bottom")
    if split:
        fig.update_yaxes(title_text="Cwnd (B)")        
        fig.update_xaxes(title_text="Time (us)")
        plotly.offline.plot(fig, filename=fpath.replace(".html", "_cwnd.html"), auto_open=False)
        fig = go.Figure()
    else:
        row_col["row"] = 5

    # throughput
    for mdf in pvs_dict['ack'].mdfs:
        if single_switch and int(mdf.fname.split("_")[1]) not in experiment.selected_servers:
            continue
        df = compute_throughput_from_ack_df(mdf.df)
        fig.add_trace(
            go.Scatter(
                x=df.index.total_seconds() * S_TO_US,
                y=df["throughputGbps"],
                mode="lines",
                name=mdf.fname,
                line={"shape": "hv"},
            ),
            **row_col
        )
    if split:
        fig.update_yaxes(title_text="Throughput (Gbps)")        
        fig.update_xaxes(title_text="Time (us)")
        plotly.offline.plot(fig, filename=fpath.replace(".html", "_cwnd.html"), auto_open=False)
        fig = go.Figure()
    else:
        fig.update_layout(title_text="<br>".join(textwrap.wrap(p.get_fig_title(aggregate=False), width=120)))
        fig.update_yaxes(title_text="RTT (us)", row=1, col=1)
        fig.update_yaxes(title_text="Queue (B)", row=2, col=1)
        fig.update_yaxes(title_text="Link utilization", row=3, col=1)
        fig.update_yaxes(title_text="Cwnd (B)", row=4, col=1)
        fig.update_yaxes(title_text="Throughput (Gbps)", row=5, col=1)
        fig.update_xaxes(title_text="Time (us)", row=5, col=1)
        plotly.offline.plot(fig, filename=fpath, auto_open=False)


# @try_except_wrapper
def throughput_plot(experiment: Experiment, single_switch=False):
    pvs = experiment.pvs
    pvs_dict = {pv.prefix: pv for pv in pvs}

    fig, ax = plt.subplots()
    for mdf in pvs_dict['ack'].mdfs:
        if single_switch and int(mdf.fname.split("_")[1]) not in experiment.selected_servers:
            continue
        df = compute_throughput_from_ack_df(mdf.df)
        ax.plot(df.index.total_seconds() * S_TO_MS, df["throughputGbps"])

    ax.set_ylim(bottom=0)
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Throughput (Gbps)")
    ax.grid(True)

    fig.tight_layout(pad=0.01)
    fname = os.path.basename(experiment.dpath).replace(".htsim_data", "_tput.svg")
    fpath = os.path.join(os.path.dirname(experiment.dpath), fname)
    fig.savefig(fpath, dpi=300, bbox_inches='tight', pad_inches=0.01)
    plt.close(fig)


# @try_except_wrapper
def rtt_plot(experiment: Experiment, single_switch=False):
    pvs = experiment.pvs
    pvs_dict = {pv.prefix: pv for pv in pvs}

    fig, ax = plt.subplots()
    for mdf in pvs_dict['ack'].mdfs:
        if single_switch and int(mdf.fname.split("_")[1]) not in experiment.selected_servers:
            continue
        df = mdf.df
        ax.plot(df["timeNs"] * NS_TO_MS, df["rttNs"] * NS_TO_US)

    ax.set_ylim(bottom=0)
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("RTT (us)")
    ax.grid(True)

    fig.tight_layout(pad=0.01)
    fname = os.path.basename(experiment.dpath).replace(".htsim_data", "_rtt.svg")
    fpath = os.path.join(os.path.dirname(experiment.dpath), fname)
    fig.savefig(fpath, dpi=300, bbox_inches='tight', pad_inches=0.01)
    plt.close(fig)


# @try_except_wrapper
def ecn_plot(experiment: Experiment, single_switch=False):
    pvs = experiment.pvs
    pvs_dict = {pv.prefix: pv for pv in pvs}

    fig, ax = plt.subplots()
    for mdf in pvs_dict['ack'].mdfs:
        if single_switch and int(mdf.fname.split("_")[1]) not in experiment.selected_servers:
            continue
        df = mdf.df
        avg_ecn_rate = 0.0
        alpha = 0.05
        ecn_rate_list = []
        for row in df.itertuples():
            avg_ecn_rate = alpha * int(row.hasECN) + (1 - alpha) * avg_ecn_rate
            ecn_rate_list.append(avg_ecn_rate)
        df["ecnRate"] = ecn_rate_list
        df.index = pd.to_timedelta(list(df["timeNs"]), unit="ns")
        df = df.resample(f"{ECN_BIN_US}us").mean()
        ax.plot(df.index.total_seconds() * S_TO_MS, df["ecnRate"])

    ax.axhline(y=0.3, color='black', linestyle='--')
    ax.set_ylim(bottom=0)
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Average ECN rate")
    ax.grid(True)

    fig.tight_layout(pad=0.01)
    fname = os.path.basename(experiment.dpath).replace(".htsim_data", "_ecn.svg")
    fpath = os.path.join(os.path.dirname(experiment.dpath), fname)
    fig.savefig(fpath, dpi=300, bbox_inches='tight', pad_inches=0.01)
    plt.close(fig)


# @try_except_wrapper
def queue_plot(experiment: Experiment, single_switch=False):
    pvs = experiment.pvs
    pvs_dict = {pv.prefix: pv for pv in pvs}

    fig, ax = plt.subplots()
    for mdf in pvs_dict['queueDeq'].mdfs:
        if single_switch and mdf.fname not in experiment.selected_queues:
            continue
        df = mdf.df
        ax.plot(df["timeNs"] * NS_TO_MS, df["queueSizeBytes"] * B_TO_KiB)

    ax.set_ylim(bottom=0)
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Queue (KiB)")
    ax.grid(True)

    fig.tight_layout(pad=0.01)
    fname = os.path.basename(experiment.dpath).replace(".htsim_data", "_queue.png")
    fpath = os.path.join(os.path.dirname(experiment.dpath), fname)
    fig.savefig(fpath, dpi=300, bbox_inches='tight', pad_inches=0.01)
    plt.close(fig)



# @try_except_wrapper
def queue_util_plot(experiment: Experiment, single_switch=False):
    pvs = experiment.pvs
    pvs_dict = {pv.prefix: pv for pv in pvs}

    fig, ax = plt.subplots()
    for mdf in pvs_dict['queueDeq'].mdfs:
        df = mdf.df
        df["end_time"] = df["timeNs"] + df["packetSizeBytes"] * 8 / 1E6 / experiment.cli_params.link_speed_mbps * S_TO_US * US_TO_NS
        bins = pd.IntervalIndex(pd.cut(df["timeNs"],
                            range(0, df["timeNs"].max() + UTILIZATION_WINDOW_SIZE, UTILIZATION_WINDOW_SIZE)))
        next_bin = np.maximum(0, df["end_time"].values - bins.right.values)
        time_sending = df["end_time"].values[1:-1] - df["timeNs"].values[1:-1] + next_bin[0:-2] - next_bin[1:-1]
        time_sending = np.insert(time_sending, 0, df["end_time"][0] - df["timeNs"][0] - next_bin[0])
        time_sending = np.append(time_sending, df["end_time"][0] - df["timeNs"][0] + next_bin[-1])
        ax.plot(bins.left,  time_sending / UTILIZATION_WINDOW_SIZE)

    ax.set_ylim(bottom=0)
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Queue Utilization")
    ax.grid(True)

    fig.tight_layout(pad=0.01)
    fname = os.path.basename(experiment.dpath).replace(".htsim_data", "_queueUtilization.png")
    fpath = os.path.join(os.path.dirname(experiment.dpath), fname)
    fig.savefig(fpath, dpi=300, bbox_inches='tight', pad_inches=0.01)
    plt.close(fig)

def find_subgraph_shape(num_subgraphs: int):
    return ceil(num_subgraphs / ceil(num_subgraphs ** .5)), ceil(num_subgraphs ** .5)

def process_entropy_df(df):
    hashmap = {}
    for i, entropy in enumerate(df["entropies"]):
        if entropy not in hashmap:
            hashmap[entropy] = i
    df['entropies'] = df['entropies'].map(lambda x: hashmap[x])
    return df


def rss_subflow_metrics_plot(experiment: Experiment, single_switch=False):
    if experiment.sim_params_df.loadBalancingAlgo[0] != LoadBalancingAlgos.RSS.value:
        return
    pvs = experiment.pvs
    pvs_dict = {pv.prefix: pv for pv in pvs}

    senders = set()
    sender_mdfs = defaultdict(list)
    for mdf in pvs_dict["rssSubflow"].mdfs:
        if single_switch and int(mdf.fname.split("_")[1]) not in experiment.selected_servers:
            continue
        parts = list(map(int, mdf.fname.split(".csv")[0].split("_")[1:]))
        senders.add(parts[0])
        sender_mdfs[int(parts[0])].append(mdf)

    grid_x, grid_y = find_subgraph_shape(len(senders))
    fig_rtt, axs_rtt = plt.subplots(grid_x, grid_y)
    fig_ecn, axs_ecn = plt.subplots(grid_x, grid_y)
    fig_entropy, axs_entropy = plt.subplots(grid_x, grid_y)
    fig_pkts, axs_pkts = plt.subplots(grid_x, grid_y)
    fig_deltaRTT, axs_deltaRTT = plt.subplots(grid_x, grid_y)
    fig_deltaECN, axs_deltaECN = plt.subplots(grid_x, grid_y)
    for i, sender_id in enumerate(sender_mdfs):
        rtt_series = []
        ecn_series = []
        for subflow_mdf in sender_mdfs[sender_id]:
            df = process_entropy_df(subflow_mdf.df)
            rtt_series.append(df[["timeNs", "mean_rtt"]].set_index("timeNs"))
            ecn_series.append(df[["timeNs", "ecn"]].set_index("timeNs"))
            axs_rtt[i // grid_y, i % grid_y].plot(df["timeNs"] * NS_TO_MS, df["mean_rtt"] * NS_TO_MS)
            axs_ecn[i // grid_y, i % grid_y].plot(df["timeNs"] * NS_TO_MS, df["ecn"])
            axs_entropy[i // grid_y, i % grid_y].plot(df["timeNs"] * NS_TO_MS, df["entropies"])
            axs_pkts[i // grid_y, i % grid_y].plot(df["timeNs"] * NS_TO_MS, df["number_of_ev_packets"])
            axs_rtt[i // grid_y, i % grid_y].set_title(f"Sender #{sender_id}")
            axs_ecn[i // grid_y, i % grid_y].set_title(f"Sender #{sender_id}")
            axs_entropy[i // grid_y, i % grid_y].set_title(f"Sender #{sender_id}")
            axs_pkts[i // grid_y, i % grid_y].set_title(f"Sender #{sender_id}")
        df_rtt = pd.concat(rtt_series, axis=1)
        df_ecn = pd.concat(ecn_series, axis=1)
        axs_deltaRTT[i // grid_y, i % grid_y].plot(df_rtt.index * NS_TO_MS,  (df_rtt.max(axis=1) - df_rtt.min(axis=1)) * NS_TO_MS)
        axs_deltaECN[i // grid_y, i % grid_y].plot(df_rtt.index * NS_TO_MS, df_ecn.max(axis=1) - df_ecn.min(axis=1))

    for unused_plot_index in range(len(sender_mdfs), grid_x * grid_y):
        fig_rtt.delaxes(axs_rtt[unused_plot_index // grid_y, unused_plot_index % grid_y])  
        fig_ecn.delaxes(axs_ecn[unused_plot_index // grid_y, unused_plot_index % grid_y])  
        fig_entropy.delaxes(axs_entropy[unused_plot_index // grid_y, unused_plot_index % grid_y])  
        fig_pkts.delaxes(axs_pkts[unused_plot_index // grid_y, unused_plot_index % grid_y])
        fig_deltaRTT.delaxes(axs_deltaRTT[unused_plot_index // grid_y, unused_plot_index % grid_y])
        fig_deltaECN.delaxes(axs_deltaECN[unused_plot_index // grid_y, unused_plot_index % grid_y])

    for fig_axs in [axs_rtt, axs_ecn, axs_pkts, axs_deltaECN, axs_deltaRTT]:
        for ax in fig_axs.flatten():
            ax.set_ylim(bottom=0)
            ax.grid(True)
    
    fig_rtt.supxlabel("Time (ms)")
    fig_rtt.supylabel("RTT (ms)")

    fig_ecn.supxlabel("Time (ms)")
    fig_ecn.supylabel("Fraction of ECN marked packets")

    fig_entropy.supxlabel("Time (ms)")
    fig_entropy.supylabel("Entropy values")

    fig_pkts.supxlabel("Time (ms)")
    fig_pkts.supylabel("Number of packets in time period")

    fig_deltaRTT.supxlabel("Time (ms)")
    fig_deltaRTT.supylabel("RTT Spread (ms)")

    fig_deltaECN.supxlabel("Time (ms)")
    fig_deltaECN.supylabel("ECN Spread")
    
    fig_rtt.tight_layout(pad=0.5)
    fig_ecn.tight_layout(pad=0.5)
    fig_entropy.tight_layout(pad=0.5)
    fig_pkts.tight_layout(pad=0.5)
    fig_deltaRTT.tight_layout(pad=0.5)
    fig_deltaECN.tight_layout(pad=0.5)

    fname_rtt = os.path.basename(experiment.dpath).replace(".htsim_data", "_rss_rtt.png")
    fname_ecn = os.path.basename(experiment.dpath).replace(".htsim_data", "_rss_ecn.png")
    fname_entropy = os.path.basename(experiment.dpath).replace(".htsim_data", "_rss_entropy.png")
    fname_pkts = os.path.basename(experiment.dpath).replace(".htsim_data", "_rss_pkts.png")
    fname_deltaRTT = os.path.basename(experiment.dpath).replace(".htsim_data", "_rss_rttSpread.png")
    fname_deltaECN = os.path.basename(experiment.dpath).replace(".htsim_data", "_rss_ecnSpread.png")

    fpath_rtt = os.path.join(os.path.dirname(experiment.dpath), fname_rtt)
    fpath_ecn = os.path.join(os.path.dirname(experiment.dpath), fname_ecn)
    fpath_entropy = os.path.join(os.path.dirname(experiment.dpath), fname_entropy)
    fpath_pkts = os.path.join(os.path.dirname(experiment.dpath), fname_pkts)
    fpath_deltaRTT = os.path.join(os.path.dirname(experiment.dpath), fname_deltaRTT)
    fpath_deltaECN = os.path.join(os.path.dirname(experiment.dpath), fname_deltaECN)

    fig_rtt.savefig(fpath_rtt, dpi=600, bbox_inches='tight', pad_inches=0.01)
    fig_ecn.savefig(fpath_ecn, dpi=600, bbox_inches='tight', pad_inches=0.01)
    fig_entropy.savefig(fpath_entropy, dpi=600, bbox_inches='tight', pad_inches=0.01)
    fig_pkts.savefig(fpath_pkts, dpi=600, bbox_inches='tight', pad_inches=0.01)
    fig_deltaRTT.savefig(fpath_deltaRTT, dpi=600, bbox_inches='tight', pad_inches=0.01)
    fig_deltaECN.savefig(fpath_deltaECN, dpi=600, bbox_inches='tight', pad_inches=0.01)

    plt.close(fig_rtt)
    plt.close(fig_ecn)
    plt.close(fig_entropy)
    plt.close(fig_pkts)
    plt.close(fig_deltaRTT)
    plt.close(fig_deltaECN)

# window_size in us
def rss_subflow_balls_bins(experiment: Experiment, dest_layer="US", window_size=15, single_switch=False):
    if experiment.sim_params_df.loadBalancingAlgo[0] != LoadBalancingAlgos.RSS.value:
        return
    dpath = Path(experiment.dpath)
    pvs = experiment.pvs
    pvs_dict = {pv.prefix: pv for pv in pvs}
    for side in ["queueDeq", "queueEnq"]:
        all_dfs = []
        for mdf in pvs_dict[side].mdfs:
            if single_switch and mdf.fname not in experiment.selected_queues:
                continue
            m = queue_fname_re.match(mdf.fname)
            if m is not None and m.group("dst_layer") == dest_layer:
                if single_switch:
                    df = (mdf.df.iloc[list(range(0, len(mdf.df.index), int(len(mdf.df.index) / 1000)))]).copy()
                else:
                    df = mdf.df
                bin_range = np.arange(0, df["timeNs"].max() + int(window_size * US_TO_NS), int(window_size * US_TO_NS))
                df["bin"] = pd.cut(df["timeNs"], bin_range, labels=(bin_range[:-1] * NS_TO_US).astype(int))
                df["uplink_src"] = int(m.group("src_number"))
                df["uplink_dest"] = int(m.group("dst_number"))
                all_dfs.append(df)
        if len(all_dfs) < 1:
            print(f"skipping {side}")
            continue
        all_dfs = dict(tuple(pd.concat(all_dfs).groupby("uplink_src")))
        root_path_all = dpath / "graphs"
        root_path_all.mkdir(parents=True, exist_ok=True)
        for src_tor, df in all_dfs.items():
            y = df.groupby(["uplink_dest", "bin"])["packetSizeBytes"].sum()
            y /= (window_size * US_TO_S * 1E9 / BYTES_TO_BITS)
            y = y.reset_index().sort_values(by=["uplink_dest", "bin"])
            y["bin"] = y["bin"].astype(int)
            fpath_rates = root_path_all / f"rss_pathDistribution_rates_{side}_src{src_tor}_mode-{dest_layer}_{experiment.cli_params.asymmetry}.png"
            plt.figure()
            groups = y.groupby("uplink_dest")
            for name, group in groups:
                plt.plot(group.bin, group.packetSizeBytes, label=name)
            plt.xlabel("time ($\mu$s)")
            plt.ylabel("rate (Gbps)")
            plt.tight_layout()
            plt.savefig(fpath_rates, dpi=600)
            plt.close()

            # start_so = time.time()
            y = df.groupby(["uplink_dest", "bin"])["ev"].unique().fillna(0).apply(lambda x: x if isinstance(x, int) else len(x))
            y = y.reset_index().sort_values(by=["bin", "uplink_dest"])
            y["bin"] = y["bin"].astype(int)
            fpath_bins = root_path_all / f"rss_pathDistribution_evs_{side}_src{src_tor}_mode-{dest_layer}_{experiment.cli_params.asymmetry}.png"
            groups = y.groupby("uplink_dest")
            for name, group in groups:
                windowed = group.rolling(4, min_periods=1).mean()
                plt.plot(windowed["bin"], windowed["ev"], label=name)
            plt.xlabel("time ($\mu$s)")
            plt.ylabel("Number of unique evs")
            plt.tight_layout()
            plt.savefig(fpath_bins, dpi=600)
            plt.close()
            # end_so = time.time()
            # start_mpl = time.time()
            # fig_stack, ax_stack = plt.subplots()
            # y = df.groupby(["uplink_dest", "bin"])["ev"].unique().fillna(0).apply(lambda x: x if isinstance(x, int) else len(x))
            # max_bin = y.index[y.index.get_level_values(1).argmax()]
            # series = {i: y[[i]] for i in df["uplink_dest"].unique()}
            # bins = series[max_bin[0]].index.get_level_values(1)
            # for i, serie in series.items():
            #     if i != max_bin[0]:
            #         missing_rows = [idx for idx in bins if idx not in serie.index.get_level_values(1)]
            #         to_append = pd.Series({(i, idx): 0 for idx in missing_rows})
            #         series[i] = serie.append(to_append)
            # ax_stack.stackplot(bins, [s.values for s in series.values()], labels=series.keys())
            # fig_stack.savefig(fpath_bins, dpi=600)
            # end_mpl = time.time()
            # print(f"Seaborn: {end_so - start_so}, MPL: {end_mpl - start_mpl}")

            # for dst_server in df.dst.unique():
            #     dst_df = df[df.dst == dst_server]
            #     y = dst_df.groupby(["uplink_dest", "bin"])["ev"].unique().fillna(0).apply(lambda x: x if isinstance(x, int) else len(x))
            #     y = y.reset_index().sort_values(by=["bin", "uplink_dest"])
            #     y["bin"] = y["bin"].astype(int)
            #     fpath_bins = root_path_all / f"rss_pathDistribution_evs_{side}_dstSrv{dst_server}.png"
            #     (so.Plot(data=y, x="bin", y="ev").add(so.Area(alpha=.7), so.Stack(), color="uplink_dest")).label(x="time ($\mu$s)", y="Number of unique evs", color="uplink_dest").layout(engine="tight").save(fpath_bins, dpi=600)


def get_all_dirs(input_dir) -> List[str]:
    """Returns list of all directories that store experiment results/logs, i.e.,
    directories that end with .htsim_data suffix in input_dir
    """

    assert os.path.isdir(input_dir), 'Invalid input directory'

    ret = []
    for root, dirs, _ in os.walk(input_dir):
        for dir in dirs:
            if dir.endswith('.htsim_data'):
                ret.append(os.path.join(root, dir))
    return ret


def plot_dir(args, expr_data_dir):
    if not os.path.isfile(os.path.join(expr_data_dir, 'globalInfo.csv')):
        return
    try:
        experiment = Experiment(expr_data_dir)
    except FileNotFoundError as e:
        print(f"Skipped {expr_data_dir} because of missing file: {e}")
        return
    if "main" in args.experiments:
        main_timeseries_plot(experiment, split=True, also_plot_util_separately=True, single_switch=args.single_switch_focus)
    if "throughput" in args.experiments:    
        throughput_plot(experiment, single_switch=args.single_switch_focus)
    if "rtt" in args.experiments:
        rtt_plot(experiment, single_switch=args.single_switch_focus)
    if "ecn" in args.experiments:
        ecn_plot(experiment, single_switch=args.single_switch_focus)
    if "queue" in args.experiments:
        queue_plot(experiment)
    if "rss_subflow_metrics" in args.experiments:
        rss_subflow_metrics_plot(experiment, single_switch=args.single_switch_focus)
    if "rss_subflow_balls_bins" in args.experiments:
        rss_subflow_balls_bins(experiment, single_switch=args.single_switch_focus)


def plot_multiflow_summary(args, expr_data_dirs):
    # Steady state queue as a function of number of flows
    if not os.path.isfile(os.path.join(expr_data_dir, 'globalInfo.csv')):
        return
    exprs = []
    for expr_data_dir in expr_data_dirs:
        tmp_exp = plot_experiment.Experiment(expr_data_dir)
        if tmp_exp.cli_params.cc_algo.startswith("smartt"):
            experiment = Experiment(expr_data_dir)
            exprs.append(experiment)

    records = []
    for expr in exprs:
        pvs = expr.pvs
        pvs_dict = {pv.prefix: pv for pv in pvs}
        p = expr.cli_params

        mdfs = pvs_dict["queueDeq"].mdfs
        for mdf in mdfs:
            if mdf.fname == 'queueDeq_LS0->DST0(0).csv':
                df = mdf.df
                avg_queue_bytes = df["queueSizeBytes"].mean()
                records.append({
                    'scheme': p.scheme_str,
                    'num_flows': p.incast_degree,
                    'avg_queue_bytes': avg_queue_bytes,
                })
    df = pd.DataFrame(records).sort_values(by=["scheme", "num_flows"])
    print(df)

    fig, ax = plt.subplots()
    for scheme, gdf in df.groupby("scheme"):
        x, y = gdf["num_flows"], gdf["avg_queue_bytes"]
        ax.plot(x, y * B_TO_KiB, label=scheme)

    exp = exprs[0]
    fdf = exp.flow_info_df
    sdf = exp.sim_params_df
    base_rtt_us = fdf["baseRttNs"].max() * NS_TO_US
    target_rtt_us = fdf["targetRttNs"].max() * NS_TO_US
    ecn_kmin_bytes = sdf["kMinBytes"].iloc[0]
    ecn_kmax_bytes = sdf["kMaxBytes"].iloc[0]
    target_bytes = (target_rtt_us-base_rtt_us) * US_TO_NS * sdf["linkSpeedGbps"].iloc[0] / BYTES_TO_BITS
    ax.axhline(y=target_bytes * B_TO_KiB, color='black', linestyle='--', label="Target RTT - Base RTT")
    ax.axhline(y=ecn_kmin_bytes * B_TO_KiB, color='gray', linestyle='--', label="KMin")
    ax.axhline(y=ecn_kmax_bytes * B_TO_KiB, color='gray', linestyle='--', label="KMax")

    ax.grid(True)
    ax.set_xlabel("# flows")
    ax.set_ylabel("Queue (KiB)")
    ax.legend()

    fig.tight_layout(pad=0.01)
    fpath = os.path.join(args.input, "ssqueue-vs-nflows.svg")
    fig.savefig(fpath, dpi=300, bbox_inches='tight', pad_inches=0.01)
    plt.close(fig)


# @try_except_wrapper
def main(args):
    if "all" in args.experiments:
        setattr(args, "experiments", all_experiments)
    if args.single_experiment:
        plot_dir(args, os.path.join(args.input, args.single_experiment))
    else:
        expr_data_dirs = get_all_dirs(args.input)
        print("Found:")
        pprint.pprint(expr_data_dirs)
        print("")

        if args.parallel:
            with multiprocessing.Pool() as pool:
                pool.starmap(
                    plot_dir, [(args, expr_data_dir) for expr_data_dir in expr_data_dirs]
                )
        else:
            for expr_data_dir in tqdm(expr_data_dirs):
                plot_dir(args, expr_data_dir)

        if args.multiflow_summary:
            plot_multiflow_summary(args, expr_data_dirs)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-i', '--input', required=True,
        type=str, action='store',
        help='path input directory')
    parser.add_argument(
        '--multiflow-summary',
        action='store_true',
        help='Plot steady state queue vs. number of flows')
    parser.add_argument(
        '-p', '--parallel',
        action='store_true',
        help='run in parallel')
    parser.add_argument(
        '-s', '--single-experiment',
        help="Only plot time series for given experiment. Specified by the path of the experiment."
    )
    parser.add_argument(
        "--single-switch-focus",
        action="store_true"
    )
    parser.add_argument(
        "--experiments",
        nargs="+",
        default="[all]",
        choices=(all_experiments + ["all"])
    )
    args = parser.parse_args()
    main(args)
