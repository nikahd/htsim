# Generate connection matrix for AI WAN Routingtest)
# By changing the iteration of region_idx, dc_idx, leafgroup_idx, torgroup_idx, and serv_idx, you can generate different connection matrices
# with one connection sending packets at a time.
# output connection matrix file: 1_routing_WAN.cm 
 
from dataclasses import dataclass

# AI region params
n_region        = 2  # number of regions
region_idx      = 0
n_dc_per_region = 2  # number of DCs per region
n_spine_per_leaf_in_leafgroup = 8  # number of Spines connected to each leaf in a leaf group, fixed
n_leafgroups_per_dc              = 2  # number of Leafs per leafgroup, fixed
n_leafs_per_leafgroup            = 8  # number of groups of leafs in each DC
n_torgroups_per_leafgroup        = 8  # number of ToR groups in each leaf group, fixed
n_tor_per_torgroups              = 2  # number of ToR switches in each ToR group, fixed
n_serv_per_torgroup              = 2  # number of servers per ToR group
n_RH_groups                      = 2  # number of region hub groups, fixed
n_RH_switch_batches_per_RH_group = 2  # fixed
n_RH_switches_per_RH_batch       = 8  # fixed

no_of_nodes = n_region * n_dc_per_region * n_leafgroups_per_dc * n_torgroups_per_leafgroup * n_serv_per_torgroup

testcase = "routing"  # Change this to "latency" or "bandwidth" as needed
delta = (10 ** 9) # picoseconds


@dataclass
class Connection:
    src: int
    dst: int
    start_time: int = 0
    size: int = 0

def generate_connection_all_cases(testcase) -> list[Connection]:
    connections = []
    this_start_time = 0
    if testcase == "routing" or testcase == "latency":   
        this_size = 1500
    elif testcase == "bandwidth":
        this_size = 1000000
    else:
        raise ValueError("Invalid testcase specified. Use 'routing', 'latency', or 'bandwidth'.")    
    # Generating src and dst under the same ToR group
    torgroup = 2
    # for serv1 in range(n_serv_per_torgroup):
    #     for serv2 in range(serv1 + 1, n_serv_per_torgroup):
    serv1 = 0
    serv2 = 1
    src = torgroup * n_serv_per_torgroup + serv1
    dst = torgroup * n_serv_per_torgroup + serv2
    this_start_time += delta 
    connections.append(Connection(src, dst, this_start_time, this_size))

    # Generating src and dst under the same leaf group but not the same ToR group
    leafgroup = 0
    # for torgroup1 in range(n_torgroups_per_leafgroup):
    #     for torgroup2 in range(torgroup1 + 1, n_torgroups_per_leafgroup):
    torgroup1 = 0
    torgroup2 = 1
    serv1 = 0
    serv2 = 0
    src = (leafgroup * n_torgroups_per_leafgroup + torgroup1) * n_serv_per_torgroup + serv1
    dst = (leafgroup * n_torgroups_per_leafgroup + torgroup2) * n_serv_per_torgroup + serv2
    this_start_time += delta
    connections.append(Connection(src, dst, this_start_time, this_size))

    # Generating src and dst under the same DC
    total_leafgroups_in_regions = n_leafgroups_per_dc * n_dc_per_region * n_region
    leafgroup1 = 0
    leafgroup2 = 1
    for leafgroup1 in range(total_leafgroups_in_regions):
        for leafgroup2 in range(leafgroup1 + 1, total_leafgroups_in_regions):
            # for torgroup1 in range(n_torgroups_per_leafgroup):
            #     for torgroup2 in range(torgroup1 + 1, n_torgroups_per_leafgroup):
            torgroup1 = 0
            torgroup2 = 1
            serv1 = 0
            serv2 = 0
            src = (leafgroup1 * n_torgroups_per_leafgroup + torgroup1) * n_serv_per_torgroup + serv1
            dst = (leafgroup2 * n_torgroups_per_leafgroup + torgroup2) * n_serv_per_torgroup + serv2
            this_start_time += delta 
            connections.append(Connection(src, dst, this_start_time, this_size))
 
    # Generating src and dst under the same region
    for dc1 in range(n_dc_per_region):
        for dc2 in range(dc1 + 1, n_dc_per_region):
            for leafgroup1 in range(n_leafgroups_per_dc):
                for leafgroup2 in range(n_leafgroups_per_dc):        
                    torgroup1 = 0
                    torgroup2 = 0
                    serv1 = 0
                    serv2 = 0
                    src = (dc1 * n_leafgroups_per_dc * n_torgroups_per_leafgroup + leafgroup1 * n_torgroups_per_leafgroup + torgroup1) * n_serv_per_torgroup + serv1
                    dst = (dc2 * n_leafgroups_per_dc * n_torgroups_per_leafgroup + leafgroup2 * n_torgroups_per_leafgroup + torgroup2) * n_serv_per_torgroup + serv2
                    this_start_time += delta 
                    connections.append(Connection(src, dst, this_start_time, this_size))

    # Generating src and dst in two different regions
    for region1 in range(n_region):
        for region2 in range(region1 + 1, n_region):
            for dc1 in range(n_dc_per_region):
                for dc2 in range(n_dc_per_region):
                    # for leafgroup1 in range(n_leafgroups_per_dc):
                    #     for leafgroup2 in range(n_leafgroups_per_dc):        
                    leafgroup1 = 0
                    leafgroup2 = 1
                    torgroup1 = 0
                    torgroup2 = 0
                    serv1 = 0
                    serv2 = 0
                    src = (region1 * n_dc_per_region * n_leafgroups_per_dc * n_torgroups_per_leafgroup + 
                            dc1 * n_leafgroups_per_dc * n_torgroups_per_leafgroup + 
                            leafgroup1 * n_torgroups_per_leafgroup + torgroup1) * n_serv_per_torgroup + serv1
                    dst = (region2 * n_dc_per_region * n_leafgroups_per_dc * n_torgroups_per_leafgroup + 
                            dc2 * n_leafgroups_per_dc * n_torgroups_per_leafgroup + 
                            leafgroup2 * n_torgroups_per_leafgroup + torgroup2) * n_serv_per_torgroup + serv2
                    this_start_time += delta 
                    connections.append(Connection(src, dst, this_start_time, this_size))

    return connections

if __name__ == "__main__":
    connections = generate_connection_all_cases(testcase)
    tm_file = f"1_{testcase}_WAN.cm"
    with open(tm_file, "w") as f:
        f.write(f"Nodes {no_of_nodes}\n")
        f.write(f"Connections {len(connections)}\n")
        for i in range(len(connections)):
            f.write(f"{connections[i].src}->{connections[i].dst} start {connections[i].start_time} size {connections[i].size}\n")