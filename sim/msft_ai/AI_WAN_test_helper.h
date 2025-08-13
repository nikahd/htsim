#ifndef AI_WAN_TEST_HELPER_H
#define AI_WAN_TEST_HELPER_H

#include "AI_WAN_helper.h"
#include "AI_WAN_msoft.h"
#include "AI_WAN_types.h"
#include "AI_switch.h"
#include "helpers.h"
#include "network.h"
#include "packet_flow.h"

std::vector<uint64_t>       get_spines_connected_to_rh(AIRegionMsoft*                region,
                                                       struct AI_wan_topology_params params,
                                                       unsigned int                  target_dc_idx,
                                                       unsigned int                  rh_batch_idx,
                                                       unsigned int                  rh_group_idx,
                                                       unsigned int                  rh_switch_idx);
vector<AISwitch::node_type> get_wan_switch_type_path(AIWANMsoft* topo_AI, int src, int dest);
vector<AISwitch::node_type> get_region_switch_type_path(AIRegionMsoft* topo_AI, int src, int dest);

map<uint64_t, int64_t> init_WAN_switch_counters(AIWANMsoft*                   topo_AI,
                                                struct AI_wan_topology_params params);
map<uint64_t, int64_t> init_region_switch_counters(AIRegionMsoft*                topo_AI,
                                                   struct AI_wan_topology_params params);

void update_wan_switch_counters(vector<AISwitch::node_type>   switch_path,
                                flowid_t                      this_flow_id,
                                struct AI_WAN_topology_idx    cur_topo_idx,
                                struct AI_WAN_topology_idx    dest_topo_idx,
                                AIRegionMsoft*                src_region,
                                AIRegionMsoft*                dest_region,
                                AIDCMsoft*                    cur_dc,
                                AIDCMsoft*                    dest_dc,
                                map<uint64_t, int64_t>&       switch_counters,
                                AIWANMsoft*                   wan_topo,
                                struct AI_wan_topology_params params,
                                struct link_params            link_params,
                                uint64_t                      cbr_src_hash_salt,
                                uint64_t                      cbr_path_id,
                                uint64_t                      AI_switch_hash_salt);

void update_region_switch_counters(vector<AISwitch::node_type> switch_path,
                                   flowid_t                    this_flow_id,
                                   struct AI_WAN_topology_idx  cur_topo_idx,
                                   struct AI_WAN_topology_idx  dest_topo_idx,

                                   AIDCMsoft*                    cur_dc,
                                   AIDCMsoft*                    dest_dc,
                                   map<uint64_t, int64_t>&       switch_counters,
                                   AIRegionMsoft*                topo_AI,
                                   struct AI_wan_topology_params params,
                                   uint64_t                      cbr_src_hash_salt,
                                   uint64_t                      cbr_path_id,
                                   uint64_t                      AI_switch_hash_salt);

std::vector<uint64_t> get_spines_connected_to_rh(AIRegionMsoft*                region,
                                                 struct AI_wan_topology_params params,
                                                 unsigned int                  target_dc_idx,
                                                 unsigned int                  rh_batch_idx,
                                                 unsigned int                  rh_group_idx,
                                                 unsigned int                  rh_switch_idx) {
    std::vector<uint64_t> connected_spines;
    AIDCMsoft*            m_dc = region->get_dc(target_dc_idx);
    for (unsigned int spine_idx = 0; spine_idx < m_dc->get_num_spine_per_dc(); ++spine_idx) {
        if (region->get_RH_batch_idx_mapped_to_spine(spine_idx) == rh_batch_idx) {
            connected_spines.push_back(spine_idx);
        }
    }
    return connected_spines;
}

vector<AISwitch::node_type> get_wan_switch_type_path(AIWANMsoft* topo_AI, int src, int dest) {
    vector<AISwitch::node_type> path;
    auto [src_topo_idx, src_region, src_dc]    = get_AI_WAN_topology_idx(topo_AI, src);
    auto [dest_topo_idx, dest_region, dest_dc] = get_AI_WAN_topology_idx(topo_AI, dest);

    if (src_topo_idx.region_idx != dest_topo_idx.region_idx) {
        // Inter-region path
        path.insert(path.end(),
                    {AISwitch::TOR,
                     AISwitch::LEAF,
                     AISwitch::SPINE,
                     AISwitch::RH,
                     AISwitch::RWA,
                     AISwitch::OWR,
                     AISwitch::OWR,
                     AISwitch::RWA,
                     AISwitch::RH,
                     AISwitch::SPINE,
                     AISwitch::LEAF,
                     AISwitch::TOR});
    } else if (src_topo_idx.dc_idx != dest_topo_idx.dc_idx) {
        // Inter-DC path
        path.insert(path.end(),
                    {AISwitch::TOR,
                     AISwitch::LEAF,
                     AISwitch::SPINE,
                     AISwitch::RH,
                     AISwitch::SPINE,
                     AISwitch::LEAF,
                     AISwitch::TOR});
    } else if (src_topo_idx.leaf_group_idx != dest_topo_idx.leaf_group_idx) {
        // Intra-DC path
        path.insert(
            path.end(),
            {AISwitch::TOR, AISwitch::LEAF, AISwitch::SPINE, AISwitch::LEAF, AISwitch::TOR});
    } else if (src_topo_idx.tor_group_idx != dest_topo_idx.tor_group_idx) {
        path.insert(path.end(), {AISwitch::TOR, AISwitch::LEAF, AISwitch::TOR});
    } else if (src != dest) {
        path.insert(path.end(), {AISwitch::TOR});
    }

    // // print switch_path
    // cout << src << " " << dest << " switch path: ";
    // for (const auto& switch_type : path) {
    //     string type_str;
    //     switch (switch_type) {
    //         case AISwitch::SERVER:
    //             type_str = "SERVER";
    //             break;
    //         case AISwitch::TOR:
    //             type_str = "TOR";
    //             break;
    //         case AISwitch::LEAF:
    //             type_str = "LEAF";
    //             break;
    //         case AISwitch::SPINE:
    //             type_str = "SPINE";
    //             break;
    //         case AISwitch::RH:
    //             type_str = "RH";
    //             break;
    //         case AISwitch::RWA:
    //             type_str = "RWA";
    //             break;
    //         case AISwitch::OWR:
    //             type_str = "OWR";
    //             break;
    //         default:
    //             type_str = "UNKNOWN";
    //             break;
    //     }
    //     cout << type_str << " ";
    // }
    // cout << endl;

    return path;
}

map<uint64_t, int64_t> init_WAN_switch_counters(AIWANMsoft*                   topo_AI,
                                                struct AI_wan_topology_params params) {
    map<uint64_t, int64_t> switch_counters;

    // Initialize switch counters
    for (unsigned int region_id = 0; region_id < params.n_region; region_id++) {
        auto region = topo_AI->get_region(region_id);

        // DC switches
        for (unsigned int dc_id = 0; dc_id < params.n_dc_per_region; dc_id++) {
            auto dc = region->get_dc(dc_id);
            for (unsigned int leafgroup_id = 0; leafgroup_id < params.n_leafgroups_per_dc;
                 leafgroup_id++) {
                // ToR switches
                for (unsigned int tor_group_id = 0; tor_group_id < params.n_torgroups_per_leafgroup;
                     tor_group_id++) {
                    for (unsigned int tor_idx = 0; tor_idx < params.n_tor_per_torgroups;
                         tor_idx++) {
                        uint64_t switch_id = dc->getToRSwitchId(
                            region_id, dc_id, leafgroup_id, tor_group_id, tor_idx);
                        switch_counters[switch_id] = 0;
                    }
                }

                // Leaf switches
                for (unsigned int leaf_idx = 0; leaf_idx < params.n_leafs_per_leafgroup;
                     leaf_idx++) {
                    uint64_t switch_id =
                        dc->getLeafSwitchId(region_id, dc_id, leafgroup_id, leaf_idx);
                    switch_counters[switch_id] = 0;
                }
            }

            // Spine switches
            for (unsigned int spine_idx = 0;
                 spine_idx < params.n_spine_per_leaf_in_leafgroup * params.n_leafs_per_leafgroup;
                 spine_idx++) {
                uint64_t switch_id         = dc->getSpineSwitchId(region_id, dc_id, spine_idx);
                switch_counters[switch_id] = 0;
            }
        }

        // RH switches
        for (unsigned int rh_group_id = 0; rh_group_id < params.n_RH_groups; rh_group_id++) {
            for (unsigned int rh_batch_idx = 0;
                 rh_batch_idx < params.n_RH_switch_batches_per_RH_group;
                 rh_batch_idx++) {
                for (unsigned int rh_switch_idx = 0;
                     rh_switch_idx < params.n_RH_switches_per_RH_batch;
                     rh_switch_idx++) {
                    uint64_t switch_id =
                        region->getRHSwitchId(region_id, rh_group_id, rh_batch_idx, rh_switch_idx);
                    switch_counters[switch_id] = 0;
                }
            }
        }

        // RWA switches
        for (unsigned int rwa_group_id = 0; rwa_group_id < params.n_RWA_groups; rwa_group_id++) {
            for (unsigned int rwa_switch_idx = 0;
                 rwa_switch_idx < params.n_RWA_switches_per_RWA_group;
                 rwa_switch_idx++) {
                uint64_t switch_id =
                    topo_AI->getRWASwitchId(region_id, rwa_group_id, rwa_switch_idx);
                switch_counters[switch_id] = 0;
            }
        }

        // OWR switches
        for (unsigned int owr_group_id = 0; owr_group_id < params.n_OWR_groups; owr_group_id++) {
            for (unsigned int owr_switch_idx = 0;
                 owr_switch_idx < params.n_OWR_switches_per_OWR_group;
                 owr_switch_idx++) {
                uint64_t switch_id =
                    topo_AI->getOWRSwitchId(region_id, owr_group_id, owr_switch_idx);
                switch_counters[switch_id] = 0;
            }
        }
    }
    return switch_counters;
}

void update_wan_switch_counters(vector<AISwitch::node_type>   switch_path,
                                flowid_t                      this_flow_id,
                                struct AI_WAN_topology_idx    cur_topo_idx,
                                struct AI_WAN_topology_idx    dest_topo_idx,
                                AIRegionMsoft*                src_region,
                                AIRegionMsoft*                dest_region,
                                AIDCMsoft*                    cur_dc,
                                AIDCMsoft*                    dest_dc,
                                map<uint64_t, int64_t>&       switch_counters,
                                AIWANMsoft*                   wan_topo,
                                struct AI_wan_topology_params params,
                                struct link_params            link_params,
                                uint64_t                      cbr_src_hash_salt,
                                uint64_t                      cbr_path_id,
                                uint64_t                      AI_switch_hash_salt) {
    AIRegionMsoft* cur_region = src_region;
    AIWANMsoft*    cur_wan    = wan_topo;

    // server ECMP choice
    unsigned int ecmp_choice     = mFreeBSDHash(this_flow_id, cbr_src_hash_salt);
    unsigned int cur_ecmp_choice = (ecmp_choice % cur_dc->get_num_tor_per_torgroups());

    // for ECMP choices of RH switches only
    unsigned int cur_rh_group_choice  = -1;
    unsigned int cur_rh_switch_choice = -1;
    unsigned int cur_rh_batch_choice  = -1;
    unsigned int cur_rh_link_choice   = -1;

    // for ECMP choices of RWA switches only
    unsigned int cur_rwa_group_choice  = -1;
    unsigned int cur_rwa_switch_choice = -1;
    unsigned int cur_rwa_link_choice   = -1;

    // for ECMP choices of OWR switches only
    unsigned int cur_owr_group_choice  = -1;
    unsigned int cur_owr_switch_choice = -1;
    unsigned int cur_owr_link_choice   = -1;

    for (auto iter = switch_path.begin(); iter != switch_path.end(); ++iter) {
        AISwitch::node_type cur_switch_type = *iter;
        auto                next_iter       = std::next(iter);

        switch (cur_switch_type) {
            // Update switch counters
            case AISwitch::OWR: {
                auto cur_owr_switch_id = cur_wan->getOWRSwitchId(
                    cur_topo_idx.region_idx, cur_owr_group_choice, cur_owr_switch_choice);
                switch_counters[cur_owr_switch_id]++;

                // cout << "[OWR] switch id " << cur_owr_switch_id << endl;

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::RWA) {
                        // Find next RWA switch
                        unsigned int switch_available_hops =
                            params.n_RWA_switches_per_RWA_group * link_params.link_factor_rwa_owr;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_rwa_group_choice  = cur_owr_group_choice;
                        cur_rwa_switch_choice = ecmp_choice / link_params.link_factor_rwa_owr;
                        cur_rwa_link_choice   = ecmp_choice % link_params.link_factor_rwa_owr;

                        // cout << "[OWR] ecmp_choice: " << ecmp_choice << " flow_id: " <<
                        // this_flow_id
                        //      << " cbr_path_id: " << cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;

                    } else {
                        // Find next OWR switch
                        unsigned int switch_available_hops =
                            link_params.link_factor_regional[cur_topo_idx.region_idx]
                                                            [dest_topo_idx.region_idx];
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        // cur_owr_group_choice doesn't change
                        // cur_owr_switch_choice doesn't change
                        cur_owr_link_choice     = ecmp_choice;
                        cur_topo_idx.region_idx = dest_topo_idx.region_idx;
                        cur_region              = dest_region;

                        // cout << "[OWR] ecmp_choice: " << ecmp_choice << " flow_id: " <<
                        // this_flow_id
                        //      << " cbr_path_id: " << cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;
                    }
                }

                break;
            }
            case AISwitch::RWA: {
                auto cur_rwa_switch_id = cur_wan->getRWASwitchId(
                    cur_topo_idx.region_idx, cur_rwa_group_choice, cur_rwa_switch_choice);
                switch_counters[cur_rwa_switch_id]++;

                // cout << "[RWA] switch id " << cur_rwa_switch_id << endl;

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::RH) {
                        // Find next RH switch
                        unsigned int switch_available_hops =
                            params.n_RH_switch_batches_per_RH_group *
                            params.n_RH_switches_per_RH_batch * link_params.link_factor_rh_rwa;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_rh_group_choice  = cur_rwa_group_choice;
                        cur_rh_batch_choice  = ecmp_choice / (params.n_RH_switches_per_RH_batch *
                                                             link_params.link_factor_rh_rwa);
                        cur_rh_switch_choice = ecmp_choice / link_params.link_factor_rh_rwa %
                                               params.n_RH_switches_per_RH_batch;
                        cur_rh_link_choice = ecmp_choice % link_params.link_factor_rh_rwa;

                        // cout << "[RWA] ecmp_choice: " << ecmp_choice << " flow_id: " <<
                        // this_flow_id
                        //      << " cbr_path_id: " << cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;
                    } else {
                        // Find next OWR switch
                        unsigned int switch_available_hops =
                            params.n_OWR_switches_per_OWR_group * link_params.link_factor_rwa_owr;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_owr_group_choice  = cur_rwa_group_choice;
                        cur_owr_switch_choice = ecmp_choice / link_params.link_factor_rwa_owr;
                        cur_owr_link_choice   = ecmp_choice % link_params.link_factor_rwa_owr;

                        // cout << "[RWA] ecmp_choice: " << ecmp_choice << " flow_id: " <<
                        // this_flow_id
                        //      << " cbr_path_id: " << cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;
                    }
                }
                break;
            }

            case AISwitch::RH: {
                auto cur_rh_switch_id = cur_region->getRHSwitchId(cur_topo_idx.region_idx,
                                                                  cur_rh_group_choice,
                                                                  cur_rh_batch_choice,
                                                                  cur_rh_switch_choice);
                switch_counters[cur_rh_switch_id]++;

                // cout << "[RH] switch id " << cur_rh_switch_id << endl;

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::SPINE) {
                        // Find next SPINE switch
                        auto connected_spines = get_spines_connected_to_rh(cur_region,
                                                                           params,
                                                                           dest_topo_idx.dc_idx,
                                                                           cur_rh_batch_choice,
                                                                           cur_rh_group_choice,
                                                                           cur_rh_switch_choice);

                        unsigned int switch_available_hops = connected_spines.size();
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_ecmp_choice     = connected_spines[ecmp_choice];  // this is spine_idx
                        cur_topo_idx.dc_idx = dest_topo_idx.dc_idx;
                        cur_dc              = dest_dc;

                        // cout << "[RH] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;

                    } else {
                        // Find next RWA switch
                        unsigned int switch_available_hops =
                            params.n_RWA_switches_per_RWA_group * link_params.link_factor_rh_rwa;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_rwa_group_choice  = cur_rh_group_choice;
                        cur_rwa_switch_choice = ecmp_choice / link_params.link_factor_rh_rwa;
                        cur_rwa_link_choice   = ecmp_choice % link_params.link_factor_rh_rwa;

                        // cout << "[RH] ecmp_choice: " << ecmp_choice << " flow_id: " <<
                        // this_flow_id
                        //      << " cbr_path_id: " << cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;
                    }
                }
                break;
            }
            case AISwitch::SPINE: {
                auto cur_spine_switch_id = cur_dc->getSpineSwitchId(
                    cur_topo_idx.region_idx, cur_topo_idx.dc_idx, cur_ecmp_choice);
                auto cur_spine_idx = cur_ecmp_choice;  // this is spine_idx
                switch_counters[cur_spine_switch_id]++;

                // cout << "[SPINE] switch id " << cur_spine_switch_id << endl;

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::RH) {  // Go up
                        // Find next RH switch
                        unsigned int switch_available_hops =
                            params.n_RH_switches_per_RH_batch * params.n_RH_groups;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_rh_batch_choice =
                            cur_dc->region->get_RH_batch_idx_mapped_to_spine(cur_spine_idx);
                        cur_rh_group_choice  = ecmp_choice / params.n_RH_switches_per_RH_batch;
                        cur_rh_switch_choice = ecmp_choice % params.n_RH_switches_per_RH_batch;

                        // cout << "[SPINE] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;

                    } else if (next_switch_type == AISwitch::LEAF) {  // Go down
                        // Find next LEAF switch
                        unsigned int switch_available_hops = 1;
                        unsigned int ecmp_choice           = 0;
                        cur_ecmp_choice = cur_dc->get_leaf_idx_mapped_to_spine(cur_spine_idx);
                        cur_topo_idx.leaf_group_idx = dest_topo_idx.leaf_group_idx;

                        // cout << "[SPINE] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;

                    } else {
                        cout << "not possible cases for SPINE" << endl;
                    }
                }

                break;
            }
            case AISwitch::LEAF: {
                auto cur_leaf_switch_id = cur_dc->getLeafSwitchId(cur_topo_idx.region_idx,
                                                                  cur_topo_idx.dc_idx,
                                                                  cur_topo_idx.leaf_group_idx,
                                                                  cur_ecmp_choice);
                switch_counters[cur_leaf_switch_id]++;

                // cout << "[LEAF] switch id " << cur_leaf_switch_id << endl;

                unsigned int cur_leaf_idx =
                    cur_ecmp_choice;  // this is leaf_switch_idx in a leaf group

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::SPINE) {
                        // Find next SPINE switch
                        unsigned int switch_available_hops = params.n_leafs_per_leafgroup;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_ecmp_choice = cur_leaf_idx * params.n_spine_per_leaf_in_leafgroup +
                                          ecmp_choice;  // this is spine_idx

                        // cout << "[LEAF] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;
                        // cout << "[LEAF] cur_ecmp_choice: " << cur_ecmp_choice
                        //      << " cur_leaf_idx: " << cur_leaf_idx << endl;

                    } else if (next_switch_type == AISwitch::TOR) {
                        // Find next TOR switch
                        unsigned int switch_available_hops = params.n_tor_per_torgroups;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;

                        // cout << "[LEAF] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;

                        cur_ecmp_choice            = ecmp_choice;  // this is tor_switch_idx
                        cur_topo_idx.tor_group_idx = dest_topo_idx.tor_group_idx;
                    } else {
                        cout << "not possible cases for LEAF" << endl;
                    }
                }

                break;
            }
            case AISwitch::TOR: {
                auto cur_tor_switch_id = cur_dc->getToRSwitchId(cur_topo_idx.region_idx,
                                                                cur_topo_idx.dc_idx,
                                                                cur_topo_idx.leaf_group_idx,
                                                                cur_topo_idx.tor_group_idx,
                                                                cur_ecmp_choice);
                switch_counters[cur_tor_switch_id]++;

                // cout << "[TOR] switch id " << cur_tor_switch_id << endl;

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::LEAF) {
                        // Find next LEAF switch
                        unsigned int switch_available_hops = params.n_leafs_per_leafgroup;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        // cout << "[TOR] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;
                        cur_ecmp_choice = ecmp_choice;  // this is leaf_switch_idx in a leaf group
                    } else {
                        cout << "not possible cases for TOR" << endl;
                    }
                }

                break;
            }
            default: {
                cout << "Unknown switch type in path: " << cur_switch_type << endl;
                break;
            }
        }
    }
}

vector<AISwitch::node_type> get_region_switch_type_path(AIRegionMsoft* topo_AI, int src, int dest) {
    vector<AISwitch::node_type> path;
    auto [src_topo_idx, src_dc]   = get_AI_region_topology_idx(topo_AI, src);
    auto [dest_topo_idx, dest_dc] = get_AI_region_topology_idx(topo_AI, dest);

    if (src_topo_idx.region_idx != dest_topo_idx.region_idx) {
        // Inter-region path
        // not belong to regional test
    } else if (src_topo_idx.dc_idx != dest_topo_idx.dc_idx) {
        // Inter-DC path
        path.insert(path.end(),
                    {AISwitch::TOR,
                     AISwitch::LEAF,
                     AISwitch::SPINE,
                     AISwitch::RH,
                     AISwitch::SPINE,
                     AISwitch::LEAF,
                     AISwitch::TOR});
    } else if (src_topo_idx.leaf_group_idx != dest_topo_idx.leaf_group_idx) {
        // Intra-DC path
        path.insert(
            path.end(),
            {AISwitch::TOR, AISwitch::LEAF, AISwitch::SPINE, AISwitch::LEAF, AISwitch::TOR});
    } else if (src_topo_idx.tor_group_idx != dest_topo_idx.tor_group_idx) {
        path.insert(path.end(), {AISwitch::TOR, AISwitch::LEAF, AISwitch::TOR});
    } else if (src != dest) {
        path.insert(path.end(), {AISwitch::TOR});
    }

    // // print switch_path
    // cout << src << " " << dest << " switch path: ";
    // for (const auto& switch_type : path) {
    //     string type_str;
    //     switch (switch_type) {
    //         case AISwitch::SERVER:
    //             type_str = "SERVER";
    //             break;
    //         case AISwitch::TOR:
    //             type_str = "TOR";
    //             break;
    //         case AISwitch::LEAF:
    //             type_str = "LEAF";
    //             break;
    //         case AISwitch::SPINE:
    //             type_str = "SPINE";
    //             break;
    //         case AISwitch::RH:
    //             type_str = "RH";
    //             break;
    //         default:
    //             type_str = "UNKNOWN";
    //             break;
    //     }
    //     cout << type_str << " ";
    // }
    // cout << endl;

    return path;
}

map<uint64_t, int64_t> init_region_switch_counters(AIRegionMsoft*                topo_AI,
                                                   struct AI_wan_topology_params params) {
    map<uint64_t, int64_t> switch_counters;
    // Initialize switch counters
    for (unsigned int region_id = 0; region_id < params.n_region; region_id++) {
        for (unsigned int dc_id = 0; dc_id < params.n_dc_per_region; dc_id++) {
            for (unsigned int leafgroup_id = 0; leafgroup_id < params.n_leafgroups_per_dc;
                 leafgroup_id++) {
                // ToR switches
                for (unsigned int tor_group_id = 0; tor_group_id < params.n_torgroups_per_leafgroup;
                     tor_group_id++) {
                    for (unsigned int tor_idx = 0; tor_idx < params.n_tor_per_torgroups;
                         tor_idx++) {
                        auto     dc        = topo_AI->get_dc(dc_id);
                        uint64_t switch_id = dc->getToRSwitchId(
                            region_id, dc_id, leafgroup_id, tor_group_id, tor_idx);
                        switch_counters[switch_id] = 0;
                    }
                }

                // Leaf switches
                for (unsigned int leaf_idx = 0; leaf_idx < params.n_leafs_per_leafgroup;
                     leaf_idx++) {
                    auto     dc = topo_AI->get_dc(dc_id);
                    uint64_t switch_id =
                        dc->getLeafSwitchId(region_id, dc_id, leafgroup_id, leaf_idx);
                    switch_counters[switch_id] = 0;
                }
            }

            // Spine switches
            for (unsigned int spine_idx = 0;
                 spine_idx < params.n_spine_per_leaf_in_leafgroup * params.n_leafs_per_leafgroup;
                 spine_idx++) {
                auto     dc                = topo_AI->get_dc(dc_id);
                uint64_t switch_id         = dc->getSpineSwitchId(region_id, dc_id, spine_idx);
                switch_counters[switch_id] = 0;
            }
        }

        // RH switches
        for (unsigned int rh_group_id = 0; rh_group_id < params.n_RH_groups; rh_group_id++) {
            for (unsigned int rh_batch_idx = 0;
                 rh_batch_idx < params.n_RH_switch_batches_per_RH_group;
                 rh_batch_idx++) {
                for (unsigned int rh_switch_idx = 0;
                     rh_switch_idx < params.n_RH_switches_per_RH_batch;
                     rh_switch_idx++) {
                    uint64_t switch_id =
                        topo_AI->getRHSwitchId(region_id, rh_group_id, rh_batch_idx, rh_switch_idx);
                    switch_counters[switch_id] = 0;
                }
            }
        }
    }
    return switch_counters;
}

void update_region_switch_counters(vector<AISwitch::node_type>   switch_path,
                                   flowid_t                      this_flow_id,
                                   struct AI_WAN_topology_idx    cur_topo_idx,
                                   struct AI_WAN_topology_idx    dest_topo_idx,
                                   AIDCMsoft*                    cur_dc,
                                   AIDCMsoft*                    dest_dc,
                                   map<uint64_t, int64_t>&       switch_counters,
                                   AIRegionMsoft*                topo_AI,
                                   struct AI_wan_topology_params params,
                                   uint64_t                      cbr_src_hash_salt,
                                   uint64_t                      cbr_path_id,
                                   uint64_t                      AI_switch_hash_salt) {
    // server ECMP choice
    unsigned int ecmp_choice     = mFreeBSDHash(this_flow_id, cbr_src_hash_salt);
    unsigned int cur_ecmp_choice = (ecmp_choice % cur_dc->get_num_tor_per_torgroups());
    // for ECMP choices of RH switches only
    unsigned int cur_rh_group_choice  = -1;
    unsigned int cur_rh_switch_choice = -1;
    unsigned int cur_rh_batch_choice  = -1;

    for (auto iter = switch_path.begin(); iter != switch_path.end(); ++iter) {
        AISwitch::node_type cur_switch_type = *iter;
        auto                next_iter       = std::next(iter);

        switch (cur_switch_type) {
            // Update switch counters
            case AISwitch::RH: {
                auto cur_rh_switch_id = topo_AI->getRHSwitchId(cur_topo_idx.region_idx,
                                                               cur_rh_group_choice,
                                                               cur_rh_batch_choice,
                                                               cur_rh_switch_choice);
                switch_counters[cur_rh_switch_id]++;

                // cout << "[RH] switch id " << cur_rh_switch_id << endl;

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::SPINE) {
                        // Find next SPINE switch
                        auto connected_spines = get_spines_connected_to_rh(topo_AI,
                                                                           params,
                                                                           dest_topo_idx.dc_idx,
                                                                           cur_rh_batch_choice,
                                                                           cur_rh_group_choice,
                                                                           cur_rh_switch_choice);

                        unsigned int switch_available_hops = connected_spines.size();
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_ecmp_choice     = connected_spines[ecmp_choice];  // this is spine_idx
                        cur_topo_idx.dc_idx = dest_topo_idx.dc_idx;
                        cur_dc              = dest_dc;

                        // cout << "[RH] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;

                    } else {
                        cout << "not possible cases for RH / not implemented yet" << endl;
                    }
                }
                break;
            }
            case AISwitch::SPINE: {
                auto cur_spine_switch_id = cur_dc->getSpineSwitchId(
                    cur_topo_idx.region_idx, cur_topo_idx.dc_idx, cur_ecmp_choice);
                auto cur_spine_idx = cur_ecmp_choice;  // this is spine_idx
                switch_counters[cur_spine_switch_id]++;

                // cout << "[SPINE] switch id " << cur_spine_switch_id << endl;

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::RH) {  // Go up
                        // Find next RH switch
                        unsigned int switch_available_hops =
                            params.n_RH_switches_per_RH_batch * params.n_RH_groups;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_rh_batch_choice =
                            cur_dc->region->get_RH_batch_idx_mapped_to_spine(cur_spine_idx);
                        cur_rh_group_choice  = ecmp_choice / params.n_RH_switches_per_RH_batch;
                        cur_rh_switch_choice = ecmp_choice % params.n_RH_switches_per_RH_batch;

                        // cout << "[SPINE] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;

                    } else if (next_switch_type == AISwitch::LEAF) {  // Go down
                        // Find next LEAF switch
                        unsigned int switch_available_hops = 1;
                        unsigned int ecmp_choice           = 0;
                        cur_ecmp_choice = cur_dc->get_leaf_idx_mapped_to_spine(cur_spine_idx);
                        cur_topo_idx.leaf_group_idx = dest_topo_idx.leaf_group_idx;

                        // cout << "[SPINE] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;

                    } else {
                        cout << "not possible cases for SPINE" << endl;
                    }
                }

                break;
            }
            case AISwitch::LEAF: {
                auto cur_leaf_switch_id = cur_dc->getLeafSwitchId(cur_topo_idx.region_idx,
                                                                  cur_topo_idx.dc_idx,
                                                                  cur_topo_idx.leaf_group_idx,
                                                                  cur_ecmp_choice);
                switch_counters[cur_leaf_switch_id]++;
                // cout << "[LEAF] switch id " << cur_leaf_switch_id << endl;
                unsigned int cur_leaf_idx =
                    cur_ecmp_choice;  // this is leaf_switch_idx in a leaf group

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::SPINE) {
                        // Find next SPINE switch
                        unsigned int switch_available_hops = params.n_leafs_per_leafgroup;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        cur_ecmp_choice = cur_leaf_idx * params.n_spine_per_leaf_in_leafgroup +
                                          ecmp_choice;  // this is spine_idx

                        // cout << "[LEAF] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;
                        // cout << "[LEAF] cur_ecmp_choice: " << cur_ecmp_choice
                        //      << " cur_leaf_idx: " << cur_leaf_idx << endl;

                    } else if (next_switch_type == AISwitch::TOR) {
                        // Find next TOR switch
                        unsigned int switch_available_hops = params.n_tor_per_torgroups;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;

                        // cout << "[LEAF] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;

                        cur_ecmp_choice            = ecmp_choice;  // this is tor_switch_idx
                        cur_topo_idx.tor_group_idx = dest_topo_idx.tor_group_idx;
                    } else {
                        cout << "not possible cases for LEAF" << endl;
                    }
                }

                break;
            }
            case AISwitch::TOR: {
                auto cur_tor_switch_id = cur_dc->getToRSwitchId(cur_topo_idx.region_idx,
                                                                cur_topo_idx.dc_idx,
                                                                cur_topo_idx.leaf_group_idx,
                                                                cur_topo_idx.tor_group_idx,
                                                                cur_ecmp_choice);
                switch_counters[cur_tor_switch_id]++;

                // cout << "[TOR] switch id " << cur_tor_switch_id << endl;

                if (next_iter != switch_path.end()) {
                    AISwitch::node_type next_switch_type = *next_iter;
                    if (next_switch_type == AISwitch::LEAF) {
                        // Find next LEAF switch
                        unsigned int switch_available_hops = params.n_leafs_per_leafgroup;
                        unsigned int ecmp_choice =
                            freeBSDHash(this_flow_id, cbr_path_id, AI_switch_hash_salt) %
                            switch_available_hops;
                        // cout << "[TOR] ecmp_choice: " << ecmp_choice
                        //      << " flow_id: " << this_flow_id << " cbr_path_id: " <<
                        //      cbr_path_id
                        //      << " AI_switch_hash_salt: " << AI_switch_hash_salt
                        //      << " switch_available_hops: " << switch_available_hops << endl;
                        cur_ecmp_choice = ecmp_choice;  // this is leaf_switch_idx in a leaf group
                    } else {
                        cout << "not possible cases for TOR" << endl;
                    }
                }

                break;
            }
            default: {
                cout << "Unknown switch type in path: " << cur_switch_type << endl;
                break;
            }
        }
    }
}

#endif  // AI_WAN_TEST_HELPER_H