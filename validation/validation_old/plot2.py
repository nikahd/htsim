import os
import re
import argparse
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt

def size_to_readable(size_bytes):
    """
    Converts size in bytes to a human-readable format (Bytes, KiB, MiB).
    
    Args:
        size_bytes (int): Size in bytes.
    
    Returns:
        str: Size in a human-readable format (Bytes, KiB, MiB).
    """
    if size_bytes >= 2**20:
        return f"{size_bytes / 2**20:.1f} MiB"
    elif size_bytes >= 2**10:
        return f"{size_bytes / 2**10:.1f} KiB"
    else:
        return f"{size_bytes} Bytes"

def extract_completion_time(filepath):
    """
    Extracts the max flow completion time from a file.
    
    Args:
        filepath (str): Path to the input file.
    
    Returns:
        float: The max completion time found in the file, or None if not found.
    """
    with open(filepath, 'r') as file:
        fcts = []
        for line in file:
            match = re.search(r'finished at (\d+\.\d+)', line)
            if match:
                fcts.append(float(match.group(1)))
            
    return max(fcts)

def plot_fct_line(args):
    """
    Creates a line plot for flow completion times (FCT) vs. message sizes.
    
    Args:
        source_folder (str): The path to the folder containing the input text files.
        output_file (str): The name of the output image file.
    """
    # Initialize an empty list to store data
    data = []

    # Iterate over files in the source folder
    for filename in os.listdir(args.source_folder):
        if filename.endswith(".tmp"):
            # Parse the message size and algorithm name from the filename
            parts = filename.split("_")
            try:
                message_size = int(parts[1].replace("size", ""))
            except ValueError:
                continue  # Skip files with unexpected naming convention
            algorithm = "_".join(parts[2:]).replace("name", "").strip()

            # Extract the first completion time from the file
            file_path = os.path.join(args.source_folder, filename)
            completion_time = extract_completion_time(file_path)

            # Plot FCT or % to ideal
            ideal_fct = None
            incast_degree = args.incast_degree
            if (args.show_theoretical):
                if (args.workload == "incast"):
                    ideal_fct = (message_size * incast_degree * 8) / (args.link_speed)
                elif (args.workload == "permutation"):
                    ideal_fct = (message_size * 8) / (args.link_speed / args.oversubscription)

                #print(f"Ideal FCT for message size {message_size} and algorithm {algorithm}: {ideal_fct/1000} vs {completion_time}")
                #print(f"Incast Degree {incast_degree} and link_speed {args.link_speed}")
                if ideal_fct is not None:
                    ideal_fct = ideal_fct / 1000
                    ideal_fct += 8.6
                    to_ideal = (completion_time / ideal_fct)
                    data.append({
                        "Message Size": message_size,
                        "Algorithm": algorithm,
                        "FCT": to_ideal
                    })
            else:
                if completion_time is not None:
                    data.append({
                        "Message Size": message_size,
                        "Algorithm": algorithm,
                        "FCT": completion_time
                    })

    # Convert the list of records into a DataFrame
    df = pd.DataFrame(data)

    if df.empty:
        print("No valid data found in the source folder.")
        return
    
    # Filter and print DataFrame for Message Size = 1
    """ df_filtered = df[df["Message Size"] == 4]
    print("Filtered DataFrame for Message Size = 1:")
    print(df_filtered) """

    # Create the line plot using Seaborn
    sns.set_theme(style="whitegrid")
    plt.figure(figsize=(10, 6))
    ax = sns.lineplot(data=df, x="Message Size", y="FCT", hue="Algorithm", marker="o")

    # Set the x-axis to log2 scale
    ax.set_xscale("log", base=2)

    # Get unique message sizes and set them as x-ticks
    unique_message_sizes = sorted(df["Message Size"].unique())
    ax.set_xticks(unique_message_sizes)

    # Customizing x-axis labels to use human-readable sizes
    new_ticks = [size_to_readable(int(tick)) for tick in unique_message_sizes]
    ax.set_xticklabels(new_ticks)
    
    # Customize plot appearance
    if (args.show_theoretical):
        plt.title("Flow Completion Times compared to ideal vs. Message Sizes", fontsize=14)
    else:
        plt.title("Flow Completion Times (FCT) vs. Message Sizes", fontsize=14)
    plt.xlabel("Message Size (Bytes)", fontsize=12)
    if (args.show_theoretical):
        plt.ylabel("FCT / Ideal FCT", fontsize=12)
    else:
        plt.ylabel("Flow Completion Time (ms)", fontsize=12)
    plt.legend(title="Algorithm", fontsize=10, loc="best")
    
    # Save the plot
    plt.savefig(f"{args.source_folder}/{args.output_file}", dpi=300, bbox_inches="tight")
    plt.show()

def main():
    """
    Main function to parse arguments and call the plotting function.
    """
    parser = argparse.ArgumentParser(description="Plot Flow Completion Times vs. Message Sizes.")
    parser.add_argument(
        "--source_folder", 
        type=str, 
        help="Path to the folder containing the input text files."
    )
    parser.add_argument(
        "--output_file", 
        type=str, 
        default="fct_line_plot.png", 
        help="Name of the output image file (default: fct_line_plot.png)."
    )
    parser.add_argument("--workload", type=str, choices=["incast", "permutation"], required=True, 
                    help="Type of workload: 'incast' or 'permutation'.")
    parser.add_argument("--incast_degree", type=int, help="Degree of incast (required if workload is 'incast').")
    parser.add_argument("--oversubscription", type=int, required=True, 
                        help="Oversubscription ratio of the network.")
    parser.add_argument("--link_speed", type=int, required=True, 
                        help="Link speed in Gbps.")
    parser.add_argument("--show_theoretical", action="store_true", help="Flag to show theoretical results (default: False).")

    args = parser.parse_args()

    # Call the plotting function with parsed arguments
    plot_fct_line(args)

if __name__ == "__main__":
    main()
