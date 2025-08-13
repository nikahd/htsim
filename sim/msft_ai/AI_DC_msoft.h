// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef AI_DC_MSOFT_TOPOLOGY
#define AI_DC_MSOFT_TOPOLOGY

#include <map>
#include <ostream>

#include "AI_WAN_types.h"
#include "AI_region_msoft.h"
#include "AI_switch.h"
#include "AI_switch_info.h"
#include "config.h"
#include "eventlist.h"
#include "logfile.h"
#include "loggers.h"
#include "network.h"
#include "pipe.h"
#include "randomqueue.h"
#include "switch.h"
#include "topology.h"

class AIRegionMsoft;

#define TOR_TIER 0
#define LEAF_TIER 1
#define SPINE_TIER 2

class AIDCMsoft : public Topology {
public:
    map<uint64_t, Switch*> switch_map;  // mapping switch id to switch module
    map<tuple<uint64_t, uint64_t, uint64_t, uint64_t>, Pipe*>
        pipe_map;  // <source switchId, source bundleId, destination switchId, destination bundleId>
                   // --> pipe module
    map<tuple<uint64_t, uint64_t, uint64_t, uint64_t>, BaseQueue*>
        queue_map;  // <source switchId, source bundleId, destination switchId, destination
                    // bundleId> --> queue module

    QueueLoggerFactory* _logger_factory;
    EventList*          _eventlist;
    queue_type          _spine_qt;
    queue_type          _leaf_qt;
    queue_type          _tor_qt;
    queue_type          _sender_qt;

    uint32_t bundlesize[3] = {1, 1, 1};  // TOR, LEAF, SPINE

    AIRegionMsoft* region = nullptr;

    AIDCMsoft(unsigned int         n_region,
              unsigned int         n_dc_per_region,
              unsigned int         region_idx,
              unsigned int         dc_idx,
              unsigned int         n_spine_per_leaf_in_leafgroup,
              unsigned int         n_leafgroups_per_dc,
              unsigned int         n_leafs_per_leafgroup,
              unsigned int         n_torgroups_per_leafgroup,
              unsigned int         n_tor_per_torgroups,
              unsigned int         n_serv_per_torgroup,
              linkspeed_bps        linkspeed,
              struct queue_params& queue_params,
              QueueLoggerFactory*  logger_factory,
              EventList*           ev);

    AIDCMsoft(linkspeed_bps        linkspeed,
              struct queue_params& queue_params,
              QueueLoggerFactory*  logger_factory,
              EventList*           ev);

    AIDCMsoft(unsigned int             n_region,
              unsigned int             n_dc_per_region,
              unsigned int             region_idx,
              unsigned int             dc_idx,
              unsigned int             n_spine_per_leaf_in_leafgroup,
              unsigned int             n_leafgroups_per_dc,
              unsigned int             n_leafs_per_leafgroup,
              unsigned int             n_torgroups_per_leafgroup,
              unsigned int             n_tor_per_torgroups,
              unsigned int             n_serv_per_torgroup,
              struct queue_params&     queue_params,
              QueueLoggerFactory*      logger_factory,
              EventList*               ev,
              linkspeed_bps            srv_tor_linkspeed,
              linkspeed_bps            tor_leaf_linkspeed,
              linkspeed_bps            leaf_spine_linkspeed,
              simtime_picosec          srv_tor_hop_latency,
              simtime_picosec          tor_leaf_hop_latency,
              vector<simtime_picosec>& leaf_spine_hop_latency,
              uint32_t                 bundle_size_tor         = 1,
              uint32_t                 bundle_size_leaf        = 1,
              uint32_t                 bundle_size_spine       = 1,
              double                   switch_random_drop_prob = 0.0,
              bool                     use_link_down           = false,
              uint32_t                 hash_salt               = 1,
              bool                     enable_pfc              = true);

    uint64_t   getServerId(uint64_t server_idx);
    uint64_t   getToRSwitchId(uint64_t region_id,
                              uint64_t datacenter_id,
                              uint64_t leaf_group_id,
                              uint64_t tor_group_id,
                              uint64_t tor_idx);
    uint64_t   getLeafSwitchId(uint64_t region_id,
                               uint64_t datacenter_id,
                               uint64_t leaf_group_id,
                               uint64_t leaf_idx);
    uint64_t   getSpineSwitchId(uint64_t region_id, uint64_t datacenter_id, uint64_t spine_idx);
    bitset<64> get_bit_format(uint64_t num);
    void       print_bit_format(uint64_t num);
    void       checkSwitchExists(uint64_t switch_id);
    void       checkSwitchNotExists(uint64_t switch_id);
    void       checkQueueExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id);
    void       checkQueueNotExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id);
    void       checkPipeExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id);
    void       checkPipeNotExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id);
    BaseQueue* get_queue(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id);
    Pipe*      get_pipe(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id);

    void                          init_network();
    void                          create_switches(uint32_t hash_salt);
    void                          allocate_spine_queues_and_pipes(QueueLogger* queueLogger);
    void                          allocate_leaf_queues_and_pipes(QueueLogger* queueLogger);
    void                          allocate_tor_queues_and_pipes(QueueLogger* queueLogger);
    virtual vector<const Route*>* get_bidir_paths(uint32_t src, uint32_t dest, bool reverse);

    BaseQueue* alloc_src_queue(QueueLogger* q);
    BaseQueue* alloc_tor_queue(QueueLogger*  queueLogger,
                               linkspeed_bps speed,
                               mem_b         queuesize,
                               Switch*       sw);
    BaseQueue* alloc_leaf_queue(QueueLogger*  queueLogger,
                                linkspeed_bps speed,
                                mem_b         queuesize,
                                Switch*       sw);
    BaseQueue* alloc_spine_queue(QueueLogger*  queueLogger,
                                 linkspeed_bps speed,
                                 mem_b         queuesize,
                                 Switch*       sw,
                                 bool          use_link_down,
                                 unsigned int  leaf_group_idx,
                                 unsigned int  leaf_idx,
                                 unsigned int  spine_idx);

    void count_queue(Queue*);

    vector<uint32_t>* get_neighbours(uint32_t src) { return NULL; };

    // add loggers to record total queue size at switches
    virtual void add_switch_loggers(Logfile& log, simtime_picosec sample_period);

    static void set_bts_threshold(int value) { bts_trigger = value; }

    static void set_ignore_data_ecn(bool value) { bts_ignore_data = value; }

    SwitchInfo parseServerId(uint64_t server_id);
    SwitchInfo parseToRSwitchId(uint64_t switch_id);
    SwitchInfo parseLeafSwitchId(uint64_t switch_id);
    SwitchInfo parseSpineSwitchId(uint64_t switch_id);

    unsigned int get_num_region() { return n_region; }

    unsigned int get_num_dc_per_region() { return n_dc_per_region; }

    unsigned int get_region_idx() { return region_idx; }

    unsigned int get_dc_idx() { return dc_idx; }

    unsigned int get_num_spine_per_leaf_in_leafgroup() { return n_spine_per_leaf_in_leafgroup; }

    unsigned int get_num_spine_per_dc() {
        return n_leafs_per_leafgroup * n_spine_per_leaf_in_leafgroup;
    }

    // given the index of leaf switch in a leaf group, return the index of the first spine mapped to
    // that leaf
    unsigned int get_spine_offset_mapped_to_leaf(unsigned int leaf_idx) {
        return leaf_idx * get_num_spine_per_leaf_in_leafgroup();
    }

    unsigned int get_leaf_idx_mapped_to_spine(unsigned int spine_idx) {
        return spine_idx / get_num_spine_per_leaf_in_leafgroup();
    }

    unsigned int get_num_leafgroups_per_dc() { return n_leafgroups_per_dc; }

    unsigned int get_num_leafs_per_leafgroup() { return n_leafs_per_leafgroup; }

    unsigned int get_num_torgroups_per_leafgroup() { return n_torgroups_per_leafgroup; }

    unsigned int get_num_tor_per_torgroups() { return n_tor_per_torgroups; }

    unsigned int get_num_serv_per_torgroup() { return n_serv_per_torgroup; }

    unsigned int get_number_of_servers_per_leaf_group() {
        return n_torgroups_per_leafgroup * get_num_serv_per_torgroup();
    }

    unsigned int get_number_of_servers_per_dc() {
        return n_leafgroups_per_dc * get_number_of_servers_per_leaf_group();
    }

    unsigned int get_number_of_servers_per_region() {
        return n_dc_per_region * get_number_of_servers_per_dc();
    }

    unsigned int get_relative_server_idx(unsigned int server_idx) {
        return (server_idx % get_num_serv_per_torgroup());
    }

    unsigned int get_tor_group_idx(unsigned int server_idx) {
        return ((server_idx % get_number_of_servers_per_leaf_group()) /
                get_num_serv_per_torgroup());
    }

    unsigned int get_leaf_group_idx(unsigned int server_idx) {
        return ((server_idx % get_number_of_servers_per_dc()) /
                get_number_of_servers_per_leaf_group());
    }

    unsigned int get_dc_idx(unsigned int server_idx) {
        return ((server_idx % get_number_of_servers_per_region()) / get_number_of_servers_per_dc());
    }

    unsigned int get_region_idx(unsigned int server_idx) {
        return (server_idx / get_number_of_servers_per_region());
    }

private:
    map<Queue*, int>    _link_usage;
    mem_b               queue_size;
    struct queue_params queue_params;
    linkspeed_bps       srv_tor_link_speed, tor_leaf_link_speed,
        leaf_spine_link_speed;  // link speed in bps
    simtime_picosec         srv_tor_hop_latency, tor_leaf_hop_latency;
    vector<simtime_picosec> leaf_spine_hop_latency;
    simtime_picosec         _switch_latency;
    static int              bts_trigger;
    static bool             bts_ignore_data;

    unsigned int n_region        = 1;  // number of regions
    unsigned int n_dc_per_region = 1;  // number of DCs per region
    unsigned int region_idx      = 0;
    unsigned int dc_idx          = 0;
    unsigned int n_spine_per_leaf_in_leafgroup =
        8;  // number of Spines connected to each leaf in a leaf group
    unsigned int n_leafgroups_per_dc       = 1;  // number of Leafs per leafgroup
    unsigned int n_leafs_per_leafgroup     = 8;  // number of groups of leafs in each DC
    unsigned int n_torgroups_per_leafgroup = 8;  // number of ToR groups in each leaf group
    unsigned int n_tor_per_torgroups       = 2;  // number of ToR switches in each ToR group
    unsigned int n_serv_per_torgroup       = 5;  // number of servers per leaf

    double _switch_random_drop_prob = 0.0;  // random drop probability at switches
    bool   _use_link_down;                  // whether to configure link down from traces

    bool _enable_pfc = true;  // whether to enable PFC for lossless queues

    uint32_t _hash_salt = 1;  // set hash salt for AISwitches for debugging purposes
};

#endif
