import os
import re
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt

# Function to extract the maximum finished time from a file
def get_max_finished_time(file_path):
    max_time = 0
    with open(file_path, 'r') as file:
        for line in file:
            match = re.search(r'finished at (\d+\.\d+)', line)
            if match:
                finished_time = float(match.group(1))
                max_time = max(max_time, finished_time)
    return max_time

def getNewFileName(filename):
    if "sleek_mixed_small" in filename:
        return "LB:REPS\nTrim:Off\nLR:SLEEK"
    elif "sleek_rss_small" in filename:
        return "LB:RSS\nTrim:Off\nLR:SLEEK"
    elif "mixed_small_trim" in filename:
        return "LB:REPS\nTrim:On\nLR:No"
    elif "mixed_small" in filename:
        return "LB:REPS\nTrim:Off\nLR:No"
    elif "rss_small_trim" in filename:
        return "LB:RSS\nTrim:On\nLR:No"
    elif "rss_small" in filename:
        return "LB:RSS\nTrim:Off\nLR:No"
    elif "rss_pflr_small" in filename:
        return "LB:RSS\nTrim:Off\nLR:PFLR"

# Folder path with files
folder_path = 'random'

# Collect data
data = []
for filename in os.listdir(folder_path):
    if filename.endswith('.txt'):  # Assuming files have a .txt extension
        file_path = os.path.join(folder_path, filename)
        max_time = get_max_finished_time(file_path)
        updated_file_name = getNewFileName(filename)
        
        # Determine trimming status
        trim_status = "On" if "Trim:On" in updated_file_name else "Off"
        
        # Append data with trim status
        data.append({'File': updated_file_name, 'Max Finished Time': max_time, 'Trim Status': trim_status})

# Create a DataFrame for Seaborn
df = pd.DataFrame(data)

# Define custom colors for trimming on and off
palette = {"On": "skyblue", "Off": "salmon"}

# Plotting
plt.figure(figsize=(10, 6))
sns.barplot(x='File', y='Max Finished Time', data=df, hue='Trim Status', dodge=False, palette=palette)
plt.xlabel('Scheme')
plt.ylabel('Runtime (us)')
#plt.title('Permutation 1MiB - 1:1 OS - 200Gbps')
plt.legend(title="Trim Status")
plt.tight_layout()
plt.savefig(f"{folder_path}/runtime.png", bbox_inches='tight')
plt.savefig(f"{folder_path}/runtime.pdf", bbox_inches='tight')
plt.show()
