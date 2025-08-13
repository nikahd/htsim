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

def plot_percentile_values(data, percentile, show_legend, save_dir):
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
                heights[setup].append(np.percentile(data[workload][setup], percentile))
            else:
                heights[setup].append(0)

    bar_width = 0.15
    x_indices = np.arange(len(workloads))

    plt.figure(figsize=(6, 2))
    plt.grid(linestyle='--', linewidth=0.5, alpha=0.7, zorder=0)

    for i, setup in enumerate(setups):
        bars = plt.bar(x_indices + i * bar_width, heights[setup], bar_width, label=setup)
        # Add text labels on top of each bar
        for bar in bars:
            height = bar.get_height()
            plt.text(bar.get_x() + bar.get_width() / 2, height, f'{height:.0f}', 
                    ha='center', va='bottom', fontsize=8)

    plt.xticks(x_indices + bar_width * (len(setups) - 1) / 2, workloads)
    if show_legend:
        plt.xlabel("Workload")
        plt.ylabel(f"Loss Detection Time {percentile} Percentile [us]")
        plt.legend(loc='lower left', bbox_to_anchor=(1, 0))
    plt.ylim([0, 100])
    plt.tight_layout()
    plt.savefig(os.path.join(save_dir, f'loss_detection_time_percentile{percentile}_legend_{"on" if show_legend else "off"}.png'), bbox_inches='tight', pad_inches=0.01, dpi = 500)
    plt.close()

def plot_violin_distribution(data, show_legend, save_dir):
    """
    Plots a violin plot for the value distributions of setups for each workload.

    Args:
        data (dict): Dictionary of workloads and their corresponding setup data.
    """
    workloads = []
    setups = []
    values = []

    for workload in ['P. 1MB', 'P. 2MB', 'I. 1MB', 'I. 2MB']:
        setups_dict = data[workload]
        for setup in ['RTO', 'Trim', 'Trim[Low]', 'pfld_3,probe_0', 'pfld_3,probe_1', 'pfld_3,probe_4']:  # Iterate in the predefined order
            if setup in setups_dict:
                setup_values = setups_dict[setup]
                workloads.extend([workload] * len(setup_values))
                setups.extend([setup] * len(setup_values))
                values.extend(list(setup_values))

    # for workload, setups_dict in data.items():
    #     for setup, setup_values in setups_dict.items():
    #         workloads.extend([workload] * len(setup_values))
    #         setups.extend([setup] * len(setup_values))
    #         values.extend(list(setup_values))

    plt.figure(figsize=(6, 2))
    ax = sns.violinplot(x=workloads, y=values, hue=setups, split=False, density_norm="width", inner=None, bw_method=0.2, linewidth=0.2, cut=0)

    percentile_99_values = {}
    for workload in ['P. 1MB', 'P. 2MB', 'I. 1MB', 'I. 2MB']:
        for setup in ['RTO', 'Trim', 'Trim[Low]', 'pfld_3,probe_0', 'pfld_3,probe_1', 'pfld_3,probe_4']:
            p99 = np.percentile(data[workload][setup], 99)  # Compute the 99th percentile
            percentile_99_values[(workload, setup)] = p99

            # Print 99th percentile values to console
            print(f"Workload: {workload}, Setup: {setup}, 99th Percentile: {p99:.2f}")

            # Plot the 99th percentile as a dot
            # ax.scatter(workload, p99, color='red', s=100, edgecolors='none', zorder=3)

    if show_legend:
        plt.xlabel("Workload")
        plt.ylabel("Loss Detection Time[us]")
        # plt.title("Value Distributions for Workloads and Setups")
        plt.legend(loc='lower left', bbox_to_anchor=(1, 0))
    else:
        ax.legend_.remove()
    plt.ylim([0, 120])
    plt.tight_layout()
    plt.savefig(os.path.join(save_dir, f'loss_detection_time_dist_legend_{"on" if show_legend else "off"}.png'), bbox_inches='tight', pad_inches=0.01, dpi = 500)
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
    plot_percentile_values(loss_detection_time_data, 99, show_legend=False, save_dir = input_dir)
    plot_percentile_values(loss_detection_time_data, 99, show_legend=True, save_dir = input_dir)
    plot_violin_distribution(loss_detection_time_data, show_legend=False, save_dir = input_dir)
    plot_violin_distribution(loss_detection_time_data, show_legend=True, save_dir = input_dir)
