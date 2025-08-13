for flows in [1, 2, 4, 8, 16, 32, 64, 128, 256]:
    output_file = f"one_one_{flows}_200MB_dcqcn.cm"
    with open(output_file, "w") as f:
        f.write(f"Nodes 512\n")
        f.write(f"Connections {flows * 8}\n")
        if flows <= 16:
            for i in range(flows):
                src = i * 16
                dst = 511 - i * 16
                for repeat in range(8):
                    f.write(f"{src}->{dst} start 0 size 25000000\n")
        else:
            for i in range(16):
                for j in range(flows // 16):
                    src = i * 16 + j
                    dst = 511 - (i * 16 + j)
                    for repeat in range(8):
                        f.write(f"{src}->{dst} start 0 size 25000000\n")