# -*- coding: utf-8 -*-
# To run this script, python3 sim/msft_ai/scripts/draw_kecbr_output_options_5runs.py

import numpy as np 
import matplotlib.pyplot as plt

from matplotlib.ticker import ScalarFormatter

from collections import defaultdict
import os

colors = ['skyblue', 'lightgreen', 'salmon', 'plum', 'lightcoral', 'lightgoldenrodyellow', 'lightcyan', 'lavender', 'lightpink', 'lightseagreen', 'lightsalmon', 'lightsteelblue', 'lightyellow']

connection_matrices = [
    "4_1_4_2GB.cm",
    # "2_1_2_200MB_same_rack.cm",
#    "2_1_2_200MB.cm",
#    "4_1_4_200MB.cm",
#    "one_one_1_200MB.cm",
#    "one_one_2_200MB.cm",
#    "one_one_4_200MB.cm",
#    "one_one_8_200MB.cm",
#    "one_one_16_200MB.cm",
#    "one_one_32_200MB.cm",
#     "one_one_64_200MB.cm",
#     "one_one_128_200MB.cm",
#     "one_one_256_200MB.cm",
]
# flow_numbers = [1, 2, 4, 8, 2, 4]  # Number of flows for each connection matrix, should be aligned with connection_matrices array
flow_numbers = [4, 2, 4, 1, 2, 4, 8, 16, 32, 64, 128, 256]  # Number of flows for each connection matrix, should be aligned with connection_matrices array

bitmap_size = [1024]
is_link_down = [0]
# drop_rate = ["0", "mean", "p99"]
drop_rate = ["p99"]
init_cwnd_ratio = 0.7   
recoverable_threshold = [1]  # recoverable threshold
full_percent = [0.9]
as_fast_recoverable = [0]  
jitter_path_replace_threshold = [0]
loss_path_replace_threshold = [0]
full_skip=[0]
experiments = [1]
use_jitter = 0
apply_mimd = 0
enable_pfc = 0

# need to be configured based on the kec file
original_num_chunks = 2004 * 3 
stripe_num_per_message = 3
rtt = 3000 # in us

folder_name = "msft_ai_wan_1ec_cc"
os.makedirs(f"{folder_name}/option_plots", exist_ok=True)

total_statistics = defaultdict(dict)


for percent in full_percent:
    total_statistics[percent] = defaultdict(dict)
    for fast_rec in as_fast_recoverable:
        total_statistics[percent][fast_rec] = defaultdict(dict)
        for isfullskip in full_skip:
            total_statistics[percent][fast_rec][isfullskip] = defaultdict(dict)
            for recoverable in recoverable_threshold:
                total_statistics[percent][fast_rec][isfullskip][recoverable] = defaultdict(dict)
                for init_cwnd in [init_cwnd_ratio] if isinstance(init_cwnd_ratio, float) else init_cwnd_ratio:
                    total_statistics[percent][fast_rec][isfullskip][recoverable][init_cwnd] = defaultdict(dict)
                    for matrix in connection_matrices:
                        total_statistics[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix] = defaultdict(dict)
                        for link_down in is_link_down:
                            total_statistics[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down] = defaultdict(dict)
                            for droprate in drop_rate:
                                total_statistics[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate] = defaultdict(dict)
                                 
                                for bitmapsize in bitmap_size:
                                    total_statistics[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate][bitmapsize] = defaultdict(dict)
                                    for loss_replace in loss_path_replace_threshold:
                                        total_statistics[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate][bitmapsize][loss_replace] = defaultdict(dict)
                                        for jitter_replace in jitter_path_replace_threshold:
                                            total_statistics[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate][bitmapsize][loss_replace][jitter_replace] = defaultdict(dict)
                                            logging_statistics = defaultdict(dict)
                                            for exp in experiments:
                                                sim_file_name = f"{folder_name}/statistics_{matrix[:-3]}_linkdown{link_down}_droprate{droprate}_usejitter{use_jitter}_initcwnd{init_cwnd}_bitmapsize{bitmapsize}_recoverable{recoverable}_isfullskip{isfullskip}_fastrecoverable{fast_rec}_bitmapfulllossy1_fullpercent{percent}_lossreplace{loss_replace}_jitterreplace{jitter_replace}_mimd{apply_mimd}_pfc{enable_pfc}_exp{exp}.txt"
                                                if not os.path.exists(sim_file_name):
                                                    continue
                                                print(f"Processing {sim_file_name}...")
                                                
                                                with open(sim_file_name, "r") as f:
                                                    cwnd_dict = defaultdict(list)
                                                    lines = f.readlines()
                                                    lines = [line.strip() for line in lines if line.strip()]
                                                    for line in lines:
                                                        if "receiving_rate" in line and "finish" not in line:
                                                            parts = line.split()
                                                            flow_id = parts[1]
                                                            timestamp = float(parts[3])
                                                            cwnd_in_bits = float(parts[5]) * 8;   
                                                            if flow_id not in cwnd_dict:
                                                                cwnd_dict[flow_id] = []
                                                            cwnd_dict[flow_id].append((timestamp, cwnd_in_bits))
                                                        # if "Timeout" in line:


                                                    if len(cwnd_dict) < flow_numbers[connection_matrices.index(matrix)]:
                                                        print(f"Not enough flow data in {sim_file_name}. Expected {flow_numbers[connection_matrices.index(matrix)]}, found {len(cwnd_dict)}.")

                                           
                                                total_statistics[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate][bitmapsize][loss_replace][jitter_replace] = cwnd_dict


def plot_sending_rate(sending_rate_dict):
    for flow_id, rates in sending_rate_dict.items():
        timestamps, sending_rates = zip(*rates)
        sending_rates = [rate for rate in sending_rates]  # Convert to Mbps
        plt.plot(timestamps, sending_rates, label=f"Flow {flow_id}", linewidth=2)
        plt.scatter(timestamps, sending_rates, s=20, alpha=0.5)  # Add scatter points for better visibility
        for i in range(len(timestamps)):
            if i > 0 and sending_rates[i] < sending_rates[i - 1]:
                print(f"Flow {flow_id} receiving rate decreased at time {timestamps[i]} {timestamps[i-1]}: {sending_rates[i]} < {sending_rates[i - 1]}")
    
 
 

# plt.rcParams.update({'font.size': 14})  # Set global font size


for matrix in connection_matrices:
    for link_down in is_link_down:
        for droprate in drop_rate:
            for bitmapsize in bitmap_size:
                figure_file_name = f"{folder_name}/option_plots/fairness_{matrix}_link{link_down}_usejitter{use_jitter}_droprate{droprate}_mimd{apply_mimd}_initcwnd{init_cwnd}_pfc{enable_pfc}.png"
                print(f"Plotting {figure_file_name}")
                plt.figure(figsize=(10, 6))
                # Prepare all group combinations
                group_combos = []
                n_groups = 0 
                for percent in full_percent:
                    for rec in recoverable_threshold:
                        for loss_replace in loss_path_replace_threshold:
                            for jitter_replace in jitter_path_replace_threshold:
                                group_combos.append((percent, rec, loss_replace, jitter_replace))
                                n_groups += 1

                max_time = 0
                for group_idx, (percent, rec, loss_replace, jitter_replace) in enumerate(group_combos):
                    plot_statistics = total_statistics[percent][fast_rec][isfullskip][rec][init_cwnd][matrix][link_down][droprate][bitmapsize][loss_replace][jitter_replace]
                    plot_sending_rate(plot_statistics)
                    for flow_id, rates in plot_statistics.items():
                        timestamps, sending_rates = zip(*rates) 
                        max_time = max(max_time, max(timestamps))  # Update max_time with the latest timestamp
                       
                    # print(total_statistics[percent][fast_rec][isfullskip][rec][init_cwnd][matrix][link_down][droprate][bitmapsize][loss_replace][jitter_replace])
                    break # plot the first configuration

                plt.title(f"receiving_rate for {matrix} (Link Down: {link_down}, Drop Rate: {droprate})")
                # plt.xlabel("Time (us)")
                # label the x-axis with RTTs 
                
                plt.grid(True, which='both', linestyle='--', linewidth=0.5)
                plt.xlabel("Time (ms)")
                plt.ylabel("receiving_rate (bits/sec)")
                
                # plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
                plt.legend()
                plt.savefig(figure_file_name)