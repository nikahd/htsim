import subprocess
import concurrent.futures
import os
import argparse
import shutil

# Function to run a command using subprocess
def run_command(cmd):
    print(f"Running command: {cmd}")
    os.system(cmd)

# TODO
# Need to automatically update CWND, ECN Marking and Queues sizes in a future work
command_template = "./htsim_uec -end 10000 -seed 42 -tm {matrix} -topo topologies/{topo_file} -q 50 -cwnd 50 -mtu 4160 -sack_threshold 0 -ecn 13 37 -linkspeed {link_speed} {info_lb} -sender_cc_only -sender_cc_algo nscc -o no_log.dat > {output_file_name}"
message_sizes = [2**14, 2**16, 2**18, 2**20, 2**22]
loss_algos = ["mixed", "mixed_trim", "rss", "rss_trim", "rss_pflr", "sleek_mixed", "sleek_rss"]

def get_lb_cmd(info):
    if info == "mixed":
        return "-disable_trim -load_balancing_algo reps -paths 128"
    elif info == "mixed_trim":
        return "-load_balancing_algo reps -paths 128"
    elif info == "rss":
        return "-disable_trim -load_balancing_algo rss -rss_parameters mean_rtt 16 32 0 0 25"
    elif info == "rss_trim":
        return "-load_balancing_algo rss -rss_parameters mean_rtt 16 32 0 0 25"
    elif info == "sleek_mixed":
        return "-disable_trim -load_balancing_algo reps -sleek -paths 128"
    elif info == "sleek_rss":
        return "-disable_trim -load_balancing_algo rss -rss_parameters mean_rtt 16 32 0 0 25 -sleek"
    elif info == "rss_pflr":
        return "-disable_trim -load_balancing_algo rss -rss_parameters mean_rtt 16 32 0 0 25 -precisefastlossrecovery 2 -pflr_proactive_probe 1 -pflr_proactive_rtx_probe -no_droping_low_header"
    else:
        exit(0)

def get_cm(args):
    list_cm = []
    if (args.workload == "incast"):
        for size in message_sizes:
            name_cm = f"incast_128n_{size}B_degree{args.incast_degree}.cm"
            list_cm.append(name_cm) 
            os.system(f"python3 connection_matrices/gen_incast.py {name_cm} 128 {args.incast_degree} {size} 0 42 1")
    elif (args.workload == "permutation"):
        for size in message_sizes:
            name_cm = f"perm_128n_{size}B.cm"
            list_cm.append(name_cm) 
            os.system(f"python3 connection_matrices/gen_permutation.py {name_cm} 128 128 {size} 0 42")

    return list_cm

def get_topology_file(oversubscription, link_speed):
    topology_file_template = "fat_tree_128_{os}os_2t_{bandwidth}g.topo" #Ideally later make the number of nodes a parameter
    return topology_file_template.format(os=oversubscription, bandwidth=link_speed)
    
def parse_args():
    parser = argparse.ArgumentParser(description="Parse networking workload parameters from the command line.")
    parser.add_argument("--oversubscription", type=int, required=True, 
                        help="Oversubscription ratio of the network.")
    parser.add_argument("--link_speed", type=int, required=True, 
                        help="Link speed in Gbps.")
    parser.add_argument("--workload", type=str, choices=["incast", "permutation"], required=True, 
                        help="Type of workload: 'incast' or 'permutation'.")
    parser.add_argument("--incast_degree", type=int, help="Degree of incast (required if workload is 'incast').")
    parser.add_argument("--parallel_degree", type=int, required=True, 
                        help="Parallel degree for the workload.")
    parser.add_argument("--result_folder", type=str, required=True, 
                        help="Where to save the results.")
    return parser.parse_args()

def main():
    args = parse_args()

    if not os.path.exists(args.result_folder):
            os.makedirs(args.result_folder)
    else:
        # Clear the folder by deleting its contents (files and subfolders)
        for item in os.listdir(args.result_folder):
            item_path = os.path.join(args.result_folder, item)
            if os.path.isfile(item_path) or os.path.islink(item_path):
                os.unlink(item_path)  # Remove file or symbolic link
            elif os.path.isdir(item_path):
                shutil.rmtree(item_path)  # Remove directory and all its contents

    connection_matrices = get_cm(args)
    
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.parallel_degree) as executor:

        for idx_msg, message_size in enumerate(message_sizes):
            for loss_algo in loss_algos:
                
                file_name = f"output_size{message_size}_name{loss_algo}.tmp"

                topology_file = get_topology_file(args.oversubscription, args.link_speed)

                print(f"Topology file: {topology_file}")
                print(f"Connection matrix: {connection_matrices[idx_msg]}")

                base_command = command_template.format(topo_file=topology_file, link_speed=int(int(args.link_speed)*1000), matrix=connection_matrices[idx_msg], info_lb=get_lb_cmd(loss_algo), output_file_name=f"{args.result_folder}/{file_name}")
                executor.submit(run_command, base_command)

if __name__ == "__main__":
    main()