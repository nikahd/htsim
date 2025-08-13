# -*- coding: utf-8 -*-
# To run this script, python3 sim/msft_ai/scripts/draw_kecbr_output_options_5runs.py

import numpy as np 
import matplotlib.pyplot as plt

from matplotlib.ticker import ScalarFormatter

from collections import defaultdict
import os

colors = ['skyblue', 'lightgreen', 'salmon', 'plum', 'lightcoral', 'lightgoldenrodyellow', 'lightcyan', 'lavender', 'lightpink', 'lightseagreen', 'lightsalmon', 'lightsteelblue', 'lightyellow']

connection_matrices = [
   "one_one_2_200MB.cm",
   "one_one_4_200MB.cm",
   "one_one_8_200MB.cm",
   "one_one_16_200MB.cm",
   "one_one_32_200MB.cm",
   "one_one_64_200MB.cm",
]
flow_numbers = [2, 4, 8, 16, 32, 64]  
bitmap_size = [256]
is_link_down = [1]
drop_rate = ["mean"]
init_cwnd_ratio = 0.7
recoverable_threshold = [1]
full_percent = [0.9]
as_fast_recoverable = [0]
jitter_path_replace_threshold = [0]
loss_path_replace_threshold = [0]
full_skip = [0]
experiments = [1, 2, 3, 4, 5]
use_jitter = 1


# need to be configured based on the kec file
original_num_chunks = 2004 * 3 
stripe_num_per_message = 3

folder_name_1 = "msft_ai_wan_1ecbr"
folder_name_k = "msft_ai_wan_kecbr"
os.makedirs(f"{folder_name_k}/compare_plots", exist_ok=True)

total_statistics_k = defaultdict(dict)
total_statistics_1 = defaultdict(dict)


plt.rcParams.update({'font.size': 18})  # Applies to all text elements


for percent in full_percent:
    total_statistics_k[percent] = defaultdict(dict)
    total_statistics_1[percent] = defaultdict(dict)
    for fast_rec in as_fast_recoverable:
        total_statistics_k[percent][fast_rec] = defaultdict(dict)
        total_statistics_1[percent][fast_rec] = defaultdict(dict)
        for isfullskip in full_skip:
            total_statistics_k[percent][fast_rec][isfullskip] = defaultdict(dict)
            total_statistics_1[percent][fast_rec][isfullskip] = defaultdict(dict)
            for recoverable in recoverable_threshold:
                total_statistics_k[percent][fast_rec][isfullskip][recoverable] = defaultdict(dict)
                total_statistics_1[percent][fast_rec][isfullskip][recoverable] = defaultdict(dict)
                for init_cwnd in [init_cwnd_ratio] if isinstance(init_cwnd_ratio, float) else init_cwnd_ratio:
                    total_statistics_k[percent][fast_rec][isfullskip][recoverable][init_cwnd] = defaultdict(dict)
                    total_statistics_1[percent][fast_rec][isfullskip][recoverable][init_cwnd] = defaultdict(dict)
                    for matrix in connection_matrices:
                        total_statistics_k[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix] = defaultdict(dict)
                        total_statistics_1[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix] = defaultdict(dict)
                        for link_down in is_link_down:
                            total_statistics_k[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down] = defaultdict(dict)
                            total_statistics_1[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down] = defaultdict(dict)
                            for droprate in drop_rate:
                                total_statistics_k[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate] = defaultdict(dict)
                                total_statistics_1[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate] = defaultdict(dict)
                                if link_down == 0 and droprate != "0" or link_down == 1 and droprate == "0":
                                    continue

                                 
                                for bitmapsize in bitmap_size:
                                    total_statistics_k[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate][bitmapsize] = defaultdict(dict)
                                    total_statistics_1[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate][bitmapsize] = defaultdict(dict)

                                    logging_statistics = defaultdict(dict)
                                    for exp in experiments:
                                        sim_file_name = f"{folder_name_k}/statistics_{matrix[:-3]}_linkdown{link_down}_droprate{droprate}_usejitter{use_jitter}_initcwnd{init_cwnd}_bitmapsize{bitmapsize}_recoverable{recoverable}_isfullskip{isfullskip}_fastrecoverable{fast_rec}_bitmapfulllossy1_fullpercent{percent}_exp{exp}.txt"
                                        
                                        
                                        if not os.path.exists(sim_file_name):
                                            continue
                                        print(f"Processing {sim_file_name}...")
                                        find_flows = 0
                                        with open(sim_file_name, "r") as f:
                                            lines = f.readlines()
                                            lines = [line.strip() for line in lines if line.strip()]
                                            cur_flow_id = None
                                            for line in lines:
                                                if "flow_completion_time" in line:
                                                    parts = line.split()
                                                    cur_flow_id = parts[2]
                                                    cur_flow_completion_time = float(parts[-3]) / 1000
                                                elif "Logging statistics" in line: 
                                                    parts = line.split()
                                                    num_received_messages = int(parts[4].strip(','))
                                                    num_received_chunks = int(parts[6].strip(','))
                                                    num_recovered_chunks = int(parts[8].strip(','))
                                                    num_bitmap_overflow_drops = int(parts[10].strip(','))
                                                    num_bitmap_underflow_drops = int(parts[12].strip(','))  
                                                    num_trivial_skips = int(parts[14].strip(','))
                                                    num_recoverable_skips = int(parts[16].strip(','))
                                                    num_lossy_skips = int(parts[18].strip(','))
                                                  

                                                    find_flows += 1
                                                        
                                                    logging_statistics["exp"+str(exp) + cur_flow_id] = {
                                                        "num_received_messages": num_received_messages,
                                                        "num_received_chunks": num_received_chunks,
                                                        "num_recovered_chunks": num_recovered_chunks,
                                                        "num_bitmap_overflow_drops": num_bitmap_overflow_drops,
                                                        "num_bitmap_underflow_drops": num_bitmap_underflow_drops,
                                                        "num_trivial_skips": num_trivial_skips,
                                                        "num_recoverable_skips": num_recoverable_skips,
                                                        "num_lossy_skips": num_lossy_skips,
                                                        "flow_completion_time": cur_flow_completion_time
                                                    }
                                        if find_flows < flow_numbers[connection_matrices.index(matrix)]:
                                            print(f"Not enough flow data in {sim_file_name}. Expected {flow_numbers[connection_matrices.index(matrix)]}, found {len(logging_statistics)}.")
                                             
                                    for flow_id, stats in logging_statistics.items():
                                        stats["num_retransmitted_chunks"] = stats["num_received_chunks"] - original_num_chunks
                                    print(logging_statistics)
                                    total_statistics_k[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate][bitmapsize] = logging_statistics



                                    logging_statistics = defaultdict(dict)
                                    for exp in experiments:
                                        sim_file_name = f"{folder_name_1}/statistics_{matrix[:-3]}_linkdown{link_down}_droprate{droprate}_usejitter{use_jitter}_initcwnd{init_cwnd}_bitmapsize{bitmapsize}_recoverable{recoverable}_isfullskip{isfullskip}_fastrecoverable{fast_rec}_bitmapfulllossy1_fullpercent{percent}_exp{exp}.txt"
                                        if not os.path.exists(sim_file_name):
                                            continue
                                        print(f"Processing {sim_file_name}...")
                                        find_flows = 0
                                        with open(sim_file_name, "r") as f:
                                            lines = f.readlines()
                                            lines = [line.strip() for line in lines if line.strip()]
                                            cur_flow_id = None
                                            for line in lines:
                                                if "flow_completion_time" in line:
                                                    parts = line.split()
                                                    cur_flow_id = parts[2]
                                                    cur_flow_completion_time = float(parts[-3]) / 1000
                                                elif "Logging statistics" in line: 
                                                    parts = line.split()
                                                    num_received_messages = int(parts[4].strip(','))
                                                    num_received_chunks = int(parts[6].strip(','))
                                                    num_recovered_chunks = int(parts[8].strip(','))
                                                    max_size_bitmap = int(parts[10].strip(','))
                                                    num_bitmap_overflow_drops = int(parts[12].strip(','))
                                                    num_bitmap_underflow_drops = int(parts[14].strip(','))  
                                                    num_trivial_skips = int(parts[16].strip(','))
                                                    num_recoverable_skips = int(parts[18].strip(','))
                                                    num_lossy_skips = int(parts[20].strip(','))
                                                  

                                                    find_flows += 1
                                                        
                                                    logging_statistics["exp"+str(exp) + cur_flow_id] = {
                                                        "num_received_messages": num_received_messages,
                                                        "num_received_chunks": num_received_chunks,
                                                        "num_recovered_chunks": num_recovered_chunks,
                                                        "num_bitmap_overflow_drops": num_bitmap_overflow_drops,
                                                        "num_bitmap_underflow_drops": num_bitmap_underflow_drops,
                                                        "num_trivial_skips": num_trivial_skips,
                                                        "num_recoverable_skips": num_recoverable_skips,
                                                        "num_lossy_skips": num_lossy_skips,
                                                        "flow_completion_time": cur_flow_completion_time
                                                    }
                                        if find_flows < flow_numbers[connection_matrices.index(matrix)]:
                                            print(f"Not enough flow data in {sim_file_name}. Expected {flow_numbers[connection_matrices.index(matrix)]}, found {len(logging_statistics)}.")
                                             
                                    for flow_id, stats in logging_statistics.items():
                                        stats["num_retransmitted_chunks"] = stats["num_received_chunks"] - original_num_chunks
                                    print(logging_statistics)
                                    total_statistics_1[percent][fast_rec][isfullskip][recoverable][init_cwnd][matrix][link_down][droprate][bitmapsize] = logging_statistics

# Statistics to plot: x axis is cbr rate, y axis is the statistics value, each label is a threshold configuration

plot_statistics = ["flow_completion_time", "num_recoverable_skips", "num_lossy_skips"]

def plot_func_all(x_values, y_values):
    # check if some entry in y_values is empty
    if any(len(y) == 0 for y in y_values):
        print("Warning: Some entries in y_values are empty. replacing with infinite value.")
        y_values = [[float('inf')] if len(y) == 0 else y for y in y_values]
    
    violin_parts  = plt.violinplot(y_values, positions=x_values, showmeans=True, showmedians=True, showextrema=True, widths=0.8)
    for i, pc in enumerate(violin_parts['bodies']):
        pc.set_facecolor(colors[x_values[0] % len(colors)])
        pc.set_edgecolor('black')
        pc.set_alpha(0.7)
    # set mean line color
    violin_parts['cmeans'].set_edgecolor('red')
    violin_parts['cmeans'].set_linewidth(2)
    # label the mean line
    plt.text(x_values[-1] + 0.2, np.mean(y_values[-1]), 'Mean', color='red', verticalalignment='center')
    # set median line color
    violin_parts['cmedians'].set_edgecolor('blue')
    violin_parts['cmedians'].set_linewidth(2)
    # label the median line
    plt.text(x_values[-1] + 0.2, np.median(y_values[-1]), 'Median', color='blue', verticalalignment='center')
    # set extrema line color
    violin_parts['cmaxes'].set_edgecolor('black')
    violin_parts['cmins'].set_edgecolor('black')
   

def get_ylabel(plots):
    if plots == "flow_completion_time":
        return "Flow Completion Time (ms)"
    elif plots == "num_retransmitted_chunks":
        return "Number of Retransmitted Stripes"
    elif plots == "num_recovered_chunks":
        return "Number of Recovered Stripes"
    elif plots == "num_bitmap_overflow_drops":
        return "Number of Bitmap Overflow Drops"
    elif plots == "num_bitmap_underflow_drops":
        return "Number of Bitmap Underflow Drops"
    elif plots == "num_trivial_skips":
        return "Number of Trivial Skips"
    elif plots == "num_recoverable_skips":
        return "Number of Recoverable Skips"
    elif plots == "num_lossy_skips":
        return "Number of Lossy Skips"
    elif plots == "max_size_bitmap":
        return "Max Bitmap Size"
    else:
        return plots
 

# Plot statistics: x axis is cbr rate, y axis is the statistics value, each label is a threshold configuration
for plots in plot_statistics:
    for matrix in connection_matrices:
        for link_down in is_link_down:
            for droprate in drop_rate:
                if link_down == 0 and droprate != "0" or link_down == 1 and droprate == "0":
                    continue
                for rec in recoverable_threshold:
                    figure_file_name = f"{folder_name_k}/compare_plots/{plots}_{matrix}_link{link_down}_usejitter{use_jitter}_droprate{droprate}_initcwnd0.7.png"
                    print(f"Plotting {figure_file_name}")
                    plt.figure(figsize=(6, 6))
                    # Prepare all group combinations
                    
                    n_groups = 2 

                    y_values = []
                    pre_aggregated_statistics = []
                    stats_dict = total_statistics_1[percent][fast_rec][isfullskip][rec][init_cwnd][matrix][link_down][droprate][bitmapsize]
                    for flow_id, stats in stats_dict.items():
                        pre_aggregated_statistics.append(stats[plots])
                    y_values.append(pre_aggregated_statistics)
                    x_values = np.array([1])
                    plot_func_all(x_values, y_values)

                    y_values = []
                    pre_aggregated_statistics = []
                    stats_dict = total_statistics_k[percent][fast_rec][isfullskip][rec][init_cwnd][matrix][link_down][droprate][bitmapsize]
                    for flow_id, stats in stats_dict.items():
                        pre_aggregated_statistics.append(stats[plots])
                    y_values.append(pre_aggregated_statistics)
                    x_values = np.array([2])
                    plot_func_all(x_values, y_values)

                    vertical_line_positions = np.arange(0) + 1
                    for x_pos in vertical_line_positions:
                        plt.axvline(x=x_pos + 0.5, color="grey", linestyle="--")

                    plt.gca().yaxis.set_major_formatter(ScalarFormatter(useMathText=False))
                    plt.ticklabel_format(style='plain', axis='y')
                    
                    plt.ylabel(get_ylabel(plots))
                    # plt.title(f"{get_ylabel(plots)} for {matrix} with Bitmap {256}")
                    plt.xticks(np.arange(1, n_groups + 1),
                            ["1ECBR", "KECBR"],
                            rotation=0)
                    # plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
                    # plt.legend()
                    # tight_layout
                    plt.tight_layout()
                    plt.savefig(figure_file_name)
            