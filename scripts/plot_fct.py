import argparse
import json
import re
import os
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from collections import defaultdict

def extract_workload(workload_dir):
    workload_dict = {'serialn_alltoall_4': 'AlltoAll 4', 
                    'serialn_alltoall_8': 'AlltoAll 8',
                    'allreduce': 'Ring\nAllreduce',
                    'allreduce_butterfly': 'Butterfly\nAllreduce'}
    return workload_dict[workload_dir]

def plot_fct(data, show_legend, save_dir):
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
                heights[setup].append(data[workload][setup])
            else:
                heights[setup].append(0)

    max_height = max(max(heights[setup]) for setup in setups)
    y_lim = max_height * 1.1  # Increase y-limit by 20% for label space
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
            plt.text(bar.get_x() + bar.get_width() / 2, height, f'{height:.0f}', 
                    ha='center', va='bottom', fontsize=6)

    plt.xticks(x_indices + bar_width * (len(setups) - 1) / 2, workloads)
    if show_legend:
        plt.xlabel("Workload")
        plt.ylabel(f"FCT[us]")
        # plt.title("Average Values for Workloads and Setups")
        plt.legend(loc='lower left', bbox_to_anchor=(1, 0))
    plt.ylim([0, y_lim])
    plt.tight_layout()
    plt.savefig(os.path.join(save_dir, f'fct_legend_{"on" if show_legend else "off"}.png'), bbox_inches='tight', pad_inches=0.01, dpi = 500)
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
    # all dir:
    workload_dirs = [d for d in os.listdir(input_dir) if os.path.isdir(os.path.join(input_dir, d))]
    workload_fcts = [pd.read_csv(os.path.join(input_dir, d, 'fct.csv')) for d in workload_dirs]
    for workload_fct in workload_fcts:
        workload_fct['setup'] = workload_fct['scheme']

    fct_data = {}
    for workload_idx in range(len(workload_dirs)):
        workload_dir = workload_dirs[workload_idx]
        workload_fct = workload_fcts[workload_idx]
        workload = extract_workload(workload_dir)
        for setup_idx in range(len(workload_fct)):
            setup = workload_fct.iloc[setup_idx]['setup']
            fct = workload_fct.iloc[setup_idx]['max_fct_us']
            # Store data
            try:
                fct_data[workload][setup] = fct
            except:
                fct_data[workload] = {}
                fct_data[workload][setup] = fct

    # plot avg
    plot_fct(fct_data, show_legend=True, save_dir = input_dir)
    plot_fct(fct_data, show_legend=False, save_dir = input_dir)
