flows = input("Enter the number of flows (1, 2, 4, 8, 16, 32, 64, 128, 256): ")
flows = int(flows)
output_file = f"one_one_{flows}_200MB.cm"
with open(output_file, "w") as f:
    f.write(f"Nodes 512\n")
    f.write(f"Connections {flows}\n")
    if flows <= 16:
        for i in range(flows):
            src = i * 16
            dst = 511 - i * 16
            f.write(f"{src}->{dst} start 0 size 200000000\n")
    else:
        for i in range(16):
            for j in range(flows // 16):
                src = i * 16 + j
                dst = 511 - (i * 16 + j)
                f.write(f"{src}->{dst} start 0 size 200000000\n")