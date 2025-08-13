from collections import defaultdict
 
filename = f"output_drop_prob.txt"

prob = defaultdict()

with open(filename, "r") as file:
    lines = file.readlines()
    for line in lines:
        if "drop_prob" in line:
            x = line.split() 
            queue_name = x[6]
            drop_prob = float(x[-1])
            if queue_name not in prob:
                prob[queue_name] = []
            prob[queue_name].append(drop_prob)

for queue_name, drops in prob.items():
    # print(f"Queue: {queue_name}, Drop Probabilities: {drops}")
    total = sum(drops)
    avg = sum(drops) / len(drops) if drops else 0
    p95 = sorted(drops)[int(len(drops) * 0.95)] if drops else 0
    p99 = sorted(drops)[int(len(drops) * 0.99)] if drops else 0
    print(f"  Total drop_prob for Queue {queue_name}: avg: {avg}, p95: {p95}, p99: {p99}")
    print()  # Add a newline for better readability