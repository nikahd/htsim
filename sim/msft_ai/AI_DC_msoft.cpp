// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "AI_DC_msoft.h"

#include <bitset>
#include <iostream>
#include <sstream>
#include <vector>

#include "compositequeue.h"
#include "ecnqueue.h"
#include "prioqueue.h"
#include "queue.h"
#include "queue_lossless.h"
#include "queue_lossless_input.h"
#include "queue_lossless_output.h"
#include "string.h"

string ntoa(double n);
string itoa(uint64_t n);

// default to 3-tier topology.  Change this with set_tiers() before calling the
// constructor.
int  AIDCMsoft::bts_trigger     = -1;
bool AIDCMsoft::bts_ignore_data = true;

//  extern int N;

AIDCMsoft::AIDCMsoft(unsigned int         n_region,
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
                     EventList*           ev)
    : queue_params(queue_params) {
    this->n_region                      = n_region;
    this->n_dc_per_region               = n_dc_per_region;
    this->region_idx                    = region_idx;
    this->dc_idx                        = dc_idx;
    this->n_spine_per_leaf_in_leafgroup = n_spine_per_leaf_in_leafgroup;
    this->n_leafgroups_per_dc           = n_leafgroups_per_dc;
    this->n_leafs_per_leafgroup         = n_leafs_per_leafgroup;
    this->n_torgroups_per_leafgroup     = n_torgroups_per_leafgroup;
    this->n_tor_per_torgroups           = n_tor_per_torgroups;
    this->n_serv_per_torgroup           = n_serv_per_torgroup;
    this->srv_tor_link_speed            = linkspeed;
    this->tor_leaf_link_speed           = linkspeed;
    this->leaf_spine_link_speed         = linkspeed;
    this->_logger_factory               = logger_factory;
    this->_eventlist                    = ev;
    this->srv_tor_hop_latency           = timeFromUs((uint32_t)1);
    this->tor_leaf_hop_latency          = timeFromUs((uint32_t)1);
    this->leaf_spine_hop_latency.push_back(timeFromUs((uint32_t)1));
    this->_switch_latency = timeFromUs((uint32_t)0);

    this->_switch_random_drop_prob = 0;
    this->_use_link_down           = false;

    init_network();
}

AIDCMsoft::AIDCMsoft(linkspeed_bps        linkspeed,
                     struct queue_params& queue_params,
                     QueueLoggerFactory*  logger_factory,
                     EventList*           ev)
    : queue_params(queue_params) {
    this->srv_tor_link_speed    = linkspeed;
    this->tor_leaf_link_speed   = linkspeed;
    this->leaf_spine_link_speed = linkspeed;
    this->_logger_factory       = logger_factory;
    this->_eventlist            = ev;
    this->srv_tor_hop_latency   = timeFromUs((uint32_t)1);
    this->tor_leaf_hop_latency  = timeFromUs((uint32_t)1);
    this->leaf_spine_hop_latency.push_back(timeFromUs((uint32_t)1));
    this->_switch_latency = timeFromUs((uint32_t)0);

    this->_switch_random_drop_prob = 0;
    this->_use_link_down           = false;

    init_network();
}

// TODO(zeying): group long parameter lists into structs to improve readability
AIDCMsoft::AIDCMsoft(unsigned int             n_region,
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
                     uint32_t                 bundle_size_tor,
                     uint32_t                 bundle_size_leaf,
                     uint32_t                 bundle_size_spine,
                     double                   switch_random_drop_prob,
                     bool                     use_link_down,
                     uint32_t                 hash_salt,
                     bool                     enable_pfc)
    : queue_params(queue_params) {
    this->n_region                      = n_region;
    this->n_dc_per_region               = n_dc_per_region;
    this->region_idx                    = region_idx;
    this->dc_idx                        = dc_idx;
    this->n_spine_per_leaf_in_leafgroup = n_spine_per_leaf_in_leafgroup;
    this->n_leafgroups_per_dc           = n_leafgroups_per_dc;
    this->n_leafs_per_leafgroup         = n_leafs_per_leafgroup;
    this->n_torgroups_per_leafgroup     = n_torgroups_per_leafgroup;
    this->n_tor_per_torgroups           = n_tor_per_torgroups;
    this->n_serv_per_torgroup           = n_serv_per_torgroup;

    this->srv_tor_link_speed    = srv_tor_linkspeed;
    this->tor_leaf_link_speed   = tor_leaf_linkspeed;
    this->leaf_spine_link_speed = leaf_spine_linkspeed;
    this->_logger_factory       = logger_factory;
    this->_eventlist            = ev;
    this->srv_tor_hop_latency   = srv_tor_hop_latency;
    this->tor_leaf_hop_latency  = tor_leaf_hop_latency;
    this->leaf_spine_hop_latency.assign(leaf_spine_hop_latency.begin(),
                                        leaf_spine_hop_latency.end());
    this->_switch_latency = timeFromUs((uint32_t)0);

    this->_switch_random_drop_prob = switch_random_drop_prob;
    this->_use_link_down           = use_link_down;

    this->bundlesize[TOR_TIER]   = bundle_size_tor;
    this->bundlesize[LEAF_TIER]  = bundle_size_leaf;
    this->bundlesize[SPINE_TIER] = bundle_size_spine;

    this->_hash_salt = hash_salt;

    this->_enable_pfc = enable_pfc;

    init_network();
}

uint64_t AIDCMsoft::getServerId(uint64_t server_idx) {
    // note that server idx is always relative to each ToR group
    // meaning that the idx of the first server inside each ToR group is always 0

    /*
        <0,3>: type = SERVER
        <4,31>: server idX
    */
    uint64_t server_id = AISwitch::SERVER;
    server_id |= (uint64_t(server_idx) << 4);
    return server_id;
}

SwitchInfo AIDCMsoft::parseServerId(uint64_t server_id) {
    SwitchInfo info;

    info.switch_type = (server_id & 0xF);             // Bits 0-3
    info.server_idx  = (server_id >> 4) & 0xFFFFFFF;  // Bits 4–31 (28 bits)

    if (info.switch_type != AISwitch::SERVER) {
        cout << "Passed ID is not related to a server!" << endl;
        abort();
    }

    return info;
}

uint64_t AIDCMsoft::getToRSwitchId(uint64_t region_id,
                                   uint64_t datacenter_id,
                                   uint64_t leaf_group_id,
                                   uint64_t tor_group_id,
                                   uint64_t tor_idx) {
    // ToRs are T0 Routers

    /*
        <0,3>: type = TOR
        <4,7>: region id
        <8,11>: DC id
        <12,15>: leaf group id
        <16,19>: tor group id
        <20,23>: tor idx inside the tor group
    */
    uint64_t switch_id = AISwitch::TOR;
    switch_id |= (uint64_t(region_id) << 4);
    switch_id |= (uint64_t(datacenter_id) << 8);
    switch_id |= (uint64_t(leaf_group_id) << 12);
    switch_id |= (uint64_t(tor_group_id) << 16);
    switch_id |= (uint64_t(tor_idx) << 20);
    return switch_id;
}

SwitchInfo AIDCMsoft::parseToRSwitchId(uint64_t switch_id) {
    SwitchInfo info;

    info.switch_type    = (switch_id & 0xF);        // Bits 0-3
    info.region_idx     = (switch_id >> 4) & 0xF;   // Bits 4-7
    info.datacenter_idx = (switch_id >> 8) & 0xF;   // Bits 8-11
    info.leaf_group_idx = (switch_id >> 12) & 0xF;  // Bits 12-15
    info.tor_group_idx  = (switch_id >> 16) & 0xF;  // Bits 16-19
    info.tor_idx        = (switch_id >> 20) & 0xF;  // Bits 20-23

    if (info.switch_type != AISwitch::TOR) {
        cout << "Passed switch ID is not related to a ToR switch!" << endl;
        abort();
    }

    return info;
}

uint64_t AIDCMsoft::getLeafSwitchId(uint64_t region_id,
                                    uint64_t datacenter_id,
                                    uint64_t leaf_group_id,
                                    uint64_t leaf_idx) {
    // Leafs are T1 Routers

    /*
        <0,3>: type = LEAF
        <4,7>: region id
        <8,11>: DC id
        <12,15>: leaf group id
        <16,19>: leaf idx inside the leaf group
    */
    uint64_t switch_id = AISwitch::LEAF;
    switch_id |= (uint64_t(region_id) << 4);
    switch_id |= (uint64_t(datacenter_id) << 8);
    switch_id |= (uint64_t(leaf_group_id) << 12);
    switch_id |= (uint64_t(leaf_idx) << 16);
    return switch_id;
}

SwitchInfo AIDCMsoft::parseLeafSwitchId(uint64_t switch_id) {
    SwitchInfo info;

    info.switch_type    = (switch_id & 0xF);        // Bits 0-3
    info.region_idx     = (switch_id >> 4) & 0xF;   // Bits 4-7
    info.datacenter_idx = (switch_id >> 8) & 0xF;   // Bits 8-11
    info.leaf_group_idx = (switch_id >> 12) & 0xF;  // Bits 12-15
    info.leaf_idx       = (switch_id >> 16) & 0xF;  // Bits 16-19

    if (info.switch_type != AISwitch::LEAF) {
        cout << "Passed switch ID is not related to a leaf switch!" << endl;
        abort();
    }

    return info;
}

uint64_t AIDCMsoft::getSpineSwitchId(uint64_t region_id,
                                     uint64_t datacenter_id,
                                     uint64_t spine_idx) {
    // Spines are T2 Routers

    /*
        <0,3>: type = SPINE
        <4,7>: region id
        <8,11>: DC id
        <12,23>: spine index
    */
    uint64_t switch_id = AISwitch::SPINE;
    switch_id |= (uint64_t(region_id) << 4);
    switch_id |= (uint64_t(datacenter_id) << 8);
    switch_id |= (uint64_t(spine_idx) << 12);
    return switch_id;
}

SwitchInfo AIDCMsoft::parseSpineSwitchId(uint64_t switch_id) {
    SwitchInfo info;

    info.switch_type    = (switch_id & 0xF);          // Bits 0–3
    info.region_idx     = (switch_id >> 4) & 0xF;     // Bits 4–7
    info.datacenter_idx = (switch_id >> 8) & 0xF;     // Bits 8–11
    info.spine_idx      = (switch_id >> 12) & 0xFFF;  // Bits 12–23

    if (info.switch_type != AISwitch::SPINE) {
        cout << "Passed switch ID is not related to a spine switch!" << endl;
        abort();
    }

    return info;
}

bitset<64> AIDCMsoft::get_bit_format(uint64_t num) {
    return bitset<64>(num);
}

void AIDCMsoft::print_bit_format(uint64_t num) {
    cout << get_bit_format(num) << endl;
}

void AIDCMsoft::checkSwitchExists(uint64_t switch_id) {
    if (switch_map.find(switch_id) == switch_map.end()) {
        cout << "Node below does not exist!" << endl;
        print_bit_format(switch_id);
        abort();
    }
}

void AIDCMsoft::checkSwitchNotExists(uint64_t switch_id) {
    if (switch_map.find(switch_id) != switch_map.end()) {
        cout << "Node below does already exists!" << endl;
        print_bit_format(switch_id);
        abort();
    }
}

void AIDCMsoft::checkQueueExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id) {
    // cout << "Validating exists -- tuple: <" <<
    //     get_bit_format(get<0>(queue_id)) << "," <<
    //     get<1>(queue_id) << "," <<
    //     get_bit_format(get<2>(queue_id)) << "," <<
    //     get<3>(queue_id) << ">" << endl;
    if (queue_map.find(queue_id) == queue_map.end()) {
        cout << "Queue does not exist! queue tuple: <" << get_bit_format(get<0>(queue_id)) << ","
             << get<1>(queue_id) << "," << get_bit_format(get<2>(queue_id)) << ","
             << get<3>(queue_id) << ">" << endl;
        abort();
    }
}

void AIDCMsoft::checkQueueNotExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id) {
    // cout << "Validating not exists -- tuple: <" <<
    //     get_bit_format(get<0>(queue_id)) << "," <<
    //     get<1>(queue_id) << "," <<
    //     get_bit_format(get<2>(queue_id)) << "," <<
    //     get<3>(queue_id) << ">" << endl;
    if (queue_map.find(queue_id) != queue_map.end()) {
        cout << "Queue already exists! queue tuple: <" << get_bit_format(get<0>(queue_id)) << ","
             << get<1>(queue_id) << "," << get_bit_format(get<2>(queue_id)) << ","
             << get<3>(queue_id) << ">" << endl;
        abort();
    }
}

BaseQueue* AIDCMsoft::get_queue(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id) {
    checkQueueExists(queue_id);
    return queue_map.find(queue_id)->second;
}

void AIDCMsoft::checkPipeExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id) {
    if (pipe_map.find(pipe_id) == pipe_map.end()) {
        cout << "Pipe does not exist! pipe tuple: <" << get_bit_format(get<0>(pipe_id)) << ","
             << get<1>(pipe_id) << "," << get_bit_format(get<2>(pipe_id)) << "," << get<3>(pipe_id)
             << ">" << endl;
        abort();
    }
}

void AIDCMsoft::checkPipeNotExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id) {
    if (pipe_map.find(pipe_id) != pipe_map.end()) {
        cout << "Pipe already exists! pipe tuple: <" << get_bit_format(get<0>(pipe_id)) << ","
             << get<1>(pipe_id) << "," << get_bit_format(get<2>(pipe_id)) << "," << get<3>(pipe_id)
             << ">" << endl;
        abort();
    }
}

Pipe* AIDCMsoft::get_pipe(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id) {
    checkPipeExists(pipe_id);
    return pipe_map.find(pipe_id)->second;
}

BaseQueue* AIDCMsoft::alloc_src_queue(QueueLogger* queueLogger) {
    switch (queue_params.sender_qt) {
        case SWIFT_SCHEDULER:
            abort();
        case PRIORITY:
            return new PriorityQueue(
                srv_tor_link_speed * 1, memFromPkt(FEEDER_BUFFER), *_eventlist, queueLogger);
        case FAIR_PRIO:
            return new FairPriorityQueue(
                srv_tor_link_speed * 1, memFromPkt(FEEDER_BUFFER), *_eventlist, queueLogger);
        default:
            abort();
    }
}

BaseQueue* AIDCMsoft::alloc_tor_queue(QueueLogger*  queueLogger,
                                      linkspeed_bps speed,
                                      mem_b         queuesize,
                                      Switch*       sw) {
    switch (queue_params.intra_region_qt) {
        case COMPOSITE: {
            CompositeQueue* q = new CompositeQueue(speed,
                                                   queuesize,
                                                   *_eventlist,
                                                   queueLogger,
                                                   /*trim_size=*/64,
                                                   false,  // no_ecn
                                                   false,
                                                   false,
                                                   false,
                                                   0,  // no drop event
                                                   _switch_random_drop_prob,
                                                   "");
            return q;
        }
        case COMP_NO_ECN: {
            CompositeQueue* q =
                new CompositeQueue(speed, queuesize, *_eventlist, queueLogger, /*trim_size=*/64);
            return q;
        }
        case LOSSLESS: {
            LosslessQueue* q = new LosslessQueue(
                speed, queuesize, *_eventlist, queueLogger, sw, 0, 0, _enable_pfc);
            return q;
        }
        default:
            cout << "alloc_tor_queue: Unknown queue!" << endl;
            abort();
    }
}

BaseQueue* AIDCMsoft::alloc_leaf_queue(QueueLogger*  queueLogger,
                                       linkspeed_bps speed,
                                       mem_b         queuesize,
                                       Switch*       sw) {
    switch (queue_params.intra_region_qt) {
        case COMPOSITE: {
            CompositeQueue* q = new CompositeQueue(speed,
                                                   queuesize,
                                                   *_eventlist,
                                                   queueLogger,
                                                   /*trim_size=*/64,
                                                   false,  // no_ecn
                                                   false,
                                                   false,
                                                   false,
                                                   0,  // no drop event
                                                   _switch_random_drop_prob,
                                                   "");
            return q;
        }
        case COMP_NO_ECN: {
            CompositeQueue* q =
                new CompositeQueue(speed, queuesize, *_eventlist, queueLogger, /*trim_size=*/64);
            return q;
        }
        case LOSSLESS: {
            LosslessQueue* q = new LosslessQueue(
                speed, queuesize, *_eventlist, queueLogger, sw, 0, 0, _enable_pfc);
            return q;
        }
        default:
            cout << "alloc_leaf_queue: Unknown queue!" << endl;
            abort();
    }
}

BaseQueue* AIDCMsoft::alloc_spine_queue(QueueLogger*  queueLogger,
                                        linkspeed_bps speed,
                                        mem_b         queuesize,
                                        Switch*       sw,
                                        bool          use_link_down,
                                        unsigned int  leaf_group_idx,
                                        unsigned int  leaf_idx,
                                        unsigned int  spine_idx) {
    string link_down_filename = "";
    if (use_link_down)
        link_down_filename = "scripts/msft_ai_link_events/" + to_string(leaf_group_idx) + "_" +
                             to_string(leaf_idx) + "_link_down_events.txt";
    switch (queue_params.intra_region_qt) {
        case COMPOSITE: {
            CompositeQueue* q = new CompositeQueue(speed,
                                                   queuesize,
                                                   *_eventlist,
                                                   queueLogger,
                                                   /*trim_size=*/64,
                                                   false,  // no_ecn
                                                   false,
                                                   false,
                                                   false,
                                                   0,  // no drop event
                                                   _switch_random_drop_prob,
                                                   link_down_filename);
            return q;
        }
        case COMP_NO_ECN: {
            CompositeQueue* q = new CompositeQueue(speed,
                                                   queuesize,
                                                   *_eventlist,
                                                   queueLogger,
                                                   /*trim_size=*/64,
                                                   true,  // no_ecn
                                                   false,
                                                   false,
                                                   false,
                                                   0,  // no drop event
                                                   _switch_random_drop_prob,
                                                   link_down_filename);
            return q;
        }
        case LOSSLESS: {
            LosslessQueue* q = new LosslessQueue(
                speed, queuesize, *_eventlist, queueLogger, sw, 0, 0, _enable_pfc);
            return q;
        }
        default:
            cout << "alloc_spine_queue: Unknown queue!" << endl;
            abort();
    }
}

void AIDCMsoft::create_switches(uint32_t hash_salt) {
    for (unsigned int spine_idx = 0; spine_idx < get_num_spine_per_dc(); spine_idx++) {
        uint64_t spine_id = getSpineSwitchId(this->region_idx, this->dc_idx, spine_idx);
        checkSwitchNotExists(spine_id);
        auto spine_switch = new AISwitch(*_eventlist,
                                         "Region" + ntoa(this->region_idx) + "-DC" +
                                             ntoa(this->dc_idx) + "-Spine" + ntoa(spine_idx),
                                         AISwitch::SPINE,
                                         spine_id,
                                         _switch_latency,
                                         this,
                                         this->dc_idx,
                                         this->region_idx);
        switch_map.insert(make_pair(spine_id, spine_switch));
        spine_switch->set_hash_salt(hash_salt);
        spine_switch->setECNKMin(queue_params.spine_ECNKmin);
        spine_switch->setECNKMax(queue_params.spine_ECNKmax);
        spine_switch->setECNPMax(queue_params.spine_ECNPmax);
        spine_switch->setECNPMin(queue_params.spine_ECNPmin);
    }
    for (unsigned int leaf_group_idx = 0; leaf_group_idx < n_leafgroups_per_dc; leaf_group_idx++) {
        for (unsigned int leaf_idx = 0; leaf_idx < n_leafs_per_leafgroup; leaf_idx++) {
            uint64_t leaf_id =
                getLeafSwitchId(this->region_idx, this->dc_idx, leaf_group_idx, leaf_idx);
            checkSwitchNotExists(leaf_id);
            auto leaf_switch =
                new AISwitch(*_eventlist,
                             "Region" + ntoa(this->region_idx) + "-DC" + ntoa(this->dc_idx) +
                                 "-LeafGroup" + ntoa(leaf_group_idx) + "-Leaf" + ntoa(leaf_idx),
                             AISwitch::LEAF,
                             leaf_id,
                             _switch_latency,
                             this,
                             this->dc_idx,
                             this->region_idx);
            switch_map.insert(make_pair(leaf_id, leaf_switch));
            leaf_switch->set_hash_salt(hash_salt);
            leaf_switch->setECNKMin(queue_params.leaf_ECNKmin);
            leaf_switch->setECNKMax(queue_params.leaf_ECNKmax);
            leaf_switch->setECNPMax(queue_params.leaf_ECNPmax);
        }
        for (unsigned int tor_group_idx = 0; tor_group_idx < n_torgroups_per_leafgroup;
             tor_group_idx++) {
            for (unsigned int tor_idx = 0; tor_idx < n_tor_per_torgroups; tor_idx++) {
                uint64_t tor_id = getToRSwitchId(
                    this->region_idx, this->dc_idx, leaf_group_idx, tor_group_idx, tor_idx);
                checkSwitchNotExists(tor_id);
                auto tor_switch =
                    new AISwitch(*_eventlist,
                                 "Region" + ntoa(this->region_idx) + "-DC" + ntoa(this->dc_idx) +
                                     "-LeafGroup" + ntoa(leaf_group_idx) + "-ToRGroup" +
                                     ntoa(tor_group_idx) + "-ToR" + ntoa(tor_idx),
                                 AISwitch::TOR,
                                 tor_id,
                                 _switch_latency,
                                 this,
                                 this->dc_idx,
                                 this->region_idx);
                switch_map.insert(make_pair(tor_id, tor_switch));
                tor_switch->set_hash_salt(hash_salt);
                tor_switch->setECNKMin(queue_params.tor_ECNKmin);
                tor_switch->setECNKMax(queue_params.tor_ECNKmax);
                tor_switch->setECNPMax(queue_params.tor_ECNPmax);
                tor_switch->setECNPMin(queue_params.tor_ECNPmin);
            }
        }
    }
}

void AIDCMsoft::allocate_spine_queues_and_pipes(QueueLogger* queueLogger) {
    // spine <--> leaf: All leafs are connected to all spines
    // each leaf in the leaf group is connected to n_spine_per_leaf_in_leafgroup distinct spines
    for (unsigned int leaf_group_idx = 0; leaf_group_idx < n_leafgroups_per_dc; leaf_group_idx++) {
        unsigned int spine_idx =
            0;  // for each leaf group we should strat from the first spine of the DC
        for (unsigned int leaf_idx = 0; leaf_idx < n_leafs_per_leafgroup; leaf_idx++) {
            uint64_t leaf_id =
                getLeafSwitchId(this->region_idx, this->dc_idx, leaf_group_idx, leaf_idx);
            checkSwitchExists(leaf_id);
            auto leaf_switch = switch_map.find(leaf_id)->second;
            for (unsigned int spine_offset = 0; spine_offset < n_spine_per_leaf_in_leafgroup;
                 spine_offset++) {
                uint64_t spine_id = getSpineSwitchId(this->region_idx, this->dc_idx, spine_idx);
                checkSwitchExists(spine_id);
                auto spine_switch = switch_map.find(spine_id)->second;
                for (unsigned int bundle_idx = 0; bundle_idx < bundlesize[SPINE_TIER];
                     bundle_idx++) {
                    // spine --> leaf
                    tuple<uint64_t, uint64_t, uint64_t, uint64_t> spine_to_leaf_tuple =
                        make_tuple(spine_id, bundle_idx, leaf_id, bundle_idx);
                    checkQueueNotExists(spine_to_leaf_tuple);
                    checkPipeNotExists(spine_to_leaf_tuple);

                    if (_logger_factory) {
                        queueLogger = _logger_factory->createQueueLogger();
                    } else {
                        queueLogger = NULL;
                    }
                    auto spine_to_leaf_queue = alloc_spine_queue(queueLogger,
                                                                 leaf_spine_link_speed,
                                                                 queue_params.spine_queuesize,
                                                                 spine_switch,
                                                                 _use_link_down,
                                                                 leaf_group_idx,
                                                                 leaf_idx,
                                                                 spine_idx);
                    spine_to_leaf_queue->setName("Queue--Reg" + itoa(this->region_idx) + "-DC" +
                                                 itoa(this->dc_idx) + "-Spine" + itoa(spine_idx) +
                                                 "->LeafGroup" + itoa(leaf_group_idx) + +"Leaf" +
                                                 itoa(leaf_idx) + "(" + ntoa(bundle_idx) + ")");
                    auto spine_to_leaf_pipe = new Pipe(
                        leaf_spine_hop_latency.at(leaf_idx % leaf_spine_hop_latency.size()),
                        *_eventlist);
                    spine_to_leaf_pipe->setName("Pipe--Reg" + itoa(this->region_idx) + "-DC" +
                                                itoa(this->dc_idx) + "-Spine" + itoa(spine_idx) +
                                                "->LeafGroup" + itoa(leaf_group_idx) + +"Leaf" +
                                                itoa(leaf_idx) + "(" + ntoa(bundle_idx) + ")");
                    queue_map.insert(make_pair(spine_to_leaf_tuple, spine_to_leaf_queue));
                    pipe_map.insert(make_pair(spine_to_leaf_tuple, spine_to_leaf_pipe));

                    // leaf --> spine
                    tuple<uint64_t, uint64_t, uint64_t, uint64_t> leaf_to_spine_tuple =
                        make_tuple(leaf_id, bundle_idx, spine_id, bundle_idx);
                    checkQueueNotExists(leaf_to_spine_tuple);
                    checkPipeNotExists(leaf_to_spine_tuple);

                    if (_logger_factory) {
                        queueLogger = _logger_factory->createQueueLogger();
                    } else {
                        queueLogger = NULL;
                    }
                    auto leaf_to_spine_queue = alloc_leaf_queue(queueLogger,
                                                                leaf_spine_link_speed,
                                                                queue_params.leaf_queuesize,
                                                                leaf_switch);
                    leaf_to_spine_queue->setName(
                        "Queue--Reg" + itoa(this->region_idx) + "-DC" + itoa(this->dc_idx) +
                        "-LeafGroup" + itoa(leaf_group_idx) + +"Leaf" + itoa(leaf_idx) + "->Spine" +
                        itoa(spine_idx) + "(" + ntoa(bundle_idx) + ")");
                    auto leaf_to_spine_pipe = new Pipe(
                        leaf_spine_hop_latency.at(leaf_idx % leaf_spine_hop_latency.size()),
                        *_eventlist);
                    leaf_to_spine_pipe->setName(
                        "Pipe--Reg" + itoa(this->region_idx) + "-DC" + itoa(this->dc_idx) +
                        "-LeafGroup" + itoa(leaf_group_idx) + +"Leaf" + itoa(leaf_idx) + "->Spine" +
                        itoa(spine_idx) + "(" + ntoa(bundle_idx) + ")");
                    queue_map.insert(make_pair(leaf_to_spine_tuple, leaf_to_spine_queue));
                    pipe_map.insert(make_pair(leaf_to_spine_tuple, leaf_to_spine_pipe));

                    // add ports
                    leaf_switch->addPort(leaf_to_spine_queue);
                    spine_switch->addPort(spine_to_leaf_queue);
                    // add remote endpoints
                    spine_to_leaf_queue->setRemoteEndpoint(leaf_switch);
                    leaf_to_spine_queue->setRemoteEndpoint(spine_switch);
                }
                spine_idx++;
            }
        }
    }
}

void AIDCMsoft::allocate_leaf_queues_and_pipes(QueueLogger* queueLogger) {
    for (unsigned int leaf_group_idx = 0; leaf_group_idx < n_leafgroups_per_dc; leaf_group_idx++) {
        // leaf in each group <--> tors in that group
        for (unsigned int leaf_idx = 0; leaf_idx < n_leafs_per_leafgroup; leaf_idx++) {
            uint64_t leaf_id =
                getLeafSwitchId(this->region_idx, this->dc_idx, leaf_group_idx, leaf_idx);
            checkSwitchExists(leaf_id);
            auto leaf_switch = switch_map.find(leaf_id)->second;
            for (unsigned int tor_group_idx = 0; tor_group_idx < n_torgroups_per_leafgroup;
                 tor_group_idx++) {
                for (unsigned int tor_idx = 0; tor_idx < n_tor_per_torgroups; tor_idx++) {
                    uint64_t tor_id = getToRSwitchId(
                        this->region_idx, this->dc_idx, leaf_group_idx, tor_group_idx, tor_idx);
                    checkSwitchExists(tor_id);
                    auto tor_switch = switch_map.find(tor_id)->second;
                    for (unsigned int bundle_idx = 0; bundle_idx < bundlesize[LEAF_TIER];
                         bundle_idx++) {
                        // leaf --> tor
                        tuple<uint64_t, uint64_t, uint64_t, uint64_t> leaf_to_tor_tuple =
                            make_tuple(leaf_id, bundle_idx, tor_id, bundle_idx);
                        checkQueueNotExists(leaf_to_tor_tuple);
                        checkPipeNotExists(leaf_to_tor_tuple);

                        if (_logger_factory) {
                            queueLogger = _logger_factory->createQueueLogger();
                        } else {
                            queueLogger = NULL;
                        }
                        auto leaf_to_tor_queue = alloc_leaf_queue(queueLogger,
                                                                  tor_leaf_link_speed,
                                                                  queue_params.leaf_queuesize,
                                                                  leaf_switch);
                        leaf_to_tor_queue->setName("Queue--Reg" + itoa(this->region_idx) + "-DC" +
                                                   itoa(this->dc_idx) + "-LeafGroup" +
                                                   itoa(leaf_group_idx) + +"Leaf" + itoa(leaf_idx) +
                                                   "->ToRGroup" + itoa(tor_group_idx) + "ToR" +
                                                   itoa(tor_idx) + "(" + ntoa(bundle_idx) + ")");
                        auto leaf_to_tor_pipe = new Pipe(tor_leaf_hop_latency, *_eventlist);
                        leaf_to_tor_pipe->setName("Pipe--Reg" + itoa(this->region_idx) + "-DC" +
                                                  itoa(this->dc_idx) + "-LeafGroup" +
                                                  itoa(leaf_group_idx) + +"Leaf" + itoa(leaf_idx) +
                                                  "->ToRGroup" + itoa(tor_group_idx) + "ToR" +
                                                  itoa(tor_idx) + "(" + ntoa(bundle_idx) + ")");
                        queue_map.insert(make_pair(leaf_to_tor_tuple, leaf_to_tor_queue));
                        pipe_map.insert(make_pair(leaf_to_tor_tuple, leaf_to_tor_pipe));

                        // tor --> leaf
                        tuple<uint64_t, uint64_t, uint64_t, uint64_t> tor_to_leaf_tuple =
                            make_tuple(tor_id, bundle_idx, leaf_id, bundle_idx);
                        checkQueueNotExists(tor_to_leaf_tuple);
                        checkPipeNotExists(tor_to_leaf_tuple);
                        if (_logger_factory) {
                            queueLogger = _logger_factory->createQueueLogger();
                        } else {
                            queueLogger = NULL;
                        }
                        auto tor_to_leaf_queue = alloc_tor_queue(queueLogger,
                                                                 tor_leaf_link_speed,
                                                                 queue_params.tor_queuesize,
                                                                 tor_switch);
                        tor_to_leaf_queue->setName("Queue--Reg" + itoa(this->region_idx) + "-DC" +
                                                   itoa(this->dc_idx) + "-ToRGroup" +
                                                   itoa(tor_group_idx) + +"ToR" + itoa(tor_idx) +
                                                   "->LeafGroup" + itoa(leaf_group_idx) + "Leaf" +
                                                   itoa(leaf_idx) + "(" + ntoa(bundle_idx) + ")");
                        auto tor_to_leaf_pipe = new Pipe(tor_leaf_hop_latency, *_eventlist);
                        tor_to_leaf_pipe->setName("Pipe--Reg" + itoa(this->region_idx) + "-DC" +
                                                  itoa(this->dc_idx) + "-ToRGroup" +
                                                  itoa(tor_group_idx) + +"ToR" + itoa(tor_idx) +
                                                  "->LeafGroup" + itoa(leaf_group_idx) + "Leaf" +
                                                  itoa(leaf_idx) + "(" + ntoa(bundle_idx) + ")");
                        queue_map.insert(make_pair(tor_to_leaf_tuple, tor_to_leaf_queue));
                        pipe_map.insert(make_pair(tor_to_leaf_tuple, tor_to_leaf_pipe));

                        // add ports
                        leaf_switch->addPort(leaf_to_tor_queue);
                        tor_switch->addPort(tor_to_leaf_queue);
                        // add remote endpoints
                        leaf_to_tor_queue->setRemoteEndpoint(tor_switch);
                        tor_to_leaf_queue->setRemoteEndpoint(leaf_switch);
                    }
                }
            }
        }
    }
}

void AIDCMsoft::allocate_tor_queues_and_pipes(QueueLogger* queueLogger) {
    for (unsigned int leaf_group_idx = 0; leaf_group_idx < n_leafgroups_per_dc; leaf_group_idx++) {
        // ToR in each group <--> servers in that group
        for (unsigned int tor_group_idx = 0; tor_group_idx < n_torgroups_per_leafgroup;
             tor_group_idx++) {
            for (unsigned int tor_idx = 0; tor_idx < n_tor_per_torgroups; tor_idx++) {
                uint64_t tor_id = getToRSwitchId(
                    this->region_idx, this->dc_idx, leaf_group_idx, tor_group_idx, tor_idx);
                checkSwitchExists(tor_id);
                auto tor_switch = switch_map.find(tor_id)->second;
                for (unsigned int server_idx = 0; server_idx < n_serv_per_torgroup; server_idx++) {
                    uint64_t server_id = getServerId(server_idx);
                    for (unsigned int bundle_idx = 0; bundle_idx < bundlesize[TOR_TIER];
                         bundle_idx++) {
                        // tor --> server
                        tuple<uint64_t, uint64_t, uint64_t, uint64_t> tor_to_server_tuple =
                            make_tuple(tor_id, bundle_idx, server_id, bundle_idx);
                        checkQueueNotExists(tor_to_server_tuple);
                        checkPipeNotExists(tor_to_server_tuple);

                        if (_logger_factory) {
                            queueLogger = _logger_factory->createQueueLogger();
                        } else {
                            queueLogger = NULL;
                        }
                        auto tor_to_server_queue = alloc_tor_queue(queueLogger,
                                                                   srv_tor_link_speed,
                                                                   queue_params.tor_queuesize,
                                                                   tor_switch);
                        tor_to_server_queue->setName(
                            "Queue--Reg" + itoa(this->region_idx) + "-DC" + itoa(this->dc_idx) +
                            "-LeafGroup" + itoa(leaf_group_idx) + "ToRGroup" + itoa(tor_group_idx) +
                            "ToR" + itoa(tor_idx) + "->Serv" + itoa(server_idx) + "(" +
                            ntoa(bundle_idx) + ")");
                        auto tor_to_server_pipe = new Pipe(srv_tor_hop_latency, *_eventlist);
                        tor_to_server_pipe->setName(
                            "Pipe--Reg" + itoa(this->region_idx) + "-DC" + itoa(this->dc_idx) +
                            "-LeafGroup" + itoa(leaf_group_idx) + "ToRGroup" + itoa(tor_group_idx) +
                            "ToR" + itoa(tor_idx) + "->Serv" + itoa(server_idx) + "(" +
                            ntoa(bundle_idx) + ")");
                        queue_map.insert(make_pair(tor_to_server_tuple, tor_to_server_queue));
                        pipe_map.insert(make_pair(tor_to_server_tuple, tor_to_server_pipe));

                        // server --> tor
                        tuple<uint64_t, uint64_t, uint64_t, uint64_t> server_to_tor_tuple =
                            make_tuple(server_id, bundle_idx, tor_id, bundle_idx);
                        checkQueueNotExists(server_to_tor_tuple);
                        checkPipeNotExists(server_to_tor_tuple);

                        if (_logger_factory) {
                            queueLogger = _logger_factory->createQueueLogger();
                        } else {
                            queueLogger = NULL;
                        }
                        auto server_to_tor_queue = alloc_src_queue(queueLogger);
                        server_to_tor_queue->setName(
                            "Queue--Reg" + itoa(this->region_idx) + "-DC" + itoa(this->dc_idx) +
                            "Serv" + itoa(server_idx) + "->LeafGroup" + itoa(leaf_group_idx) +
                            "ToRGroup" + itoa(tor_group_idx) + "ToR" + itoa(tor_idx) + "(" +
                            ntoa(bundle_idx) + ")");
                        auto server_to_tor_pipe = new Pipe(srv_tor_hop_latency, *_eventlist);
                        server_to_tor_pipe->setName("Pipe--Reg" + itoa(this->region_idx) + "-DC" +
                                                    itoa(this->dc_idx) + "Serv" + itoa(server_idx) +
                                                    "->LeafGroup" + itoa(leaf_group_idx) +
                                                    "ToRGroup" + itoa(tor_group_idx) + "ToR" +
                                                    itoa(tor_idx) + "(" + ntoa(bundle_idx) + ")");
                        queue_map.insert(make_pair(server_to_tor_tuple, server_to_tor_queue));
                        pipe_map.insert(make_pair(server_to_tor_tuple, server_to_tor_pipe));

                        // add ports
                        tor_switch->addPort(tor_to_server_queue);
                        // add remote endpoints
                        server_to_tor_queue->setRemoteEndpoint(tor_switch);
                    }
                }
            }
        }
    }
}

void AIDCMsoft::init_network() {
    QueueLogger* queueLogger = nullptr;

    cout << "Creating DC network. Info for each DC: " << endl
         << "\t Region index: " << get_region_idx() << endl
         << "\t DC index: " << get_dc_idx() << endl
         << "\t Num spines: " << get_num_spine_per_dc() << endl
         << "\t Num groups of leafs: " << n_leafgroups_per_dc << endl
         << "\t Num leafs per group: " << n_leafs_per_leafgroup << endl
         << "\t Num groups of ToRs per leaf group: " << n_torgroups_per_leafgroup << endl
         << "\t Num ToRs per ToR group: " << n_tor_per_torgroups << endl
         << "\t Num servers per group: " << n_serv_per_torgroup << "\n"
         << endl;

    //  Create switches
    std::cout << "Creating DC switches!" << endl;
    create_switches(this->_hash_salt);
    // allocate queues and pipes and connect end points
    std::cout << "Creating spine's queues and pipes!" << endl;
    allocate_spine_queues_and_pipes(queueLogger);
    std::cout << "Creating leaf's queues and pipes!" << endl;
    allocate_leaf_queues_and_pipes(queueLogger);
    std::cout << "Creating tor's queues and pipes!" << endl;
    allocate_tor_queues_and_pipes(queueLogger);
}

void AIDCMsoft::count_queue(Queue* queue) {
    if (_link_usage.find(queue) == _link_usage.end()) {
        _link_usage[queue] = 0;
    }

    _link_usage[queue] = _link_usage[queue] + 1;
}

vector<const Route*>* AIDCMsoft::get_bidir_paths(uint32_t src, uint32_t dest, bool reverse) {
    return NULL;
}

void AIDCMsoft::add_switch_loggers(Logfile& log, simtime_picosec sample_period) {
    return;
}
