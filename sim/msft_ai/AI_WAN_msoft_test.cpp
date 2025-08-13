// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "AI_WAN_msoft.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>

#include "AI_WAN_helper.h"
#include "AI_WAN_test_helper.h"
#include "AI_WAN_types.h"
#include "AI_switch.h"
#include "connection_matrix.h"
#include "helpers.h"
#include "kecbr.h"
#include "packet.h"
#include "packet_flow.h"

class AIWANMsoftTest : public ::testing::Test {
protected:
    // Simulation params
    struct link_params  link_params;
    struct queue_params queue_params;

    struct hop_latency_params hop_latency;
    vector<double>            switch_drop_event_probs  = {0, 0, 0, 0, 0, 0, 0};
    vector<double>            switch_random_drop_probs = {0, 0, 0, 0, 0, 0, 0};

    simtime_picosec switch_latency = timeFromNs((uint32_t)0);

    // WAN topology parmas
    struct AI_wan_topology_params AI_WAN_topo_params = {
        .n_region        = 2,  // number of regions
        .n_dc_per_region = 2,  // number of DCs per region
        .n_spine_per_leaf_in_leafgroup =
            8,  // number of Spines connected to each leaf in a leaf group
        .n_leafgroups_per_dc              = 2,  // number of groups of leafs in each DC
        .n_leafs_per_leafgroup            = 8,  // number of Leafs per leafgroup
        .n_torgroups_per_leafgroup        = 8,  // number of ToR groups in each leaf group
        .n_tor_per_torgroups              = 2,  // number of ToR switches in each ToR group
        .n_serv_per_torgroup              = 2,  // number of servers per ToR group
        .n_RH_groups                      = 2,  // number of region hub groups
        .n_RH_switch_batches_per_RH_group = 2,
        .n_RH_switches_per_RH_batch       = 8,
        .n_RWA_groups                     = 2,  // number of RWA groups
        .n_RWA_switches_per_RWA_group     = 4,  // number of RWA switches in each RWA group
        .n_OWR_groups                     = 2,  // number of OWR groups
        .n_OWR_switches_per_OWR_group     = 4,  // number of OWR switches in each OWR group
    };

    mem_b inter_queuesize = INFINITE_BUFFER_SIZE;
    mem_b intra_queuesize = INFINITE_BUFFER_SIZE;

    EventList eventlist;
    double    def_end_time = 5000.0;

    AIWANMsoft*                topo_AI                    = nullptr;
    uint32_t                   AI_switch_hash_salt        = 1;
    AISwitch::routing_strategy AI_switch_routing_strategy = AISwitch::ECMP;

    // Connection Matrix params
    ConnectionMatrix* conns       = nullptr;
    uint32_t          no_of_nodes = 0;
    const char*       tm_file     = "../sim/msft_ai/testdata/1_routing_WAN.cm";

    // host params
    // CBR
    uint32_t      cbr_src_hash_salt = 0;
    uint32_t      cbr_dst_hash_salt = 0;
    RouteStrategy route_strategy    = SINGLE_PATH;
    uint32_t      cbr_path_id       = 0;

    map<pair<int, int>, flowid_t> flow_id_map;

    virtual void SetUp() {
        link_params.inter_dc_link_speed = speedFromGbps((double)400);
        link_params.intra_dc_link_speed = speedFromMbps((double)HOST_NIC);
        link_params.link_factor_rh_rwa  = 12;
        link_params.link_factor_rwa_owr = 20;
        link_params.link_factor_regional.resize(AI_WAN_topo_params.n_region);
        for (unsigned int i = 0; i < AI_WAN_topo_params.n_region; i++) {
            link_params.link_factor_regional[i].resize(AI_WAN_topo_params.n_region);
            for (unsigned int j = 0; j < AI_WAN_topo_params.n_region; j++) {
                link_params.link_factor_regional[i][j] = 30;
            }
        }

        hop_latency.regional_backbone_hop_latency = timeFromUs((uint32_t)100);
        hop_latency.intra_region_hop_latency      = timeFromUs((uint32_t)1);
        hop_latency.inter_owr_hop_latency         = {timeFromUs((uint32_t)25000)};  // PHX-SN

        eventlist.setEndtime(timeFromMs(def_end_time));

        topo_AI = new AIWANMsoft(AI_WAN_topo_params,
                                 link_params,
                                 queue_params,
                                 NULL,
                                 &eventlist,
                                 hop_latency.inter_owr_hop_latency,
                                 switch_drop_event_probs,
                                 switch_random_drop_probs);

        topo_AI->set_switch_hash_salt(AI_switch_hash_salt);

        no_of_nodes = AI_WAN_topo_params.get_no_of_nodes_in_WAN();

        conns = new ConnectionMatrix(no_of_nodes);

        AISwitch::set_strategy(AI_switch_routing_strategy);
        KECbrSrc::setRouteStrategy(route_strategy);
        KECbrSink::setRouteStrategy(route_strategy);
    }

    virtual void TearDown() {}

    map<uint64_t, int64_t>      generate_switch_counter_answers(vector<connection*>*          all_conns,
                                                                AIWANMsoft*                   topo_AI,
                                                                struct AI_wan_topology_params params);
    vector<AISwitch::node_type> get_switch_type_path(AIWANMsoft* topo_AI, int src, int dest);

    std::vector<uint64_t> get_spines_connected_to_rh(AIRegionMsoft*                region,
                                                     struct AI_wan_topology_params params,
                                                     unsigned int                  target_dc_idx,
                                                     unsigned int                  rh_batch_idx,
                                                     unsigned int                  rh_group_idx,
                                                     unsigned int                  rh_switch_idx);
};

vector<AISwitch::node_type> AIWANMsoftTest::get_switch_type_path(AIWANMsoft* topo_AI,
                                                                 int         src,
                                                                 int         dest) {
    return get_wan_switch_type_path(topo_AI, src, dest);
}

map<uint64_t, int64_t> AIWANMsoftTest::generate_switch_counter_answers(
    vector<connection*>* all_conns, AIWANMsoft* topo_AI, struct AI_wan_topology_params params) {
    auto switch_counters = init_WAN_switch_counters(topo_AI, params);

    cout << "Number of switches in the topology: " << switch_counters.size() << endl;

    for (size_t c = 0; c < all_conns->size(); c++) {
        connection* crt          = all_conns->at(c);
        int         src          = crt->src;
        int         dest         = crt->dst;
        flowid_t    this_flow_id = flow_id_map.at(make_pair(src, dest));

        auto switch_path = get_switch_type_path(topo_AI, src, dest);

        auto [cur_topo_idx, cur_region, cur_dc]    = get_AI_WAN_topology_idx(topo_AI, src);
        auto [dest_topo_idx, dest_region, dest_dc] = get_AI_WAN_topology_idx(topo_AI, dest);

        update_wan_switch_counters(switch_path,
                                   this_flow_id,
                                   cur_topo_idx,
                                   dest_topo_idx,
                                   cur_region,
                                   dest_region,
                                   cur_dc,
                                   dest_dc,
                                   switch_counters,
                                   topo_AI,
                                   AI_WAN_topo_params,
                                   link_params,
                                   cbr_src_hash_salt,
                                   cbr_path_id,
                                   AI_switch_hash_salt);
    }

    return switch_counters;
}

TEST_F(AIWANMsoftTest, RoutingTest) {
    ASSERT_NE(conns, nullptr) << "Failed to create ConnectionMatrix";
    ASSERT_NE(tm_file, nullptr) << "Traffic matrix file is not set";
    ASSERT_TRUE(conns->load(tm_file)) << "Failed to load connection matrix " << tm_file;
    ASSERT_EQ(conns->N, no_of_nodes)
        << "Connection matrix size does not match expected number of nodes";

    map<flowid_t, EventSource*>  flowmap;
    vector<connection*>*         all_conns = conns->getAllConnections();
    vector<KECbrSrc*>            inter_srcs;
    KECbrSrc*                    cbrSrc;
    KECbrSink*                   cbrSink;
    KECbrSrcRestartTimerScanner  cbrSrcRestartScanner(timeFromMs(14 * 256), eventlist);
    KECbrSinkRestartTimerScanner cbrSinkRestartScanner(timeFromMs(10), eventlist);

    ofstream statistics_outfile("output_statistics.txt");

    for (size_t c = 0; c < all_conns->size(); c++) {
        connection* crt  = all_conns->at(c);
        int         src  = crt->src;
        int         dest = crt->dst;

        cbrSrc = new KECbrSrc(eventlist, speedFromPktps(7999), statistics_outfile, Packet::PRIO_LO);
        cbrSink = new KECbrSink(eventlist);

        if (crt->flowid) {
            cbrSrc->set_flowid(crt->flowid);
            ASSERT_TRUE(flowmap.find(crt->flowid) == flowmap.end());  // don't have dups
            flowmap[crt->flowid] = cbrSrc;
        }

        if (crt->size > 0) {
            cbrSrc->setFlowSize(crt->size);
        }

        cbrSrc->set_hash_salt(cbr_src_hash_salt);   // Use fixed hash salt for unit test
        cbrSink->set_hash_salt(cbr_dst_hash_salt);  // Use fixed hash salt for unit test

        cbrSrc->set_dst(dest);
        cbrSink->set_src(src);

        cbrSrcRestartScanner.registerCbr(*cbrSrc);
        cbrSinkRestartScanner.registerCbr(*cbrSink);

        inter_srcs.push_back(cbrSrc);

        switch (route_strategy) {
            case ECMP_FIB:
            case ECMP_FIB_ECN:
            case ECMP_RANDOM2_ECN:
            case SINGLE_PATH:
            case SCATTER_RANDOM:
            case SIMPLE_SUBFLOW:
            case ECMP_RANDOM_ECN:
            case REACTIVE_ECN: {
                Route* srctotor = new Route();
                Route* dsttotor = new Route();

                // Create src and snk routes to ToR in AI region
                AI_WAN_create_route(topo_AI, src, cbrSrc->_hash_salt, srctotor, cbrSrc->flow_id());
                AI_WAN_create_route(
                    topo_AI, dest, cbrSink->_hash_salt, dsttotor, cbrSrc->flow_id());

                // printf("Creating 1 Flow from %d to %d\n", src, dest);
                cbrSrc->connect(srctotor, dsttotor, *cbrSink, crt->start);

                // register src and snk to the topo_AI to receive packets src their respective ToRs
                AI_WAN_register_host(topo_AI, src, cbrSrc, cbrSrc->flow_id());
                AI_WAN_register_host(topo_AI, dest, cbrSink, cbrSrc->flow_id());
                flow_id_map[make_pair(src, dest)] = cbrSrc->flow_id();

                break;
            }
            case NOT_SET: {
                FAIL() << "Route strategy NOT_SET";
                break;
            }
            default: {
                FAIL() << "Unknown Route strategy";
                break;
            }
        }
    }

    cout << "\nEntering the event running loop" << endl;
    while (eventlist.doNextEvent()) {
    }

    cout << "Event running loop done" << endl;

    auto switch_counters = topo_AI->get_switch_counters();
    auto ground_truth_counters =
        generate_switch_counter_answers(all_conns, topo_AI, AI_WAN_topo_params);

    ASSERT_EQ(switch_counters.size(), ground_truth_counters.size())
        << "Switch size does not match expected number of switches";

    ASSERT_EQ(switch_counters, ground_truth_counters)
        << "Switch counters do not match expected values";

    for (std::size_t i = 0; i < inter_srcs.size(); ++i) {
        delete inter_srcs[i];
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}