// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef WAN_TYPES_H
#define WAN_TYPES_H

#include "config.h"
#include "types.h"

#ifndef QT
#define QT

typedef enum {
    UNDEFINED,
    RANDOM,
    ECN,
    COMPOSITE,
    COMP_NO_ECN,  // composite queue without ECN
    COMPOSITE_BTS,
    PRIORITY,
    CTRL_PRIO,
    FAIR_PRIO,
    LOSSLESS,
    LOSSLESS_INPUT,
    LOSSLESS_INPUT_ECN,
    COMPOSITE_ECN,
    COMPOSITE_ECN_LB,
    SWIFT_SCHEDULER
} queue_type;

typedef enum { UPLINK, DOWNLINK } link_direction;

#endif  // QT

enum TopologyType { INTRA_DC = 1, INTER_DC, INTER_REGION };

struct AI_wan_topology_params {
    unsigned int n_region;
    unsigned int n_dc_per_region;
    unsigned int n_spine_per_leaf_in_leafgroup;
    unsigned int n_leafgroups_per_dc;
    unsigned int n_leafs_per_leafgroup;
    unsigned int n_torgroups_per_leafgroup;
    unsigned int n_tor_per_torgroups;
    unsigned int n_serv_per_torgroup;
    unsigned int n_RH_groups;
    unsigned int n_RH_switch_batches_per_RH_group;
    unsigned int n_RH_switches_per_RH_batch;
    unsigned int n_RWA_groups;
    unsigned int n_RWA_switches_per_RWA_group;
    unsigned int n_OWR_groups;
    unsigned int n_OWR_switches_per_OWR_group;

    unsigned int get_no_of_servers_per_region() {
        return n_dc_per_region * n_leafgroups_per_dc * n_torgroups_per_leafgroup *
               n_serv_per_torgroup;
    }

    unsigned int get_no_of_nodes_in_WAN() {
        return n_region * n_dc_per_region * n_leafgroups_per_dc * n_torgroups_per_leafgroup *
               n_serv_per_torgroup;
    }
};

struct AI_WAN_topology_idx {
    unsigned int region_idx;
    unsigned int dc_idx;
    unsigned int leaf_group_idx;
    unsigned int tor_group_idx;
    unsigned int relative_srv_idx;

    AI_WAN_topology_idx(
        unsigned int r, unsigned int d, unsigned int l, unsigned int t, unsigned int s)
        : region_idx(r), dc_idx(d), leaf_group_idx(l), tor_group_idx(t), relative_srv_idx(s) {}
};

struct hop_latency_params {
    simtime_picosec         intra_region_hop_latency;  // Below Regional Hub, down to the servers
    simtime_picosec         regional_backbone_hop_latency;  // Between RH-RWA, RWA-OWR
    vector<simtime_picosec> inter_owr_hop_latency;          // Between OWRs 1-1 mapping

    hop_latency_params(simtime_picosec          intra,
                       simtime_picosec          regional,
                       vector<simtime_picosec>& inter_owr)
        : intra_region_hop_latency(intra),
          regional_backbone_hop_latency(regional),
          inter_owr_hop_latency(inter_owr) {}

    hop_latency_params() : intra_region_hop_latency(0), regional_backbone_hop_latency(0) {
        inter_owr_hop_latency.clear();
    }
};

const unsigned int MAX_REGIONS = 16;  // Maximum number of regions

struct link_params {
    linkspeed_bps
        inter_dc_link_speed;  // Speed of the link between regions, RH-RWA, RWA-OWR, OWR-OWR
    linkspeed_bps intra_dc_link_speed;  // Speed of the link within a region, up to Regional Hub
    unsigned int  link_factor_rh_rwa;   // Factor to multiply the link number for RH-RWA links
    unsigned int  link_factor_rwa_owr;  // Factor to multiply the link number for RWA-OWR links
    std::vector<std::vector<unsigned int>>
        link_factor_regional;  // Factor to multiply the link number for inter-region links
                               // (OWR-OWR)

    link_params(linkspeed_bps inter, linkspeed_bps intra, int rh_rwa, int rwa_owr)
        : inter_dc_link_speed(inter),
          intra_dc_link_speed(intra),
          link_factor_rh_rwa(rh_rwa),
          link_factor_rwa_owr(rwa_owr) {}

    link_params()
        : inter_dc_link_speed(0),
          intra_dc_link_speed(0),
          link_factor_rh_rwa(0),
          link_factor_rwa_owr(0) {}
};

struct queue_params {
    queue_type sender_qt       = FAIR_PRIO;
    queue_type intra_region_qt = LOSSLESS;
    queue_type inter_region_qt = COMP_NO_ECN;

    mem_b tor_queuesize;
    mem_b leaf_queuesize;
    mem_b spine_queuesize;
    mem_b RH_queuesize;
    mem_b RWA_queuesize;
    mem_b OWR_queuesize;

    // ECN K values in microseconds
    uint32_t tor_ECNKmin   = 80;
    uint32_t tor_ECNKmax   = 160;
    uint32_t leaf_ECNKmin  = 160;
    uint32_t leaf_ECNKmax  = 800;
    uint32_t spine_ECNKmin = 600;
    uint32_t spine_ECNKmax = 840;
    uint32_t RH_ECNKmin    = 1000;
    uint32_t RH_ECNKmax    = 2000;

    double tor_ECNPmin   = 0;
    double tor_ECNPmax   = 1;
    double leaf_ECNPmin  = 0;
    double leaf_ECNPmax  = 1;
    double spine_ECNPmin = 0;
    double spine_ECNPmax = 1;
    double RH_ECNPmin    = 0;
    double RH_ECNPmax    = 1;

    queue_params(queue_type intra,
                 queue_type inter,
                 mem_b      tor,
                 mem_b      leaf,
                 mem_b      spine,
                 mem_b      rh,
                 mem_b      rwa,
                 mem_b      owr,
                 double     tor_ECNKmin,
                 double     tor_ECNKmax,
                 double     leaf_ECNKmin,
                 double     leaf_ECNKmax,
                 double     spine_ECNKmin,
                 double     spine_ECNKmax,
                 double     RH_ECNKmin,
                 double     RH_ECNKmax,
                 double     tor_ECNPmin,
                 double     tor_ECNPmax,
                 double     leaf_ECNPmin,
                 double     leaf_ECNPmax,
                 double     spine_ECNPmin,
                 double     spine_ECNPmax,
                 double     RH_ECNPmin,
                 double     RH_ECNPmax)
        : intra_region_qt(intra),
          inter_region_qt(inter),
          tor_queuesize(tor),
          leaf_queuesize(leaf),
          spine_queuesize(spine),
          RH_queuesize(rh),
          RWA_queuesize(rwa),
          OWR_queuesize(owr),
          tor_ECNKmin(tor_ECNKmin),
          tor_ECNKmax(tor_ECNKmax),
          leaf_ECNKmin(leaf_ECNKmin),
          leaf_ECNKmax(leaf_ECNKmax),
          spine_ECNKmin(spine_ECNKmin),
          spine_ECNKmax(spine_ECNKmax),
          RH_ECNKmin(RH_ECNKmin),
          RH_ECNKmax(RH_ECNKmax),
          tor_ECNPmin(tor_ECNPmin),
          tor_ECNPmax(tor_ECNPmax),
          leaf_ECNPmin(leaf_ECNPmin),
          leaf_ECNPmax(leaf_ECNPmax),
          spine_ECNPmin(spine_ECNPmin),
          spine_ECNPmax(spine_ECNPmax),
          RH_ECNPmin(RH_ECNPmin),
          RH_ECNPmax(RH_ECNPmax) {}

    queue_params()
        : intra_region_qt(LOSSLESS),
          inter_region_qt(COMP_NO_ECN),
          tor_queuesize(INFINITE_BUFFER_SIZE),
          leaf_queuesize(INFINITE_BUFFER_SIZE),
          spine_queuesize(INFINITE_BUFFER_SIZE),
          RH_queuesize(INFINITE_BUFFER_SIZE),
          RWA_queuesize(INFINITE_BUFFER_SIZE),
          OWR_queuesize(INFINITE_BUFFER_SIZE),
          tor_ECNKmin(80),
          tor_ECNKmax(160),
          leaf_ECNKmin(160),
          leaf_ECNKmax(800),
          spine_ECNKmin(600),
          spine_ECNKmax(840),
          RH_ECNKmin(1000),
          RH_ECNKmax(2000),
          tor_ECNPmin(0),
          tor_ECNPmax(0.05),
          leaf_ECNPmin(0),
          leaf_ECNPmax(0.05),
          spine_ECNPmin(0),
          spine_ECNPmax(1),
          RH_ECNPmin(0),
          RH_ECNPmax(1) {}
};

#endif  // WAN_TYPES_H