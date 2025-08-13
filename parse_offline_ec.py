
from collections import defaultdict

filename = "msft_ai_wan_cbr/output_one_one_4_200MB_linkdown1_dropratep99_usejitter0_initcwnd0.7_bitmapsize1024_recoverable1_isfullskip0_fastrecoverable0_bitmapfulllossy1_fullpercent0.9_exp1.txt"
# only flap one link; p99 drop only on one link over a period of time

# 1. uniform drops and link flapping
def find_recovered_pkts(psn):
    recovered_pkts = 0
    # sort psn
    psn = sorted(psn)
    stripe_id = defaultdict(int)
    for id in psn:
        if id // 8 not in stripe_id:
            stripe_id[id // 8] = 0
        stripe_id[id // 8] += 1  # 6+2
    for stripe in stripe_id.values():
        if stripe >= 6:
            recovered_pkts += 8
        else:
            recovered_pkts += stripe
    return recovered_pkts

psn = defaultdict(int)
with open(filename, "r") as file:
    lines = file.readlines()
    for line in lines:
        if "pkt_id:" in line:
            x = line.split()
            flow_id = x[1]
            pkt_id = int(x[-1])
            if flow_id not in psn:
                psn[flow_id] = []
            psn[flow_id].append(pkt_id)


avg_ec_drop_rate = 0
avg_no_ec_drop_rate = 0
for flow_id in psn.keys():
    no_ec_drop_rate = 1 - len(psn[flow_id]) * 1.0 / 48832
    ec_drop_rate = 1 - find_recovered_pkts(psn[flow_id]) / 48832
    avg_no_ec_drop_rate += no_ec_drop_rate
    avg_ec_drop_rate += ec_drop_rate


    print("flow_id:", flow_id)
    print("no EC drop rate:" , no_ec_drop_rate)
    print("EC drop rate:" , ec_drop_rate)

print("===== Summary ========")
print("Average no EC drop rate:", avg_no_ec_drop_rate / len(psn))
print("Average EC drop rate:", avg_ec_drop_rate / len(psn))