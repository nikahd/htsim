from collections import defaultdict

filename_prefix = "msft_ai_wan_kecbr/"

conn_matrices = [
    "one_one_1_200MB",
    "one_one_2_200MB",
    "one_one_4_200MB",
    "one_one_8_200MB",
    "one_one_16_200MB",
    "one_one_32_200MB",
    "one_one_64_200MB"
]

cbr_rate = [2103364, 2403844, 2704325, 3004805]

for matrix in conn_matrices:
    for cbr in cbr_rate:
        filename = f"output_{matrix}_drop0.00001_linkdown1_jitter0_cbr{cbr}_mbl2048_recoverable1_lossy64_isfullskip1_fastrecoverable1_bitmapfulllossy1_fullpercent0.9_exp1.txt"

        counting = defaultdict(lambda: defaultdict(int))

        with open(filename_prefix + filename, "r") as file:
            lines = file.readlines()
            for line in lines:
                if "ECN_CE" in line:
                    x = line.split()
                    flow_id = x[-1]  
                    queue_name = x[-3]
                    if flow_id not in counting:
                        counting[flow_id] = defaultdict(int)
                    if queue_name not in counting[flow_id]:
                        counting[flow_id][queue_name] = 0
                    counting[flow_id][queue_name] += 1

        print(f"Connection Matrix : {matrix}, CBR Rate: {cbr}")
        for flow_id, queues in counting.items():
            print(f"Flow ID: {flow_id}")
            total = 0
            for queue_name, count in queues.items():
                print(f"  Queue: {queue_name}, Count: {count}")
                total += count
            print(f"  Total ECN_CE for Flow ID {flow_id}: {total}")
            print()  # Add a newline for better readability