import argparse
import subprocess

def run_scripts(link_speed, workload, oversubscription, parallel_degree, result_folder, incast_degree=None, show_theoretical=False):
    # Construct the arguments for the run.py script
    run_command = [
        "python3", "run.py",
        "--link_speed", str(link_speed),
        "--workload", workload,
        "--incast_degree", str(incast_degree) if incast_degree is not None else "0",
        "--oversubscription", str(oversubscription),
        "--parallel_degree", str(parallel_degree),
        "--result_folder", result_folder
    ]

    # Construct the arguments for the plot.py script
    plot_command = [
        "python3", "plot2.py",
        "--link_speed", str(link_speed),
        "--workload", workload,
        "--incast_degree", str(incast_degree) if incast_degree is not None else "0",
        "--oversubscription", str(oversubscription),
        "--source_folder", result_folder,
    ]

    if (show_theoretical):
        plot_command.append("--show_theoretical")

    # Run the run.py script
    print("Running run.py...")
    #print(" ".join(str(run_command)))
    subprocess.run(run_command, check=True)

    # Run the plot.py script
    print("Running plot.py...")
    subprocess.run(plot_command, check=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run simulation and plotting scripts sequentially.")
    parser.add_argument("--link_speed", type=int, required=True, help="Link speed for the simulation (Gbps).")
    parser.add_argument("--workload", type=str, choices=["incast", "permutation"], required=True, help="Workload type for the simulation.")
    parser.add_argument("--oversubscription", type=int, required=True, help="Oversubscription ratio.")
    parser.add_argument("--parallel_degree", type=int, required=True, help="Parallel degree for the simulation.")
    parser.add_argument("--result_folder", type=str, required=True, help="Folder to store the results.")
    parser.add_argument("--incast_degree", type=int, help="Degree of incast (required if workload is 'incast').")
    parser.add_argument("--show_theoretical", action="store_true", help="Flag to show theoretical results (default: False).")

    args = parser.parse_args()

    if args.workload == "incast" and args.incast_degree is None:
        parser.error("--incast_degree is required when workload is 'incast'.")

    run_scripts(
        link_speed=args.link_speed,
        workload=args.workload,
        oversubscription=args.oversubscription,
        parallel_degree=args.parallel_degree,
        result_folder=args.result_folder,
        incast_degree=args.incast_degree,
        show_theoretical=args.show_theoretical
    )
