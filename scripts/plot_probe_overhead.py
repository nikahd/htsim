import argparse
import json
import re
import os
import matplotlib.pyplot as plt
import numpy as np
import seaborn as sns
from collections import defaultdict

def extract_workload_and_setup(filepath):
    # workload
    workload_match = re.match(r'.*matrix=(permutation|incast):msg_size_bytes=(\d+).*', filepath)
    matrix = workload_match.group(1)
    msg_size_bytes = int(workload_match.group(2))
    msg_size_mega_bytes = msg_size_bytes//1024//1024
    workload = f"{matrix[0].upper()}. {msg_size_mega_bytes}MB"

    # setup
    setup=''
    if ':pfld_2' in filepath:
        setup += 'pfld_2'
    elif ':pfld_3' in filepath:
        setup += 'pfld_3'

    if ':probe_0' in filepath:
        setup += ',probe_0'
    elif ':probe_1' in filepath:
        setup += ',probe_1'
    elif ':probe_4' in filepath:
        setup += ',probe_4'

    return workload, setup

def plot_queue_overhead(data, show_legend, save_dir):
    """
    Plots the average values of setups for each workload.

    Args:
        data (dict): Dictionary of workloads and their corresponding setup data.
    """
    workloads = list(data.keys())
    workloads = ['P. 1MB', 'P. 2MB', 'I. 1MB', 'I. 2MB']
    setups = set()
    for setups_dict in data.values():
        setups.update(setups_dict.keys())
    setups = sorted(setups)

    x = []
    heights = defaultdict(list)

    for workload in workloads:
        x.append(workload)
        for setup in setups:
            if setup in data[workload]:
                heights[setup].append((data[workload][setup]-1)*100)
            else:
                heights[setup].append(0)

    bar_width = 0.15
    x_indices = np.arange(len(workloads))

    plt.figure(figsize=(6, 2))
    plt.grid(linestyle='--', linewidth=0.7, alpha=0.7, zorder=0)

    for i, setup in enumerate(setups):
        bars = plt.bar(x_indices + i * bar_width, heights[setup], bar_width, label=setup)
        print(f"value{heights[setup]}, setup{setup}")
        # Add text labels on top of each bar
        for bar in bars:
            height = bar.get_height()
            plt.text(bar.get_x() + bar.get_width() / 2, height, f'{height:.2f}', 
                    ha='center', va='bottom', fontsize=6)

    plt.xticks(x_indices + bar_width * (len(setups) - 1) / 2, workloads)
    if show_legend:
        plt.xlabel("Workload")
        plt.ylabel(f"Max Queue \n Size Overhead[%]")
        # plt.title("Average Values for Workloads and Setups")
        plt.legend(loc='lower left', bbox_to_anchor=(1, 0))
    plt.ylim([0, 0.12])
    plt.tight_layout()
    plt.savefig(os.path.join(save_dir, f'queue_size_overhead_legend_{"on" if show_legend else "off"}.png'), bbox_inches='tight', pad_inches=0.01, dpi = 500)
    plt.close()

def plot_link_overhead(data, show_legend, save_dir):
    """
    Plots the average values of setups for each workload.

    Args:
        data (dict): Dictionary of workloads and their corresponding setup data.
    """
    workloads = list(data.keys())
    workloads = ['P. 1MB', 'P. 2MB', 'I. 1MB', 'I. 2MB']
    setups = set()
    for setups_dict in data.values():
        setups.update(setups_dict.keys())
    setups = sorted(setups)

    x = []
    heights = defaultdict(list)

    for workload in workloads:
        x.append(workload)
        for setup in setups:
            if setup in data[workload]:
                heights[setup].append(data[workload][setup]*100)
            else:
                heights[setup].append(0)

    bar_width = 0.15
    x_indices = np.arange(len(workloads))

    plt.figure(figsize=(6, 2))
    plt.grid(linestyle='--', linewidth=0.5, alpha=0.7, zorder=0)

    for i, setup in enumerate(setups):
        bars = plt.bar(x_indices + i * bar_width, heights[setup], bar_width, label=setup)
        print(f"value{heights[setup]}, setup{setup}")
        # Add text labels on top of each bar
        for bar in bars:
            height = bar.get_height()
            plt.text(bar.get_x() + bar.get_width() / 2, height, f'{height:.1f}', 
                    ha='center', va='bottom', fontsize=8)

    plt.xticks(x_indices + bar_width * (len(setups) - 1) / 2, workloads)
    if show_legend:
        plt.xlabel("Workload")
        plt.ylabel(f"Link Bandwidth \n Overhead[%]")
        # plt.title("Average Values for Workloads and Setups")
        plt.legend(loc='lower left', bbox_to_anchor=(1, 0))
    plt.ylim([0, 3.3])
    plt.tight_layout()
    plt.savefig(os.path.join(save_dir, f'link_bandwidth_overhead_legend_{"on" if show_legend else "off"}.png'), bbox_inches='tight', pad_inches=0.01, dpi = 500)
    plt.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-i', '--input',
        type=str,
        default="../outputs",
        help='path input directory')
    args = parser.parse_args()

    input_dir = args.input # Replace with your actual directory
    with open(os.path.join(input_dir, 'output.json'), 'r') as file:
        output = json.load(file)
    dirpath = output['dirpath']
    max_queue_util = output['max_queue_util']
    link_overhead = output['link_overhead']

    max_queue_util_data = {}
    link_overhead_data = {}
    for idx in range(len(output['dirpath'])):
        # Read the .npy file
        dirpath = output['dirpath'][idx]
        max_queue_util = output['max_queue_util'][idx]
        link_overhead = output['link_overhead'][idx]
        # Extract workload and setup
        workload, setup = extract_workload_and_setup(dirpath)
        # Store data
        try:
            max_queue_util_data[workload][setup] = max_queue_util
            link_overhead_data[workload][setup] = link_overhead
        except:
            max_queue_util_data[workload] = {}
            link_overhead_data[workload] = {}
            max_queue_util_data[workload][setup] = max_queue_util
            link_overhead_data[workload][setup] = link_overhead

    # plot avg
    plot_queue_overhead(max_queue_util_data, show_legend=True, save_dir = input_dir)
    plot_queue_overhead(max_queue_util_data, show_legend=False, save_dir = input_dir)
    plot_link_overhead(link_overhead_data, show_legend=True, save_dir = input_dir)
    plot_link_overhead(link_overhead_data, show_legend=False, save_dir = input_dir)
