import subprocess
import concurrent.futures
import os
import argparse

parallel_degree = 8
topology_file = "topologies/fat_tree_128_1os_2t_200g.topo"

# Function to run a command using subprocess
def run_command(cmd):
    print(f"Running command: {cmd}")
    os.system(cmd)

command_template = "./htsim_uec -end 10000 -seed 42 -tm connection_matrices/{matrix} -topo topologies/{topo_file} -q 50 -cwnd 65 -mtu 4160 -sack_threshold 0 -ecn 13 37 -linkspeed 200000 {info_lb} -sender_cc_only -sender_cc_algo nscc -o /home/tbonato/EuroSys/uec-transport-simulation-code/scripts/euroSys/permutation/topo=fat_tree:over_sub=8:nodes=1024:link_speed_gbps=100:matrix=permutation:msg_size_bytes=65536:asymmetry=sym/cc=uec_mprdma:lb=rss:rss_metric=mean_rtt:sf=16:ui=32:no_trim:pfld_2.htsim_data/log.dat > {output_file_name}"

message_sizes = [1, 2, 4, 8]
message_sizes = [4]
loss_algos = ["mixed_small", "mixed_small_trim", "rss_small", "rss_small_trim", "sleek_mixed_small", "sleek_rss_small", "rss_pflr_small"]
loss_algos = ["mixed_small", "mixed_small_trim", "rss_small", "rss_small_trim", "rss_pflr_small"]

def get_cm_cmd(size):
    if size == 1:
        return "perm_128n_128c_1MB.cm"
    elif size == 2:
        return "perm_128n_128c_2MB.cm"
    elif size == 4:
        return "perm_128n_128c_4MB.cm"
    elif size == 8:
        return "perm_128n_128c_8MB.cm"
    else:
        exit(0) 

def get_lb_cmd(info):
    if info == "mixed_small":
        return "-disable_trim -load_balancing_algo mixed -paths 128"
    elif info == "mixed_small_trim":
        return "-load_balancing_algo mixed -paths 128"
    elif info == "rss_small":
        return "-disable_trim -load_balancing_algo rss -rss_parameters mean_rtt 16 32 0 0 25"
    elif info == "rss_small_trim":
        return "-load_balancing_algo rss -rss_parameters mean_rtt 16 32 0 0 25"
    elif info == "sleek_mixed_small":
        return "-disable_trim -load_balancing_algo mixed -sleek -paths 128"
    elif info == "sleek_rss_small":
        return "-disable_trim -load_balancing_algo rss -rss_parameters mean_rtt 16 32 0 0 25 -sleek"
    elif info == "rss_pflr_small":
        return "-disable_trim -load_balancing_algo rss -rss_parameters mean_rtt 16 32 0 0 25 -precisefastlossrecovery 2"
    else:
        exit(0)

with concurrent.futures.ThreadPoolExecutor(max_workers=parallel_degree) as executor:

    folder = f"output_test_128_1os"
    if not os.path.exists(folder):
        os.makedirs(folder)

    for message_size in message_sizes:
        for loss_algo in loss_algos:
            
            file_name = f"output_size{message_size}_name{loss_algo}.txt"

            base_command = command_template.format(topo_file=topology_file, matrix=get_cm_cmd(message_size), info_lb=get_lb_cmd(loss_algo), output_file_name=f"{folder}/{file_name}")
            executor.submit(run_command, base_command)