// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "AI_region_msoft.h"

#include <bitset>
#include <iostream>
#include <sstream>
#include <vector>

#include "AI_WAN_types.h"
#include "AI_switch.h"
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
int  AIRegionMsoft::bts_trigger     = -1;
bool AIRegionMsoft::bts_ignore_data = true;

//  extern int N;

AIRegionMsoft::AIRegionMsoft(unsigned int                  region_idx,
                             struct AI_wan_topology_params params,
                             linkspeed_bps                 intra_dc_linkspeed,
                             linkspeed_bps                 inter_dc_linkspeed,
                             struct queue_params&          queue_params,
                             vector<double>&               switch_drop_event_probs,
                             vector<double>&               switch_random_drop_probs,
                             QueueLoggerFactory*           logger_factory,
                             EventList*                    ev,
                             bool                          enable_pfc)
    : queue_params(queue_params),
      _switch_random_drop_probs(switch_random_drop_probs),
      _switch_drop_event_probs(switch_drop_event_probs) {
    this->n_region                         = params.n_dc_per_region;
    this->n_dc_per_region                  = params.n_dc_per_region;
    this->region_idx                       = region_idx;
    this->n_spine_per_leaf_in_leafgroup    = params.n_spine_per_leaf_in_leafgroup;
    this->n_leafgroups_per_dc              = params.n_leafgroups_per_dc;
    this->n_leafs_per_leafgroup            = params.n_leafs_per_leafgroup;
    this->n_torgroups_per_leafgroup        = params.n_torgroups_per_leafgroup;
    this->n_tor_per_torgroups              = params.n_tor_per_torgroups;
    this->n_serv_per_torgroup              = params.n_serv_per_torgroup;
    this->n_RH_groups                      = params.n_RH_groups;
    this->n_RH_switch_batches_per_RH_group = params.n_RH_switch_batches_per_RH_group;
    this->n_RH_switches_per_RH_batch       = params.n_RH_switches_per_RH_batch;

    this->_intra_dc_link_speed = intra_dc_linkspeed;
    this->_inter_dc_link_speed = inter_dc_linkspeed;
    this->_logger_factory      = logger_factory;
    this->_eventlist           = ev;
    this->hop_latency          = timeFromUs((uint32_t)1);
    this->_switch_latency      = timeFromUs((uint32_t)0);
    this->_enable_pfc          = enable_pfc;

    init_network();
}

void AIRegionMsoft::checkDCExists(unsigned int dc_idx) {
    if (dc_map.find(dc_idx) == dc_map.end()) {
        cout << "Region " << this->region_idx << " DC " << dc_idx << "does not exist" << endl;
        abort();
    }
}

void AIRegionMsoft::checkDCNotExists(unsigned int dc_idx) {
    if (dc_map.find(dc_idx) != dc_map.end()) {
        cout << "Region " << this->region_idx << " DC " << dc_idx << "already exists" << endl;
        abort();
    }
}

void AIRegionMsoft::checkQueueExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id) {
    // cout << "Validating exists -- tuple: <" <<
    //     get_bit_format(get<0>(queue_id)) << "," <<
    //     get<1>(queue_id) << "," <<
    //     get_bit_format(get<2>(queue_id)) << "," <<
    //     get<3>(queue_id) << ">" << endl;
    if (region_queue_map.find(queue_id) == region_queue_map.end()) {
        cout << "AIRegionMsoft::Queue does not exist! queue tuple: <"
             << get_bit_format(get<0>(queue_id)) << "," << get<1>(queue_id) << ","
             << get_bit_format(get<2>(queue_id)) << "," << get<3>(queue_id) << ">" << endl;
        abort();
    }
}

void AIRegionMsoft::checkQueueNotExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id) {
    // cout << "Validating not exists -- tuple: <" <<
    //     get_bit_format(get<0>(queue_id)) << "," <<
    //     get<1>(queue_id) << "," <<
    //     get_bit_format(get<2>(queue_id)) << "," <<
    //     get<3>(queue_id) << ">" << endl;
    if (region_queue_map.find(queue_id) != region_queue_map.end()) {
        cout << "AIRegionMsoft::Queue already exists! queue tuple: <"
             << get_bit_format(get<0>(queue_id)) << "," << get<1>(queue_id) << ","
             << get_bit_format(get<2>(queue_id)) << "," << get<3>(queue_id) << ">" << endl;
        abort();
    }
}

BaseQueue* AIRegionMsoft::get_queue(tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_id) {
    checkQueueExists(queue_id);
    return region_queue_map.find(queue_id)->second;
}

void AIRegionMsoft::checkPipeExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id) {
    if (region_pipe_map.find(pipe_id) == region_pipe_map.end()) {
        cout << "AIRegionMsoft::Pipe does not exist! pipe tuple: <"
             << get_bit_format(get<0>(pipe_id)) << "," << get<1>(pipe_id) << ","
             << get_bit_format(get<2>(pipe_id)) << "," << get<3>(pipe_id) << ">" << endl;
        abort();
    }
}

void AIRegionMsoft::checkPipeNotExists(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id) {
    if (region_pipe_map.find(pipe_id) != region_pipe_map.end()) {
        cout << "AIRegionMsoft::Pipe already exists! pipe tuple: <"
             << get_bit_format(get<0>(pipe_id)) << "," << get<1>(pipe_id) << ","
             << get_bit_format(get<2>(pipe_id)) << "," << get<3>(pipe_id) << ">" << endl;
        abort();
    }
}

Pipe* AIRegionMsoft::get_pipe(tuple<uint64_t, uint64_t, uint64_t, uint64_t> pipe_id) {
    checkPipeExists(pipe_id);
    return region_pipe_map.find(pipe_id)->second;
}

AIDCMsoft* AIRegionMsoft::get_dc(unsigned int dc_idx) {
    checkDCExists(dc_idx);
    return dc_map.find(dc_idx)->second;
}

bitset<64> AIRegionMsoft::get_bit_format(uint64_t num) {
    return bitset<64>(num);
}

void AIRegionMsoft::print_bit_format(uint64_t num) {
    cout << get_bit_format(num) << endl;
}

void AIRegionMsoft::checkSwitchExists(uint64_t switch_id) {
    if (region_switch_map.find(switch_id) == region_switch_map.end()) {
        cout << "AIRegionMsoft::Node below does not exist!" << endl;
        print_bit_format(switch_id);
        abort();
    }
}

void AIRegionMsoft::checkSwitchNotExists(uint64_t switch_id) {
    if (region_switch_map.find(switch_id) != region_switch_map.end()) {
        cout << "AIRegionMsoft::Node below does already exists!" << endl;
        print_bit_format(switch_id);
        abort();
    }
}

void AIRegionMsoft::create_DCs() {
    for (unsigned int dc_idx = 0; dc_idx < get_num_dc_per_region(); dc_idx++) {
        checkDCNotExists(dc_idx);
        auto m_dc    = new AIDCMsoft(this->n_region,
                                  this->n_dc_per_region,
                                  this->region_idx,
                                  dc_idx,
                                  this->n_spine_per_leaf_in_leafgroup,
                                  this->n_leafgroups_per_dc,
                                  this->n_leafs_per_leafgroup,
                                  this->n_torgroups_per_leafgroup,
                                  this->n_tor_per_torgroups,
                                  this->n_serv_per_torgroup,
                                  this->_intra_dc_link_speed,
                                  this->queue_params,
                                  this->_logger_factory,
                                  this->_eventlist);
        m_dc->region = this;
        this->num_total_servers_in_region += m_dc->get_number_of_servers_per_dc();
        dc_map.insert(make_pair(dc_idx, m_dc));
    }
}

uint64_t AIRegionMsoft::getRHSwitchId(uint64_t region_id,
                                      uint64_t rh_group_id,
                                      uint64_t rh_batch_idx,
                                      uint64_t rh_switch_idx) {
    // RH are Region Hub Routers

    /*
        <0,3>: type = RH
        <4,7>: region id
        <8,11>: RH Group ID
        <12,15>: RH switch batch index
        <16,19>: RH switch index
    */
    uint64_t switch_id = AISwitch::RH;
    switch_id |= (uint64_t(region_id) << 4);
    switch_id |= (uint64_t(rh_group_id) << 8);
    switch_id |= (uint64_t(rh_batch_idx) << 12);
    switch_id |= (uint64_t(rh_switch_idx) << 16);
    return switch_id;
}

SwitchInfo AIRegionMsoft::parseRHSwitchId(uint64_t switch_id) {
    SwitchInfo info;

    info.switch_type   = (switch_id & 0xF);        // Bits 0–3
    info.region_idx    = (switch_id >> 4) & 0xF;   // Bits 4–7
    info.rh_group_idx  = (switch_id >> 8) & 0xF;   // Bits 8–11
    info.rh_batch_idx  = (switch_id >> 12) & 0xF;  // Bits 12–15
    info.rh_switch_idx = (switch_id >> 16) & 0xF;  // Bits 16–19

    if (info.switch_type != AISwitch::RH) {
        cout << "Passed switch ID is not related to a region hub switch!" << endl;
        abort();
    }

    return info;
}

BaseQueue* AIRegionMsoft::alloc_queue(QueueLogger*  queueLogger,
                                      linkspeed_bps speed,
                                      mem_b         queuesize,
                                      AISwitch*     sw) {
    switch (queue_params.intra_region_qt) {
        case COMP_NO_ECN: {
            CompositeQueue* q =
                new CompositeQueue(speed, queuesize, *_eventlist, queueLogger, /*trim_size=*/64);
            return q;
        }
        case LOSSLESS: {
            LosslessQueue* q = new LosslessQueue(speed,
                                                 queuesize,
                                                 *_eventlist,
                                                 queueLogger,
                                                 sw,
                                                 _switch_drop_event_probs[sw->getType()],
                                                 _switch_random_drop_probs[sw->getType()],
                                                 _enable_pfc);
            return q;
        }
        default:
            cout << "alloc_queue: Unknown queue!" << endl;
            abort();
    }
}

AIDCMsoft* AIRegionMsoft::get_dc_for_server(unsigned int server_idx) {
    unsigned int original_server_idx = server_idx;
    for (unsigned int dc_idx = 0; dc_idx < n_dc_per_region; dc_idx++) {
        checkDCExists(dc_idx);
        auto m_dc = dc_map.find(dc_idx)->second;
        server_idx %= m_dc->get_number_of_servers_per_region();
        if (server_idx < m_dc->get_number_of_servers_per_dc())
            return m_dc;
        server_idx -= m_dc->get_number_of_servers_per_dc();
    }
    cout << "get_dc_for_server returning NULL for server " << original_server_idx << endl;
    abort();
}

void AIRegionMsoft::create_switches() {
    for (unsigned int rh_group_idx = 0; rh_group_idx < n_RH_groups; rh_group_idx++) {
        for (unsigned int rh_batch_idx = 0; rh_batch_idx < n_RH_switch_batches_per_RH_group;
             rh_batch_idx++) {
            for (unsigned int rh_switch_idx = 0; rh_switch_idx < n_RH_switches_per_RH_batch;
                 rh_switch_idx++) {
                uint64_t rh_switch_id =
                    getRHSwitchId(this->region_idx, rh_group_idx, rh_batch_idx, rh_switch_idx);
                checkSwitchNotExists(rh_switch_id);
                auto rh_switch = new AISwitch(
                    *_eventlist,
                    "Region" + ntoa(this->region_idx) + "-RHGroup" + ntoa(rh_group_idx) +
                        "-RHBatch" + ntoa(rh_batch_idx) + "-RHSwitch" + ntoa(rh_switch_idx),
                    AISwitch::RH,
                    rh_switch_id,
                    _switch_latency,
                    this,
                    this->region_idx);
                region_switch_map.insert(make_pair(rh_switch_id, rh_switch));
                // Set ECN parameters
                static_cast<AISwitch*>(rh_switch)->setECNKMin(queue_params.RH_ECNKmin);
                static_cast<AISwitch*>(rh_switch)->setECNKMax(queue_params.RH_ECNKmax);
                static_cast<AISwitch*>(rh_switch)->setECNPMax(queue_params.RH_ECNPmax);
                static_cast<AISwitch*>(rh_switch)->setECNPMin(queue_params.RH_ECNPmin);
            }
        }
    }
}

void AIRegionMsoft::allocate_RH_queues_and_pipes(QueueLogger* queueLogger) {
    // RH <--> spine: All leafs are connected to all spines
    // each spine is connected to one RH switch per RH group
    for (unsigned int dc_idx = 0; dc_idx < n_dc_per_region; dc_idx++) {
        checkDCExists(dc_idx);
        auto m_dc = dc_map.find(dc_idx)->second;
        for (unsigned int spine_idx = 0; spine_idx < m_dc->get_num_spine_per_dc(); spine_idx++) {
            uint64_t spine_id = m_dc->getSpineSwitchId(this->region_idx, dc_idx, spine_idx);
            m_dc->checkSwitchExists(spine_id);
            auto         spine_switch                 = m_dc->switch_map.find(spine_id)->second;
            unsigned int rh_batch_idx_mapped_to_spine = get_RH_batch_idx_mapped_to_spine(spine_idx);
            for (unsigned int region_group_idx = 0; region_group_idx < n_RH_groups;
                 region_group_idx++) {
                for (unsigned int rh_switch_idx = 0; rh_switch_idx < n_RH_switches_per_RH_batch;
                     rh_switch_idx++) {
                    uint64_t rh_switch_id = getRHSwitchId(this->region_idx,
                                                          region_group_idx,
                                                          rh_batch_idx_mapped_to_spine,
                                                          rh_switch_idx);

                    checkSwitchExists(rh_switch_id);
                    auto rh_switch = region_switch_map.find(rh_switch_id)->second;
                    for (unsigned int bundle_idx = 0; bundle_idx < region_bundlesize[RH_TIER];
                         bundle_idx++) {
                        // rh --> spine
                        tuple<uint64_t, uint64_t, uint64_t, uint64_t> rh_to_spine_tuple =
                            make_tuple(rh_switch_id, bundle_idx, spine_id, bundle_idx);
                        checkQueueNotExists(rh_to_spine_tuple);
                        checkPipeNotExists(rh_to_spine_tuple);

                        if (_logger_factory) {
                            queueLogger = _logger_factory->createQueueLogger();
                        } else {
                            queueLogger = NULL;
                        }
                        auto rh_to_spine_queue = alloc_queue(queueLogger,
                                                             _inter_dc_link_speed,
                                                             queue_params.RH_queuesize,
                                                             static_cast<AISwitch*>(rh_switch));
                        rh_to_spine_queue->setName("Queue--Reg" + itoa(this->region_idx) +
                                                   "-RHGroup" + itoa(region_group_idx) +
                                                   "-RHBatch" + itoa(rh_batch_idx_mapped_to_spine) +
                                                   "RHSwitch" + itoa(rh_switch_idx) + "->DC" +
                                                   itoa(m_dc->get_dc_idx()) + "-Spine" +
                                                   itoa(spine_idx) + "(" + ntoa(bundle_idx) + ")");
                        auto rh_to_spine_pipe = new Pipe(hop_latency, *_eventlist);
                        rh_to_spine_pipe->setName("Pipe--Reg" + itoa(this->region_idx) +
                                                  "-RHGroup" + itoa(region_group_idx) + "-RHBatch" +
                                                  itoa(rh_batch_idx_mapped_to_spine) + "RHSwitch" +
                                                  itoa(rh_switch_idx) + "->DC" +
                                                  itoa(m_dc->get_dc_idx()) + "-Spine" +
                                                  itoa(spine_idx) + "(" + ntoa(bundle_idx) + ")");
                        region_queue_map.insert(make_pair(rh_to_spine_tuple, rh_to_spine_queue));
                        region_pipe_map.insert(make_pair(rh_to_spine_tuple, rh_to_spine_pipe));

                        // spine --> RH
                        tuple<uint64_t, uint64_t, uint64_t, uint64_t> spine_to_rh_tuple =
                            make_tuple(spine_id, bundle_idx, rh_switch_id, bundle_idx);
                        checkQueueNotExists(spine_to_rh_tuple);
                        checkPipeNotExists(spine_to_rh_tuple);

                        if (_logger_factory) {
                            queueLogger = _logger_factory->createQueueLogger();
                        } else {
                            queueLogger = NULL;
                        }
                        auto spine_to_rh_queue =
                            m_dc->alloc_spine_queue(queueLogger,
                                                    _inter_dc_link_speed,
                                                    queue_params.spine_queuesize,
                                                    spine_switch,
                                                    false,
                                                    0,
                                                    0,
                                                    spine_idx);
                        spine_to_rh_queue->setName(
                            "Queue--Reg" + itoa(this->region_idx) + "-DC" +
                            itoa(m_dc->get_dc_idx()) + "-Spine" + itoa(spine_idx) + "->RHGroup" +
                            itoa(region_group_idx) + "-RHBatch" +
                            itoa(rh_batch_idx_mapped_to_spine) + "RHSwitch" + itoa(rh_switch_idx) +
                            "(" + ntoa(bundle_idx) + ")");
                        auto spine_to_rh_pipe = new Pipe(hop_latency, *_eventlist);
                        spine_to_rh_pipe->setName(
                            "Pipe--Reg" + itoa(this->region_idx) + "-DC" +
                            itoa(m_dc->get_dc_idx()) + "-Spine" + itoa(spine_idx) + "->RHGroup" +
                            itoa(region_group_idx) + "-RHBatch" +
                            itoa(rh_batch_idx_mapped_to_spine) + "RHSwitch" + itoa(rh_switch_idx) +
                            "(" + ntoa(bundle_idx) + ")");
                        region_queue_map.insert(make_pair(spine_to_rh_tuple, spine_to_rh_queue));
                        region_pipe_map.insert(make_pair(spine_to_rh_tuple, spine_to_rh_pipe));

                        // add ports
                        rh_switch->addPort(rh_to_spine_queue);
                        spine_switch->addPort(spine_to_rh_queue);
                        // add remote endpoints
                        spine_to_rh_queue->setRemoteEndpoint(rh_switch);
                        rh_to_spine_queue->setRemoteEndpoint(spine_switch);
                    }
                }
            }
        }
    }
}

void AIRegionMsoft::init_network() {
    QueueLogger* queueLogger = nullptr;

    cout << "Creating region. Info for each region: " << endl
         << "\t Region index: " << get_region_idx() << endl
         << "\t Num RH groups: " << n_RH_groups << endl
         << "\t Num RH batches per RH group: " << n_RH_switch_batches_per_RH_group << endl
         << "\t Num RH switches per RH batch: " << n_RH_switches_per_RH_batch << "\n"
         << endl;

    // creating DCs
    create_DCs();

    std::cout << "Creating Region switches!" << endl;
    create_switches();
    // allocate queues and pipes and connect end points
    std::cout << "Creating RH's queues and pipes!" << endl;
    allocate_RH_queues_and_pipes(queueLogger);
}

vector<const Route*>* AIRegionMsoft::get_bidir_paths(uint32_t src, uint32_t dest, bool reverse) {
    return NULL;
}

void AIRegionMsoft::add_switch_loggers(Logfile& log, simtime_picosec sample_period) {
    return;
}

void AIRegionMsoft::set_switch_hash_salt(uint32_t salt) {
    // RH switches
    for (auto& switch_pair : region_switch_map) {
        AISwitch* ai_sw = dynamic_cast<AISwitch*>(switch_pair.second);
        assert(ai_sw != nullptr && "RH Switch is not actually an AISwitch");
        ai_sw->set_hash_salt(salt);
    }

    cout << "Setting hash salt, RH switch number: " << region_switch_map.size() << endl;
    cout << "DC number:" << dc_map.size() << endl;

    // DC switches
    for (auto& dc_pair : dc_map) {
        AIDCMsoft* dc = dc_pair.second;
        cout << dc->switch_map.size() << " switches in DC " << dc->get_dc_idx() << endl;

        for (auto& switch_pair : dc->switch_map) {
            AISwitch* ai_sw = dynamic_cast<AISwitch*>(switch_pair.second);
            assert(ai_sw != nullptr && "DC Switch is not actually an AISwitch");
            ai_sw->set_hash_salt(salt);
        }
    }
}

map<uint64_t, int64_t> AIRegionMsoft::get_switch_counters() {
    map<uint64_t, int64_t> counter_map;  // <switchId, switch receive packet counter>

    // Iterate through all switches in the region and get their receive packet counters
    // RH switches
    for (auto& switch_pair : region_switch_map) {
        AISwitch* ai_sw = dynamic_cast<AISwitch*>(switch_pair.second);
        assert(ai_sw != nullptr && "RH Switch is not actually an AISwitch");
        int64_t counter = ai_sw->get_receive_packet_counter();
        counter_map.insert(make_pair(switch_pair.first, counter));
    }

    // DC switches
    for (auto& dc_pair : dc_map) {
        AIDCMsoft* dc = dc_pair.second;
        for (auto& switch_pair : dc->switch_map) {
            AISwitch* ai_sw = dynamic_cast<AISwitch*>(switch_pair.second);
            assert(ai_sw != nullptr && "DC Switch is not actually an AISwitch");
            int64_t counter = ai_sw->get_receive_packet_counter();
            counter_map.insert(make_pair(switch_pair.first, counter));
        }
    }

    return counter_map;
}