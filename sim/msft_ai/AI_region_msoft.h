// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef AI_REGION_MSOFT_TOPOLOGY
#define AI_REGION_MSOFT_TOPOLOGY

#include <map>
#include <ostream>

#include "AI_DC_msoft.h"
#include "AI_WAN_msoft.h"
#include "AI_WAN_types.h"
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

class AISwitch;
class AIDCMsoft;
class AIWANMsoft;

#define RH_TIER 0

class AIRegionMsoft {
public:
    map<uint64_t, Switch*> region_switch_map;  // mapping switch id to switch module
    map<tuple<uint64_t, uint64_t, uint64_t, uint64_t>, Pipe*>
        region_pipe_map;  // <source switchId, source bundleId, destination switchId, destination
                          // bundleId> --> pipe module
    map<tuple<uint64_t, uint64_t, uint64_t, uint64_t>, BaseQueue*>
        region_queue_map;  // <source switchId, source bundleId, destination switchId, destination
                           // bundleId> --> queue module
    AIWANMsoft* wan                  = nullptr;
    uint32_t    region_bundlesize[1] = {1};

    AIRegionMsoft(unsigned int                  region_idx,
                  struct AI_wan_topology_params params,
                  linkspeed_bps                 intra_dc_linkspeed,
                  linkspeed_bps                 inter_dc_linkspeed,
                  struct queue_params&          queue_params,
                  vector<double>&               switch_drop_event_probs,
                  vector<double>&               switch_random_drop_probs,
                  QueueLoggerFactory*           logger_factory,
                  EventList*                    ev,
                  bool                          enable_pfc = true);

    void       checkDCExists(unsigned int dc_idx);
    void       checkDCNotExists(unsigned int dc_idx);
    void       checkSwitchExists(uint64_t switch_id);
    void       checkSwitchNotExists(uint64_t switch_id);
    void       checkQueueExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id);
    void       checkQueueNotExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id);
    void       checkPipeExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id);
    void       checkPipeNotExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id);
    BaseQueue* get_queue(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id);
    Pipe*      get_pipe(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id);
    bitset<64> get_bit_format(uint64_t num);
    void       print_bit_format(uint64_t num);

    // add loggers to record total queue size at switches
    virtual void add_switch_loggers(Logfile& log, simtime_picosec sample_period);

    unsigned int get_num_region() { return n_region; }

    unsigned int get_num_dc_per_region() { return n_dc_per_region; }

    unsigned int get_region_idx() { return region_idx; }

    uint64_t   getRHSwitchId(uint64_t region_id,
                             uint64_t rh_group_id,
                             uint64_t rh_batch_idx,
                             uint64_t rh_switch_idx);
    SwitchInfo parseRHSwitchId(uint64_t switch_id);
    BaseQueue* alloc_queue(QueueLogger*  queueLogger,
                           linkspeed_bps speed,
                           mem_b         queuesize,
                           AISwitch*     sw);

    virtual vector<const Route*>* get_bidir_paths(uint32_t src, uint32_t dest, bool reverse);

    AIDCMsoft* get_dc(unsigned int dc_idx);

    unsigned int get_num_RH_groups() { return n_RH_groups; }

    unsigned int get_num_RH_switch_batches_per_RH_group() {
        return n_RH_switch_batches_per_RH_group;
    }

    unsigned int get_num_RH_switches_per_RH_batch() { return n_RH_switches_per_RH_batch; }

    unsigned int get_num_RH_switches_per_RH_group() {
        return get_num_RH_switch_batches_per_RH_group() * get_num_RH_switches_per_RH_batch();
    }

    unsigned int get_num_total_servers_in_region() { return num_total_servers_in_region; }

    AIDCMsoft* get_dc_for_server(unsigned int server_idx);

    unsigned int get_RH_batch_idx_mapped_to_spine(unsigned int spine_idx) {
        unsigned int spine_group_index = (spine_idx / n_spine_per_leaf_in_leafgroup);
        return spine_group_index % n_RH_switch_batches_per_RH_group;
    }

    void                   set_switch_hash_salt(uint32_t salt);
    map<uint64_t, int64_t> get_switch_counters();

private:
    map<Queue*, int>     _link_usage;
    linkspeed_bps        _inter_dc_link_speed;
    linkspeed_bps        _intra_dc_link_speed;
    simtime_picosec      hop_latency, _switch_latency;
    QueueLoggerFactory*  _logger_factory;
    EventList*           _eventlist;
    struct queue_params& queue_params;
    vector<double>       _switch_drop_event_probs;  // drop event probability at switches
    vector<double>       _switch_random_drop_probs;
    static int           bts_trigger;
    static bool          bts_ignore_data;

    bool _enable_pfc = true;  // whether to enable PFC for lossless queues

    unsigned int n_region        = 1;  // number of regions
    unsigned int n_dc_per_region = 2;  // number of DCs per region
    unsigned int region_idx      = 0;
    unsigned int n_spine_per_leaf_in_leafgroup =
        8;  // number of Spines connected to each leaf in a leaf group
    unsigned int n_leafgroups_per_dc              = 1;  // number of Leafs per leafgroup
    unsigned int n_leafs_per_leafgroup            = 8;  // number of groups of leafs in each DC
    unsigned int n_torgroups_per_leafgroup        = 8;  // number of ToR groups in each leaf group
    unsigned int n_tor_per_torgroups              = 2;  // number of ToR switches in each ToR group
    unsigned int n_serv_per_torgroup              = 5;  // number of servers per leaf
    unsigned int n_RH_groups                      = 2;  // number of region hub groups
    unsigned int n_RH_switch_batches_per_RH_group = 2;
    unsigned int n_RH_switches_per_RH_batch       = 8;

    map<unsigned int, AIDCMsoft*> dc_map;  // dc_idx --> DC object

    unsigned int num_total_servers_in_region;

    void init_network();
    void create_DCs();
    void create_switches();
    void allocate_RH_queues_and_pipes(QueueLogger* queueLogger);
};

#endif
