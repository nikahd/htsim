# -*- coding: utf-8 -*-
# To run this script, python3 sim/msft_ai/scripts/draw_kecbr_output_options_5runs.py

import numpy as np 
import matplotlib.pyplot as plt

from matplotlib.ticker import ScalarFormatter

from collections import defaultdict
import os


foldername = "msft_ai_wan_1ec_cc"
filename = "output_4_1_4_2GB_linkdown0_dropratep99_usejitter1_initcwnd0.7_bitmapsize1024_recoverable1_isfullskip0_fastrecoverable0_bitmapfulllossy1_fullpercent0.9_lossreplace0_jitterreplace0_mimd0_pfc0_exp1.txt"

os.makedirs(f"{foldername}/compare_plots", exist_ok=True)
figure_name = f"{foldername}/compare_plots/" + "queue_size_4_1_4_disable_pfc" + ".png"


this_queue_name = "queue(100000Mb/s,10500000bytes)Queue--Reg1-DC0-LeafGroup0ToRGroup15ToR0->Serv15(0)"

with open(foldername + "/" + filename, "r") as file:
    lines = file.readlines()
    queue_sizes = defaultdict(list)
    for line in lines:
        if "queue_size:" in line and line.startswith("LOSSLESS queue, name:"):
            x = line.split(" ")
            queue_name = x[3]
            if queue_name == this_queue_name:
                timestamp = x[4]
                queue_size = int(x[-1])
                queue_sizes[queue_name].append((float(timestamp), queue_size))
                # print(timestamp, queue_size)
                if float(timestamp) > 700000:
                    break

plot_queue_size = queue_sizes[this_queue_name]
plt.figure(figsize=(10, 6))
plt.plot([x[0] for x in plot_queue_size], [x[1]
                for x in plot_queue_size], label="Queue Size", color='blue')
plt.xlabel("Time (ms)")
plt.ylabel("Queue Size (bytes)")            
plt.title(f"Queue Size for {this_queue_name}")
plt.savefig(figure_name)
