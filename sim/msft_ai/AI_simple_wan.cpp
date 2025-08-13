// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-

#include "AI_simple_wan.h"

#include "AI_DC_msoft.h"

// Simple Cross-region Topology Implementation:
// Topology figure:
// https://microsoft-my.sharepoint.com/:p:/p/nadeengebara/EUqfNOXn4olAp6mLtfM5u-cB-MykRq42BVasxVqXg4yWsw?e=ICibS2&nav=eyJzSWQiOjE2ODksImNJZCI6MjQ2NzQwMzM3Mn0
// This topology is for baseline simulations, with each region including tunable number of ToRs,
// and each ToR having tunable number of servers.
// Each ToR connects to T_leaf switches, T_leaf switches connect to T_core switches,
// while T_core switches simulating the bi-directional links between T_leaf switches.

SimpleWAN::SimpleWAN(unsigned int             n_region,
                     unsigned int             n_core_per_region,
                     unsigned int             n_tor_per_region,
                     unsigned int             n_servers_per_tor,
                     linkspeed_bps            intra_region_link_speed,
                     linkspeed_bps            inter_region_link_speed,
                     mem_b                    queuesize,
                     QueueLoggerFactory*      logger_factory,
                     EventList*               ev,
                     simtime_picosec          srv_tor_hop_latency,
                     simtime_picosec          tor_core_hop_latency,
                     vector<simtime_picosec>& core_core_hop_latency_half,
                     queue_type               sender_qt,
                     queue_type               tor_qt,
                     queue_type               spine_qt,
                     queue_type               core_qt,
                     double                   switch_random_drop_prob,
                     bool                     use_link_down,
                     uint32_t                 hash_salt)
    : AIDCMsoft(0,                  // original DC n_region
                1,                  // original DC n_dc_per_region
                0,                  // original DC region_idx
                0,                  // original DC dc_idx
                1,                  // original DC n_spine_per_leaf_in_leafgroup
                n_region,           // original DC n_leafgroups_per_dc
                n_core_per_region,  // original DC n_leafs_per_leafgroup
                n_tor_per_region,   // original DC n_torgroups_per_leafgroup
                1,                  // original DC n_tor_per_torgroups
                n_servers_per_tor,  // original DC n_serv_per_torgroup
                queuesize,
                logger_factory,
                ev,
                intra_region_link_speed,
                intra_region_link_speed,
                inter_region_link_speed,
                srv_tor_hop_latency,
                tor_core_hop_latency,
                core_core_hop_latency_half,  // original T_core-T_core latency divided by 2
                sender_qt,
                tor_qt,
                spine_qt,
                core_qt,
                1,
                1,
                1,  // spine_tier bundle size
                switch_random_drop_prob,
                use_link_down,
                hash_salt)  // NO_ECN for T2 switches in AI_DC topology as a dump switch
{
    this->n_region          = n_region;
    this->n_core_per_region = n_core_per_region;
    this->n_tor_per_region  = n_tor_per_region;
    this->n_servers_per_tor = n_servers_per_tor;

    this->intra_region_link_speed = intra_region_link_speed;
    this->inter_region_link_speed = inter_region_link_speed;
    this->queue_size              = queuesize;
    this->_logger_factory         = logger_factory;
    this->_eventlist              = ev;
    this->_sender_qt              = sender_qt;
    this->_tor_qt                 = tor_qt;
    this->_spine_qt               = spine_qt;
    this->_core_qt                = core_qt;

    cout << "Set SPINE switch random drop probability = " << switch_random_drop_prob << endl;
}