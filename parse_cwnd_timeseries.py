from collections import defaultdict
import matplotlib.pyplot as plt
 
filename = f"output_32_cwnd_timeseries.txt"

plot_cwnd = defaultdict(list)

with open(filename, "r") as file:
    lines = file.readlines()
    for line in lines:
        if "_cwnd:" in line:
            x = line.split() 
            flow_id = x[-3][:-1]
            cwnd = float(x[-1])
            time = x[1][:-1] 
            if flow_id in plot_cwnd:
                plot_cwnd[flow_id].append((time, cwnd))  
            else:
                plot_cwnd[flow_id] = [(time, cwnd)]

# plot timeseries for each flow
plt.figure(figsize=(10, 10))
for flow_id, timeseries in plot_cwnd.items():
    times, cwnds = zip(*timeseries)
    plt.plot(times, cwnds, label=f"Flow {flow_id}")

plt.xlabel("Time (us)")
plt.ylabel("Congestion Window (bytes)")
plt.title("Congestion Window Timeseries")
plt.legend()
plt.grid()
plt.savefig("cwnd_timeseries.png")