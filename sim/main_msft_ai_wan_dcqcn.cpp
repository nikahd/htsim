// -*- c-basic-offset: 4; tab-width: 8; indent-tabs-mode: t -*-
#include <math.h>
#include <string.h>

#include <filesystem>
#include <iostream>
#include <sstream>

#include "AI_WAN_types.h"
#include "compositequeue.h"
#include "config.h"
#include "connection_matrix.h"
#include "dcqcn.h"
#include "dcqcn_logger.h"
#include "eventlist.h"
#include "logfile.h"
#include "loggers.h"
#include "pipe.h"
#include "queue_lossless.h"
#include "queue_lossless_input.h"
#include "randomqueue.h"
#include "topology.h"
#include "uec_sink.h"
#include "uec_src.h"

// Fat Tree topology was modified to work with this script, others won't work
// correctly
#include <list>

#include "AI_DC_msoft.h"
#include "AI_WAN_helper.h"
#include "AI_WAN_msoft.h"
#include "AI_region_msoft.h"
#include "AI_switch.h"
#include "network.h"

// Simulation params

#define PRINT_PATHS 1

// int RTT = 10; // this is per link delay; identical RTT microseconds = 0.02 ms
uint32_t RTT = 400;  // this is per link delay in ns; identical RTT microseconds
                     // = 0.02 ms

unsigned int subflow_count = 1;

// #define SWITCH_BUFFER (SERVICE * RTT / 1000)

EventList eventlist;

Logfile* lg;

void exit_error(char* progr) {
    cout << "Usage " << progr
         << " [UNCOUPLED(DEFAULT)|COUPLED_INC|FULLY_COUPLED|COUPLED_EPSILON] "
            "[epsilon][COUPLED_SCALABLE_TCP"
         << endl;
    exit(1);
}

// === Simulation params
struct simulation_parms {
    // KEC parameters

    uint32_t lossy_threshold                          = 28;
    uint32_t recoverable_threshold                    = 20;
    bool     use_as_fast_as_possible_recoverable_skip = true;
    bool     use_bitmap_full_condition_lossy_skip     = true;
    double   bitmap_full_percent                      = 0.9;
    bool     use_full_skip                            = false;

    bool         use_erasure_coding        = true;
    unsigned int erasure_coding_k          = 6;  // k in k+m erasure coding
    unsigned int erasure_coding_m          = 2;  // m in k+m erasure coding
    unsigned int erasure_coding_stripe_num = 3;

    // CC parameters
    double   loss_path_replace_threshold_percentage    = 0.5;
    double   jittery_path_replace_threshold_percentage = 0.1;
    double   init_cwnd_ratio                           = 0.7;
    uint32_t cbr_rate = 1502403;  // in pktps, 1502403 is 50% of 100 Gbps

    string statistics_filename = "";

    // Topology parameters
    std::string drop_rate = "mean";

    uint32_t switch_hash_salt = 1;

    mem_b inter_queuesize = INFINITE_BUFFER_SIZE;
    mem_b intra_queuesize = INFINITE_BUFFER_SIZE;

    struct queue_params     queue_params;
    vector<simtime_picosec> owr_owr_hop_latency;
    int                     no_of_conns = 0, cwnd = MAX_CWD_MODERN_UEC;
    stringstream            filename{ios_base::out};
    RouteStrategy           route_strategy = NOT_SET;
    std::string             goal_filename;
    linkspeed_bps           linkspeed = speedFromMbps((double)HOST_NIC);
    struct link_params      link_params;
    simtime_picosec         hop_latency    = timeFromNs((uint32_t)RTT);
    simtime_picosec         switch_latency = timeFromNs((uint32_t)0);
    simtime_picosec         pacing_delay   = 0;

    vector<double>  switch_random_drop_probs = {0, 0, 0, 0, 0, 0, 0};
    vector<double>  switch_drop_event_probs  = {0, 0, 0, 0, 0, 0, 0};
    std::string     link_down_file           = "";
    bool            use_link_down            = false;
    bool            enable_pfc               = true;  // whether to enable PFC for lossless queues
    uint32_t        jitter_path_num          = 0;
    bool            use_jitter               = false;
    simtime_picosec max_rtt                  = timeFromUs((uint32_t)28000);

    int             force_queue_size                = -1;
    int             bts_threshold                   = -1;
    bool            reuse_entropy                   = false;
    int             number_entropies                = 256;
    queue_type      queue_choice                    = COMPOSITE;
    bool            ignore_ecn_data                 = true;
    bool            ignore_ecn_ack                  = true;
    bool            do_jitter                       = false;
    bool            do_exponential_gain             = false;
    bool            use_fast_increase               = false;
    int             target_rtt_percentage_over_base = 50;
    bool            collect_data                    = false;
    int             fat_tree_k                      = 1;  // 1:1 default
    bool            use_super_fast_increase         = false;
    double          y_gain                          = 1;
    double          x_gain                          = 0.15;
    double          z_gain                          = 1;
    double          w_gain                          = 1;
    double          bonus_drop                      = 1;
    double          drop_value_buffer               = 1;
    uint64_t        actual_starting_cwnd            = 1;
    uint64_t        explicit_base_rtt               = 0;
    uint64_t        explicit_target_rtt             = 0;
    double          queue_size_ratio                = 1.0;
    bool            disable_case_3                  = false;
    bool            disable_case_4                  = false;
    int             ratio_os_stage_1                = 1;
    int             pfc_low                         = 0;
    int             pfc_high                        = 0;
    int             pfc_marking                     = 0;
    double          quickadapt_lossless_rtt         = 2.0;
    int             reaction_delay                  = 1;
    char*           tm_file                         = NULL;
    int             precision_ts                    = 1;
    int             once_per_rtt                    = 0;
    bool            enable_bts                      = false;
    bool            use_mixed                       = false;
    int             phantom_size;
    int             phantom_slowdown      = 10;
    bool            use_phantom           = false;
    double          exp_avg_ecn_value     = .3;
    double          exp_avg_rtt_value     = .3;
    char*           topo_file             = NULL;
    double          exp_avg_alpha         = 0.125;
    bool            use_exp_avg_ecn       = false;
    bool            use_exp_avg_rtt       = false;
    int             stop_pacing_after_rtt = 0;
    int             num_failed_links      = 0;
    simtime_picosec interdc_delay         = 0;
    double          def_end_time          = 200000.0;
    int             num_periods           = 1;
    bool            use_inter_gemini      = false;
    bool            use_intra_gemini      = false;
    bool            use_bbr               = false;
    bool            use_uec               = false;
    bool            use_mprdma            = false;

    int topology_type = INTER_REGION;

    // Region topology params
    struct AI_wan_topology_params wan_params = {
        .n_region        = 2,  // number of regions
        .n_dc_per_region = 1,  // number of DCs per region
        .n_spine_per_leaf_in_leafgroup =
            8,  // number of Spines connected to each leaf in a leaf group
        .n_leafgroups_per_dc              = 1,   // number of leaf groups per DC
        .n_leafs_per_leafgroup            = 8,   // number of leafs in each leaf group
        .n_torgroups_per_leafgroup        = 16,  // number of ToR groups in each leaf group
        .n_tor_per_torgroups              = 2,   // number of ToR switches in each ToR group
        .n_serv_per_torgroup              = 16,  // number of servers per ToR group
        .n_RH_groups                      = 2,   // number of region hub groups
        .n_RH_switch_batches_per_RH_group = 2,
        .n_RH_switches_per_RH_batch       = 8,
        .n_RWA_groups                     = 2,  // number of RWA groups
        .n_RWA_switches_per_RWA_group     = 4,  // number of RWA switches in each RWA group
        .n_OWR_groups                     = 2,  // number of OWR groups
        .n_OWR_switches_per_OWR_group     = 4,  // number of OWR switches in each OWR group
    };
} sim_params;

// Util to parse simulation params from command line flags.
void parseCommandLine(int argc, char** argv);

void record_set_up(Logfile& logfile, list<const Route*>& routes);

void executeTraffixMatrix(Logfile& logfile, uint64_t base_inter_rtt, uint64_t base_intra_rtt) {
    eventlist.setEndtime(timeFromMs(sim_params.def_end_time));

    AIWANMsoft*                   topo_AI        = NULL;
    struct AI_wan_topology_params topo_ai_params = {
        .n_region                         = sim_params.wan_params.n_region,
        .n_dc_per_region                  = sim_params.wan_params.n_dc_per_region,
        .n_spine_per_leaf_in_leafgroup    = sim_params.wan_params.n_spine_per_leaf_in_leafgroup,
        .n_leafgroups_per_dc              = sim_params.wan_params.n_leafgroups_per_dc,
        .n_leafs_per_leafgroup            = sim_params.wan_params.n_leafs_per_leafgroup,
        .n_torgroups_per_leafgroup        = sim_params.wan_params.n_torgroups_per_leafgroup,
        .n_tor_per_torgroups              = sim_params.wan_params.n_tor_per_torgroups,
        .n_serv_per_torgroup              = sim_params.wan_params.n_serv_per_torgroup,
        .n_RH_groups                      = sim_params.wan_params.n_RH_groups,
        .n_RH_switch_batches_per_RH_group = sim_params.wan_params.n_RH_switch_batches_per_RH_group,
        .n_RH_switches_per_RH_batch       = sim_params.wan_params.n_RH_switches_per_RH_batch,
        .n_RWA_groups                     = sim_params.wan_params.n_RWA_groups,
        .n_RWA_switches_per_RWA_group     = sim_params.wan_params.n_RWA_switches_per_RWA_group,
        .n_OWR_groups                     = sim_params.wan_params.n_OWR_groups,
        .n_OWR_switches_per_OWR_group     = sim_params.wan_params.n_OWR_switches_per_OWR_group};

    if (sim_params.topology_type == INTER_REGION) {
        cout << "Microsoft WAN topology!!" << endl;
        assert(sim_params.switch_random_drop_probs.size() == 7);
        assert(sim_params.switch_drop_event_probs.size() == 7);
        sim_params.switch_drop_event_probs[AISwitch::RH]  = 37.0 / (8 * 24 * 60 / 5.0);
        sim_params.switch_drop_event_probs[AISwitch::RWA] = 2990.0 / (16 * 24 * 60 / 5.0);
        sim_params.switch_drop_event_probs[AISwitch::OWR] = 2990.0 / (16 * 24 * 60 / 5.0);
        // Mean drop rates
        if (sim_params.drop_rate == "mean") {
            sim_params.switch_random_drop_probs[AISwitch::RH]  = pow(10, -4.3);
            sim_params.switch_random_drop_probs[AISwitch::RWA] = pow(10, -6.2);
            sim_params.switch_random_drop_probs[AISwitch::OWR] = pow(10, -6.2);
        } else if (sim_params.drop_rate == "p95") {
            sim_params.switch_random_drop_probs[AISwitch::RH]  = pow(10, -3.4);
            sim_params.switch_random_drop_probs[AISwitch::RWA] = pow(10, -4.4);
            sim_params.switch_random_drop_probs[AISwitch::OWR] = pow(10, -4.4);
        } else if (sim_params.drop_rate == "p99") {
            sim_params.switch_random_drop_probs[AISwitch::RH]  = pow(10, -3.4);
            sim_params.switch_random_drop_probs[AISwitch::RWA] = pow(10, -2.4);
            sim_params.switch_random_drop_probs[AISwitch::OWR] = pow(10, -2.4);
        } else {
            sim_params.switch_random_drop_probs[AISwitch::RH]  = 0;
            sim_params.switch_random_drop_probs[AISwitch::RWA] = 0;
            sim_params.switch_random_drop_probs[AISwitch::OWR] = 0;
        }

        topo_AI = new AIWANMsoft(topo_ai_params,
                                 sim_params.link_params,
                                 sim_params.queue_params,
                                 NULL,
                                 &eventlist,
                                 sim_params.owr_owr_hop_latency,
                                 sim_params.switch_drop_event_probs,
                                 sim_params.switch_random_drop_probs,
                                 sim_params.use_link_down,
                                 sim_params.enable_pfc);
    } else {
        cout << "Unknown topolgoy type!!" << endl;
        abort();
    }

    int no_of_nodes = topo_ai_params.get_no_of_nodes_in_WAN();
    cout << "Number of nodes in the WAN: " << no_of_nodes << endl;
    auto* conns = new ConnectionMatrix(no_of_nodes);

    if (sim_params.tm_file) {
        cout << "Loading connection matrix from  " << sim_params.tm_file << endl;
        if (!conns->load(sim_params.tm_file)) {
            cout << "Failed to load connection matrix " << sim_params.tm_file << endl;
            exit(-1);
        }
    } else {
        cout << "Loading connection matrix from  standard input" << endl;
        conns->load(cin);
    }

    map<flowid_t, TriggerTarget*> flowmap;
    vector<connection*>*          all_conns = conns->getAllConnections();
    RoceSrcACKTimerScanner        DCQCNsrcRestartScanner(timeFromMs(60), eventlist);  // 3*RTT

    vector<DCQCNSrc*> inter_srcs;

    linkspeed_bps SERVICE1          = speedFromMbps((uint64_t)7500);
    linkspeed_bps network_linkspeed = speedFromMbps((uint64_t)12500);

    FLOW_ACTIVE = all_conns->size();
    cout << "Number of flows: " << all_conns->size() << endl;

    ofstream statistics_outfile(sim_params.statistics_filename);

    for (size_t c = 0; c < all_conns->size(); c++) {
        connection* crt  = all_conns->at(c);
        int         src  = crt->src;
        int         dest = crt->dst;

        DCQCNSrc*  dcqcnSrc;
        DCQCNSink* dcqcnSink;

        dcqcnSrc =
            new DCQCNSrc(NULL, NULL, eventlist, network_linkspeed, SERVICE1, statistics_outfile);
        dcqcnSrc->setName("DCQCN" + ntoa(c));
        logfile.writeName(*dcqcnSrc);
        dcqcnSink = new DCQCNSink(eventlist, statistics_outfile);
        ((EventSource*)dcqcnSink)->setName("DCQCNSink" + ntoa(c));
        logfile.writeName(*(EventSource*)dcqcnSink);

        inter_srcs.push_back(dcqcnSrc);

        dcqcnSrc->set_dst(dest);
        dcqcnSink->set_src(src);

        // dcqcnSrc->setPath(c % 1); // this is to study CC behavior

        DCQCNsrcRestartScanner.registerRoce(*static_cast<RoceSrc*>(dcqcnSrc));

        if (crt->flowid) {
            dcqcnSrc->set_flowid(crt->flowid);
            assert(flowmap.find(crt->flowid) == flowmap.end());  // don't have dups
            flowmap[crt->flowid] = dcqcnSrc;
        }

        if (crt->size > 0) {
            dcqcnSrc->set_flowsize(crt->size);
            cout << "flow size: " << crt->size << endl;
        }

        if (crt->trigger) {
            Trigger* trig = conns->getTrigger(crt->trigger, eventlist);
            trig->add_target(*dcqcnSrc);
        }
        if (crt->send_done_trigger) {
            Trigger* trig = conns->getTrigger(crt->send_done_trigger, eventlist);
            dcqcnSrc->set_end_trigger(*trig);
        }

        switch (sim_params.route_strategy) {
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

                AI_WAN_create_route(topo_AI, src, random(), srctotor, dcqcnSrc->flow_id());
                AI_WAN_create_route(topo_AI, dest, random(), dsttotor, dcqcnSrc->flow_id());

                // printf("Creating 1 Flow from %d to %d\n", src, dest);
                dcqcnSrc->connect(srctotor, dsttotor, *dcqcnSink, crt->start);

                // register src and snk to the topo_AI to receive packets src their respective ToRs
                AI_WAN_register_host(topo_AI, src, dcqcnSrc, dcqcnSrc->flow_id());
                AI_WAN_register_host(topo_AI, dest, dcqcnSink, dcqcnSrc->flow_id());

                break;
            }
            case NOT_SET: {
                cout << "Route strategy NOT_SET" << endl;
                abort();
                break;
            }
            default: {
                cout << "Unknown Route strategy " << sim_params.route_strategy << endl;
                abort();
                break;
            }
        }
    }

    cout << "\nEntering the event running loop" << endl;
    bool ret                  = false;
    int  finished_flow_number = 0;
    for (auto iter = inter_srcs.begin(); iter != inter_srcs.end(); ++iter) {
        DCQCNSrc* dcqcnsrc = *iter;
        finished_flow_number += int(dcqcnsrc->get_flow_finished());
    }
    while ((ret = eventlist.doNextEvent()) || finished_flow_number < all_conns->size()) {
        finished_flow_number = 0;
        for (auto iter = inter_srcs.begin(); iter != inter_srcs.end(); ++iter) {
            DCQCNSrc* dcqcnsrc = *iter;
            finished_flow_number += int(dcqcnsrc->get_flow_finished());
        }
    }
    // Cout last timestamp of the events; see if the loop is terminated due to end time
    cout << "Event running loop terminated at " << timeAsUs(eventlist.now()) << " us" << endl;

    cout << "Event running loop done" << endl;

    for (std::size_t i = 0; i < inter_srcs.size(); ++i) {
        delete inter_srcs[i];
    }
}

int main(int argc, char** argv) {
    srand(time(NULL));

    COLLECT_DATA = sim_params.collect_data;
    sim_params.filename << "logout.dat";
    parseCommandLine(argc, argv);

    cout << "All the arguments are parsed!" << endl;
    cout << "---------------------------\n\n" << endl;

    Packet::set_packet_size(PKT_SIZE_MODERN);
    cout << "MTU is " << PKT_SIZE_MODERN << " B" << endl;

    SINGLE_PKT_TRASMISSION_TIME_MODERN = Packet::data_packet_size() * 8 / (LINK_SPEED_MODERN);

    initializeLoggingFolders();

    if (sim_params.pfc_high != 0) {
        LosslessInputQueue::_high_threshold = sim_params.pfc_high;
        LosslessInputQueue::_low_threshold  = sim_params.pfc_low;
        // TODO(vtlam): support _mark_pfc_amount
        // LosslessInputQueue::_mark_pfc_amount = pfc_marking;
    } else {
        LosslessInputQueue::_high_threshold = Packet::data_packet_size() * 50;
        LosslessInputQueue::_low_threshold  = Packet::data_packet_size() * 25;
        // TODO(vtlam): support _mark_pfc_amount
        // LosslessInputQueue::_mark_pfc_amount = pfc_marking;
    }

    if (sim_params.route_strategy == NOT_SET) {
        fprintf(stderr,
                "Route Strategy not set.  Use the -strat param.  "
                "\nValid values are perm, rand, pull, rg and single\n");
        exit(1);
    }

    sim_params.link_params.inter_dc_link_speed = speedFromGbps((double)400);
    sim_params.link_params.intra_dc_link_speed = speedFromMbps((double)HOST_NIC);
    sim_params.link_params.link_factor_rh_rwa  = 12;
    sim_params.link_params.link_factor_rwa_owr = 20;

    sim_params.link_params.link_factor_regional.resize(sim_params.wan_params.n_region);
    for (unsigned int i = 0; i < sim_params.wan_params.n_region; i++) {
        sim_params.link_params.link_factor_regional[i].resize(sim_params.wan_params.n_region);
        for (unsigned int j = 0; j < sim_params.wan_params.n_region; j++) {
            sim_params.link_params.link_factor_regional[i][j] = 30;
        }
    }

    if (sim_params.wan_params.n_region > 1) {
        // Hard-coded for 2 regions for now
        // for (size_t idx = 0;
        //      idx < sim_params.link_params.link_factor_regional[0][1] -
        //      sim_params.jitter_path_num; idx++) {
        //     sim_params.owr_owr_hop_latency.push_back(timeFromUs((uint32_t)14000));
        // }
        // for (size_t idx = 0; idx < sim_params.jitter_path_num;
        //      idx++) {  // sweep 8, 10, 12, timer = propotional to the jitter = 2 *
        //      jitter_difference
        //     sim_params.owr_owr_hop_latency.push_back(timeFromUs((uint32_t)24000));
        // }
        // if (sim_params.jitter_path_num > 0) {
        //     sim_params.max_rtt = timeFromUs((uint32_t)24000 * 2);
        // } else {
        //     sim_params.max_rtt = timeFromUs((uint32_t)14000 * 2);
        // }
        vector<simtime_picosec> latency_pool;
        simtime_picosec         max_latency = 0;

        if (sim_params.use_jitter) {
            for (size_t idx = 0; idx < sim_params.link_params.link_factor_regional[0][1]; idx++) {
                latency_pool.push_back(timeFromUs((uint32_t)(10000 + idx * 333)));
            }
        } else {
            for (size_t idx = 0; idx < sim_params.link_params.link_factor_regional[0][1]; idx++) {
                latency_pool.push_back(timeFromUs((uint32_t)(10000)));
            }
        }

        // randomly choose one latency from the pool for each link
        for (size_t idx = 0; idx < sim_params.link_params.link_factor_regional[0][1]; idx++) {
            sim_params.owr_owr_hop_latency.push_back(latency_pool[rand() % latency_pool.size()]);
            if (max_latency < sim_params.owr_owr_hop_latency.back()) {
                max_latency = sim_params.owr_owr_hop_latency.back();
            }
        }

        sim_params.max_rtt = max_latency * 2;
    } else {
        sim_params.owr_owr_hop_latency.push_back(timeFromUs((uint32_t)14000));
        sim_params.max_rtt = timeFromUs((uint32_t)14303 * 2);
    }

    sim_params.queue_params.intra_region_qt = LOSSLESS;
    sim_params.queue_params.inter_region_qt = COMP_NO_ECN;
    sim_params.queue_params.tor_queuesize   = 10500000;
    sim_params.queue_params.leaf_queuesize  = 10500000;
    sim_params.queue_params.spine_queuesize = 10500000;
    sim_params.queue_params.RH_queuesize    = 200000000;
    sim_params.queue_params.RWA_queuesize   = 200000000;
    sim_params.queue_params.OWR_queuesize   = 200000000;

    cout << "queue_params.tor_queuesize: " << sim_params.queue_params.tor_queuesize << endl;

    // Calculate Network Info
    int inter_hops = 9;  // hardcoded for now
    int intra_hops = 6;  // hardcoded for now

    // both base rtts are in ps
    uint64_t base_inter_rtt =
        2 * (inter_hops - 1) *
            (1000 * LINK_DELAY_MODERN + sim_params.switch_latency) +  // All DCN latencies.
        1000 * inter_hops * (PKT_SIZE_MODERN + 64) * 8 /
            INTER_LINK_SPEED_MODERN +                          // Packet transmission delays.
        1000 * inter_hops * 64 * 8 / INTER_LINK_SPEED_MODERN;  // Ack transmission delays.

    cout << "Before adding the inter-DC delay, the base_inter_rtt is " << timeAsUs(base_inter_rtt)
         << " us." << endl;
    base_inter_rtt += 2 * (sim_params.interdc_delay);  // InterDC latencies.

    uint64_t base_intra_rtt =
        2 * intra_hops *
            (LINK_DELAY_MODERN * 1000 + sim_params.switch_latency) +  // All DCN latencies.
        1000 * intra_hops * (PKT_SIZE_MODERN + 64) * 8 /
            LINK_SPEED_MODERN +                          // Packet transmission delays.
        1000 * intra_hops * 64 * 8 / LINK_SPEED_MODERN;  // Ack transmission delays.

    // TODO(vtlam): support phantom queue
    // CompositeQueue::set_phantom_queue_size(phantom_size);

    cout << "BW: " << LINK_SPEED_MODERN << " Gbps" << endl;

    cout << "Using subflow count " << subflow_count << endl;

    // prepare the loggers

    cout << "Logging to " << sim_params.filename.str() << endl;
    // Logfile
    Logfile logfile(sim_params.filename.str(), eventlist);

#if PRINT_PATHS
    sim_params.filename << ".paths";
    cout << "Logging path choices to " << sim_params.filename.str() << endl;
    std::ofstream paths(sim_params.filename.str().c_str());
    if (!paths) {
        cout << "Can't open for writing paths file!" << endl;
        exit(1);
    }
#endif

    lg = &logfile;

    logfile.setStartTime(timeFromSec(0));

    TrafficLoggerSimple traffic_logger = TrafficLoggerSimple();
    logfile.addLogger(traffic_logger);

    fflush(stdout);

    map<int, vector<int>*>::iterator it;

    // used just to print out stats data at the end
    list<const Route*> routes;

    if (sim_params.tm_file != NULL) {
        executeTraffixMatrix(logfile, base_inter_rtt, base_intra_rtt);
    } else if (sim_params.goal_filename.size() > 0) {
        // TODO(vtlam): for simplicity disable LGS for now.
        // executeLgs(traffic_logger);
        abort();
    }

    record_set_up(logfile, routes);
}

void record_set_up(Logfile& logfile, list<const Route*>& routes) {
    // Record the setup
    int pktsize = Packet::data_packet_size();
    logfile.write("# pktsize=" + ntoa(pktsize) + " bytes");
    logfile.write("# subflows=" + ntoa(subflow_count));
    logfile.write("# hostnicrate = " + ntoa(HOST_NIC) + " pkt/sec");
    logfile.write("# corelinkrate = " + ntoa(HOST_NIC * CORE_TO_HOST) + " pkt/sec");
    // logfile.write("# buffer = " + ntoa((double)
    // (queues_na_ni[0][1]->_maxsize) / ((double) pktsize)) + " pkt");
    double rtt = timeAsSec(timeFromUs(RTT));
    logfile.write("# rtt =" + ntoa(rtt));

    cout << "Done" << endl;
    list<const Route*>::iterator rt_i;
    int                          counts[10];
    int                          hop;
    for (int i = 0; i < 10; i++)
        counts[i] = 0;
    for (rt_i = routes.begin(); rt_i != routes.end(); rt_i++) {
        const Route* r = (*rt_i);
        // print_route(*r);
        cout << "Path:" << endl;
        hop = 0;
        for (std::size_t i = 0; i < r->size(); i++) {
            PacketSink*     ps = r->at(i);
            CompositeQueue* q  = dynamic_cast<CompositeQueue*>(ps);
            if (q == 0) {
                cout << ps->nodename() << endl;
            } else {
                cout << q->nodename() << " id=" << 0 /*q->id*/ << " " << q->num_packets() << "pkts "
                     << q->num_headers() << "hdrs " << q->num_acks() << "acks " << q->num_nacks()
                     << "nacks " << q->num_stripped() << "stripped"
                     << endl;  // TODO(tommaso): compositequeues don't have id.
                               // Need to add that or find an alternative way.
                               // Verify also that compositequeue is the right
                               // queue to use here.
                counts[hop] += q->num_stripped();
                hop++;
            }
        }
        cout << endl;
    }
    for (int i = 0; i < 10; i++)
        cout << "Hop " << i << " Count " << counts[i] << endl;
}

// TODO(zeying): move global variables defined in common.cpp and parsed here into a sim_config
// struct Util to parse simulation params from command line flags.
void parseCommandLine(int argc, char** argv) {
    int i = 1;
    while (i < argc) {
        if (!strcmp(argv[i], "-o")) {
            sim_params.filename.str(std::string());
            sim_params.filename << argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-sub")) {
            subflow_count = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-conns")) {
            sim_params.no_of_conns = atoi(argv[i + 1]);
            cout << "no_of_conns " << sim_params.no_of_conns << endl;
            cout << "!!currently hardcoded to 8, value will be ignored!!" << endl;
            i++;
        } else if (!strcmp(argv[i], "-cwnd")) {
            sim_params.cwnd = atoi(argv[i + 1]);
            cout << "cwnd " << sim_params.cwnd << endl;
            i++;
        } else if (!strcmp(argv[i], "-use_mixed")) {
            sim_params.use_mixed = atoi(argv[i + 1]);
            // LcpSrc::set_use_mixed(use_mixed);
            // TODO(vtlam): support phantom queue
            //        CompositeQueue::set_use_mixed(use_mixed);
            // printf("UseMixed: %d\n", use_mixed);
            i++;
        } else if (!strcmp(argv[i], "-topo")) {
            sim_params.topo_file = argv[i + 1];
            cout << "FatTree topology input file: " << sim_params.topo_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-once_per_rtt")) {
            sim_params.once_per_rtt = atoi(argv[i + 1]);
            // LcpSrc::set_once_per_rtt(once_per_rtt);
            // UecSrc::set_once_per_rtt(once_per_rtt);
            // printf("OnceRTTDecrease: %d\n", once_per_rtt);
            i++;
        } else if (!strcmp(argv[i], "-stop_pacing_after_rtt")) {
            sim_params.stop_pacing_after_rtt = atoi(argv[i + 1]);
            // LcpSrc::set_stop_pacing(stop_pacing_after_rtt);
            // UecSrc::set_stop_pacing(stop_pacing_after_rtt);
            i++;
        } else if (!strcmp(argv[i], "-linkspeed")) {
            // linkspeed specified is in Mbps
            sim_params.linkspeed = speedFromMbps(atof(argv[i + 1]));
            LINK_SPEED_MODERN    = atoi(argv[i + 1]);
            cout << "Link speed: " << atof(argv[i + 1]) << " Mbps" << endl;
            LINK_SPEED_MODERN       = LINK_SPEED_MODERN / 1000;  // switch to gbps
            INTER_LINK_SPEED_MODERN = LINK_SPEED_MODERN;
            // Saving this for UEC reference, Gbps
            i++;
        } else if (!strcmp(argv[i], "-dctcp")) {
            // LcpSrc::using_dctcp = true;
            cout << "Using DCTCP" << endl;
        } else if (!strcmp(argv[i], "-adaptive_reroute")) {
            cout << "Using Adaptive Re-Route" << endl;
            // LcpSrc::_subflow_adaptive_reroute = true;
        } else if (!strcmp(argv[i], "-subflow_reroute")) {
            cout << "Re-route on subflow" << endl;
            // LcpSrc::_subflow_reroute = true;
        } else if (!strcmp(argv[i], "-fail_one")) {
        } else if (!strcmp(argv[i], "-k")) {
            sim_params.fat_tree_k = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-ratio_os_stage_1")) {
            sim_params.ratio_os_stage_1 = atoi(argv[i + 1]);
            // LcpSrc::set_os_ratio_stage_1(ratio_os_stage_1);
            // UecSrc::set_os_ratio_stage_1(ratio_os_stage_1);
            i++;
        } else if (!strcmp(argv[i], "-pfc_marking")) {
            sim_params.pfc_marking = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-quickadapt_lossless_rtt")) {
            sim_params.quickadapt_lossless_rtt = std::stod(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-bts_trigger")) {
            sim_params.bts_threshold = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-forceQueueSize")) {
            sim_params.force_queue_size = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-mtu")) {
            int packet_size = atoi(argv[i + 1]);
            PKT_SIZE_MODERN = packet_size;  // Saving this for UEC reference, Bytes
            i++;
        } else if (!strcmp(argv[i], "-reuse_entropy")) {
            sim_params.reuse_entropy = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-num_periods")) {
            sim_params.num_periods = atoi(argv[i + 1]);
            // LcpSrc::set_frequency(num_periods);
            // UecSrc::set_frequency(num_periods);
            i++;
        } else if (!strcmp(argv[i], "-disable_case_3")) {
            sim_params.disable_case_3 = atoi(argv[i + 1]);
            // LcpSrc::set_disable_case_3(disable_case_3);
            // UecSrc::set_disable_case_3(disable_case_3);
            // printf("DisableCase3: %d\n", disable_case_3);
            i++;
        } else if (!strcmp(argv[i], "-jump_to")) {
            // UecSrc::jump_to = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-reaction_delay")) {
            sim_params.reaction_delay = atoi(argv[i + 1]);
            // LcpSrc::set_reaction_delay(reaction_delay);
            // UecSrc::set_reaction_delay(reaction_delay);
            // printf("ReactionDelay: %d\n", reaction_delay);
            i++;
        } else if (!strcmp(argv[i], "-precision_ts")) {
            sim_params.precision_ts = atoi(argv[i + 1]);
            // LcpSrc::set_precision_ts(precision_ts * 1000);
            // UecSrc::set_precision_ts(precision_ts * 1000);
            //  printf("Precision: %d\n", precision_ts * 1000);
            i++;
        } else if (!strcmp(argv[i], "-disable_case_4")) {
            sim_params.disable_case_4 = atoi(argv[i + 1]);
            // LcpSrc::set_disable_case_4(disable_case_4);
            // UecSrc::set_disable_case_4(disable_case_4);
            // printf("DisableCase4: %d\n", disable_case_4);
            i++;
        } else if (!strcmp(argv[i], "-stop_after_quick")) {
            // LcpSrc::set_stop_after_quick(true);
            // UecSrc::set_stop_after_quick(true);
            // printf("StopAfterQuick: %d\n", true);

        } else if (!strcmp(argv[i], "-number_entropies")) {
            sim_params.number_entropies = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-switch_latency")) {
            sim_params.switch_latency = timeFromNs(atof(argv[i + 1]));
            i++;
            cout << "Switch latency is " << sim_params.switch_latency << " ps" << endl;
        } else if (!strcmp(argv[i], "-hop_latency")) {
            sim_params.hop_latency = timeFromNs(atof(argv[i + 1]));
            LINK_DELAY_MODERN =
                sim_params.hop_latency / 1000;  // Saving this for UEC reference, ps to ns
            cout << "link/hop latency is " << sim_params.hop_latency << "ps" << endl;
            i++;
        } else if (!strcmp(argv[i], "-ignore_ecn_ack")) {
            sim_params.ignore_ecn_ack = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-ignore_ecn_data")) {
            sim_params.ignore_ecn_data = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-target_to_baremetal_ratio")) {
            float target_to_baremetal_ratio = atof(argv[i + 1]);
            cout << "target_to_baremetal_ratio: " << target_to_baremetal_ratio << endl;
            TARGET_TO_BAREMETAL_RATIO = target_to_baremetal_ratio;
            i++;
        } else if (!strcmp(argv[i], "-cwndBdpRatio")) {
            float starting_cwnd_bdp_ratio = atof(argv[i + 1]);
            cout << "starting_cwnd_bdp_ratio: " << starting_cwnd_bdp_ratio << endl;
            STARTING_CWND_BDP_RATIO = starting_cwnd_bdp_ratio;
            i++;
        } else if (!strcmp(argv[i], "-fast_drop")) {
            // LcpSrc::set_fast_drop(atoi(argv[i + 1]));
            // UecSrc::set_fast_drop(atoi(argv[i + 1]));
            // printf("FastDrop: %d\n", atoi(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-interdcDelay")) {
            sim_params.interdc_delay = timeFromNs(atoi(argv[i + 1]));
            cout << "interdc_delay is: " << sim_params.interdc_delay << "ps" << endl;
            i++;
        } else if (!strcmp(argv[i], "-pfc_low")) {
            sim_params.pfc_low = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-pfc_high")) {
            sim_params.pfc_high = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-collect_data")) {
            sim_params.collect_data = atoi(argv[i + 1]);
            COLLECT_DATA            = sim_params.collect_data;
            i++;
            std::cout << "Collecting data in on" << endl;
        } else if (!strcmp(argv[i], "-do_jitter")) {
            sim_params.do_jitter = atoi(argv[i + 1]);
            // LcpSrc::set_do_jitter(do_jitter);
            // UecSrc::set_do_jitter(do_jitter);
            // printf("DoJitter: %d\n", do_jitter);
            i++;
        } else if (!strcmp(argv[i], "-do_exponential_gain")) {
            sim_params.do_exponential_gain = atoi(argv[i + 1]);
            // LcpSrc::set_do_exponential_gain(do_exponential_gain);
            // UecSrc::set_do_exponential_gain(do_exponential_gain);
            // printf("DoExpGain: %d\n", do_exponential_gain);
            i++;
        } else if (!strcmp(argv[i], "-use_fast_increase")) {
            sim_params.use_fast_increase = atoi(argv[i + 1]);
            // LcpSrc::set_use_fast_increase(use_fast_increase);
            // UecSrc::set_use_fast_increase(use_fast_increase);
            // printf("FastIncrease: %d\n", use_fast_increase);
            i++;
        } else if (!strcmp(argv[i], "-use_super_fast_increase")) {
            sim_params.use_super_fast_increase = atoi(argv[i + 1]);
            // LcpSrc::set_use_super_fast_increase(use_super_fast_increase);
            // UecSrc::set_use_super_fast_increase(use_super_fast_increase);
            // printf("FastIncreaseSuper: %d\n", use_super_fast_increase);
            i++;
        } else if (!strcmp(argv[i], "-decrease_on_nack")) {
            double decrease_on_nack = std::stod(argv[i + 1]);
            // LcpSrc::set_decrease_on_nack(decrease_on_nack);
            // UecSrc::set_decrease_on_nack(decrease_on_nack);
            i++;
        } else if (!strcmp(argv[i], "-tm")) {
            sim_params.tm_file = argv[i + 1];
            cout << "traffic matrix input file: " << sim_params.tm_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-target_rtt_percentage_over_base")) {
            sim_params.target_rtt_percentage_over_base = atoi(argv[i + 1]);
            // LcpSrc::set_target_rtt_percentage_over_base(target_rtt_percentage_over_base);
            // UecSrc::set_target_rtt_percentage_over_base(target_rtt_percentage_over_base);
            // printf("TargetRTT: %d\n", target_rtt_percentage_over_base);
            i++;
        } else if (!strcmp(argv[i], "-num_failed_links")) {
            sim_params.num_failed_links = atoi(argv[i + 1]);
            // FatTreeTopology::set_failed_links(num_failed_links);
            i++;
        } else if (!strcmp(argv[i], "-fast_drop_rtt")) {
            // LcpSrc::set_fast_drop_rtt(atoi(argv[i + 1]));
            // UecSrc::set_fast_drop_rtt(atoi(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-y_gain")) {
            sim_params.y_gain = std::stod(argv[i + 1]);
            // LcpSrc::set_y_gain(y_gain);
            // UecSrc::set_y_gain(y_gain);
            // printf("YGain: %f\n", y_gain);
            i++;
        } else if (!strcmp(argv[i], "-x_gain")) {
            sim_params.x_gain = std::stod(argv[i + 1]);
            // LcpSrc::set_x_gain(x_gain);
            // UecSrc::set_x_gain(x_gain);
            // printf("XGain: %f\n", x_gain);
            i++;
        } else if (!strcmp(argv[i], "-z_gain")) {
            sim_params.z_gain = std::stod(argv[i + 1]);
            // LcpSrc::set_z_gain(z_gain);
            // UecSrc::set_z_gain(z_gain);
            // printf("ZGain: %f\n", z_gain);
            i++;
        } else if (!strcmp(argv[i], "-end_time")) {
            sim_params.def_end_time = std::stod(argv[i + 1]);
            // printf("def_end_time: %f\n", def_end_time);
            i++;
        } else if (!strcmp(argv[i], "-w_gain")) {
            sim_params.w_gain = std::stod(argv[i + 1]);
            // LcpSrc::set_w_gain(w_gain);
            // UecSrc::set_w_gain(w_gain);
            // printf("WGain: %f\n", w_gain);
            i++;
        } else if (!strcmp(argv[i], "-starting_cwnd")) {
            sim_params.actual_starting_cwnd = atoi(argv[i + 1]);
            // LcpSrc::set_starting_cwnd(actual_starting_cwnd);
            std::cout << "Starting cwnd set to " << sim_params.actual_starting_cwnd << endl;
            // printf("StartingWindowForced: %d\n", actual_starting_cwnd);
            i++;
        } else if (!strcmp(argv[i], "-explicit_base_rtt")) {
            sim_params.explicit_base_rtt = ((uint64_t)atoi(argv[i + 1])) * 1000;
            // printf("BaseRTTForced: %d\n", explicit_base_rtt);
            // LcpSrc::set_explicit_rtt(explicit_base_rtt);
            // UecSrc::set_explicit_rtt(explicit_base_rtt);
            i++;
        } else if (!strcmp(argv[i], "-explicit_target_rtt")) {
            sim_params.explicit_target_rtt = ((uint64_t)atoi(argv[i + 1])) * 1000;
            // printf("TargetRTTForced: %lu\n", explicit_target_rtt);
            // LcpSrc::set_explicit_target_rtt(explicit_target_rtt);
            // UecSrc::set_explicit_target_rtt(explicit_target_rtt);
            i++;
        } else if (!strcmp(argv[i], "-queueSizeRatio")) {
            sim_params.queue_size_ratio = std::stod(argv[i + 1]);
            cout << "QueueSizeRatio: " << sim_params.queue_size_ratio << endl;
            if (sim_params.queue_size_ratio <= 0) {
                cout << "invalid queue_size_ratio" << endl;
                exit(0);
            }
            i++;
        } else if (!strcmp(argv[i], "-bonus_drop")) {
            sim_params.bonus_drop = std::stod(argv[i + 1]);
            // UecSrc::set_bonus_drop(bonus_drop);
            //  printf("BonusDrop: %f\n", bonus_drop);
            i++;
        } else if (!strcmp(argv[i], "-drop_value_buffer")) {
            sim_params.drop_value_buffer = std::stod(argv[i + 1]);
            // LcpSrc::set_buffer_drop(drop_value_buffer);
            // UecSrc::set_buffer_drop(drop_value_buffer);
            // printf("BufferDrop: %f\n", drop_value_buffer);
            i++;
        } else if (!strcmp(argv[i], "-goal")) {
            sim_params.goal_filename = argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-use_exp_avg_ecn")) {
            sim_params.use_exp_avg_ecn = atoi(argv[i + 1]);
            // printf("UseExpAvgEcn: %d\n", use_exp_avg_ecn);
            // LcpSrc::set_exp_avg_ecn(use_exp_avg_ecn);
            // UecSrc::set_exp_avg_ecn(use_exp_avg_ecn);
            i++;
        } else if (!strcmp(argv[i], "-use_exp_avg_rtt")) {
            sim_params.use_exp_avg_rtt = atoi(argv[i + 1]);
            // printf("UseExpAvgRtt: %d\n", use_exp_avg_rtt);
            // LcpSrc::set_exp_avg_rtt(use_exp_avg_rtt);
            // UecSrc::set_exp_avg_rtt(use_exp_avg_rtt);
            i++;
        } else if (!strcmp(argv[i], "-exp_avg_rtt_value")) {
            sim_params.exp_avg_rtt_value = std::stod(argv[i + 1]);
            // printf("UseExpAvgRttValue: %d\n", exp_avg_rtt_value);
            // LcpSrc::set_exp_avg_rtt_value(exp_avg_rtt_value);
            // UecSrc::set_exp_avg_rtt_value(exp_avg_rtt_value);
            i++;
        } else if (!strcmp(argv[i], "-exp_avg_ecn_value")) {
            sim_params.exp_avg_ecn_value = std::stod(argv[i + 1]);
            // printf("UseExpAvgecn_value: %d\n", exp_avg_ecn_value);
            // LcpSrc::set_exp_avg_ecn_value(exp_avg_ecn_value);
            // UecSrc::set_exp_avg_ecn_value(exp_avg_ecn_value);
            i++;
        } else if (!strcmp(argv[i], "-exp_avg_alpha")) {
            sim_params.exp_avg_alpha = std::stod(argv[i + 1]);
            // printf("UseExpAvgalpha: %d\n", exp_avg_alpha);
            // LcpSrc::set_exp_avg_alpha(exp_avg_alpha);
            // UecSrc::set_exp_avg_alpha(exp_avg_alpha);
            i++;
        } else if (!strcmp(argv[i], "-num_subflow")) {
            // LcpSrc::_total_subflows = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-os_border")) {
            int os_b = atoi(argv[i + 1]);
            cout << "set_os_ratio_border is: " << atoi(argv[i + 1]) << endl;
            i++;
        } else if (!strcmp(argv[i], "-strat")) {
            cout << "routing strategy is: " << argv[i + 1] << endl;
            if (!strcmp(argv[i + 1], "perm")) {
                sim_params.route_strategy = SCATTER_PERMUTE;
            } else if (!strcmp(argv[i + 1], "rand")) {
                sim_params.route_strategy = SCATTER_RANDOM;
                AISwitch::set_strategy(AISwitch::ECMP);
            } else if (!strcmp(argv[i + 1], "pull")) {
                sim_params.route_strategy = PULL_BASED;
            } else if (!strcmp(argv[i + 1], "single")) {
                sim_params.route_strategy = SINGLE_PATH;
                AISwitch::set_strategy(AISwitch::SINGLE);
            } else if (!strcmp(argv[i + 1], "ecmp_host")) {
                sim_params.route_strategy = ECMP_FIB;
                AISwitch::set_strategy(AISwitch::ECMP);
            } else if (!strcmp(argv[i + 1], "ecmp_classic")) {
                sim_params.route_strategy = ECMP_RANDOM_ECN;
                AISwitch::set_strategy(AISwitch::ECMP);
            } else if (!strcmp(argv[i + 1], "simple_subflow")) {
                sim_params.route_strategy = SIMPLE_SUBFLOW;
                AISwitch::set_strategy(AISwitch::ECMP);
            } else if (!strcmp(argv[i + 1], "ecmp_host_random2_ecn")) {
                sim_params.route_strategy = ECMP_RANDOM2_ECN;
                AISwitch::set_strategy(AISwitch::ECMP);
            }
            i++;
        } else if (!strcmp(argv[i], "-topology")) {
            if (!strcmp(argv[i + 1], "normal")) {
                cout << "Topology is intradc" << endl;
                sim_params.topology_type = INTRA_DC;
            } else if (!strcmp(argv[i + 1], "interdc")) {
                cout << "Topology is interdc" << endl;
                sim_params.topology_type = INTER_DC;
            } else if (!strcmp(argv[i + 1], "interregion")) {
                sim_params.topology_type = INTER_REGION;
            }
            i++;
        } else if (!strcmp(argv[i], "-queue_type")) {
            cout << "queueing type is: " << argv[i + 1] << endl;
            if (!strcmp(argv[i + 1], "composite")) {
                sim_params.queue_choice = COMPOSITE;
                // LcpSrc::set_queue_type("composite");
                // UecSrc::set_queue_type("composite");
            } else if (!strcmp(argv[i + 1], "composite_bts")) {
                sim_params.queue_choice = COMPOSITE_BTS;
                // LcpSrc::set_queue_type("composite_bts");
                // UecSrc::set_queue_type("composite_bts");
                printf("Name Running: UEC BTS\n");
            } else if (!strcmp(argv[i + 1], "lossless_input")) {
                sim_params.queue_choice = LOSSLESS_INPUT;
                // LcpSrc::set_queue_type("lossless_input");
                // UecSrc::set_queue_type("lossless_input");
                printf("Name Running: UEC Queueless\n");
            }
            i++;
        } else if (!strcmp(argv[i], "-target-low-us")) {
            cout << "TARGET_RTT is: " << argv[i + 1] << "us" << endl;
            TARGET_RTT = timeFromUs(atof(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-target-high-us")) {
            cout << "QA_TRIGGER_RTT is: " << argv[i + 1] << "us" << endl;
            QA_TRIGGER_RTT = timeFromUs(atof(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-baremetal-us")) {
            cout << "BAREMETAL_RTT is: " << argv[i + 1] << "us" << endl;
            BAREMETAL_RTT = timeFromUs(atof(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-ecnAlpha")) {
            cout << "LCP_ECN_ALPHA is: " << argv[i + 1] << endl;
            LCP_ECN_ALPHA = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-aiInter")) {
            cout << "AI_BYTES_INTER is: " << argv[i + 1] << endl;
            AI_BYTES_INTER = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-aiIntra")) {
            cout << "AI_BYTES_INTRA is: " << argv[i + 1] << endl;
            AI_BYTES_INTRA = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-lcpK")) {
            cout << "LCP_K (packet num) is: " << argv[i + 1] << endl;
            LCP_K = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-pacingBonus")) {
            cout << "LCP_PACING_BONUS is: " << argv[i + 1] << endl;
            LCP_PACING_BONUS = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-InterFiT")) {
            cout << "LCP_FAST_INCREASE_THRESHOLD_INTER is: " << argv[i + 1] << endl;
            LCP_FAST_INCREASE_THRESHOLD_INTER = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-IntraFiT")) {
            cout << "LCP_FAST_INCREASE_THRESHOLD_INTRA is: " << argv[i + 1] << endl;
            LCP_FAST_INCREASE_THRESHOLD_INTRA = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-qaCwndRatio")) {
            cout << "QA_CWND_RATIO_THRESHOLD is " << argv[i + 1] << endl;
            QA_CWND_RATIO_THRESHOLD = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-maxCwndRatio")) {
            cout << "MAX_CWND_BDP_RATIO is " << argv[i + 1] << endl;
            MAX_CWND_BDP_RATIO = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-init-cwnd")) {
            cout << "INIT_CWND_RATIO is: " << argv[i + 1] << endl;
            sim_params.init_cwnd_ratio = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-noQaInter")) {
            cout << "Disabling QA for Inter-Dc lcp" << endl;
            LCP_USE_QUICK_ADAPT_INTER = false;
        } else if (!strcmp(argv[i], "-noQaIntra")) {
            cout << "Disabling QA for intra DC lcp" << endl;
            LCP_USE_QUICK_ADAPT_INTRA = false;
        } else if (!strcmp(argv[i], "-noFi")) {
            cout << "Disabling fast increase" << endl;
            LCP_USE_FAST_INCREASE = false;
        } else if (!strcmp(argv[i], "-separate")) {
            cout << "Separating the queues for intra- and inter-DC flows" << endl;
            LCP_USE_DIFFERENT_QUEUES = true;
        } else if (!strcmp(argv[i], "-aiEpoch")) {
            cout << "Applyin AI per epoch" << endl;
            LCP_APPLY_AI_PER_EPOCH = true;
        } else if (!strcmp(argv[i], "-noRto")) {
            cout << "Disabling Cwnd reduction upon retransmission" << endl;
            LCP_RTO_REDUCE_CWND = false;
        } else if (!strcmp(argv[i], "-noRtt")) {
            cout << "Disabling epoch rtt reaction" << endl;
            LCP_USE_RTT = false;
        } else if (!strcmp(argv[i], "-noEcn")) {
            cout << "Disabling epoch ecn reaction" << endl;
            LCP_USE_ECN = false;
        } else if (!strcmp(argv[i], "-interQSize")) {
            cout << "Inter queue size set to: " << argv[i + 1] << endl;
            sim_params.inter_queuesize = atol(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-intraQSize")) {
            cout << "Intra queue size set to: " << argv[i + 1] << endl;
            sim_params.intra_queuesize = atol(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-is-link-down")) {
            cout << "Is link down: " << argv[i + 1] << endl;
            sim_params.use_link_down = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-jitter-path-num")) {
            cout << "Jitter path num: " << argv[i + 1] << endl;
            sim_params.jitter_path_num = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-use-jitter")) {
            cout << "Use Jittery Paths: " << argv[i + 1] << endl;
            sim_params.use_jitter = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-cbr-rate")) {
            cout << "CBR rate: " << argv[i + 1] << endl;
            sim_params.cbr_rate = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-recoverable-threshold")) {
            cout << "EC Recoverable threshold " << argv[i + 1] << endl;
            sim_params.recoverable_threshold = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-lossy-threshold")) {
            cout << "EC Lossy threshold " << argv[i + 1] << endl;
            sim_params.lossy_threshold = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-statistics-filename")) {
            cout << "Statistics filename: " << argv[i + 1] << endl;
            sim_params.statistics_filename = argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-use-full-skip")) {
            cout << "Use full skip: " << argv[i + 1] << endl;
            sim_params.use_full_skip = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-use-bitmap-full-lossy-skip")) {
            cout << "Use bitmap full condition for lossy skip: " << argv[i + 1] << endl;
            sim_params.use_bitmap_full_condition_lossy_skip = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-use-as-fast-as-possible-recoverable-skip")) {
            cout << "Use as fast as possible for recoverable skip: " << argv[i + 1] << endl;
            sim_params.use_as_fast_as_possible_recoverable_skip = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-bitmap_full_percent")) {
            cout << "Bitmap full percent: " << argv[i + 1] << endl;
            sim_params.bitmap_full_percent = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-loss-path-replace-threshold")) {
            cout << "Loss path replace threshold (%) of Bitmap Size: " << argv[i + 1] << endl;
            sim_params.loss_path_replace_threshold_percentage = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-jittery-path-replace-threshold")) {
            cout << "Jittery path replace threshold (%) of mean_rtt: " << argv[i + 1] << endl;
            sim_params.jittery_path_replace_threshold_percentage = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-drop-rate")) {
            cout << "Drop rate used: " << argv[i + 1] << endl;
            sim_params.drop_rate = argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-enable-pfc")) {
            cout << "Enable PFC for Lossless queues: " << argv[i + 1] << endl;
            sim_params.enable_pfc = atoi(argv[i + 1]);
            i++;
        } else {
            cout << "Unknown option " << argv[i] << endl;
            exit_error(argv[0]);
        }
        i++;
    }
}
