# MSFT WAN Topology, ECBR (Erasure Coding-CBR), EC-AIMD

## Run simulation with configured scripts
Configuration parameters are set in the run_xxx.sh bash script, including Connection Matrix `CONN_MATRICES`, whether link flapping `IS_LINK_DOWN`, whether having various path latency `use_jitter`, switch drop probability in RH, RWA, OWR `drop_rates`, pfc in lossless queues `enable_pfc`. 

Hop latency between OWR-OWR switches are hard-coded in the main file. 

EC bitmap size is hard coded in `erasure_coding.h`, but need to be modified in the `sim/msft_ai/run_main_msft_ai_wan_xxx.sh` bash scripts.

Possible values for each configuration parameter:
```
#!/bin/bash
 
CONN_MATRICES=("one_one_1_200MB.cm" "one_one_2_200MB.cm" "one_one_4_200MB.cm" "one_one_8_200MB.cm" "one_one_16_200MB.cm" "one_one_32_200MB.cm" "one_one_64_200MB.cm" "one_one_128_200MB.cm" "one_one_256_200MB.cm" 
4_1_4_2GB.cm")
IS_LINK_DOWN=(0 1)
drop_rates=("0" "mean" "p99")
use_jitter=0 # 0: use fixed path latency; 1: use various path latency
enable_pfc=0 # 0: disable PFC for all lossless queues in MSFT WAN topology; 1: enable PFC for all lossless queues

INIT_CWND_RATIO=(0.7) # 70%, 80%, 90%
RECOVERABLE_THRESHOLD=(1) # recoverable threshold
bitmap_size_list=(1024) # bitmap size per path; need to be aligned with the hard-coded bitmap size value in `erasure_coding.cpp`
fullskip=(0) # whether use full skip
as_fast_recoverable=(0) # 0: no, 1: yes 
bitmap_full_percent=(0.9) # using bitmap occupancy as EC lossy skip threshold. 90% occupancy as the threshold for example 
use_replace_path=(0 1)  # whether using dynamic repathing
original_loss_path_replace_threshold=(0) # if using dynamic repathing, the threshold for replacing lossy path 
original_jittery_path_replace_threshold=(0) # if using dynamic repathing, the threshold for replacing jittery path
apply_mimd=0 # 0: using AIMD, 1: using MIMD

experiment_run=(1 2 3 4 5) # number of runs of simulation for randomness
```



Example running command:
```
cd msft-htsim/
bash sim/msft_ai/run_main_msft_ai_wan_1ec_cc_fixed_jitter_0_no_jitter_no_flapping_aimd.sh
```

This script will automatically create a folder to store the output files. 

## Drawing plots 

The plotting scripts in `msft-htsim/msft_ai/scripts` reads output files created by the above simulation runs and generate plots. 

Example command:
```
cd msft-htsim/
python3 sim/msft_ai/script/draw_1ec_cc_options_fairness_cwnd.py
```

## Evalute CC by disabling the second ToR in the receiver DC, and ECMP hash by only subflow(pathid), instead of flow_id & subflow_id
To modify the routing, in `AI_switch.cpp`:
* Disable the second ToR in the receiver DC (by disabling the downlink from LEAF down to the ToR in receiver DC)
```
void AISwitch::addAvailableHopsLeafDown(Packet&              pkt,
                                        BaseQueue*           ingress_port,
                                        AI_WAN_topology_idx& dst_idx) {
    assert(dc_topo);
    // send the packet down if the destination is under the same leaf group. Send to
    // destination Tor/Tor group
    // cout << "[AISwitch::addAvailableHops][LEAF] send the packet down if the destination is under
    // "
    //         "the same leaf group. Send to destination Tor/Tor group"
    //      << endl;
    for (uint64_t k = 0; k < dc_topo->get_num_tor_per_torgroups(); k++) {
        // Note: this is for evaluating CC, disable the second ToR
        if (k % 2 == 1) {
            continue;
        }
        // End of evaluating CC
```
* ECMP hash based only on subflow ids: `ecmp_choice = freeBSDHash(1, pkt.pathid(), _hash_salt) % available_hops->size();`
  The `pkt.pathid()` here refers to the subflow_id, and `pkt.flow_id()` refers to the flow_id.
```
  Route* AISwitch::getNextAvailableHop(Packet&            pkt,
                                     BaseQueue*         ingress_port,
                                     vector<FibEntry*>* available_hops) {
    // implement a form of ECMP hashing; might need to revisit based on
    // measured performance.
    uint32_t ecmp_choice = 0;

    // std::cout << "available hops is available: " << available_hops->size() << endl;

    assert(available_hops->size() > 0);

    if (available_hops->size() > 1)
        switch (_strategy) {
            case NIX:
                abort();
            case SINGLE:
                ecmp_choice = 0;
                break;
            case ECMP:
                // ecmp_choice =
                //     freeBSDHash(pkt.flow_id(), pkt.pathid(), _hash_salt) % available_hops->size();

                // Note: For evaluate CC, all subflows will use the same T1, T2, etc.
                ecmp_choice =
                    freeBSDHash(1, pkt.pathid(), _hash_salt) % available_hops->size();

                // cout << "Switch " << _type << ":" << _id << " choosing path " << ecmp_choice
                //      << " for flow " << pkt.flow_id() << " pathid " << pkt.pathid()
                //      << " _hash_salt " << _hash_salt << " available_hops "
                //      << available_hops->size() << endl;
                break;
```
