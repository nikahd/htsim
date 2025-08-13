// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef AI_WAN_MSOFT_TOPOLOGY
#define AI_WAN_MSOFT_TOPOLOGY

#include <map>

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

class AISwitch;
class AIRegionMsoft;

class AIWANMsoft {
public:
    map<uint64_t, Switch*> wan_switch_map;  // mapping switch id to switch module
    map<tuple<uint64_t, uint64_t, uint64_t>, Pipe*>
        wan_pipe_map;  // <source switchId, destination switchId> --> pipe module
    map<tuple<uint64_t, uint64_t, uint64_t>, BaseQueue*>
        wan_queue_map;  // <source switchId, destination switchId> --> queue module

    AIWANMsoft(struct AI_wan_topology_params params,
               struct link_params&           link_params,
               struct queue_params&          queue_params,
               QueueLoggerFactory*           logger_factory,
               EventList*                    ev,
               vector<simtime_picosec>&      owr_owr_hop_latency,
               vector<double>&               switch_drop_event_probs,
               vector<double>&               switch_random_drop_probs,
               bool                          use_link_down = false,
               bool                          enable_pfc    = true);

    AIWANMsoft(struct link_params&  link_params,
               struct queue_params& queue_params,
               QueueLoggerFactory*  logger_factory,
               EventList*           ev);

    void           checkRegionExists(unsigned int dc_idx);
    void           checkRegionNotExists(unsigned int dc_idx);
    void           checkSwitchExists(uint64_t switch_id);
    void           checkSwitchNotExists(uint64_t switch_id);
    void           checkQueueExists(tuple<uint64_t, uint64_t, uint64_t> queue_id);
    void           checkQueueNotExists(tuple<uint64_t, uint64_t, uint64_t> queue_id);
    void           checkPipeExists(tuple<uint64_t, uint64_t, uint64_t> pipe_id);
    void           checkPipeNotExists(tuple<uint64_t, uint64_t, uint64_t> pipe_id);
    BaseQueue*     get_queue(tuple<uint64_t, uint64_t, uint64_t> queue_id);
    Pipe*          get_pipe(tuple<uint64_t, uint64_t, uint64_t> pipe_id);
    AIRegionMsoft* get_region(unsigned int region_idx);
    bitset<64>     get_bit_format(uint64_t num);
    void           print_bit_format(uint64_t num);

    BaseQueue* alloc_queue(QueueLogger*  queueLogger,
                           linkspeed_bps speed,
                           mem_b         queuesize,
                           AISwitch*     sw,
                           size_t        link_idx);
    BaseQueue* alloc_owr_queue(QueueLogger*  queueLogger,
                               linkspeed_bps speed,
                               mem_b         queuesize,
                               size_t        link_idx);

    inline unsigned int get_num_region() { return n_region; }

    uint64_t   getRWASwitchId(uint64_t region_id, uint64_t rwa_group_id, uint64_t rwa_switch_idx);
    SwitchInfo parseRWASwitchId(uint64_t switch_id);
    uint64_t   getOWRSwitchId(uint64_t region_id, uint64_t owr_group_id, uint64_t owr_switch_idx);
    SwitchInfo parseOWRSwitchId(uint64_t switch_id);

    AIRegionMsoft* get_region_for_server(unsigned int server_idx);

    unsigned int get_RWA_group_id_mapped_to_RH_group(unsigned int rh_group_idx);
    unsigned int get_OWR_group_id_mapped_to_RWA_group(unsigned int rwa_group_idx);

    unsigned int get_RWA_group_idx_mapped_to_OWR_group(unsigned int owr_group_idx) {
        return owr_group_idx;
    }

    unsigned int get_RH_group_idx_mapped_to_RWA_group(unsigned int rwa_group_idx) {
        return rwa_group_idx;
    }

    unsigned int get_num_OWR_switches_per_OWR_group() { return n_OWR_switches_per_OWR_group; }

    unsigned int get_num_RWA_switches_per_RWA_group() { return n_RWA_switches_per_RWA_group; }

    inline unsigned int get_number_of_servers_in_wan() {
        return n_region * n_dc_per_region * n_leafgroups_per_dc * n_torgroups_per_leafgroup *
               n_serv_per_torgroup;
    }

    inline unsigned int get_number_of_servers_per_region() {
        return n_dc_per_region * n_leafgroups_per_dc * n_torgroups_per_leafgroup *
               n_serv_per_torgroup;
    }

    unsigned int get_num_links_per_RH_RWA() { return link_params.link_factor_rh_rwa; }

    unsigned int get_num_links_per_RWA_OWR() { return link_params.link_factor_rwa_owr; }

    unsigned int get_num_links_between_regions(unsigned int region_idx1, unsigned int region_idx2) {
        assert((region_idx1 < n_region && region_idx2 < n_region) && "Region index out of bounds!");
        return link_params.link_factor_regional[region_idx1][region_idx2];
    }

    string getOWROWRQueueName(unsigned int region_idx1,
                              unsigned int owr_group_idx1,
                              unsigned int region_idx2,
                              unsigned int owr_group_idx2,
                              unsigned int owr_switch_idx,
                              unsigned int link_idx);

    string getOWROWRPipeName(unsigned int region_idx1,
                             unsigned int owr_group_idx1,
                             unsigned int region_idx2,
                             unsigned int owr_group_idx2,
                             unsigned int owr_switch_idx,
                             unsigned int link_idx);

    vector<const Route*>* get_bidir_paths(uint32_t src, uint32_t dest, bool reverse);
    void                  add_switch_loggers(Logfile& log, simtime_picosec sample_period);

    void                   set_switch_hash_salt(uint32_t salt);
    map<uint64_t, int64_t> get_switch_counters();

private:
    map<Queue*, int>          _link_usage;
    struct link_params        link_params;
    struct queue_params       queue_params;
    struct hop_latency_params hop_latency;
    simtime_picosec           _switch_latency;
    QueueLoggerFactory*       _logger_factory;
    EventList*                _eventlist;
    queue_type                _sender_qt;

    unsigned int n_region        = 2;  // number of regions
    unsigned int n_dc_per_region = 2;  // number of DCs per region
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
    unsigned int n_RWA_groups                     = 2;  // number of RWA groups
    unsigned int n_RWA_switches_per_RWA_group     = 4;  // number of RWA switches in each RWA group
    unsigned int n_OWR_groups                     = 2;  // number of OWR groups
    unsigned int n_OWR_switches_per_OWR_group     = 4;  // number of OWR switches in each OWR group

    map<unsigned int, AIRegionMsoft*> region_map;  // region idx --> region object

    unsigned int num_total_servers_in_wan;

    vector<double> _switch_drop_event_probs;
    vector<double> _switch_random_drop_probs;  // random drop probability at switches
    bool           _use_link_down;             // whether to configure link down from traces

    bool _enable_pfc = true;

    void init_network();
    void create_regions();
    void create_switches();
    void allocate_RWA_queues_and_pipes(QueueLogger* queueLogger);
    void allocate_OWR_queues_and_pipes(QueueLogger* queueLogger);
    void allocate_inter_region_queues_and_pipes(QueueLogger* queueLogger);
    void allocate_owr_owr_queues_and_pipes(QueueLogger* queueLogger,
                                           uint64_t     owr_switch_id1,
                                           uint64_t     owr_switch_id2,
                                           unsigned int region_idx1,
                                           unsigned int region_idx2,
                                           unsigned int owr_group_idx,
                                           unsigned int owr_switch_idx);

    void allocate_rwa_owr_queues_and_pipes(QueueLogger* queueLogger,
                                           unsigned int region_idx,
                                           uint64_t     rwa_switch_id,
                                           uint64_t     owr_switch_id,
                                           unsigned int rwa_group_idx,
                                           unsigned int owr_group_id,
                                           unsigned int rwa_switch_idx,
                                           unsigned int owr_switch_idx);

    void allocate_owr_rwa_queues_and_pipes(QueueLogger* queueLogger,
                                           unsigned int region_idx,
                                           uint64_t     owr_switch_id,
                                           uint64_t     rwa_switch_id,
                                           unsigned int owr_group_idx,
                                           unsigned int rwa_group_idx,
                                           unsigned int owr_switch_idx,
                                           unsigned int rwa_switch_idx);

    void allocate_rwa_rh_queues_and_pipes(QueueLogger* queueLogger,
                                          unsigned int region_idx,
                                          uint64_t     rwa_switch_id,
                                          uint64_t     rh_switch_id,
                                          unsigned int rwa_group_id,
                                          unsigned int rh_group_idx,
                                          unsigned int rh_batch_idx,
                                          unsigned int rwa_switch_idx,
                                          unsigned int rh_switch_idx);

    void allocate_rh_rwa_queues_and_pipes(QueueLogger* queueLogger,
                                          unsigned int region_idx,
                                          uint64_t     rh_switch_id,
                                          uint64_t     rwa_switch_id,
                                          unsigned int rh_group_idx,
                                          unsigned int rwa_group_id,
                                          unsigned int rh_batch_idx,
                                          unsigned int rh_switch_idx,
                                          unsigned int rwa_switch_idx);
};

#endif  // AI_WAN_MSOFT_TOPOLOGY