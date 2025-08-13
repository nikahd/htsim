# -*- coding: utf-8 -*-
# To run this script, python3 sim/msft_ai/scripts/draw_1ecbr_output_options_5runs.py

import numpy as np 
import matplotlib.pyplot as plt

from matplotlib.ticker import ScalarFormatter

from collections import defaultdict
import os

colors = ['skyblue', 'lightgreen', 'salmon', 'plum', 'lightcoral', 'lightgoldenrodyellow', 'lightcyan', 'lavender', 'lightpink', 'lightseagreen', 'lightsalmon', 'lightsteelblue', 'lightyellow']

connection_matrices = [
    "2_1_2_200MB_same_rack_dcqcn.cm",
#    "one_one_1_200MB_dcqcn.cm",
#    "one_one_2_200MB_dcqcn.cm",
#    "one_one_4_200MB_dcqcn.cm",
#    "one_one_8_200MB_dcqcn.cm",
#    "one_one_16_200MB_dcqcn.cm",
#    "one_one_32_200MB_dcqcn.cm",
#    "one_one_64_200MB_dcqcn.cm",
#    "one_one_128_200MB_dcqcn.cm",
#    "one_one_256_200MB_dcqcn.cm",
]
# flow_numbers = [1, 2, 4, 8, 2, 4]  # Number of flows for each connection matrix, should be aligned with connection_matrices array
flow_numbers = [2, 1, 2, 4, 8, 16, 32, 64, 128, 256]  # Number of flows for each connection matrix, should be aligned with connection_matrices array

 
is_link_down = [0]
# drop_rate = ["mean", "p99"]
drop_rate = ["0"]
use_jitter = 0
experiments = [1]
enable_pfc = 1

n_subflows = 8
 
folder_name = "msft_ai_wan_dcqcn"
os.makedirs(f"{folder_name}/option_plots", exist_ok=True)

total_statistics = defaultdict(dict)

 
for matrix in connection_matrices:
    total_statistics[matrix] = defaultdict(dict)
    for link_down in is_link_down:
        total_statistics[matrix][link_down] = defaultdict(dict)
        for droprate in drop_rate:
            total_statistics[matrix][link_down][droprate] = defaultdict(dict)
            for exp in experiments:
                sim_file_name = f"{folder_name}/statistics_{matrix[:-3]}_linkdown{link_down}_droprate{droprate}_usejitter{use_jitter}_pfc{enable_pfc}_exp{exp}.txt"
                print("sim_file_name:", sim_file_name)
                if not os.path.exists(sim_file_name):
                    continue
                print(f"Processing {sim_file_name}...")
                with open(sim_file_name, "r") as f:
                    lines = f.readlines()
                    lines = [line.strip() for line in lines if line.strip()]
                    receiving_rate_dict = defaultdict(list)
                    for line in lines:
                        if "receiving_rate" in line:
                            parts = line.split()
                            flow_id = parts[1]
                            timestamp = float(parts[3])
                            receiving_rate = float(parts[-2])
                            if flow_id not in receiving_rate_dict:
                                receiving_rate_dict[flow_id] = []
                            receiving_rate_dict[flow_id].append((timestamp, receiving_rate * 8))  # Convert to bits
                            if len(receiving_rate_dict[flow_id]) > 1000000:
                                break
                    if len(receiving_rate_dict) < flow_numbers[connection_matrices.index(matrix)]:
                        print(f"Warning: Not enough flows in {sim_file_name}. Expected {flow_numbers[connection_matrices.index(matrix)]}, found {len(receiving_rate_dict)}")

                    total_statistics[matrix][link_down][droprate] = receiving_rate_dict

  

 

# plt.rcParams.update({'font.size': 14})  # Set global font size

def plot_rate(rate_dict):
    for flow_id, rates in rate_dict.items():
        timestamps, bitrates = zip(*rates)
        plt.plot(timestamps, bitrates, label=f"Flow {flow_id}", linewidth=2)

rtt = 30000 # in us

for matrix in connection_matrices:
    for link_down in is_link_down:
        for droprate in drop_rate:
            figure_file_name = f"{folder_name}/option_plots/fairness_receiving_rate_{matrix}_link{link_down}_usejitter{use_jitter}_droprate{droprate}_pfc{enable_pfc}_initcwnd0.6.png"
            print(f"Plotting {figure_file_name}")
            plt.figure(figsize=(14, 6))

            plot_rate(total_statistics[matrix][link_down][droprate])
            max_time = 0
            for flow_id, rates in total_statistics[matrix][link_down][droprate].items():
                timestamps, bitrates = zip(*rates)
                max_time = max(max_time, max(timestamps))

            plt.title(f"Receiving rates for {matrix} (Link Down: {link_down}, Drop Rate: {droprate})")
            print(f"max_time: {max_time} us")
            max_time_range = max_time // rtt
            rtts = np.arange(0, max_time_range + 1) * rtt  
            plt.xticks(rtts, [f"{int(rtt/1000)} ms" for rtt in rtts], rotation=45)
            plt.ylabel("Receiving Rate (bps)")

            plt.legend(loc='upper left', bbox_to_anchor=(0.8, 1), borderaxespad=0)
            # plt.legend()
            plt.savefig(figure_file_name)
    