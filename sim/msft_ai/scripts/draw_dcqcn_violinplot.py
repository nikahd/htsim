# -*- coding: utf-8 -*-
# To run this script, python3 sim/msft_ai/scripts/draw_1ecbr_output_options_5runs.py

import numpy as np 
import matplotlib.pyplot as plt

from matplotlib.ticker import ScalarFormatter

from collections import defaultdict
import os

colors = ['skyblue', 'lightgreen', 'salmon', 'plum', 'lightcoral', 'lightgoldenrodyellow', 'lightcyan', 'lavender', 'lightpink', 'lightseagreen', 'lightsalmon', 'lightsteelblue', 'lightyellow']

connection_matrices = [
   "one_one_1_200MB_dcqcn.cm",
   "one_one_2_200MB_dcqcn.cm",
   "one_one_4_200MB_dcqcn.cm",
   "one_one_8_200MB_dcqcn.cm",
   "one_one_16_200MB_dcqcn.cm",
#    "one_one_32_200MB_dcqcn.cm",
#    "one_one_64_200MB_dcqcn.cm",
#    "one_one_128_200MB_dcqcn.cm",
#    "one_one_256_200MB_dcqcn.cm",
]
# flow_numbers = [1, 2, 4, 8, 2, 4]  # Number of flows for each connection matrix, should be aligned with connection_matrices array
flow_numbers = [1, 2, 4, 8, 16, 32, 64, 128, 256]  # Number of flows for each connection matrix, should be aligned with connection_matrices array

 
is_link_down = [0]
# drop_rate = ["mean", "p99"]
drop_rate = ["0"]
use_jitter = 0
experiments = [1]

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
            logging_statistics = defaultdict(dict)
            for exp in experiments:
                sim_file_name = f"{folder_name}/statistics_{matrix[:-3]}_linkdown{link_down}_droprate{droprate}_usejitter{use_jitter}_exp{exp}.txt"
                print("sim_file_name:", sim_file_name)
                if not os.path.exists(sim_file_name):
                    continue
                print(f"Processing {sim_file_name}...")
                with open(sim_file_name, "r") as f:
                    lines = f.readlines()
                    lines = [line.strip() for line in lines if line.strip()]
                    fct_dict = defaultdict(list)
                    for line in lines:
                        if "finish" in line:
                            parts = line.split()
                            flow_id = parts[1]
                            fct = float(parts[5])
                            fct_dict[flow_id].append(fct)
                    if len(fct_dict) < flow_numbers[connection_matrices.index(matrix)]:
                        print(f"Warning: Not enough flows in {sim_file_name}. Expected {flow_numbers[connection_matrices.index(matrix)]}, found {len(fct_dict)}")
                         
                    print(fct_dict)

                    # Aggregate statistics
                    sorted_fct_items = sorted(fct_dict.items(), key=lambda item: int(item[0][5:]))
                    subflow_id = 0
                    aggr_fct = defaultdict(dict)
                    for flow_id, fct_list in sorted_fct_items:
                        if flow_id != "DCQCN" + str(subflow_id):
                            error_msg = f"Flow ID mismatch: expected DCQCN{subflow_id}, found {flow_id} in {sim_file_name}"
                            print(error_msg)
                            subflow_id += 1
                            aggr_fct[int(subflow_id / n_subflows)] = {"flow_completion_time": float('inf')}
                        else:
                            aggr_fct[int(subflow_id / n_subflows)]["flow_completion_time"] = max(aggr_fct[int(subflow_id / n_subflows)].get("flow_completion_time", 0), fct_list[0])
                            subflow_id += 1

                    # Store statistics
                    for flow_id, stats in aggr_fct.items():
                        logging_statistics["exp"+str(exp) + str(flow_id)] = {
                            "flow_completion_time": stats["flow_completion_time"],
                        }
            print(logging_statistics)
            total_statistics[matrix][link_down][droprate] = logging_statistics

# Statistics to plot: x axis is cbr rate, y axis is the statistics value, each label is a threshold configuration

plot_statistics = ["flow_completion_time"]

def plot_func_all(x_values, y_values):
    # check if some entry in y_values is empty
    if any(len(y) == 0 for y in y_values):
        print("Warning: Some entries in y_values are empty. replacing with infinite value.")
        y_values = [[float('inf')] if len(y) == 0 else y for y in y_values]
    
    violin_parts  = plt.violinplot(y_values, positions=x_values, showmeans=True, showmedians=True, showextrema=True, widths=0.8)
    for i, pc in enumerate(violin_parts['bodies']):
        pc.set_facecolor(colors[i % len(colors)])
        pc.set_edgecolor('black')
        pc.set_alpha(0.7)
    # set mean line color
    violin_parts['cmeans'].set_edgecolor('red')
    violin_parts['cmeans'].set_linewidth(2)
    # label the mean line
    plt.text(x_values[-1] + 0.2, np.mean(y_values[-1]), 'Mean', color='red', fontsize=12, verticalalignment='center')
    # set median line color
    violin_parts['cmedians'].set_edgecolor('blue')
    violin_parts['cmedians'].set_linewidth(2)
    # label the median line
    plt.text(x_values[-1] + 0.2, np.median(y_values[-1]), 'Median', color='blue', fontsize=12, verticalalignment='center')
    # set extrema line color
    violin_parts['cmaxes'].set_edgecolor('black')
    violin_parts['cmins'].set_edgecolor('black')
   

def get_ylabel(plots):
    if plots == "flow_completion_time":
        return "Flow Completion Time (us)"
    else:
        return plots


plt.rcParams.update({'font.size': 14})  # Set global font size

# Plot statistics: x axis is cbr rate, y axis is the statistics value, each label is a threshold configuration
for plots in plot_statistics:
    for matrix in connection_matrices:
        for link_down in is_link_down:
            for droprate in drop_rate:
                figure_file_name = f"{folder_name}/option_plots/{plots}_{matrix}_link{link_down}_usejitter{use_jitter}_droprate{droprate}_initcwnd0.7.png"
                print(f"Plotting {figure_file_name}")
                plt.figure(figsize=(24, 16))
                
             
                y_values = []
                pre_aggregated_statistics = []

                stats_dict = total_statistics[matrix][link_down][droprate]
                for flow_id, stats in stats_dict.items():
                    pre_aggregated_statistics.append(stats[plots])
                y_values.append(pre_aggregated_statistics)

                x_values = [1]

                plot_func_all(x_values, y_values)

                vertical_line_positions = np.arange(0) + 1
                for x_pos in vertical_line_positions:
                    plt.axvline(x=x_pos + 0.5, color="grey", linestyle="--")

                plt.gca().yaxis.set_major_formatter(ScalarFormatter(useMathText=False))
                plt.ticklabel_format(style='plain', axis='y')
                
                plt.xlabel("Configurations")
                
                plt.ylabel(get_ylabel(plots))
                plt.title(f"{get_ylabel(plots)} for DCQCN")
                plt.xticks(np.arange(1, 2),
                        ["dcqcn"],
                        rotation=0, ha='right')
                plt.legend(loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
                # plt.legend()
                plt.savefig(figure_file_name)
        