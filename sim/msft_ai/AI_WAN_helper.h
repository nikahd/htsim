#ifndef AI_WAN_HELPER_H
#define AI_WAN_HELPER_H

#include "AI_DC_msoft.h"
#include "AI_WAN_types.h"
#include "AI_region_msoft.h"
#include "network.h"

tuple<struct AI_WAN_topology_idx, AIDCMsoft*> get_AI_region_topology_idx(AIRegionMsoft* topo_AI,
                                                                         int            srv);
tuple<struct AI_WAN_topology_idx, AIRegionMsoft*, AIDCMsoft*> get_AI_WAN_topology_idx(
    AIWANMsoft* topo_AI, int srv);
void AI_region_register_host(AIRegionMsoft* topo_AI, int srv, PacketSink* transport, int flowid);
void AI_region_create_route(
    AIRegionMsoft* topo_AI, int srv, uint32_t srv_hash_salt, Route* srvtotor, int flowid);
void AI_WAN_create_route(
    AIWANMsoft* topo_AI, int srv, uint32_t srv_hash_salt, Route* srvtotor, int flowid);
void AI_WAN_register_host(AIWANMsoft* topo_AI, int srv, PacketSink* transport, int flowid);

tuple<struct AI_WAN_topology_idx, AIDCMsoft*> get_AI_region_topology_idx(AIRegionMsoft* topo_AI,
                                                                         int            srv) {
    assert(topo_AI);
    auto         srv_dc             = topo_AI->get_dc_for_server(srv);
    unsigned int relative_srv_idx   = srv_dc->get_relative_server_idx(srv);
    unsigned int srv_tor_group_idx  = srv_dc->get_tor_group_idx(srv);
    unsigned int srv_leaf_group_idx = srv_dc->get_leaf_group_idx(srv);
    unsigned int srv_dc_idx         = srv_dc->get_dc_idx(srv);
    unsigned int srv_region_idx     = srv_dc->get_region_idx(srv);
    return make_tuple(
        AI_WAN_topology_idx(
            srv_region_idx, srv_dc_idx, srv_leaf_group_idx, srv_tor_group_idx, relative_srv_idx),
        srv_dc);

    return make_tuple(AI_WAN_topology_idx(-1, -1, -1, -1, -1),
                      nullptr);  // Default values if topo_AI is NULL
}

void AI_region_register_host(AIRegionMsoft* topo_AI, int srv, PacketSink* transport, int flowid) {
    // register srv and snk to receive packets srv their respective
    // TORs.
    assert(topo_AI);
    auto [srv_topo_idx, srv_dc] = get_AI_region_topology_idx(topo_AI, srv);

    for (unsigned int tor_idx = 0; tor_idx < srv_dc->get_num_tor_per_torgroups(); tor_idx++) {
        uint64_t srv_tor_switch_id = srv_dc->getToRSwitchId(srv_topo_idx.region_idx,
                                                            srv_topo_idx.dc_idx,
                                                            srv_topo_idx.leaf_group_idx,
                                                            srv_topo_idx.tor_group_idx,
                                                            tor_idx);
        srv_dc->checkSwitchExists(srv_tor_switch_id);

        auto srv_tor_switch_itr = srv_dc->switch_map.find(srv_tor_switch_id);
        // TODO(vtlam): if lcpsrv represents a "flow", are we creating switch host
        // port per flow?
        srv_tor_switch_itr->second->addHostPort(srv_topo_idx.relative_srv_idx, flowid, transport);
    }
}

void AI_region_create_route(
    AIRegionMsoft* topo_AI, int srv, uint32_t srv_hash_salt, Route* srvtotor, int flowid) {
    unsigned int srv_ecmp_choice = 0;

    assert(topo_AI);
    auto [srv_topo_idx, srv_dc] = get_AI_region_topology_idx(topo_AI, srv);

    unsigned int ecmp_choice = mFreeBSDHash(flowid, srv_hash_salt);
    srv_ecmp_choice          = (ecmp_choice % srv_dc->get_num_tor_per_torgroups());

    // TODO: here we are adding multiple routes but only one is always used at index
    // 0
    for (unsigned int tor_idx = 0; tor_idx < srv_dc->get_num_tor_per_torgroups(); tor_idx++) {
        if (tor_idx != srv_ecmp_choice)
            continue;

        uint64_t srv_tor_switch_id = srv_dc->getToRSwitchId(srv_topo_idx.region_idx,
                                                            srv_topo_idx.dc_idx,
                                                            srv_topo_idx.leaf_group_idx,
                                                            srv_topo_idx.tor_group_idx,
                                                            tor_idx);
        srv_dc->checkSwitchExists(srv_tor_switch_id);
        uint64_t srv_server_id = srv_dc->getServerId(srv_topo_idx.relative_srv_idx);

        // for now assume one link from server to each tor
        tuple<uint64_t, uint64_t, uint64_t, uint64_t> srv_to_tor_tuple =
            make_tuple(srv_server_id, 0, srv_tor_switch_id, 0);
        srv_dc->checkQueueExists(srv_to_tor_tuple);
        srv_dc->checkPipeExists(srv_to_tor_tuple);
        auto srv_queue_itr = srv_dc->queue_map.find(srv_to_tor_tuple);
        auto srv_pipe_itr  = srv_dc->pipe_map.find(srv_to_tor_tuple);
        srvtotor->push_back(srv_queue_itr->second);
        srvtotor->push_back(srv_pipe_itr->second);
        srvtotor->push_back(srv_queue_itr->second->getRemoteEndpoint());
    }
}

tuple<struct AI_WAN_topology_idx, AIRegionMsoft*, AIDCMsoft*> get_AI_WAN_topology_idx(
    AIWANMsoft* topo_AI, int srv) {
    assert(topo_AI);
    auto srv_region = topo_AI->get_region_for_server(srv);
    assert(srv_region != nullptr);
    unsigned int srv_region_idx = srv_region->get_region_idx();
    auto         srv_dc         = srv_region->get_dc_for_server(srv);
    assert(srv_dc != nullptr);
    unsigned int srv_tor_group_idx  = srv_dc->get_tor_group_idx(srv);
    unsigned int srv_leaf_group_idx = srv_dc->get_leaf_group_idx(srv);
    unsigned int srv_dc_idx         = srv_dc->get_dc_idx(srv);

    unsigned int relative_srv_idx = srv_dc->get_relative_server_idx(srv);  // relative to the region

    return make_tuple(
        AI_WAN_topology_idx(
            srv_region_idx, srv_dc_idx, srv_leaf_group_idx, srv_tor_group_idx, relative_srv_idx),
        srv_region,
        srv_dc);

    return make_tuple(AI_WAN_topology_idx(-1, -1, -1, -1, -1),
                      nullptr,
                      nullptr);  // Default values if topo_AI is NULL
}

void AI_WAN_create_route(
    AIWANMsoft* topo_AI, int srv, uint32_t srv_hash_salt, Route* srvtotor, int flowid) {
    unsigned int srv_ecmp_choice = 0;

    assert(topo_AI);
    auto [srv_topo_idx, srv_region, srv_dc] = get_AI_WAN_topology_idx(topo_AI, srv);

    unsigned int ecmp_choice = mFreeBSDHash(flowid, srv_hash_salt);
    srv_ecmp_choice          = (ecmp_choice % srv_dc->get_num_tor_per_torgroups());

    // TODO: here we are adding multiple routes but only one is always used at index
    // 0
    for (unsigned int tor_idx = 0; tor_idx < srv_dc->get_num_tor_per_torgroups(); tor_idx++) {
        if (tor_idx != srv_ecmp_choice)
            continue;

        uint64_t srv_tor_switch_id = srv_dc->getToRSwitchId(srv_topo_idx.region_idx,
                                                            srv_topo_idx.dc_idx,
                                                            srv_topo_idx.leaf_group_idx,
                                                            srv_topo_idx.tor_group_idx,
                                                            tor_idx);
        srv_dc->checkSwitchExists(srv_tor_switch_id);
        uint64_t srv_server_id = srv_dc->getServerId(srv_topo_idx.relative_srv_idx);

        // for now assume one link from server to each tor
        tuple<uint64_t, uint64_t, uint64_t, uint64_t> srv_to_tor_tuple =
            make_tuple(srv_server_id, 0, srv_tor_switch_id, 0);
        srv_dc->checkQueueExists(srv_to_tor_tuple);
        srv_dc->checkPipeExists(srv_to_tor_tuple);
        auto srv_queue_itr = srv_dc->queue_map.find(srv_to_tor_tuple);
        auto srv_pipe_itr  = srv_dc->pipe_map.find(srv_to_tor_tuple);
        srvtotor->push_back(srv_queue_itr->second);
        srvtotor->push_back(srv_pipe_itr->second);
        srvtotor->push_back(srv_queue_itr->second->getRemoteEndpoint());
    }
}

void AI_WAN_register_host(AIWANMsoft* topo_AI, int srv, PacketSink* transport, int flowid) {
    // register srv and snk to receive packets srv their respective
    // TORs.
    assert(topo_AI);
    auto [srv_topo_idx, srv_region, srv_dc] = get_AI_WAN_topology_idx(topo_AI, srv);

    for (unsigned int tor_idx = 0; tor_idx < srv_dc->get_num_tor_per_torgroups(); tor_idx++) {
        uint64_t srv_tor_switch_id = srv_dc->getToRSwitchId(srv_topo_idx.region_idx,
                                                            srv_topo_idx.dc_idx,
                                                            srv_topo_idx.leaf_group_idx,
                                                            srv_topo_idx.tor_group_idx,
                                                            tor_idx);
        srv_dc->checkSwitchExists(srv_tor_switch_id);

        auto srv_tor_switch_itr = srv_dc->switch_map.find(srv_tor_switch_id);
        // TODO(vtlam): if lcpsrv represents a "flow", are we creating switch host
        // port per flow?
        srv_tor_switch_itr->second->addHostPort(srv_topo_idx.relative_srv_idx, flowid, transport);
    }
}

#endif  // AI_WAN_HELPER_H