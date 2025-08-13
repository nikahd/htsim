#pragma once

#include "AI_simple_wan.h"
#include "network.h"

struct simple_wan_topology_idx {
    unsigned int region_idx;     // DC leaf group idx
    unsigned int tor_group_idx;  // TOR group idx
    unsigned int relative_srv_idx;

    simple_wan_topology_idx(unsigned int r, unsigned int t, unsigned int s)
        : region_idx(r), tor_group_idx(t), relative_srv_idx(s) {}
};

struct simple_wan_topology_idx get_simple_wan_topology_idx(SimpleWAN* topo, int srv);
void simple_wan_register_host(SimpleWAN* topo, int srv, PacketSink* transport, int flowid);
void simple_wan_create_route(
    SimpleWAN* topo, int srv, uint32_t srv_hash_salt, Route* srvtotor, int flowid);

struct simple_wan_topology_idx get_simple_wan_topology_idx(SimpleWAN* topo, int srv) {
    assert(topo != NULL);
    unsigned int relative_srv_idx  = topo->get_relative_server_idx(srv);
    unsigned int srv_tor_group_idx = topo->get_tor_group_idx(srv);
    unsigned int srv_region_idx    = topo->get_leaf_group_idx(srv);
    return simple_wan_topology_idx(srv_region_idx, srv_tor_group_idx, relative_srv_idx);
}

void simple_wan_register_host(SimpleWAN* topo, int srv, PacketSink* transport, int flowid) {
    // register srv and snk to receive packets srv their respective
    // TORs.
    assert(topo != NULL);
    auto srv_topo_idx = get_simple_wan_topology_idx(topo, srv);

    assert(topo->get_num_tor_per_torgroups() == 1);
    for (unsigned int tor_idx = 0; tor_idx < topo->get_num_tor_per_torgroups(); tor_idx++) {
        uint64_t srv_tor_switch_id =
            topo->getToRSwitchId(0, 0, srv_topo_idx.region_idx, srv_topo_idx.tor_group_idx, 0);
        topo->checkSwitchExists(srv_tor_switch_id);

        auto srv_tor_switch_itr = topo->switch_map.find(srv_tor_switch_id);
        // TODO(vtlam): if lcpsrv represents a "flow", are we creating switch host
        // port per flow?

        srv_tor_switch_itr->second->addHostPort(srv_topo_idx.relative_srv_idx, flowid, transport);
    }
}

void simple_wan_create_route(
    SimpleWAN* topo, int srv, uint32_t srv_hash_salt, Route* srvtotor, int flowid) {
    unsigned int srv_ecmp_choice = 0;

    assert(topo != NULL);
    auto srv_topo_idx = get_simple_wan_topology_idx(topo, srv);

    unsigned int ecmp_choice = mFreeBSDHash(flowid, srv_hash_salt);
    srv_ecmp_choice          = (ecmp_choice % topo->get_num_tor_per_torgroups());

    // TODO: here we are adding multiple routes but only one is always used at index
    // 0
    assert(topo->get_num_tor_per_torgroups() == 1);
    for (unsigned int tor_idx = 0; tor_idx < topo->get_num_tor_per_torgroups(); tor_idx++) {
        if (tor_idx != srv_ecmp_choice)
            continue;

        uint64_t srv_tor_switch_id =
            topo->getToRSwitchId(0, 0, srv_topo_idx.region_idx, srv_topo_idx.tor_group_idx, 0);
        topo->checkSwitchExists(srv_tor_switch_id);
        uint64_t srv_server_id = topo->getServerId(srv_topo_idx.relative_srv_idx);

        // for now assume one link from server to each tor
        tuple<uint64_t, uint64_t, uint64_t, uint64_t> srv_to_tor_tuple =
            make_tuple(srv_server_id, 0, srv_tor_switch_id, 0);
        topo->checkQueueExists(srv_to_tor_tuple);
        topo->checkPipeExists(srv_to_tor_tuple);
        auto srv_queue_itr = topo->queue_map.find(srv_to_tor_tuple);
        auto srv_pipe_itr  = topo->pipe_map.find(srv_to_tor_tuple);
        srvtotor->push_back(srv_queue_itr->second);
        srvtotor->push_back(srv_pipe_itr->second);
        srvtotor->push_back(srv_queue_itr->second->getRemoteEndpoint());
    }
}