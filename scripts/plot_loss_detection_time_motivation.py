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
    if ':pfld_' in filepath:
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
    elif ':no_trim' in filepath:
        setup = 'RTO'
    elif ':low_trim' in filepath:
        setup = 'Trim[Low]'
    else:
        setup = 'Trim'
    return workload, setup

def print_percentile_values(data, percentile, show_legend, save_dir):
    """
    Plots the average values of setups for each workload.

    Args:
        data (dict): Dictionary of workloads and their corresponding setup data.
    """
    workloads = list(data.keys())
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
                heights[setup].append(np.percentile(data[workload][setup], percentile))
            else:
                heights[setup].append(0)

    print(workloads)
    for i, setup in enumerate(setups):
        print(f"value{heights[setup]}, setup{setup}")

def plot_percentile_values(data, percentile, show_legend, save_dir):
    """
    Plots the average values of setups for each workload.

    Args:
        data (dict): Dictionary of workloads and their corresponding setup data.
    """
    workloads = list(data.keys())
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
                heights[setup].append(np.percentile(data[workload][setup], percentile))
            else:
                heights[setup].append(0)

    bar_width = 0.15
    x_indices = np.arange(len(workloads))

    plt.figure(figsize=(6, 2))
    plt.grid(linestyle='--', linewidth=0.5, alpha=0.7, zorder=0)

    for i, setup in enumerate(setups):
        plt.bar(x_indices + i * bar_width, heights[setup], bar_width, label=setup)

    plt.xticks(x_indices + bar_width * (len(setups) - 1) / 2, workloads)
    if show_legend:
        plt.xlabel("Workload")
        plt.ylabel(f"Loss Detection Time {percentile} Percentile [us]")
        plt.legend(loc='lower left', bbox_to_anchor=(1, 0))
    plt.tight_layout()
    plt.xlim([-0.5, 1.5])
    plt.savefig(os.path.join(save_dir, f'motivation_plot_legend_{"on" if show_legend else "off"}.png'), bbox_inches='tight', pad_inches=0.01, dpi = 500)
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
    loss_detection_time_files = output['loss_detection_filename']

    loss_detection_time_data = {}
    for loss_detection_time_file in loss_detection_time_files:
        # Read the .npy file
        array_data = np.load(loss_detection_time_file)
        # Extract workload and setup
        workload, setup = extract_workload_and_setup(loss_detection_time_file)
        # Store data
        try:
            loss_detection_time_data[workload][setup] = array_data
        except:
            loss_detection_time_data[workload] = {}
            loss_detection_time_data[workload][setup] = array_data

    # plot avg
    print_percentile_values(loss_detection_time_data, 99, show_legend=False, save_dir = input_dir)


    # plot
    # use Incast 2 MB
    loss_detection_time_data = {
        'No Silent Drop':{
            'RTO': 86.5488,
            'Trim': 8.082600000000001,
        },
        '0.1% Silent Drop':{
            'RTO': 86.5488,
            'Trim': 85.50726600000002,
        }
    }
    plot_percentile_values(loss_detection_time_data, 99, show_legend=False, save_dir = os.path.dirname(input_dir))
    plot_percentile_values(loss_detection_time_data, 99, show_legend=True, save_dir = os.path.dirname(input_dir))