// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "AI_WAN_msoft.h"

#include <bitset>
#include <iostream>
#include <sstream>
#include <vector>

#include "compositequeue.h"
#include "queue.h"
#include "queue_lossless.h"
#include "queue_lossless_input.h"
#include "queue_lossless_output.h"

AIWANMsoft::AIWANMsoft(struct AI_wan_topology_params params,
                       struct link_params&           link_params,
                       struct queue_params&          queue_params,
                       QueueLoggerFactory*           logger_factory,
                       EventList*                    ev,
                       vector<simtime_picosec>&      owr_owr_hop_latency,
                       vector<double>&               switch_drop_event_probs,
                       vector<double>&               switch_random_drop_probs,
                       bool                          use_link_down,
                       bool                          enable_pfc) {
    this->n_region                         = params.n_region;
    this->n_dc_per_region                  = params.n_dc_per_region;
    this->n_spine_per_leaf_in_leafgroup    = params.n_spine_per_leaf_in_leafgroup;
    this->n_leafgroups_per_dc              = params.n_leafgroups_per_dc;
    this->n_leafs_per_leafgroup            = params.n_leafs_per_leafgroup;
    this->n_torgroups_per_leafgroup        = params.n_torgroups_per_leafgroup;
    this->n_tor_per_torgroups              = params.n_tor_per_torgroups;
    this->n_serv_per_torgroup              = params.n_serv_per_torgroup;
    this->n_RH_groups                      = params.n_RH_groups;
    this->n_RH_switch_batches_per_RH_group = params.n_RH_switch_batches_per_RH_group;
    this->n_RH_switches_per_RH_batch       = params.n_RH_switches_per_RH_batch;
    this->n_RWA_groups                     = params.n_RWA_groups;
    this->n_RWA_switches_per_RWA_group     = params.n_RWA_switches_per_RWA_group;
    this->n_OWR_groups                     = params.n_OWR_groups;
    this->n_OWR_switches_per_OWR_group     = params.n_OWR_switches_per_OWR_group;

    this->link_params.inter_dc_link_speed = link_params.inter_dc_link_speed;
    this->link_params.intra_dc_link_speed = link_params.intra_dc_link_speed;
    this->link_params.link_factor_rh_rwa  = link_params.link_factor_rh_rwa;
    this->link_params.link_factor_rwa_owr = link_params.link_factor_rwa_owr;
    this->link_params.link_factor_regional =
        link_params.link_factor_regional;  // This is the factor for the regional backbone links

    this->_logger_factory                           = logger_factory;
    this->_eventlist                                = ev;
    this->hop_latency.regional_backbone_hop_latency = timeFromUs((uint32_t)100);
    this->hop_latency.intra_region_hop_latency      = timeFromUs((uint32_t)1);
    this->hop_latency.inter_owr_hop_latency.assign(owr_owr_hop_latency.begin(),
                                                   owr_owr_hop_latency.end());
    this->_switch_latency = timeFromUs((uint32_t)0);

    this->queue_params = queue_params;

    this->_switch_drop_event_probs.assign(switch_drop_event_probs.begin(),
                                          switch_drop_event_probs.end());

    this->_switch_random_drop_probs.assign(switch_random_drop_probs.begin(),
                                           switch_random_drop_probs.end());
    this->_use_link_down = use_link_down;
    this->_enable_pfc    = enable_pfc;

    init_network();
}

AIWANMsoft::AIWANMsoft(struct link_params&  link_params,
                       struct queue_params& queue_params,
                       QueueLoggerFactory*  logger_factory,
                       EventList*           ev) {
    this->link_params.inter_dc_link_speed = link_params.inter_dc_link_speed;
    this->link_params.intra_dc_link_speed = link_params.intra_dc_link_speed;
    this->link_params.link_factor_rh_rwa  = link_params.link_factor_rh_rwa;
    this->link_params.link_factor_rwa_owr = link_params.link_factor_rwa_owr;

    this->_logger_factory = logger_factory;
    this->_eventlist      = ev;

    this->hop_latency.regional_backbone_hop_latency = timeFromUs((uint32_t)100);
    this->hop_latency.intra_region_hop_latency      = timeFromUs((uint32_t)1);

    this->hop_latency.inter_owr_hop_latency = {timeFromUs((uint32_t)14000)};
    this->_switch_latency                   = timeFromUs((uint32_t)0);

    this->queue_params = queue_params;

    init_network();
}

void AIWANMsoft::checkRegionExists(unsigned int region_idx) {
    if (region_map.find(region_idx) == region_map.end()) {
        cout << "WAN, Region " << region_idx << "does not exist" << endl;
        abort();
    }
}

void AIWANMsoft::checkRegionNotExists(unsigned int region_idx) {
    if (region_map.find(region_idx) != region_map.end()) {
        cout << "WAN, Region " << region_idx << "already exists" << endl;
        abort();
    }
}

AIRegionMsoft* AIWANMsoft::get_region(unsigned int region_idx) {
    checkRegionExists(region_idx);
    return region_map.find(region_idx)->second;
}

AIRegionMsoft* AIWANMsoft::get_region_for_server(unsigned int server_idx) {
    unsigned int original_server_idx = server_idx;
    for (unsigned int region_idx = 0; region_idx < get_num_region(); region_idx++) {
        checkRegionExists(region_idx);
        auto m_region = region_map.find(region_idx)->second;
        assert(server_idx < get_number_of_servers_in_wan());
        if (server_idx < m_region->get_num_total_servers_in_region())
            return m_region;
        server_idx -= m_region->get_num_total_servers_in_region();
    }
    cout << "get_region_for_server returning NULL for server " << original_server_idx << endl;
    abort();
}

bitset<64> AIWANMsoft::get_bit_format(uint64_t num) {
    return bitset<64>(num);
}

void AIWANMsoft::print_bit_format(uint64_t num) {
    cout << get_bit_format(num) << endl;
}

void AIWANMsoft::checkQueueExists(tuple<uint64_t, uint64_t, uint64_t> queue_id) {
    // cout << "Validating exists -- tuple: <" <<
    //     get_bit_format(get<0>(queue_id)) << "," <<
    //     get<1>(queue_id) << "," <<
    //     get_bit_format(get<2>(queue_id)) << "," <<
    //     get<3>(queue_id) << ">" << endl;
    if (wan_queue_map.find(queue_id) == wan_queue_map.end()) {
        cout << "AIWANMsoft::Queue does not exist! queue tuple: <"
             << get_bit_format(get<0>(queue_id)) << "," << get<1>(queue_id) << ","
             << get<2>(queue_id) << ">" << endl;
        abort();
    }
}

void AIWANMsoft::checkQueueNotExists(tuple<uint64_t, uint64_t, uint64_t> queue_id) {
    // cout << "Validating not exists -- tuple: <" <<
    //     get_bit_format(get<0>(queue_id)) << "," <<
    //     get<1>(queue_id) << "," <<
    //     get_bit_format(get<2>(queue_id)) << ">" << endl;
    if (wan_queue_map.find(queue_id) != wan_queue_map.end()) {
        cout << "AIWANMsoft::Queue already exists! queue tuple: <"
             << get_bit_format(get<0>(queue_id)) << "," << get<1>(queue_id) << ","
             << get<2>(queue_id) << ">" << endl;
        abort();
    }
}

BaseQueue* AIWANMsoft::get_queue(tuple<uint64_t, uint64_t, uint64_t> queue_id) {
    checkQueueExists(queue_id);
    return wan_queue_map.find(queue_id)->second;
}

void AIWANMsoft::checkPipeExists(tuple<uint64_t, uint64_t, uint64_t> pipe_id) {
    if (wan_pipe_map.find(pipe_id) == wan_pipe_map.end()) {
        cout << "AIWANMsoft::Pipe does not exist! pipe tuple: <" << get_bit_format(get<0>(pipe_id))
             << "," << get<1>(pipe_id) << "," << get<2>(pipe_id) << ">" << endl;
        abort();
    }
}

void AIWANMsoft::checkPipeNotExists(tuple<uint64_t, uint64_t, uint64_t> pipe_id) {
    if (wan_pipe_map.find(pipe_id) != wan_pipe_map.end()) {
        cout << "AIWANMsoft::Pipe already exists! pipe tuple: <" << get_bit_format(get<0>(pipe_id))
             << "," << get<1>(pipe_id) << "," << get<2>(pipe_id) << ">" << endl;
        abort();
    }
}

Pipe* AIWANMsoft::get_pipe(tuple<uint64_t, uint64_t, uint64_t> pipe_id) {
    checkPipeExists(pipe_id);
    return wan_pipe_map.find(pipe_id)->second;
}

void AIWANMsoft::checkSwitchExists(uint64_t switch_id) {
    if (wan_switch_map.find(switch_id) == wan_switch_map.end()) {
        cout << "AIWANMsoft::Node below does not exist!" << endl;
        print_bit_format(switch_id);
        abort();
    }
}

void AIWANMsoft::checkSwitchNotExists(uint64_t switch_id) {
    if (wan_switch_map.find(switch_id) != wan_switch_map.end()) {
        cout << "AIWANMsoft::Node below does already exists!" << endl;
        print_bit_format(switch_id);
        abort();
    }
}

void AIWANMsoft::create_regions() {
    struct AI_wan_topology_params params = {this->n_region,
                                            this->n_dc_per_region,
                                            this->n_spine_per_leaf_in_leafgroup,
                                            this->n_leafgroups_per_dc,
                                            this->n_leafs_per_leafgroup,
                                            this->n_torgroups_per_leafgroup,
                                            this->n_tor_per_torgroups,
                                            this->n_serv_per_torgroup,
                                            this->n_RH_groups,
                                            this->n_RH_switch_batches_per_RH_group,
                                            this->n_RH_switches_per_RH_batch,
                                            this->n_RWA_groups,
                                            this->n_RWA_switches_per_RWA_group,
                                            this->n_OWR_groups,
                                            this->n_OWR_switches_per_OWR_group};

    cout << "number of regions: " << get_num_region() << endl;

    for (unsigned int region_idx = 0; region_idx < get_num_region(); region_idx++) {
        checkRegionNotExists(region_idx);
        auto m_region = new AIRegionMsoft(region_idx,
                                          params,
                                          this->link_params.intra_dc_link_speed,
                                          this->link_params.inter_dc_link_speed,
                                          this->queue_params,
                                          this->_switch_drop_event_probs,
                                          this->_switch_random_drop_probs,
                                          this->_logger_factory,
                                          this->_eventlist);
        m_region->wan = this;
        this->num_total_servers_in_wan += m_region->get_num_total_servers_in_region();
        region_map.insert(make_pair(region_idx, m_region));
    }
}

void AIWANMsoft::init_network() {
    QueueLogger* queueLogger = nullptr;

    cout << "Creating WAN." << endl
         << "\t Number of regions: " << n_region << endl
         << "\t Num RWA groups: " << n_RWA_groups << endl
         << "\t Num RWA switches per RWA group: " << n_RWA_switches_per_RWA_group << endl
         << "\t Num OWR groups: " << n_OWR_groups << endl
         << "\t Num OWR switches per OWR group: " << n_OWR_switches_per_OWR_group << endl
         << endl;

    // creating regions
    create_regions();

    std::cout << "Creating WAN backbone switches!" << endl;
    create_switches();
    // allocate queues and pipes and connect end points
    std::cout << "Creating RWA's and OWR's queues and pipes!" << endl;
    allocate_RWA_queues_and_pipes(queueLogger);
    allocate_OWR_queues_and_pipes(queueLogger);
    allocate_inter_region_queues_and_pipes(queueLogger);
}

uint64_t AIWANMsoft::getRWASwitchId(uint64_t region_id,
                                    uint64_t rwa_group_id,
                                    uint64_t rwa_switch_idx) {
    // RWA

    /*
        <0,3>: type = RWA
        <4,7>: region id
        <8,11>: RWA Group ID
        <12,15>: RWA switch batch index
    */
    uint64_t switch_id = AISwitch::RWA;
    switch_id |= (uint64_t(region_id) << 4);
    switch_id |= (uint64_t(rwa_group_id) << 8);
    switch_id |= (uint64_t(rwa_switch_idx) << 12);
    return switch_id;
}

SwitchInfo AIWANMsoft::parseRWASwitchId(uint64_t switch_id) {
    SwitchInfo info;

    info.switch_type    = (switch_id & 0xF);        // Bits 0–3
    info.region_idx     = (switch_id >> 4) & 0xF;   // Bits 4–7
    info.rwa_group_idx  = (switch_id >> 8) & 0xF;   // Bits 8–11
    info.rwa_switch_idx = (switch_id >> 12) & 0xF;  // Bits 12–15

    if (info.switch_type != AISwitch::RWA) {
        cout << "Passed switch ID is not related to a RWA switch!" << endl;
        abort();
    }

    return info;
}

uint64_t AIWANMsoft::getOWRSwitchId(uint64_t region_id,
                                    uint64_t owr_group_id,
                                    uint64_t owr_switch_idx) {
    /*
        <0,3>: type = OWR
        <4,7>: region id
        <8,11>: OWR Group ID
        <12,15>: OWR switch index
    */
    uint64_t switch_id = AISwitch::OWR;
    switch_id |= (uint64_t(region_id) << 4);
    switch_id |= (uint64_t(owr_group_id) << 8);
    switch_id |= (uint64_t(owr_switch_idx) << 12);
    return switch_id;
}

SwitchInfo AIWANMsoft::parseOWRSwitchId(uint64_t switch_id) {
    SwitchInfo info;

    info.switch_type    = (switch_id & 0xF);        // Bits 0–3
    info.region_idx     = (switch_id >> 4) & 0xF;   // Bits 4–7
    info.owr_group_idx  = (switch_id >> 8) & 0xF;   // Bits 8–11
    info.owr_switch_idx = (switch_id >> 12) & 0xF;  // Bits 12–15

    if (info.switch_type != AISwitch::OWR) {
        cout << "Passed switch ID is not related to a OWR switch!" << endl;
        abort();
    }

    return info;
}

void AIWANMsoft::create_switches() {
    for (unsigned int region_idx = 0; region_idx < n_region; region_idx++) {
        // TODO: some "region" only has OWR switches and no RWA switches.
        for (unsigned int rwa_group_idx = 0; rwa_group_idx < n_RWA_groups; rwa_group_idx++) {
            for (unsigned int rwa_switch_idx = 0; rwa_switch_idx < n_RWA_switches_per_RWA_group;
                 rwa_switch_idx++) {
                uint64_t rwa_switch_id = getRWASwitchId(region_idx, rwa_group_idx, rwa_switch_idx);
                checkSwitchNotExists(rwa_switch_id);
                auto rwa_switch =
                    new AISwitch(*_eventlist,
                                 "Region" + ntoa(region_idx) + "-RWAGroup" + ntoa(rwa_group_idx) +
                                     "-RWASwitch" + ntoa(rwa_switch_idx),
                                 AISwitch::RWA,
                                 rwa_switch_id,
                                 _switch_latency,
                                 this,
                                 region_idx);
                wan_switch_map.insert(make_pair(rwa_switch_id, rwa_switch));
            }
        }
        for (unsigned int owr_group_idx = 0; owr_group_idx < n_OWR_groups; owr_group_idx++) {
            for (unsigned int owr_switch_idx = 0; owr_switch_idx < n_OWR_switches_per_OWR_group;
                 owr_switch_idx++) {
                uint64_t owr_switch_id = getOWRSwitchId(region_idx, owr_group_idx, owr_switch_idx);
                checkSwitchNotExists(owr_switch_id);
                auto owr_switch =
                    new AISwitch(*_eventlist,
                                 "Region" + ntoa(region_idx) + "-OWRGroup" + ntoa(owr_group_idx) +
                                     "-OWRSwitch" + ntoa(owr_switch_idx),
                                 AISwitch::OWR,
                                 owr_switch_id,
                                 _switch_latency,
                                 this,
                                 region_idx);
                wan_switch_map.insert(make_pair(owr_switch_id, owr_switch));
            }
        }
    }
}

unsigned int AIWANMsoft::get_RWA_group_id_mapped_to_RH_group(unsigned int rh_group_idx) {
    // RWA groups are mapped to RH groups in a 1:1 fashion
    if (rh_group_idx >= n_RH_groups) {
        cout << "RH group index " << rh_group_idx
             << " is out of bounds for the number of RH groups " << n_RH_groups << endl;
        abort();
    }
    return rh_group_idx;
}

unsigned int AIWANMsoft::get_OWR_group_id_mapped_to_RWA_group(unsigned int rwa_group_idx) {
    // OWR groups are mapped to RWA groups in a 1:1 fashion
    if (rwa_group_idx >= n_RWA_groups) {
        cout << "RWA group index " << rwa_group_idx
             << " is out of bounds for the number of RWA groups " << n_RWA_groups << endl;
        abort();
    }
    return rwa_group_idx;
}

BaseQueue* AIWANMsoft::alloc_owr_queue(QueueLogger*  queueLogger,
                                       linkspeed_bps speed,
                                       mem_b         queuesize,
                                       size_t        link_idx) {
    string link_down_filename = "";
    // TODO: currently just randomly choose a link down event file
    uint32_t idx_1 = random() % 2;
    uint32_t idx_2 = random() % 8;
    if (_use_link_down)
        link_down_filename = "scripts/msft_ai_link_events/" + to_string(idx_1) + "_" +
                             to_string(idx_2) + "_link_down_events.txt";
    switch (queue_params.inter_region_qt) {
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
                                                   _switch_drop_event_probs[AISwitch::OWR],
                                                   _switch_random_drop_probs[AISwitch::OWR]);
            //    (link_idx == 0 || link_idx == 1) ? _switch_drop_event_probs[AISwitch::OWR] : 0,
            //    (link_idx == 0 || link_idx == 1) ? _switch_random_drop_probs[AISwitch::OWR] : 0,
            //    (link_idx == 0 || link_idx == 1) ? link_down_filename : ""); // This is to create
            //    asymmetric drops and link flapping, and demonstrate EC benefits
            return q;
        }
        case LOSSLESS: {
            LosslessQueue* q = new LosslessQueue(
                speed, queuesize, *_eventlist, queueLogger, NULL, 0, 0, _enable_pfc);
            return q;
        }
        default:
            cout << "AIWANMsoft alloc_owr_queue: Unknown queue!" << endl;
            abort();
    }
}

BaseQueue* AIWANMsoft::alloc_queue(
    QueueLogger* queueLogger, linkspeed_bps speed, mem_b queuesize, AISwitch* sw, size_t link_idx) {
    switch (queue_params.inter_region_qt) {
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
                                                   _switch_drop_event_probs[sw->getType()],
                                                   _switch_random_drop_probs[sw->getType()]);
            //    link_idx == 0 ? _switch_drop_event_probs[sw->getType()] : 0,
            //    link_idx == 0 ? _switch_random_drop_probs[sw->getType()] : 0);
            return q;
        }
        case LOSSLESS: {
            LosslessQueue* q = new LosslessQueue(
                speed, queuesize, *_eventlist, queueLogger, NULL, 0, 0, _enable_pfc);
            return q;
        }
        default:
            cout << "AIWANMsoft alloc_queue: Unknown queue!" << endl;
            abort();
    }
}

void AIWANMsoft::allocate_rwa_rh_queues_and_pipes(QueueLogger* queueLogger,
                                                  unsigned int region_idx,
                                                  uint64_t     rwa_switch_id,
                                                  uint64_t     rh_switch_id,
                                                  unsigned int rwa_group_id,
                                                  unsigned int rh_group_idx,
                                                  unsigned int rh_batch_idx,
                                                  unsigned int rwa_switch_idx,
                                                  unsigned int rh_switch_idx) {
    checkSwitchExists(rwa_switch_id);
    auto rwa_switch = wan_switch_map.find(rwa_switch_id)->second;
    assert(rwa_switch != NULL);

    auto m_region = region_map.find(region_idx)->second;
    m_region->checkSwitchExists(rh_switch_id);
    auto rh_switch =
        region_map.find(region_idx)->second->region_switch_map.find(rh_switch_id)->second;
    assert(rh_switch != NULL);

    // RWA --> RH
    for (unsigned int link_idx = 0; link_idx < link_params.link_factor_rh_rwa; link_idx++) {
        tuple<uint64_t, uint64_t, uint64_t> rwa_to_rh_tuple =
            make_tuple(rwa_switch_id, rh_switch_id, link_idx);
        checkQueueNotExists(rwa_to_rh_tuple);
        checkPipeNotExists(rwa_to_rh_tuple);

        if (_logger_factory) {
            queueLogger = _logger_factory->createQueueLogger();
        } else {
            queueLogger = NULL;
        }
        auto rwa_to_rh_queue = alloc_queue(queueLogger,
                                           link_params.inter_dc_link_speed,
                                           queue_params.RWA_queuesize,
                                           static_cast<AISwitch*>(rwa_switch),
                                           link_idx);
        rwa_to_rh_queue->setName(
            "Queue--WAN-Region" + itoa(region_idx) + "-RWAGroup" + itoa(rwa_group_id) +
            "-RWASwitch" + itoa(rwa_switch_idx) + "->RHGroup" + itoa(rh_group_idx) + "-RHBatch" +
            itoa(rh_batch_idx) + "-RHSwitch" + itoa(rh_switch_idx) + "-Link" + itoa(link_idx));

        auto rwa_to_rh_pipe = new Pipe(hop_latency.regional_backbone_hop_latency, *_eventlist);
        rwa_to_rh_pipe->setName("Pipe--WAN-Region" + itoa(region_idx) + "-RWAGroup" +
                                itoa(rwa_group_id) + "-RWASwitch" + itoa(rwa_switch_idx) +
                                "->RHGroup" + itoa(rh_group_idx) + "-RHBatch" + itoa(rh_batch_idx) +
                                "-RHSwitch" + itoa(rh_switch_idx) + "-Link" + itoa(link_idx));
        wan_queue_map.insert(make_pair(rwa_to_rh_tuple, rwa_to_rh_queue));
        wan_pipe_map.insert(make_pair(rwa_to_rh_tuple, rwa_to_rh_pipe));

        // add ports
        rwa_switch->addPort(rwa_to_rh_queue);

        // add remote endpoints
        rwa_to_rh_queue->setRemoteEndpoint(rh_switch);
    }
}

void AIWANMsoft::allocate_rh_rwa_queues_and_pipes(QueueLogger* queueLogger,
                                                  unsigned int region_idx,
                                                  uint64_t     rh_switch_id,
                                                  uint64_t     rwa_switch_id,
                                                  unsigned int rh_group_idx,
                                                  unsigned int rwa_group_id,
                                                  unsigned int rh_batch_idx,
                                                  unsigned int rh_switch_idx,
                                                  unsigned int rwa_switch_idx) {
    auto m_region = region_map.find(region_idx)->second;
    m_region->checkSwitchExists(rh_switch_id);
    auto rh_switch =
        region_map.find(region_idx)->second->region_switch_map.find(rh_switch_id)->second;
    assert(rh_switch != NULL);

    checkSwitchExists(rwa_switch_id);
    auto rwa_switch = wan_switch_map.find(rwa_switch_id)->second;
    assert(rwa_switch != NULL);

    // RH --> RWA
    for (unsigned int link_idx = 0; link_idx < link_params.link_factor_rh_rwa; link_idx++) {
        tuple<uint64_t, uint64_t, uint64_t> rh_to_rwa_tuple =
            make_tuple(rh_switch_id, rwa_switch_id, link_idx);
        checkQueueNotExists(rh_to_rwa_tuple);
        checkPipeNotExists(rh_to_rwa_tuple);
        if (_logger_factory) {
            queueLogger = _logger_factory->createQueueLogger();
        } else {
            queueLogger = NULL;
        }
        auto rh_to_rwa_queue = alloc_queue(queueLogger,
                                           link_params.inter_dc_link_speed,
                                           queue_params.RH_queuesize,
                                           static_cast<AISwitch*>(rh_switch),
                                           link_idx);
        rh_to_rwa_queue->setName("Queue--WAN-Region" + itoa(region_idx) + "-RHGroup" +
                                 itoa(rh_group_idx) + "-RHBatch" + itoa(rh_batch_idx) + "RHSwitch" +
                                 itoa(rh_switch_idx) + "->RWAGroup" + itoa(rwa_group_id) +
                                 "-RWASwitch" + itoa(rwa_switch_idx) + "-Link" + itoa(link_idx));
        auto rh_to_rwa_pipe = new Pipe(hop_latency.regional_backbone_hop_latency, *_eventlist);
        rh_to_rwa_pipe->setName("Pipe--WAN-Region" + itoa(region_idx) + "-RHGroup" +
                                itoa(rh_group_idx) + "-RHBatch" + itoa(rh_batch_idx) + "RHSwitch" +
                                itoa(rh_switch_idx) + "->RWAGroup" + itoa(rwa_group_id) +
                                "-RWASwitch" + itoa(rwa_switch_idx) + "-Link" + itoa(link_idx));
        wan_queue_map.insert(make_pair(rh_to_rwa_tuple, rh_to_rwa_queue));
        wan_pipe_map.insert(make_pair(rh_to_rwa_tuple, rh_to_rwa_pipe));

        // add ports
        rh_switch->addPort(rh_to_rwa_queue);

        // add remote endpoints
        rh_to_rwa_queue->setRemoteEndpoint(rwa_switch);
    }
}

void AIWANMsoft::allocate_RWA_queues_and_pipes(QueueLogger* queueLogger) {
    // RWA <--> RH: All RHs in a RH group are connected to all RWAs in a RWA group, with a factor of
    // multiplied link numbers
    for (unsigned int region_idx = 0; region_idx < n_region; region_idx++) {
        checkRegionExists(region_idx);
        auto m_region = region_map.find(region_idx)->second;
        for (unsigned int rh_group_idx = 0; rh_group_idx < n_RH_groups; rh_group_idx++) {
            for (unsigned int rh_batch_idx = 0; rh_batch_idx < n_RH_switch_batches_per_RH_group;
                 rh_batch_idx++) {
                for (unsigned int rh_switch_idx = 0; rh_switch_idx < n_RH_switches_per_RH_batch;
                     rh_switch_idx++) {
                    uint64_t rh_switch_id = m_region->getRHSwitchId(
                        region_idx, rh_group_idx, rh_batch_idx, rh_switch_idx);
                    m_region->checkSwitchExists(rh_switch_id);

                    unsigned int rwa_group_id = get_RWA_group_id_mapped_to_RH_group(rh_group_idx);
                    for (unsigned int rwa_switch_idx = 0;
                         rwa_switch_idx < n_RWA_switches_per_RWA_group;
                         rwa_switch_idx++) {
                        uint64_t rwa_switch_id =
                            getRWASwitchId(region_idx, rwa_group_id, rwa_switch_idx);
                        checkSwitchExists(rwa_switch_id);

                        // RWA --> RH
                        allocate_rwa_rh_queues_and_pipes(queueLogger,
                                                         region_idx,
                                                         rwa_switch_id,
                                                         rh_switch_id,
                                                         rwa_group_id,
                                                         rh_group_idx,
                                                         rh_batch_idx,
                                                         rwa_switch_idx,
                                                         rh_switch_idx);

                        // RH --> RWA
                        allocate_rh_rwa_queues_and_pipes(queueLogger,
                                                         region_idx,
                                                         rh_switch_id,
                                                         rwa_switch_id,
                                                         rh_group_idx,
                                                         rwa_group_id,
                                                         rh_batch_idx,
                                                         rh_switch_idx,
                                                         rwa_switch_idx);
                    }
                }
            }
        }
    }
}

void AIWANMsoft::allocate_rwa_owr_queues_and_pipes(QueueLogger* queueLogger,
                                                   unsigned int region_idx,
                                                   uint64_t     rwa_switch_id,
                                                   uint64_t     owr_switch_id,
                                                   unsigned int rwa_group_idx,
                                                   unsigned int owr_group_id,
                                                   unsigned int rwa_switch_idx,
                                                   unsigned int owr_switch_idx) {
    checkSwitchExists(rwa_switch_id);
    auto rwa_switch = wan_switch_map.find(rwa_switch_id)->second;
    assert(rwa_switch != NULL);

    checkSwitchExists(owr_switch_id);
    auto owr_switch = wan_switch_map.find(owr_switch_id)->second;
    assert(rwa_switch != NULL);

    // RWA --> OWR
    for (unsigned int link_idx = 0; link_idx < link_params.link_factor_rwa_owr; link_idx++) {
        tuple<uint64_t, uint64_t, uint64_t> rwa_to_owr_tuple =
            make_tuple(rwa_switch_id, owr_switch_id, link_idx);
        checkQueueNotExists(rwa_to_owr_tuple);
        checkPipeNotExists(rwa_to_owr_tuple);

        if (_logger_factory) {
            queueLogger = _logger_factory->createQueueLogger();
        } else {
            queueLogger = NULL;
        }
        auto rwa_to_owr_queue = alloc_queue(queueLogger,
                                            link_params.inter_dc_link_speed,
                                            queue_params.RWA_queuesize,
                                            static_cast<AISwitch*>(rwa_switch),
                                            link_idx);

        rwa_to_owr_queue->setName("Queue--WAN-Region" + itoa(region_idx) + "-RWAGroup" +
                                  itoa(rwa_group_idx) + "-RWASwitch" + itoa(rwa_switch_idx) +
                                  "->OWRGroup" + itoa(owr_group_id) + "-OWRSwitch" +
                                  itoa(owr_switch_idx) + "-Link" + itoa(link_idx));
        auto rwa_to_owr_pipe = new Pipe(hop_latency.regional_backbone_hop_latency, *_eventlist);
        rwa_to_owr_pipe->setName("Pipe--WAN-Region" + itoa(region_idx) + "-RWAGroup" +
                                 itoa(rwa_group_idx) + "-RWASwitch" + itoa(rwa_switch_idx) +
                                 "->OWRGroup" + itoa(owr_group_id) + "-OWRSwitch" +
                                 itoa(owr_switch_idx) + "-Link" + itoa(link_idx));
        wan_queue_map.insert(make_pair(rwa_to_owr_tuple, rwa_to_owr_queue));
        wan_pipe_map.insert(make_pair(rwa_to_owr_tuple, rwa_to_owr_pipe));

        // add ports
        rwa_switch->addPort(rwa_to_owr_queue);

        // add remote endpoints
        rwa_to_owr_queue->setRemoteEndpoint(owr_switch);
    }
}

void AIWANMsoft::allocate_owr_rwa_queues_and_pipes(QueueLogger* queueLogger,
                                                   unsigned int region_idx,
                                                   uint64_t     owr_switch_id,
                                                   uint64_t     rwa_switch_id,
                                                   unsigned int owr_group_id,
                                                   unsigned int rwa_group_idx,
                                                   unsigned int owr_switch_idx,
                                                   unsigned int rwa_switch_idx) {
    checkSwitchExists(owr_switch_id);
    auto owr_switch = wan_switch_map.find(owr_switch_id)->second;
    assert(owr_switch != NULL);

    checkSwitchExists(rwa_switch_id);
    auto rwa_switch = wan_switch_map.find(rwa_switch_id)->second;
    assert(rwa_switch != NULL);

    // OWR --> RWA

    for (unsigned int link_idx = 0; link_idx < link_params.link_factor_rwa_owr; link_idx++) {
        tuple<uint64_t, uint64_t, uint64_t> owr_to_rwa_tuple =
            make_tuple(owr_switch_id, rwa_switch_id, link_idx);
        checkQueueNotExists(owr_to_rwa_tuple);
        checkPipeNotExists(owr_to_rwa_tuple);
        if (_logger_factory) {
            queueLogger = _logger_factory->createQueueLogger();
        } else {
            queueLogger = NULL;
        }
        auto owr_to_rwa_queue = alloc_queue(queueLogger,
                                            link_params.inter_dc_link_speed,
                                            queue_params.OWR_queuesize,
                                            static_cast<AISwitch*>(owr_switch),
                                            link_idx);
        owr_to_rwa_queue->setName("Queue--WAN-Region" + itoa(region_idx) + "-OWRGroup" +
                                  itoa(owr_group_id) + "-OWRSwitch" + itoa(owr_switch_idx) +
                                  "->RWAGroup" + itoa(rwa_group_idx) + "-RWASwitch" +
                                  itoa(rwa_switch_idx) + "-Link" + itoa(link_idx));
        auto owr_to_rwa_pipe = new Pipe(hop_latency.regional_backbone_hop_latency, *_eventlist);
        owr_to_rwa_pipe->setName("Pipe--WAN-Region" + itoa(region_idx) + "-OWRGroup" +
                                 itoa(owr_group_id) + "-OWRSwitch" + itoa(owr_switch_idx) +
                                 "->RWAGroup" + itoa(rwa_group_idx) + "-RWASwitch" +
                                 itoa(rwa_switch_idx) + "-Link" + itoa(link_idx));
        wan_queue_map.insert(make_pair(owr_to_rwa_tuple, owr_to_rwa_queue));
        wan_pipe_map.insert(make_pair(owr_to_rwa_tuple, owr_to_rwa_pipe));

        // add ports
        owr_switch->addPort(owr_to_rwa_queue);

        // add remote endpoints
        owr_to_rwa_queue->setRemoteEndpoint(rwa_switch);
    }
}

void AIWANMsoft::allocate_OWR_queues_and_pipes(QueueLogger* queueLogger) {
    // OWR <--> RWA: All RWAs in a RWA group are connected to all OWRs in an OWR group
    for (unsigned int region_idx = 0; region_idx < n_region; region_idx++) {
        checkRegionExists(region_idx);
        for (unsigned int rwa_group_idx = 0; rwa_group_idx < n_RWA_groups; rwa_group_idx++) {
            for (unsigned int rwa_switch_idx = 0; rwa_switch_idx < n_RWA_switches_per_RWA_group;
                 rwa_switch_idx++) {
                uint64_t rwa_switch_id = getRWASwitchId(region_idx, rwa_group_idx, rwa_switch_idx);

                unsigned int owr_group_id = get_OWR_group_id_mapped_to_RWA_group(rwa_group_idx);
                for (unsigned int owr_switch_idx = 0; owr_switch_idx < n_OWR_switches_per_OWR_group;
                     owr_switch_idx++) {
                    uint64_t owr_switch_id =
                        getOWRSwitchId(region_idx, owr_group_id, owr_switch_idx);

                    // RWA --> OWR
                    allocate_rwa_owr_queues_and_pipes(queueLogger,
                                                      region_idx,
                                                      rwa_switch_id,
                                                      owr_switch_id,
                                                      rwa_group_idx,
                                                      owr_group_id,
                                                      rwa_switch_idx,
                                                      owr_switch_idx);

                    // OWR --> RWA
                    allocate_owr_rwa_queues_and_pipes(queueLogger,
                                                      region_idx,
                                                      owr_switch_id,
                                                      rwa_switch_id,
                                                      owr_group_id,
                                                      rwa_group_idx,
                                                      owr_switch_idx,
                                                      rwa_switch_idx);
                }
            }
        }
    }
}

string AIWANMsoft::getOWROWRQueueName(unsigned int region_idx1,
                                      unsigned int owr_group_idx1,
                                      unsigned int region_idx2,
                                      unsigned int owr_group_idx2,
                                      unsigned int owr_switch_idx,
                                      unsigned int link_idx) {
    return "Queue--WAN-Region" + itoa(region_idx1) + "-OWRGroup" + itoa(owr_group_idx1) +
           "-OWRSwitch" + itoa(owr_switch_idx) + "->Region" + itoa(region_idx2) + "-OWRGroup" +
           itoa(owr_group_idx2) + "-OWRSwitch" + itoa(owr_switch_idx) + "-Link" + itoa(link_idx);
}

string AIWANMsoft::getOWROWRPipeName(unsigned int region_idx1,
                                     unsigned int owr_group_idx1,
                                     unsigned int region_idx2,
                                     unsigned int owr_group_idx2,
                                     unsigned int owr_switch_idx,
                                     unsigned int link_idx) {
    return "Pipe--WAN-Region" + itoa(region_idx1) + "-OWRGroup" + itoa(owr_group_idx1) +
           "-OWRSwitch" + itoa(owr_switch_idx) + "->Region" + itoa(region_idx2) + "-OWRGroup" +
           itoa(owr_group_idx2) + "-OWRSwitch" + itoa(owr_switch_idx) + "-Link" + itoa(link_idx);
}

void AIWANMsoft::allocate_owr_owr_queues_and_pipes(QueueLogger* queueLogger,
                                                   uint64_t     owr_switch_id1,
                                                   uint64_t     owr_switch_id2,
                                                   unsigned int region_idx1,
                                                   unsigned int region_idx2,
                                                   unsigned int owr_group_idx,
                                                   unsigned int owr_switch_idx) {
    checkSwitchExists(owr_switch_id1);
    checkSwitchExists(owr_switch_id2);
    auto owr_switch1 = wan_switch_map.find(owr_switch_id1)->second;
    auto owr_switch2 = wan_switch_map.find(owr_switch_id2)->second;

    for (unsigned int link_idx = 0;
         link_idx < link_params.link_factor_regional[region_idx1][region_idx2];
         link_idx++) {
        tuple<uint64_t, uint64_t, uint64_t> owr1_to_owr2_tuple =
            make_tuple(owr_switch_id1, owr_switch_id2, link_idx);
        checkQueueNotExists(owr1_to_owr2_tuple);
        checkPipeNotExists(owr1_to_owr2_tuple);

        if (_logger_factory) {
            queueLogger = _logger_factory->createQueueLogger();
        } else {
            queueLogger = NULL;
        }
        auto owr1_to_owr2_queue = alloc_owr_queue(
            queueLogger, link_params.inter_dc_link_speed, queue_params.OWR_queuesize, link_idx);
        owr1_to_owr2_queue->setName(getOWROWRQueueName(
            region_idx1, owr_group_idx, region_idx2, owr_group_idx, owr_switch_idx, link_idx));
        auto owr1_to_owr2_pipe = new Pipe(
            hop_latency.inter_owr_hop_latency[link_idx % hop_latency.inter_owr_hop_latency.size()],
            *_eventlist);
        owr1_to_owr2_pipe->setName(getOWROWRPipeName(
            region_idx1, owr_group_idx, region_idx2, owr_group_idx, owr_switch_idx, link_idx));

        wan_queue_map.insert(make_pair(owr1_to_owr2_tuple, owr1_to_owr2_queue));
        wan_pipe_map.insert(make_pair(owr1_to_owr2_tuple, owr1_to_owr2_pipe));

        // add ports
        owr_switch1->addPort(owr1_to_owr2_queue);

        // add remote endpoints
        owr1_to_owr2_queue->setRemoteEndpoint(owr_switch2);
    }
}

void AIWANMsoft::allocate_inter_region_queues_and_pipes(QueueLogger* queueLogger) {
    // OWR <--> OWR: 1-1 mapping, representing the WAN backbone connection between regions.
    // TODO: currently fixed to all OWRs connected directly, with a special case of two OWRs
    // connected directly for current testing
    for (unsigned int region_idx1 = 0; region_idx1 < n_region; region_idx1++) {
        checkRegionExists(region_idx1);
        for (unsigned int region_idx2 = region_idx1 + 1; region_idx2 < n_region; region_idx2++) {
            checkRegionExists(region_idx2);

            for (unsigned int owr_group_idx = 0; owr_group_idx < n_OWR_groups; owr_group_idx++) {
                for (unsigned int owr_switch_idx = 0; owr_switch_idx < n_OWR_switches_per_OWR_group;
                     owr_switch_idx++) {
                    uint64_t owr_switch_id1 =
                        getOWRSwitchId(region_idx1, owr_group_idx, owr_switch_idx);
                    uint64_t owr_switch_id2 =
                        getOWRSwitchId(region_idx2, owr_group_idx, owr_switch_idx);

                    // OWR1 <--> OWR2
                    allocate_owr_owr_queues_and_pipes(queueLogger,
                                                      owr_switch_id1,
                                                      owr_switch_id2,
                                                      region_idx1,
                                                      region_idx2,
                                                      owr_group_idx,
                                                      owr_switch_idx);
                    // OWR2 --> OWR1
                    allocate_owr_owr_queues_and_pipes(queueLogger,
                                                      owr_switch_id2,
                                                      owr_switch_id1,
                                                      region_idx2,
                                                      region_idx1,
                                                      owr_group_idx,
                                                      owr_switch_idx);
                }
            }
        }
    }
}

vector<const Route*>* AIWANMsoft::get_bidir_paths(uint32_t src, uint32_t dest, bool reverse) {
    return NULL;
}

void AIWANMsoft::add_switch_loggers(Logfile& log, simtime_picosec sample_period) {
    return;
}

void AIWANMsoft::set_switch_hash_salt(uint32_t salt) {
    // OWR and RWA switches
    for (auto& switch_pair : wan_switch_map) {
        AISwitch* ai_sw = dynamic_cast<AISwitch*>(switch_pair.second);
        assert(ai_sw != nullptr && "RH Switch is not actually an AISwitch");
        ai_sw->set_hash_salt(salt);
    }

    // RH and DC switches
    for (auto& region_pair : region_map) {
        AIRegionMsoft* region = region_pair.second;
        region->set_switch_hash_salt(salt);
    }
}

map<uint64_t, int64_t> AIWANMsoft::get_switch_counters() {
    map<uint64_t, int64_t> counter_map;  // <switchId, switch receive packet counter>

    // Iterate through all switches in the WAN and get their receive packet counters

    // OWR and RWA switches
    for (auto& switch_pair : wan_switch_map) {
        AISwitch* ai_sw = dynamic_cast<AISwitch*>(switch_pair.second);
        assert(ai_sw != nullptr && "This Switch is not actually an AISwitch");
        int64_t counter = ai_sw->get_receive_packet_counter();
        counter_map.insert(make_pair(switch_pair.first, counter));
    }

    // RH and DC switches
    for (auto& region_pair : region_map) {
        AIRegionMsoft* region            = region_pair.second;
        auto           other_counter_map = region->get_switch_counters();
        counter_map.insert(other_counter_map.begin(), other_counter_map.end());
    }

    return counter_map;
}