import argparse
import itertools
import json
import math
import re
import os
import textwrap
import shutil
import matplotlib.pyplot as plt
from collections import defaultdict
from dataclasses import dataclass, fields
from typing import Any, Callable, DefaultDict, List, Optional
from run_experiment import CM_DIR, Params
from enum import Enum
from tqdm import tqdm
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import multiprocessing

lss = ['solid', 'dashdot', 'dashed', 'dotted',] * 10
ms = ['o', 's', 'v', 'D', 'x', 'P', 'p', 'H', 'X', 'd', '1', '2', '3', '4', '8', 'h', 'H', '+', 'x', '|', '_'] * 10
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

    def __post_init__(self):
        for field_info in fields(self):
            val = getattr(self, field_info.name)
            if val is not None:
                setattr(self, field_info.name, field_info.type(val))

def parse_experiments(input_dir) -> DefaultDict[str, List[Experiment]]:
    assert os.path.isdir(input_dir), 'Invalid input directory'

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

def find_output_txt_files(root_dir):
    # List to store directories containing output.txt
    dirs_with_output_txt = []

    # Walk through all directories and subdirectories
    for dirpath, dirnames, filenames in os.walk(root_dir):
        if "output.txt" in filenames:
            # Add the directory path to the list if output.txt is found
            dirs_with_output_txt.append(dirpath)

    return dirs_with_output_txt

def match_pattern(pattern, data, line):
    match = re.match(pattern, line.strip())
    if match:
        time = float(match.group(1))
        flow_id = int(match.group(2))
        pkt_type = str(match.group(3))
        psn = int(match.group(4))
        ev = int(match.group(5))
        
        # each flow
        if flow_id not in data[pkt_type]:
            data[pkt_type][flow_id] = {'time': [], 'count': []}

        count = len(data[pkt_type][flow_id]['count']) + 1
        data[pkt_type][flow_id]['time'].append(time)
        data[pkt_type][flow_id]['count'].append(count)
        # all flows
        flow_id = 'all'
        if flow_id not in data[pkt_type]:
            data[pkt_type][flow_id] = {'time': [], 'count': []}

        count = len(data[pkt_type][flow_id]['count']) + 1
        data[pkt_type][flow_id]['time'].append(time)
        data[pkt_type][flow_id]['count'].append(count)
    return match

# Function to extract time, flowid, psn from the relevant lines in output.txt
def parse_output_file(file_path):
    sender_send_data = {'data':{}, 'rtx':{}, 'proactive data probe':{}, 'proactive rtx probe':{}, 'probe':{}, 'sleek probe':{}}
    # receiver_send_data = {'pflr nack': {}, 'trim nack': {}, 'spurious nack': {}}
    # sender_receive_data = {'ack':{}, 'nack': {}}
    receiver_receive_data = {'data':{}, 'rtx':{}, 'probe':{}, 'trim':{}}
    switch_drop_data = {'data':{}, 'rtx':{}, 'probe':{}, 'trim':{}, 'sleek probe':{}}
    switch_trim_data = {'data':{}, 'rtx':{}}
    sender_rtx_enqueue_data = {'rto':{}, 'nack':{}, 'sleek':{}, 'pflr0':{}, 'spurious rtx':{}, 'spurious rtx for future loss':{}}
    packet_tracking_types = ['data', 'rtx']
    flying_packet = {}
    drop_trim_packet = {}
    drop_trim_packet_timestamps = {}
    loss_detection_time = np.array([])
    spurious_rtx_packet = {}

    sender_send_pattern = r'(\d+\.?\d*) flow (\d+) sending (data|rtx|proactive data probe|proactive rtx probe|probe|sleek probe) packet for psn (\d+) ev (\d+)'
    # receiver_send_pattern = r'(\d+\.?\d*) flow (\d+) sending (pflr nack|trim nack) packet for psn (\d+) ev (\d+)'
    # sender_receive_pattern = r'(\d+\.?\d*) flow (\d+) receive (ack|nack) packet for psn (\d+) ev (\d+)'
    receiver_receive_pattern = r'(\d+\.?\d*) flow (\d+) receive (data|rtx|probe|trim) packet for psn (\d+) ev (\d+)'
    switch_drop_pattern = r'(\d+\.?\d*) flow (\d+) drop (data|rtx|probe|trim|sleek probe) packet for psn (\d+) ev (\d+)'
    switch_trim_pattern = r'(\d+\.?\d*) flow (\d+) trim (data|rtx) packet for psn (\d+) ev (\d+)'
    sender_rtx_enqueue_pattern = r'(\d+\.?\d*) flow (\d+) (rto|nack|sleek|pflr0) queue rtx packet for psn (\d+)'

    with open(file_path, 'r') as file:
        for line in file:
            # sender send pkt
            data = sender_send_data
            pattern = sender_send_pattern
            match = re.match(pattern, line.strip())
            if match:
                time = float(match.group(1))
                flow_id = int(match.group(2))
                pkt_type = str(match.group(3))
                psn = int(match.group(4))
                ev = int(match.group(5))
                
                # each flow
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)

                if pkt_type in packet_tracking_types:
                    if flow_id not in flying_packet:
                        flying_packet[flow_id] = []
                    flying_packet[flow_id].append(psn)
                
                # all flows
                flow_id = 'all'
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)
                continue
            
            # switch trim pkt
            data = switch_trim_data
            pattern = switch_trim_pattern
            match = re.match(pattern, line.strip())
            if match:
                time = float(match.group(1))
                flow_id = int(match.group(2))
                pkt_type = str(match.group(3))
                psn = int(match.group(4))
                ev = int(match.group(5))
                
                # each flow
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)

                if pkt_type in packet_tracking_types:
                    if not (psn in flying_packet[flow_id]):
                        print(f"trim psn not found")
                        # import pdb;pdb.set_trace()
                    else:
                        flying_packet[flow_id].remove(psn)
                        if not (flow_id in drop_trim_packet):
                            drop_trim_packet[flow_id]=[psn]
                            drop_trim_packet_timestamps[flow_id]=[time]
                        else:
                            drop_trim_packet[flow_id].append(psn)
                            drop_trim_packet_timestamps[flow_id].append(time)
                
                # all flows
                flow_id = 'all'
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)
                continue
            
            # switch drop pkt
            data = switch_drop_data
            pattern = switch_drop_pattern
            match = re.match(pattern, line.strip())
            if match:
                time = float(match.group(1))
                flow_id = int(match.group(2))
                pkt_type = str(match.group(3))
                psn = int(match.group(4))
                ev = int(match.group(5))
                
                # each flow
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)

                if pkt_type in packet_tracking_types:
                    if (flow_id not in flying_packet) or (psn not in flying_packet[flow_id]):
                        print(f"drop psn not found")
                        # import pdb;pdb.set_trace()
                    else:
                        flying_packet[flow_id].remove(psn)
                        if not (flow_id in drop_trim_packet):
                            drop_trim_packet[flow_id]=[psn]
                            drop_trim_packet_timestamps[flow_id]=[time]
                        else:
                            drop_trim_packet[flow_id].append(psn)
                            drop_trim_packet_timestamps[flow_id].append(time)
                
                # all flows
                flow_id = 'all'
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)
                continue

            # receiver receive pkt
            data = receiver_receive_data
            pattern = receiver_receive_pattern
            match = re.match(pattern, line.strip())
            if match:
                time = float(match.group(1))
                flow_id = int(match.group(2))
                pkt_type = str(match.group(3))
                psn = int(match.group(4))
                ev = int(match.group(5))
                
                # each flow
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)

                if pkt_type in packet_tracking_types:
                    if (flow_id not in flying_packet) or (psn not in flying_packet[flow_id]):
                        print(f"receive psn not found")
                        # import pdb;pdb.set_trace()
                    else:
                        flying_packet[flow_id].remove(psn)
                
                # all flows
                flow_id = 'all'
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)
                continue
            
            # sender rtx
            data = sender_rtx_enqueue_data
            pattern = sender_rtx_enqueue_pattern
            match = re.match(pattern, line.strip())
            if match:
                time = float(match.group(1))
                flow_id = int(match.group(2))
                pkt_type = str(match.group(3))
                psn = int(match.group(4))
                
                # each flow
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)

                if (flow_id not in drop_trim_packet) or (psn not in drop_trim_packet[flow_id]):
                    # spurious rtx
                    if flow_id not in sender_rtx_enqueue_data['spurious rtx']:
                        sender_rtx_enqueue_data['spurious rtx'][flow_id] = {'time': [], 'count': []}
                    count = len(sender_rtx_enqueue_data['spurious rtx'][flow_id]['count']) + 1
                    sender_rtx_enqueue_data['spurious rtx'][flow_id]['time'].append(time)
                    sender_rtx_enqueue_data['spurious rtx'][flow_id]['count'].append(count)

                    if flow_id not in spurious_rtx_packet:
                        spurious_rtx_packet[flow_id] = {'time': [], 'psn': []}
                    spurious_rtx_packet[flow_id]['time'].append(time)
                    spurious_rtx_packet[flow_id]['psn'].append(psn)
                    
                    if 'all' not in sender_rtx_enqueue_data['spurious rtx']:
                        sender_rtx_enqueue_data['spurious rtx']['all'] = {'time': [], 'count': []}
                    count = len(sender_rtx_enqueue_data['spurious rtx']['all']['count']) + 1
                    sender_rtx_enqueue_data['spurious rtx']['all']['time'].append(time)
                    sender_rtx_enqueue_data['spurious rtx']['all']['count'].append(count)
                else:
                    psn_idx = drop_trim_packet[flow_id].index(psn)
                    loss_detection_time = np.append(loss_detection_time, (time - drop_trim_packet_timestamps[flow_id][psn_idx]))
                    drop_trim_packet[flow_id].pop(psn_idx)
                    drop_trim_packet_timestamps[flow_id].pop(psn_idx)
                
                # all flows
                flow_id = 'all'
                if flow_id not in data[pkt_type]:
                    data[pkt_type][flow_id] = {'time': [], 'count': []}

                count = len(data[pkt_type][flow_id]['count']) + 1
                data[pkt_type][flow_id]['time'].append(time)
                data[pkt_type][flow_id]['count'].append(count)
                continue

    # get rtx prediction
    for flow_id in spurious_rtx_packet:
        for time, psn in zip(spurious_rtx_packet[flow_id]['time'], spurious_rtx_packet[flow_id]['psn']):
            if (flow_id in drop_trim_packet) and (psn in drop_trim_packet[flow_id]):
                # record correct prediction
                psn_idx = drop_trim_packet[flow_id].index(psn)
                drop_trim_packet[flow_id].pop(psn_idx)
                drop_trim_packet_timestamps[flow_id].pop(psn_idx)
                # spurious rtx
                if flow_id not in sender_rtx_enqueue_data['spurious rtx for future loss']:
                    sender_rtx_enqueue_data['spurious rtx for future loss'][flow_id] = {'time': [], 'count': []}
                count = len(sender_rtx_enqueue_data['spurious rtx for future loss'][flow_id]['count']) + 1
                sender_rtx_enqueue_data['spurious rtx for future loss'][flow_id]['time'].append(time)
                sender_rtx_enqueue_data['spurious rtx for future loss'][flow_id]['count'].append(count)
                
                if 'all' not in sender_rtx_enqueue_data['spurious rtx for future loss']:
                    sender_rtx_enqueue_data['spurious rtx for future loss']['all'] = {'time': [], 'count': []}
                count = len(sender_rtx_enqueue_data['spurious rtx for future loss']['all']['count']) + 1
                sender_rtx_enqueue_data['spurious rtx for future loss']['all']['time'].append(time)
                sender_rtx_enqueue_data['spurious rtx for future loss']['all']['count'].append(count)
    if 'all' in sender_rtx_enqueue_data['spurious rtx for future loss']:
        sender_rtx_enqueue_data['spurious rtx for future loss']['all']['time'].sort()

    # queue util
    queue_util_data = {}
    cwnd_data = {}
    with open(file_path, 'r') as file:
        for line in file:
            # queue util
            data = queue_util_data # queue id: size, time
            pattern = r'(\d+\.?\d*) queue (\d+) low size (\d+) max (\d+)' # 1.3328 queue 133 low size 4160 max 178880
            match = re.match(pattern, line.strip())
            if match:
                time = float(match.group(1))
                queue_id = int(match.group(2))
                queue_size = int(match.group(3))
                max_size = int(match.group(4))
                
                # each flow
                if queue_id not in data:
                    data[queue_id] = {'time': [], 'util': []}

                data[queue_id]['time'].append(time)
                data[queue_id]['util'].append(queue_size/max_size)
                
                # all flows
                queue_id = 'all'
                if queue_id not in data:
                    data[queue_id] = {'time': [], 'util': []}
                
                last_util = []
                for key, queue_data in data.items():
                    if isinstance(key, int):
                        last_util.append(queue_data['util'][-1])
                max_util = max(last_util)
                data[queue_id]['time'].append(time)
                data[queue_id]['util'].append(max_util)
                continue
            
            # queue util
            data = cwnd_data # queue id: size, time
            pattern = r'(\d+\.?\d*) flow (\d+) cwnd (\d+)' # 1.9968 flow 7 cwnd 178880
            match = re.match(pattern, line.strip())
            if match:
                time = float(match.group(1))
                flow_id = int(match.group(2))
                cwnd = int(match.group(3))
                
                # each flow
                if flow_id not in data:
                    data[flow_id] = {'time': [], 'cwnd': []}

                data[flow_id]['time'].append(time)
                data[flow_id]['cwnd'].append(cwnd)
                
                # all flows
                flow_id = 'all'
                if flow_id not in data:
                    data[flow_id] = {'time': [], 'min': [], 'q01': [], 'q25': [], 'q50': [], 'q75': [], 'q99': [], 'max': []}
                
                last_cwnd = []
                for key, flow_cwnd_data in data.items():
                    if isinstance(key, int):
                        last_cwnd.append(flow_cwnd_data['cwnd'][-1])
                last_cwnd = np.array(last_cwnd, dtype=float)
                data[flow_id]['time'].append(time)
                data[flow_id]['min'].append(last_cwnd.min())
                data[flow_id]['q01'].append(np.quantile(last_cwnd, 0.01))
                data[flow_id]['q25'].append(np.quantile(last_cwnd, 0.25))
                data[flow_id]['q50'].append(np.quantile(last_cwnd, 0.50))
                data[flow_id]['q75'].append(np.quantile(last_cwnd, 0.75))
                data[flow_id]['q99'].append(np.quantile(last_cwnd, 0.99))
                data[flow_id]['max'].append(last_cwnd.max())
                continue


    return sender_send_data,receiver_receive_data,switch_drop_data,switch_trim_data,sender_rtx_enqueue_data,loss_detection_time,queue_util_data,cwnd_data

def draw_flow_trace(sender_send_data,receiver_receive_data,switch_drop_data,switch_trim_data,sender_rtx_enqueue_data,queue_util_data,cwnd_data,plot_dir,experiment):
    # Ensure the plot directory exists
    # if not os.path.exists(plot_dir):
    #     os.makedirs(plot_dir)
    
    for flow_id in ['all']: # tqdm(data_send_data.keys()):
        plt.figure()
        def draw_single_line(line_data, label, color, **kwargs):
            if flow_id in line_data.keys():
                plt.plot(line_data[flow_id]['time'], line_data[flow_id]['count'], color=color, **kwargs)
                plt.annotate(
                    label, 
                    (line_data[flow_id]['time'][-1], line_data[flow_id]['count'][-1]), 
                    xytext=(line_data[flow_id]['time'][-1]*1.02, line_data[flow_id]['count'][-1]), 
                    color=color,
                    fontsize=8,
                    weight='bold',
                    textcoords="data", 
                    va='center',  # Centered vertically
                    ha='left',  # Left align the text box horizontally
                )
        
        def draw_block(data, label_prefix, color, **kwargs):
            for idx, pkt_type in enumerate(data.keys()):
                line_data = data[pkt_type]
                draw_single_line(line_data, label=f'{label_prefix} {pkt_type}', color=color, marker=ms[idx], markersize=2, **kwargs)
        linestyle=''
        draw_block(sender_send_data, label_prefix='send',color='blue',linestyle=linestyle)
        # draw_block(receiver_send_data, label_prefix='Dst send',color='green')
        # receiver_send_data['pflr nack'] = correct_pflr_nack_count
        # draw_block({k:receiver_send_data[k] for k in receiver_send_data.keys() if k!='ack'}, label_prefix='Dst send',color='green')
        draw_block(sender_rtx_enqueue_data, label_prefix='enqueue rtx:',color='green',linestyle=linestyle)
        # draw_block(sender_receive_data, label_prefix='Src receive')
        # draw_block(receiver_receive_data, label_prefix='Dst receive')
        draw_block(switch_drop_data, label_prefix='drop',color='red',linestyle=linestyle)
        draw_block(switch_trim_data, label_prefix='trim',color='red',linestyle=linestyle)

        plt.xlabel('Time (us)')
        plt.ylabel('Count')
        # plt.title(f'Trace for Flow {flow_id}')
        plt.title("\n".join(textwrap.wrap(experiment.cli_params.get_fig_title(), width=80)))
        # plt.legend(bbox_to_anchor=(1, 0), loc='lower left')
        plt.grid(True)
        
        # Save the plot
        plot_filename = os.path.join(plot_dir, f"flow_{flow_id}_trace")
        plt.savefig(plot_filename+'.png', bbox_inches='tight', pad_inches=0.01, dpi = 500)
        # plt.savefig(plot_filename+'.svg', bbox_inches='tight', pad_inches=0.01)
        # plt.savefig(plot_filename+'.pdf', bbox_inches='tight', pad_inches=0.01)
        plt.close()  # Close the figure to avoid displaying or memory overload

        # # draw a new plot
        # plt.figure()
        fig, (ax1, ax2) = plt.subplots(2, 1, sharex=True, figsize=(6, 8))
        sender_send_types = len(sender_send_data.keys())
        sender_rtx_enqueue_types = len(sender_rtx_enqueue_data.keys())
        switch_drop_types = len(switch_drop_data.keys())
        switch_trim_types = len(switch_trim_data.keys())

        y_position_lst = []
        label_lst = []
        # Plot horizontal lines and scatter points
        def draw_hline_scatter(data, key_list, marker_lst, label_prefix, **kwargs):
            idx = 0
            for key in key_list:
                if (key in data) and (flow_id in data[key]):
                    # plot 1: line
                    linestyle=''
                    ax1.plot(data[key][flow_id]['time'], data[key][flow_id]['count'],
                            marker=marker_lst[idx], markersize=2,markeredgewidth=1,linestyle=linestyle,**kwargs)
                    ax1.annotate(
                        f'{label_prefix} {key}', 
                        (data[key][flow_id]['time'][-1], data[key][flow_id]['count'][-1]), 
                        xytext=(data[key][flow_id]['time'][-1]*1.02, data[key][flow_id]['count'][-1]), 
                        color=kwargs['c'],
                        fontsize=8,
                        weight='bold',
                        textcoords="data", 
                        va='center',  # Centered vertically
                        ha='left',  # Left align the text box horizontally
                    )
                    # plot 2: scatter
                    current_y_position = len(y_position_lst) + 1
                    y_position_lst.append(current_y_position)
                    label_lst.append(f'{label_prefix} {key}')
                    ax2.scatter(data[key][flow_id]['time'], [current_y_position] * len(data[key][flow_id]['time']), 
                                marker=marker_lst[idx],s=5,linewidths=1,**kwargs)
                    idx += 1

                    # current_y_position = len(y_position_lst) + 1
                    # y_position_lst.append(current_y_position)
                    # label_lst.append(f'{label_prefix} {key}')
                    # plt.scatter(data[key][flow_id]['time'], [current_y_position] * len(data[key][flow_id]['time']), s=5,**kwargs)
        
        draw_hline_scatter(sender_send_data, ['data', 'proactive data probe', 'probe'], marker_lst=ms, label_prefix='send',c='blue')
        draw_hline_scatter(switch_drop_data, ['data','trim','probe', 'sleek probe'], marker_lst=ms, label_prefix='drop',c='red')
        draw_hline_scatter(switch_trim_data, ['data'], marker_lst=ms[5:], label_prefix='trim',c='red')
        draw_hline_scatter(sender_rtx_enqueue_data, ['rto', 'nack', 'sleek', 'pflr0', 'spurious rtx', 'spurious rtx for future loss'], marker_lst=ms,label_prefix='rtx enqueue:', c='green')
        draw_hline_scatter(sender_send_data, ['rtx', 'proactive rtx probe'], marker_lst=ms[5:], label_prefix='send',c='blue')
        draw_hline_scatter(switch_drop_data, ['rtx'], marker_lst=ms[5:], label_prefix='drop',c='red')
        draw_hline_scatter(switch_trim_data, ['rtx'], marker_lst=ms[10:],label_prefix='trim',c='red')

        ax1.set_ylabel('Count')
        ax2.set_xlabel('Time [us]')
        ax2.set_yticks(y_position_lst, label_lst)
        fig.suptitle("\n".join(textwrap.wrap(experiment.cli_params.get_fig_title(), width=80)))
        # plt.legend(bbox_to_anchor=(1, 0), loc='lower left')
        ax1.grid(True)
        ax2.grid(True)

        # plt.xlabel('Time (us)')
        # plt.yticks(y_position_lst, label_lst)
        # # plt.ylabel('Count')
        # # plt.title(f'Trace for Flow {flow_id}')
        # plt.title("\n".join(textwrap.wrap(experiment.cli_params.get_fig_title(), width=80)))
        # # plt.legend(bbox_to_anchor=(1, 0), loc='lower left')
        # plt.grid(True)
        
        # Save the plot
        plot_filename = os.path.join(plot_dir, f"flow_{flow_id}_trace_all")
        plt.savefig(plot_filename+'.png', bbox_inches='tight', pad_inches=0.01, dpi = 500)
        # plt.savefig(plot_filename+'.svg', bbox_inches='tight', pad_inches=0.01)
        # plt.savefig(plot_filename+'.pdf', bbox_inches='tight', pad_inches=0.01)
        plt.close() 
    
    # queue util
    for queue_id in ['all']: # tqdm(data_send_data.keys()):
        if queue_id in queue_util_data:
            plt.figure()
            x = queue_util_data[queue_id]['time']
            y = queue_util_data[queue_id]['util']
            max_value = max(y)
            max_index = y.index(max_value)
            max_x = x[max_index]
            plt.plot(x, y, marker='x', markersize=2)
            plt.axhline(y=max_value, color='r', linestyle='--')
            plt.text(max_x, max_value, f'{max_value}', color='black', ha='left', va='bottom')

            plt.xlabel('Time (us)')
            plt.ylabel('Max Queue Util')
            # plt.title(f'Trace for Flow {flow_id}')
            plt.title("\n".join(textwrap.wrap(experiment.cli_params.get_fig_title(), width=80)))
            # plt.legend(bbox_to_anchor=(1, 0), loc='lower left')
            plt.grid(True)
            
            # Save the plot
            plot_filename = os.path.join(plot_dir, f"queue_{flow_id}_util")
            plt.savefig(plot_filename+'.png', bbox_inches='tight', pad_inches=0.01, dpi = 500)
            # plt.savefig(plot_filename+'.svg', bbox_inches='tight', pad_inches=0.01)
            # plt.savefig(plot_filename+'.pdf', bbox_inches='tight', pad_inches=0.01)
            plt.close()  # Close the figure to avoid displaying or memory overload  
    
    # cwnd
    for flow_id in ['all']: # tqdm(data_send_data.keys()):
        if flow_id in cwnd_data:
            plt.figure()
            x = cwnd_data[flow_id]['time']
            plt.plot(x, cwnd_data[flow_id]['max'], label = 'max cwnd', marker='x', markersize=2)
            plt.plot(x, cwnd_data[flow_id]['q99'], label = 'q99 cwnd', marker='x', markersize=2)
            plt.plot(x, cwnd_data[flow_id]['q75'], label = 'q75 cwnd', marker='x', markersize=2)
            plt.plot(x, cwnd_data[flow_id]['q50'], label = 'q50 cwnd', marker='x', markersize=2)
            plt.plot(x, cwnd_data[flow_id]['q25'], label = 'q25 cwnd', marker='x', markersize=2)
            plt.plot(x, cwnd_data[flow_id]['q01'], label = 'q01 cwnd', marker='x', markersize=2)
            plt.plot(x, cwnd_data[flow_id]['min'], label = 'min cwnd', marker='x', markersize=2)

            plt.xlabel('Time (us)')
            plt.ylabel('Cwnd [Byte]')
            # plt.title(f'Trace for Flow {flow_id}')
            plt.title("\n".join(textwrap.wrap(experiment.cli_params.get_fig_title(), width=80)))
            plt.legend(bbox_to_anchor=(1, 0), loc='lower left')
            # plt.legend()
            plt.grid(True)
            
            # Save the plot
            plot_filename = os.path.join(plot_dir, f"cwnd_{flow_id}")
            plt.savefig(plot_filename+'.png', bbox_inches='tight', pad_inches=0.01, dpi = 500)
            # plt.savefig(plot_filename+'.svg', bbox_inches='tight', pad_inches=0.01)
            # plt.savefig(plot_filename+'.pdf', bbox_inches='tight', pad_inches=0.01)
            plt.close()  # Close the figure to avoid displaying or memory overload  

def save_stat(sender_send_data, queue_util_data, sender_rtx_enqueue_data, loss_detection_time, dirpath):
    stat = {}
    # max queue util
    stat['max_queue_util'] = max(queue_util_data['all']['util'])
    
    # pkt stat
    try:
        stat['total_data'] = sender_send_data['data']['all']['count'][-1]
    except:
        stat['total_data'] = 0

    try:
        stat['total_rtx'] = sender_send_data['rtx']['all']['count'][-1]
    except:
        stat['total_rtx'] = 0

    try:
        stat['total_data_probe'] = sender_send_data['proactive data probe']['all']['count'][-1]
    except:
        stat['total_data_probe'] = 0

    try:
        stat['total_rtx_probe'] = sender_send_data['proactive rtx probe']['all']['count'][-1]
    except:
        stat['total_rtx_probe'] = 0

    try:
        stat['total_probe'] = sender_send_data['probe']['all']['count'][-1]
    except:
        stat['total_probe'] = 0

    try:
        stat['spurious_rtx'] = sender_rtx_enqueue_data['spurious rtx']['all']['count'][-1]
    except:
        stat['spurious_rtx'] = 0

    # Save dictionary to JSON file
    with open(os.path.join(dirpath, "stat.json"), "w") as json_file:
        json.dump(stat, json_file)
    
    np.save(os.path.join(dirpath, "loss_detection_time.npy"), loss_detection_time)


# Main function to process all output.txt files
def process_output_files(dirs_with_output_txt, num_processes):
    # mp
    results = []
    with multiprocessing.Pool(processes=num_processes) as pool:
        # List to keep track of result objects
        result_objects = []
        for dirpath in dirs_with_output_txt:
            result = pool.apply_async(process_output_file, args=(dirpath,))
            result_objects.append(result)

        # Retrieve results
        for result in result_objects:
            try:
                results.append(result.get())  # .get() waits for the result and retrieves it
            except Exception as e:
                results.append(f"Error: {e}")
    # single process
    # results = []
    # for dirpath in dirs_with_output_txt:
    #     results.append(process_output_file(dirpath))

    output = {}
    for key in results[0]:
        output[key] = [result[key] for result in results]
    
    return output

def process_output_file(dirpath):
    experiment = Experiment(dirpath)
    print(f"current working dir: {dirpath}")
    output_file = os.path.join(dirpath, "output.txt")

    # Parse output.txt for PSN progression data
    sender_send_data,receiver_receive_data,switch_drop_data,switch_trim_data,sender_rtx_enqueue_data,loss_detection_time,queue_util_data,cwnd_data = parse_output_file(output_file)

    save_stat(sender_send_data, queue_util_data, sender_rtx_enqueue_data, loss_detection_time,dirpath)
    
    # Create a subdirectory to store plots
    plot_dir = os.path.join(dirpath, "plots")
    if os.path.exists(plot_dir):
        shutil.rmtree(plot_dir)
    os.makedirs(plot_dir)
    draw_flow_trace(sender_send_data,receiver_receive_data,switch_drop_data,switch_trim_data,sender_rtx_enqueue_data,queue_util_data,cwnd_data,plot_dir,experiment)
    # plot hist for loss_detection_time
    plt.figure(figsize=(8, 6))
    plt.hist(loss_detection_time, bins=60, edgecolor='k', alpha=0.7)

    # Add title and labels
    plt.title('Distribution of Loss Detection Time', fontsize=16)
    plt.xlabel('Loss Detection Time', fontsize=14)
    plt.ylabel('Frequency', fontsize=14)

    # Show grid
    plt.grid(axis='y', linestyle='--', alpha=0.7)

    # Save the plot as an image file
    plot_filename = os.path.join(dirpath, f"loss_detection_time")
    plt.savefig(plot_filename+'.png', bbox_inches='tight', pad_inches=0.01, dpi = 500)
    plt.close()

    # return something
    # plot 1
    loss_detection_time_filename = os.path.join(dirpath, f"loss_detection_time.npy")
    # plot 2
    max_queue_util = max(queue_util_data['all']['util'])

    total_data = sender_send_data['data']['all']['count'][-1]
    try:
        total_rtx = sender_send_data['rtx']['all']['count'][-1]
    except:
        total_rtx = 0

    try:
        total_data_probe = sender_send_data['proactive data probe']['all']['count'][-1]
    except:
        total_data_probe = 0

    try:
        total_rtx_probe = sender_send_data['proactive rtx probe']['all']['count'][-1]
    except:
        total_rtx_probe = 0

    try:
        total_ev_change_probe = sender_send_data['probe']['all']['count'][-1]
    except:
        total_ev_change_probe = 0
    
    result = {'loss_detection_filename': loss_detection_time_filename,
            'dirpath': dirpath,
            'max_queue_util': max_queue_util,
            'link_overhead': (total_data_probe+total_rtx_probe+total_ev_change_probe)*64/total_data/(4096+64),
            'total_rtx': total_rtx,
            }

    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-i', '--input',
        type=str,
        default="../outputs",
        help='path input directory')
    parser.add_argument(
        '-p', '--parallel',
        type=int,
        default=64,
        help='run experiments in parallel, and set the size of the multiprocessing pool'
    )
    args = parser.parse_args()
    input_dir =  args.input # Replace with your actual directory
    dirs_with_output_txt = find_output_txt_files(input_dir)
    output = process_output_files(dirs_with_output_txt, args.parallel)
    with open(os.path.join(input_dir, "output.json"), "w") as json_file:
        json.dump(output, json_file)
