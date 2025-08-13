#ifndef AI_SWITCH_INFO_H
#define AI_SWITCH_INFO_H

#include <stdint.h>

struct SwitchInfo {
    uint64_t switch_type;
    uint64_t region_idx;
    uint64_t datacenter_idx;
    uint64_t spine_idx;
    uint64_t leaf_group_idx;
    uint64_t leaf_idx;
    uint64_t tor_group_idx;
    uint64_t tor_idx;
    uint64_t server_idx;

    // RH
    uint64_t rh_group_idx;
    uint64_t rh_batch_idx;
    uint64_t rh_switch_idx;

    // RWA
    uint64_t rwa_group_idx;
    uint64_t rwa_switch_idx;

    // OWR
    uint64_t owr_group_idx;
    uint64_t owr_switch_idx;
};

#endif