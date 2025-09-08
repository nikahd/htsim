// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "AI_switch.h"

#include "callback_pipe.h"
#include "queue_lossless.h"
#include "queue_lossless_output.h"
#include "route_table.h"
#include "roce.h"   // for RocePacket, flow_id()

// ---- file-scope definitions for AISwitch static members ----
std::ofstream                AISwitch::_crumb_csv;
bool                         AISwitch::_crumb_opened = false;
std::unordered_set<uint64_t> AISwitch::_seen_pair;
// -----------------------------------------------------------

void AISwitch::open_crumb_csv_if_needed() {
    if (_crumb_opened) return;
    _crumb_csv.open("fabric_breadcrumbs.csv", std::ios::out | std::ios::app);
    if (_crumb_csv && _crumb_csv.tellp() == 0) {
        _crumb_csv << "time_ps,switch_id,switch_name,switch_type,flow_id\n";
    }
    _crumb_opened = true;
}

void AISwitch::breadcrumb_once_per_flow(flowid_t flow_id) {
    open_crumb_csv_if_needed();
    const uint64_t key = make_key(get_id(), flow_id);
    if (_seen_pair.insert(key).second) {
        // first time this (switch,flow) seen
        _crumb_csv
            << eventlist().now() << ","
            << get_id() << ","
            << nodename() << ","
            << (int)_type  << ","   // or a string if you have one; e.g., typeToString(_type)
            << flow_id
            << "\n";
        // no flush per line; append is fine
    }
}

int AISwitch::precision_ts = 1;

AISwitch::AISwitch(EventList&      eventlist,
                   string          switch_name,
                   node_type       type,
                   uint64_t        switch_id,
                   simtime_picosec delay,
                   AIDCMsoft*      dc_topo,
                   uint64_t        dc_id,
                   uint64_t        region_id)
    : Switch(eventlist, switch_name) {
    this->_id          = switch_id;
    this->_type        = type;
    this->_pipe        = new CallbackPipe(delay, eventlist, this);
    this->_uproutes    = NULL;
    this->dc_topo      = dc_topo;
    this->_crt_route   = 0;
    this->_hash_salt   = random();
    this->_last_choice = eventlist.now();
    this->_fib         = new RouteTable();
    this->dc_id        = dc_id;
    this->region_id    = region_id;

    // fill switch info
    switch (this->_type) {
        case TOR:
            this->switch_info = this->dc_topo->parseToRSwitchId(this->_id);
            break;
        case LEAF:
            this->switch_info = this->dc_topo->parseLeafSwitchId(this->_id);
            break;
        case SPINE:
            this->switch_info = this->dc_topo->parseSpineSwitchId(this->_id);
            break;
        default:
            cout << "Switch is not for a DC!" << endl;
            abort();
            break;
    }
}

AISwitch::AISwitch(EventList&      eventlist,
                   string          switch_name,
                   node_type       type,
                   uint64_t        switch_id,
                   simtime_picosec delay,
                   AIRegionMsoft*  region_topo,
                   uint64_t        region_id)
    : Switch(eventlist, switch_name) {
    this->_id          = switch_id;
    this->_type        = type;
    this->_pipe        = new CallbackPipe(delay, eventlist, this);
    this->_uproutes    = NULL;
    this->region_topo  = region_topo;
    this->_crt_route   = 0;
    this->_hash_salt   = random();
    this->_last_choice = eventlist.now();
    this->_fib         = new RouteTable();
    this->region_id    = region_id;

    // fill switch info
    switch (this->_type) {
        case RH:
            this->switch_info = this->region_topo->parseRHSwitchId(this->_id);
            break;
        default:
            cout << "Switch is not region hub!" << endl;
            abort();
            break;
    }
}

AISwitch::AISwitch(EventList&      eventlist,
                   string          switch_name,
                   node_type       type,
                   uint64_t        switch_id,
                   simtime_picosec delay,
                   AIWANMsoft*     wan_topo,
                   uint64_t        region_id)
    : Switch(eventlist, switch_name) {
    this->_id          = switch_id;
    this->_type        = type;
    this->_pipe        = new CallbackPipe(delay, eventlist, this);
    this->_uproutes    = NULL;
    this->wan_topo     = wan_topo;
    this->_crt_route   = 0;
    this->_hash_salt   = random();
    this->_last_choice = eventlist.now();
    this->_fib         = new RouteTable();
    this->region_id    = region_id;

    // fill switch info
    switch (this->_type) {
        case RWA:
            this->switch_info = this->wan_topo->parseRWASwitchId(this->_id);
            break;
        case OWR:
            this->switch_info = this->wan_topo->parseOWRSwitchId(this->_id);
            break;
        default:
            cout << "Switch is not for WAN backbone!" << endl;
            abort();
            break;
    }
}

void AISwitch::receivePacket(Packet& pkt) {
    // std::cout << nodename() << ": received packet called!"
    //           << " time (us): " << timeAsUs(eventlist().now()) << endl;

    // Handle PAUSE frames first (unchanged)
    if (pkt.type() == ETH_PAUSE) {
        EthPausePacket* p = (EthPausePacket*)&pkt;
        // I must be in lossless mode!
        // find the egress queue that should process this, and pass it over for processing.
        printf("Received a Pause Packet\n");
        for (size_t i = 0; i < _ports.size(); i++) {
            LosslessQueue* q = (LosslessQueue*)_ports.at(i);
            if (q->getRemoteEndpoint() &&
                ((Switch*)q->getRemoteEndpoint())->getID() == p->senderID()) {
                q->receivePacket(pkt);
                break;
            }
        }
        return;
    }

    // --- BEGIN: fabric breadcrumb (once per (switch,flow)) ---
    {
        flowid_t fid = 0;
        if (RocePacket* rp = dynamic_cast<RocePacket*>(&pkt)) {
            fid = rp->flow_id();
        }
        if (fid) {
            breadcrumb_once_per_flow(fid);
        }
    }
    // --- END: fabric breadcrumb ---

    // === Original ingress/egress pipeline (keep it inside the function) ===
    if (_packets.find(&pkt) == _packets.end()) {
        // ingress pipeline processing.
        _packets[&pkt] = true;
        receive_packet_counter++;

        const Route* nh = getNextHop(pkt, NULL); // set next hop peer switch
        pkt.set_route(*nh);

        // emulate switching latency before enqueue on egress queue
        _pipe->receivePacket(pkt);
    } else {
        // egress pipeline processing.
        _packets.erase(&pkt);
        // cout << "Switch type " << _type <<  " id " << _id << " pkt dst "
        //      << pkt.dst() << " dir " << pkt.get_direction() << endl;
        pkt.sendOn();
    }
}


void AISwitch::addHostPort(int addr, int flowid, PacketSink* transport) {
    if (this->_type != TOR) {
        cout << "Adding host to a switch that is not ToR?" << endl;
        abort();
    }
    uint64_t                                      server_id = dc_topo->getServerId(addr);
    tuple<uint64_t, uint64_t, uint64_t, uint64_t> queue_and_pipe_id =
        make_tuple(this->_id, 0, server_id, 0);
    auto   tor_to_server_queue = dc_topo->get_queue(queue_and_pipe_id);
    auto   tor_to_server_pipe  = dc_topo->get_pipe(queue_and_pipe_id);
    Route* rt                  = new Route();
    rt->push_back(tor_to_server_queue);
    rt->push_back(tor_to_server_pipe);
    rt->push_back(transport);
    _fib->addHostRoute(addr, rt, flowid);
}

uint32_t AISwitch::adaptive_route_p2c(vector<FibEntry*>* ecmp_set,
                                      int8_t (*cmp)(FibEntry*, FibEntry*)) {
    uint32_t              choice = 0, min = UINT32_MAX;
    uint32_t              start, i        = 0;
    static const uint16_t nr_choices = 2;

    do {
        start = random() % ecmp_set->size();

        Route* r = (*ecmp_set)[start]->getEgressPort();
        assert(r && r->size() > 1);
        BaseQueue* q = (BaseQueue*)(r->at(0));
        assert(q);
        if (q->queuesize() < min) {
            choice = start;
            min    = q->queuesize();
        }
        i++;
    } while (i < nr_choices);
    return choice;
}

uint32_t AISwitch::adaptive_route(vector<FibEntry*>* ecmp_set,
                                  int8_t (*cmp)(FibEntry*, FibEntry*)) {
    uint32_t choice = 0;

    uint32_t best_choices[256];
    uint32_t best_choices_count = 0;

    FibEntry* min                      = (*ecmp_set)[choice];
    best_choices[best_choices_count++] = choice;

    for (uint32_t i = 1; i < ecmp_set->size(); i++) {
        int8_t c = cmp(min, (*ecmp_set)[i]);

        if (c < 0) {
            choice                             = i;
            min                                = (*ecmp_set)[choice];
            best_choices_count                 = 0;
            best_choices[best_choices_count++] = choice;
        } else if (c == 0) {
            assert(best_choices_count < 256);
            best_choices[best_choices_count++] = i;
        }
    }

    assert(best_choices_count >= 1);
    choice = best_choices[random() % best_choices_count];
    return choice;
}

uint32_t AISwitch::replace_worst_choice(vector<FibEntry*>* ecmp_set,
                                        int8_t (*cmp)(FibEntry*, FibEntry*),
                                        uint32_t my_choice) {
    uint32_t best_choice  = 0;
    uint32_t worst_choice = 0;

    uint32_t best_choices[256];
    uint32_t best_choices_count = 0;

    FibEntry* min                      = (*ecmp_set)[best_choice];
    FibEntry* max                      = (*ecmp_set)[worst_choice];
    best_choices[best_choices_count++] = best_choice;

    for (uint32_t i = 1; i < ecmp_set->size(); i++) {
        int8_t c = cmp(min, (*ecmp_set)[i]);

        if (c < 0) {
            best_choice                        = i;
            min                                = (*ecmp_set)[best_choice];
            best_choices_count                 = 0;
            best_choices[best_choices_count++] = best_choice;
        } else if (c == 0) {
            assert(best_choices_count < 256);
            best_choices[best_choices_count++] = i;
        }

        if (cmp(max, (*ecmp_set)[i]) > 0) {
            worst_choice = i;
            max          = (*ecmp_set)[worst_choice];
        }
    }

    // might need to play with different alternatives here, compare to worst
    // rather than just to worst index.
    int8_t r = cmp((*ecmp_set)[my_choice], (*ecmp_set)[worst_choice]);
    assert(r >= 0);

    if (r == 0) {
        assert(best_choices_count >= 1);
        return best_choices[random() % best_choices_count];
    } else
        return my_choice;
}

int8_t AISwitch::compare_pause(FibEntry* left, FibEntry* right) {
    Route* r1 = left->getEgressPort();
    assert(r1 && r1->size() > 1);
    LosslessOutputQueue* q1 = dynamic_cast<LosslessOutputQueue*>(r1->at(0));
    Route*               r2 = right->getEgressPort();
    assert(r2 && r2->size() > 1);
    LosslessOutputQueue* q2 = dynamic_cast<LosslessOutputQueue*>(r2->at(0));

    if (!q1->is_paused() && q2->is_paused())
        return 1;
    else if (q1->is_paused() && !q2->is_paused())
        return -1;
    else
        return 0;
}

int8_t AISwitch::compare_queuesize(FibEntry* left, FibEntry* right) {
    Route* r1 = left->getEgressPort();
    assert(r1 && r1->size() > 1);
    BaseQueue* q1 = dynamic_cast<BaseQueue*>(r1->at(0));
    Route*     r2 = right->getEgressPort();
    assert(r2 && r2->size() > 1);
    BaseQueue* q2 = dynamic_cast<BaseQueue*>(r2->at(0));

    if (q1->quantized_queuesize() < q2->quantized_queuesize())
        return 1;
    else if (q1->quantized_queuesize() > q2->quantized_queuesize())
        return -1;
    else
        return 0;
}

int8_t AISwitch::compare_bandwidth(FibEntry* left, FibEntry* right) {
    Route* r1 = left->getEgressPort();
    assert(r1 && r1->size() > 1);
    BaseQueue* q1 = dynamic_cast<BaseQueue*>(r1->at(0));
    Route*     r2 = right->getEgressPort();
    assert(r2 && r2->size() > 1);
    BaseQueue* q2 = dynamic_cast<BaseQueue*>(r2->at(0));

    if (q1->quantized_utilization() < q2->quantized_utilization())
        return 1;
    else if (q1->quantized_utilization() > q2->quantized_utilization())
        return -1;
    else
        return 0;
}

int8_t AISwitch::compare_pqb(FibEntry* left, FibEntry* right) {
    // compare pause, queuesize, bandwidth.
    int8_t p = compare_pause(left, right);

    if (p != 0)
        return p;

    p = compare_queuesize(left, right);

    if (p != 0)
        return p;

    return compare_bandwidth(left, right);
}

int8_t AISwitch::compare_pq(FibEntry* left, FibEntry* right) {
    // compare pause, queuesize, bandwidth.
    int8_t p = compare_pause(left, right);

    if (p != 0)
        return p;

    return compare_queuesize(left, right);
}

int8_t AISwitch::compare_qb(FibEntry* left, FibEntry* right) {
    // compare pause, queuesize, bandwidth.
    int8_t p = compare_queuesize(left, right);

    if (p != 0)
        return p;

    return compare_bandwidth(left, right);
}

int8_t AISwitch::compare_pb(FibEntry* left, FibEntry* right) {
    // compare pause, queuesize, bandwidth.
    int8_t p = compare_pause(left, right);

    if (p != 0)
        return p;

    return compare_bandwidth(left, right);
}

void AISwitch::permute_paths(vector<FibEntry*>* uproutes) {
    int len = uproutes->size();
    for (int i = 0; i < len; i++) {
        int       ix             = random() % (len - i);
        FibEntry* tmppath        = (*uproutes)[ix];
        (*uproutes)[ix]          = (*uproutes)[len - 1 - i];
        (*uproutes)[len - 1 - i] = tmppath;
    }
}

void AISwitch::permute_paths_deterministic(vector<FibEntry*>* uproutes) {
    return;
}

AISwitch::routing_strategy AISwitch::_strategy               = AISwitch::NIX;
uint16_t                   AISwitch::_ar_fraction            = 0;
uint16_t                   AISwitch::_ar_sticky              = AISwitch::PER_PACKET;
simtime_picosec            AISwitch::_sticky_delta           = timeFromUs((uint32_t)10);
double                     AISwitch::_ecn_threshold_fraction = 1.0;
int8_t (*AISwitch::fn)(FibEntry*, FibEntry*)                 = &AISwitch::compare_queuesize;

Route* AISwitch::getNextAvailableHop(Packet&            pkt,
                                     BaseQueue*         ingress_port,
                                     vector<FibEntry*>* available_hops) {
    // implement a form of ECMP hashing; might need to revisit based on
    // measured performance.
    uint32_t ecmp_choice = 0;

    // std::cout << "available hops is available: " << available_hops->size() << endl;

    assert(available_hops->size() > 0);

    if (available_hops->size() > 1)
        switch (_strategy) {
            case NIX:
                abort();
            case SINGLE:
                ecmp_choice = 0;
                break;
            case ECMP:
                ecmp_choice =
                    freeBSDHash(pkt.flow_id(), pkt.pathid(), _hash_salt) % available_hops->size();

                // Note: For evaluate CC, all subflows will use the same T1, T2, etc.
                // ecmp_choice =
                //     freeBSDHash(1, pkt.pathid(), _hash_salt) % available_hops->size();

                // cout << "Switch " << _type << ":" << _id << " choosing path " << ecmp_choice
                //      << " for flow " << pkt.flow_id() << " pathid " << pkt.pathid()
                //      << " _hash_salt " << _hash_salt << " available_hops "
                //      << available_hops->size() << endl;
                break;
            case ADAPTIVE_ROUTING:
                if (_ar_sticky == AISwitch::PER_PACKET) {
                    ecmp_choice = adaptive_route(available_hops, fn);
                } else if (_ar_sticky == AISwitch::PER_FLOWLET) {
                    if (_flowlet_maps.find(pkt.flow_id()) != _flowlet_maps.end()) {
                        FlowletInfo* f = _flowlet_maps[pkt.flow_id()];

                        // only reroute an existing flow if its inter packet
                        // time is larger than _sticky_delta and and 50% chance
                        // happens. and (commented out) if the switch has not
                        // taken any other placement decision that we've not
                        // seen the effects of.
                        if (eventlist().now() - f->_last > _sticky_delta &&
                            /*eventlist().now() - _last_choice > _pipe->delay()
                            + BaseQueue::_update_period  &&*/
                            random() % 2 == 0) {
                            uint32_t new_route = adaptive_route(available_hops, fn);
                            if (fn(available_hops->at(f->_egress), available_hops->at(new_route)) <
                                0) {
                                f->_egress   = new_route;
                                _last_choice = eventlist().now();
                                // cout << "Switch " << _type << ":" << _id << "choosing new
                                // path "<<  f->_egress << " for "
                                // << pkt.flow_id() << " at " <<
                                // timeAsUs(eventlist().now()) << " last is " <<
                                // timeAsUs(f->_last) << endl;
                            }
                        }
                        ecmp_choice = f->_egress;

                        f->_last = eventlist().now();
                    } else {
                        ecmp_choice = adaptive_route(available_hops, fn);
                        // cout << "Switch " << _type << ":" << getID() << "
                        // choosing first path "<<  ecmp_choice << " for " <<
                        // pkt.flow_id() << " at " <<
                        // timeAsUs(eventlist().now()) << endl;
                        _last_choice = eventlist().now();

                        _flowlet_maps[pkt.flow_id()] =
                            new FlowletInfo(ecmp_choice, eventlist().now());
                    }
                }

                break;
            case ECMP_ADAPTIVE:
                ecmp_choice =
                    freeBSDHash(pkt.flow_id(), pkt.pathid(), _hash_salt) % available_hops->size();
                if (random() % 100 < 50)
                    ecmp_choice = replace_worst_choice(available_hops, fn, ecmp_choice);
                break;
            case RR:
                if (_crt_route >= 5 * available_hops->size()) {
                    _crt_route = 0;
                    permute_paths(available_hops);
                }
                ecmp_choice = _crt_route % available_hops->size();
                _crt_route++;
                break;
            case RR_ECMP:
                if (_type == TOR) {
                    if (_crt_route >= 5 * available_hops->size()) {
                        _crt_route = 0;
                        permute_paths(available_hops);
                    }
                    ecmp_choice = _crt_route % available_hops->size();
                    _crt_route++;
                } else
                    ecmp_choice = freeBSDHash(pkt.flow_id(), pkt.pathid(), _hash_salt) %
                                  available_hops->size();

                break;
        }

    assert(ecmp_choice < available_hops->size());

    FibEntry* e = (*available_hops)[ecmp_choice];

    fflush(stdout);

    pkt.set_direction(e->getDirection());

    return e->getEgressPort();
}

Route* AISwitch::getNextHop(Packet& pkt, BaseQueue* ingress_port) {
    // std::cout << nodename() << ": getNextHop called!" << endl;
    // cout << "Switch " << _type << ":" << _id << endl;

    vector<FibEntry*>* available_hops = _fib->getRoutes(pkt.dst());

    if (available_hops) {
        return getNextAvailableHop(pkt, ingress_port, available_hops);
    } else {
        // std::cout << "available hops is null!" << endl;

        // no route table entries for this destination. Add them to FIB or fail.
        if ((dc_topo && region_topo) && (wan_topo || dc_topo) && (region_topo || region_topo) &&
            wan_topo) {
            cout << "How is the switch both inside DC, region, and WAN?" << endl;
            abort();
        }

        AI_WAN_topology_idx dst_idx = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
        if (dc_topo) {
            dst_idx.region_idx = (pkt.dst() / dc_topo->get_number_of_servers_per_region());
            dst_idx.dc_idx     = (pkt.dst() % dc_topo->get_number_of_servers_per_region()) /
                             dc_topo->get_number_of_servers_per_dc();
            dst_idx.leaf_group_idx = (pkt.dst() % dc_topo->get_number_of_servers_per_dc()) /
                                     dc_topo->get_number_of_servers_per_leaf_group();
            dst_idx.tor_group_idx = (pkt.dst() % dc_topo->get_number_of_servers_per_leaf_group()) /
                                    dc_topo->get_num_serv_per_torgroup();
        } else if (region_topo) {
            dst_idx.region_idx = (pkt.dst() / region_topo->get_num_total_servers_in_region());
        } else if (wan_topo) {
            dst_idx.region_idx = (pkt.dst() / wan_topo->get_number_of_servers_per_region());
        } else {
            cout << "No topology information available!" << endl;
            abort();
        }

        if (this->_type == TOR) {
            assert(dc_topo);
            if (is_dst_torgroup(&dst_idx, &switch_info)) {
                // this destination is directly connected to this ToR switch, send it down
                // cout << "[AISwitch::addAvailableHops][TOR] this destination is directly connected
                // "
                //         "to this ToR switch, send it down"
                //      << endl;
                HostFibEntry* fe =
                    _fib->getHostRoute(dc_topo->get_relative_server_idx(pkt.dst()), pkt.flow_id());
                assert(fe);
                pkt.set_direction(DOWN);
                return fe->getEgressPort();
            }
        }

        addAvailableHops(pkt, ingress_port, dst_idx);
        assert(_fib->getRoutes(pkt.dst()));

        // FIB has been filled in; return choice.
        available_hops = _fib->getRoutes(pkt.dst());

        return getNextAvailableHop(pkt, ingress_port, available_hops);
    }
};

void AISwitch::addAvailableHops(Packet&              pkt,
                                BaseQueue*           ingress_port,
                                AI_WAN_topology_idx& dst_idx) {
    if (this->_type == TOR) {
        assert(dc_topo);
        if (!is_dst_torgroup(&dst_idx, &switch_info)) {
            addAvailableHopsToRUp(pkt, ingress_port, dst_idx);
        }
    } else if (this->_type == LEAF) {
        if (is_dst_leafgroup(&dst_idx, &switch_info)) {
            addAvailableHopsLeafDown(pkt, ingress_port, dst_idx);
        } else {
            addAvailableHopsLeafUp(pkt, ingress_port, dst_idx);
        }
    } else if (this->_type == SPINE) {
        if (is_dst_dc(&dst_idx, &switch_info)) {
            addAvailableHopsSpineDown(pkt, ingress_port, dst_idx);
        } else {
            addAvailableHopsSpineUp(pkt, ingress_port, dst_idx);
        }
    } else if (this->_type == RH) {
        if (is_dst_region(&dst_idx, &switch_info)) {
            addAvailableHopsRHDown(pkt, ingress_port, dst_idx);
        } else {
            addAvailableHopsRHUp(pkt, ingress_port, dst_idx);
        }
    } else if (this->_type == RWA) {
        if (is_dst_region(&dst_idx, &switch_info)) {
            addAvailableHopsRWADown(pkt, ingress_port, dst_idx);
        } else {
            addAvailableHopsRWAUp(pkt, ingress_port, dst_idx);
        }
    } else if (this->_type == OWR) {
        if (is_dst_region(&dst_idx, &switch_info)) {
            addAvailableHopsOWRDown(pkt, ingress_port, dst_idx);
        } else {
            addAvailableHopsOWRUp(pkt, ingress_port, dst_idx);
        }
    } else {
        cerr << "Route lookup on switch with no proper type: " << _type << endl;
        abort();
    }
}

void AISwitch::addAvailableHopsToRUp(Packet&              pkt,
                                     BaseQueue*           ingress_port,
                                     AI_WAN_topology_idx& dst_idx) {
    // route packet up at ToR!
    // cout << "[AISwitch::addAvailableHops][TOR] route packet up!" << endl;

    assert(dc_topo);

    if (_uproutes)
        _fib->setRoutes(pkt.dst(), _uproutes);
    else {
        // connect ToR to leaf switches in that specific leaf group
        for (uint64_t k = 0; k < dc_topo->get_num_leafs_per_leafgroup(); k++) {
            uint64_t target_leaf_switch_id = dc_topo->getLeafSwitchId(
                switch_info.region_idx, switch_info.datacenter_idx, switch_info.leaf_group_idx, k);
            dc_topo->checkSwitchExists(target_leaf_switch_id);
            // Sending up: bundle size is corresponding to leaf tier
            for (uint64_t bundle_idx = 0; bundle_idx < dc_topo->bundlesize[LEAF_TIER];
                 bundle_idx++) {
                tuple<uint64_t, uint64_t, uint64_t, uint64_t> tor_to_leaf_tuple =
                    make_tuple(this->_id, bundle_idx, target_leaf_switch_id, bundle_idx);
                dc_topo->checkQueueExists(tor_to_leaf_tuple);
                dc_topo->checkPipeExists(tor_to_leaf_tuple);
                auto   queue_itr = dc_topo->queue_map.find(tor_to_leaf_tuple);
                auto   pipe_itr  = dc_topo->pipe_map.find(tor_to_leaf_tuple);
                Route* r         = new Route();
                r->push_back(queue_itr->second);
                assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                r->push_back(pipe_itr->second);
                r->push_back(queue_itr->second->getRemoteEndpoint());
                _fib->addRoute(pkt.dst(), r, 1, UP);
            }
        }
        _uproutes = _fib->getRoutes(pkt.dst());
        permute_paths_deterministic(_uproutes);
    }
}

void AISwitch::addAvailableHopsLeafDown(Packet&              pkt,
                                        BaseQueue*           ingress_port,
                                        AI_WAN_topology_idx& dst_idx) {
    assert(dc_topo);
    // send the packet down if the destination is under the same leaf group. Send to
    // destination Tor/Tor group
    // cout << "[AISwitch::addAvailableHops][LEAF] send the packet down if the destination is under
    // "
    //         "the same leaf group. Send to destination Tor/Tor group"
    //      << endl;
    for (uint64_t k = 0; k < dc_topo->get_num_tor_per_torgroups(); k++) {
        // Note: this is for evaluating CC, disable the second ToR
        // if (k % 2 == 1) {
        //     continue;
        // }
        // End of evaluating CC

        uint64_t target_tor_id = dc_topo->getToRSwitchId(switch_info.region_idx,
                                                         switch_info.datacenter_idx,
                                                         switch_info.leaf_group_idx,
                                                         dst_idx.tor_group_idx,
                                                         k);
        dc_topo->checkSwitchExists(target_tor_id);
        // sending down: bundle size is corresponding to leaf tier
        for (uint64_t bundle_idx = 0; bundle_idx < dc_topo->bundlesize[LEAF_TIER]; bundle_idx++) {
            tuple<uint64_t, uint64_t, uint64_t, uint64_t> leaf_to_tor_tuple =
                make_tuple(this->_id, bundle_idx, target_tor_id, bundle_idx);
            dc_topo->checkQueueExists(leaf_to_tor_tuple);
            dc_topo->checkPipeExists(leaf_to_tor_tuple);
            auto   queue_itr = dc_topo->queue_map.find(leaf_to_tor_tuple);
            auto   pipe_itr  = dc_topo->pipe_map.find(leaf_to_tor_tuple);
            Route* r         = new Route();
            r->push_back(queue_itr->second);
            assert(((BaseQueue*)r->at(0))->getSwitch() == this);

            r->push_back(pipe_itr->second);
            r->push_back(queue_itr->second->getRemoteEndpoint());
            _fib->addRoute(pkt.dst(), r, 1, DOWN);
        }
    }
}

void AISwitch::addAvailableHopsLeafUp(Packet&              pkt,
                                      BaseQueue*           ingress_port,
                                      AI_WAN_topology_idx& dst_idx) {
    assert(dc_topo);
    // send packet up to the spine tier
    // cout << "[AISwitch::addAvailableHops][LEAF] send packet up to the spine tier" << endl;
    if (_uproutes)
        _fib->setRoutes(pkt.dst(), _uproutes);
    else {
        for (uint64_t k = 0; k < dc_topo->get_num_spine_per_leaf_in_leafgroup(); k++) {
            // k is the offset
            unsigned int spine_idx =
                dc_topo->get_spine_offset_mapped_to_leaf(switch_info.leaf_idx) + k;
            uint64_t target_spine_switch_id = dc_topo->getSpineSwitchId(
                switch_info.region_idx, switch_info.datacenter_idx, spine_idx);
            dc_topo->checkSwitchExists(target_spine_switch_id);
            for (uint64_t bundle_idx = 0; bundle_idx < dc_topo->bundlesize[SPINE_TIER];
                 bundle_idx++) {
                tuple<uint64_t, uint64_t, uint64_t, uint64_t> leaf_to_spine_tuple =
                    make_tuple(this->_id, bundle_idx, target_spine_switch_id, bundle_idx);
                dc_topo->checkQueueExists(leaf_to_spine_tuple);
                dc_topo->checkPipeExists(leaf_to_spine_tuple);
                auto   queue_itr = dc_topo->queue_map.find(leaf_to_spine_tuple);
                auto   pipe_itr  = dc_topo->pipe_map.find(leaf_to_spine_tuple);
                Route* r         = new Route();
                r->push_back(queue_itr->second);
                assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                r->push_back(pipe_itr->second);
                r->push_back(queue_itr->second->getRemoteEndpoint());
                _fib->addRoute(pkt.dst(), r, 1, UP);
            }
        }
        _uproutes = _fib->getRoutes(pkt.dst());
        permute_paths_deterministic(_uproutes);
    }
}

void AISwitch::addAvailableHopsSpineDown(Packet&              pkt,
                                         BaseQueue*           ingress_port,
                                         AI_WAN_topology_idx& dst_idx) {
    assert(dc_topo);
    // send the packet down if the destination is in the same DC
    // cout << "[AISwitch::addAvailableHops][SPINE] send the packet down if the destination is in"
    //      << "the same DC" << endl;
    unsigned int target_leaf_idx = dc_topo->get_leaf_idx_mapped_to_spine(switch_info.spine_idx);
    uint64_t     target_leaf_id  = dc_topo->getLeafSwitchId(switch_info.region_idx,
                                                       switch_info.datacenter_idx,
                                                       dst_idx.leaf_group_idx,
                                                       target_leaf_idx);
    dc_topo->checkSwitchExists(target_leaf_id);
    // sending down: bundle size is corresponding to spine tier
    for (uint64_t bundle_idx = 0; bundle_idx < dc_topo->bundlesize[SPINE_TIER]; bundle_idx++) {
        tuple<uint64_t, uint64_t, uint64_t, uint64_t> spine_to_leaf_tuple =
            make_tuple(this->_id, bundle_idx, target_leaf_id, bundle_idx);
        dc_topo->checkQueueExists(spine_to_leaf_tuple);
        dc_topo->checkPipeExists(spine_to_leaf_tuple);
        auto   queue_itr = dc_topo->queue_map.find(spine_to_leaf_tuple);
        auto   pipe_itr  = dc_topo->pipe_map.find(spine_to_leaf_tuple);
        Route* r         = new Route();
        r->push_back(queue_itr->second);
        assert(((BaseQueue*)r->at(0))->getSwitch() == this);

        r->push_back(pipe_itr->second);
        r->push_back(queue_itr->second->getRemoteEndpoint());
        _fib->addRoute(pkt.dst(), r, 1, DOWN);
    }
}

void AISwitch::addAvailableHopsSpineUp(Packet&              pkt,
                                       BaseQueue*           ingress_port,
                                       AI_WAN_topology_idx& dst_idx) {
    assert(dc_topo);
    // send packet up to the region hub tier
    // cout << "[AISwitch::addAvailableHops][SPINE] send packet up to the region hub tier" << endl;
    if (_uproutes)
        _fib->setRoutes(pkt.dst(), _uproutes);
    else {
        assert(dc_topo->region);
        unsigned int rh_batch_idx_mapped_to_spine =
            dc_topo->region->get_RH_batch_idx_mapped_to_spine(switch_info.spine_idx);
        for (unsigned int rh_group_idx = 0; rh_group_idx < dc_topo->region->get_num_RH_groups();
             rh_group_idx++) {
            for (unsigned int rh_switch_idx = 0;
                 rh_switch_idx < dc_topo->region->get_num_RH_switches_per_RH_batch();
                 rh_switch_idx++) {
                uint64_t target_rh_switch_id =
                    dc_topo->region->getRHSwitchId(switch_info.region_idx,
                                                   rh_group_idx,
                                                   rh_batch_idx_mapped_to_spine,
                                                   rh_switch_idx);
                dc_topo->region->checkSwitchExists(target_rh_switch_id);
                for (uint64_t bundle_idx = 0;
                     bundle_idx < dc_topo->region->region_bundlesize[RH_TIER];
                     bundle_idx++) {
                    tuple<uint64_t, uint64_t, uint64_t, uint64_t> spine_to_rh_tuple =
                        make_tuple(this->_id, bundle_idx, target_rh_switch_id, bundle_idx);
                    dc_topo->region->checkQueueExists(spine_to_rh_tuple);
                    dc_topo->region->checkPipeExists(spine_to_rh_tuple);
                    auto   queue_itr = dc_topo->region->region_queue_map.find(spine_to_rh_tuple);
                    auto   pipe_itr  = dc_topo->region->region_pipe_map.find(spine_to_rh_tuple);
                    Route* r         = new Route();
                    r->push_back(queue_itr->second);
                    assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                    r->push_back(pipe_itr->second);
                    r->push_back(queue_itr->second->getRemoteEndpoint());
                    _fib->addRoute(pkt.dst(), r, 1, UP);
                }
            }
        }
        _uproutes = _fib->getRoutes(pkt.dst());
        permute_paths_deterministic(_uproutes);
    }
}

void AISwitch::addAvailableHopsRHDown(Packet&              pkt,
                                      BaseQueue*           ingress_port,
                                      AI_WAN_topology_idx& dst_idx) {
    assert(region_topo);
    // send packet down to the destination dc
    // cout << "[AISwitch::addAvailableHops][RH] send packet down to the destination dc" << endl;
    auto target_dc = region_topo->get_dc_for_server(pkt.dst());
    for (unsigned int spine_idx = 0; spine_idx < target_dc->get_num_spine_per_dc(); spine_idx++) {
        unsigned int rh_batch_idx_mapped_to_spine =
            region_topo->get_RH_batch_idx_mapped_to_spine(spine_idx);
        if (rh_batch_idx_mapped_to_spine == switch_info.rh_batch_idx) {
            uint64_t target_spine_switch_id = dc_topo->getSpineSwitchId(
                switch_info.region_idx, target_dc->get_dc_idx(), spine_idx);
            target_dc->checkSwitchExists(target_spine_switch_id);
            for (uint64_t bundle_idx = 0; bundle_idx < region_topo->region_bundlesize[RH_TIER];
                 bundle_idx++) {
                tuple<uint64_t, uint64_t, uint64_t, uint64_t> rh_to_spine_tuple =
                    make_tuple(this->_id, bundle_idx, target_spine_switch_id, bundle_idx);
                region_topo->checkQueueExists(rh_to_spine_tuple);
                region_topo->checkPipeExists(rh_to_spine_tuple);
                auto   queue_itr = region_topo->region_queue_map.find(rh_to_spine_tuple);
                auto   pipe_itr  = region_topo->region_pipe_map.find(rh_to_spine_tuple);
                Route* r         = new Route();
                r->push_back(queue_itr->second);
                assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                r->push_back(pipe_itr->second);
                r->push_back(queue_itr->second->getRemoteEndpoint());
                _fib->addRoute(pkt.dst(), r, 1, DOWN);
            }
        }
    }
}

void AISwitch::addAvailableHopsRHUp(Packet&              pkt,
                                    BaseQueue*           ingress_port,
                                    AI_WAN_topology_idx& dst_idx) {
    assert(region_topo);
    // send packet up to the RWA tier
    // cout << "[AISwitch::addAvailableHops][RH] send packet up to the RWA tier" << endl;

    if (_uproutes)
        _fib->setRoutes(pkt.dst(), _uproutes);
    else {
        assert(region_topo->wan);
        unsigned int rwa_group_id =
            region_topo->wan->get_RWA_group_id_mapped_to_RH_group(switch_info.rh_group_idx);
        for (unsigned int rwa_switch_idx = 0;
             rwa_switch_idx < region_topo->wan->get_num_RWA_switches_per_RWA_group();
             rwa_switch_idx++) {
            uint64_t target_rwa_switch_id = region_topo->wan->getRWASwitchId(
                switch_info.region_idx, rwa_group_id, rwa_switch_idx);
            region_topo->wan->checkSwitchExists(target_rwa_switch_id);

            for (unsigned int link_idx = 0; link_idx < region_topo->wan->get_num_links_per_RH_RWA();
                 link_idx++) {
                tuple<uint64_t, uint64_t, uint64_t> rh_to_rwa_tuple =
                    make_tuple(this->_id, target_rwa_switch_id, link_idx);
                region_topo->wan->checkQueueExists(rh_to_rwa_tuple);
                region_topo->wan->checkPipeExists(rh_to_rwa_tuple);
                auto   queue_itr = region_topo->wan->wan_queue_map.find(rh_to_rwa_tuple);
                auto   pipe_itr  = region_topo->wan->wan_pipe_map.find(rh_to_rwa_tuple);
                Route* r         = new Route();
                r->push_back(queue_itr->second);
                assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                r->push_back(pipe_itr->second);
                r->push_back(queue_itr->second->getRemoteEndpoint());
                _fib->addRoute(pkt.dst(), r, 1, UP);
            }
        }
        _uproutes = _fib->getRoutes(pkt.dst());
        permute_paths_deterministic(_uproutes);
    }
}

void AISwitch::addAvailableHopsRWADown(Packet&              pkt,
                                       BaseQueue*           ingress_port,
                                       AI_WAN_topology_idx& dst_idx) {
    assert(wan_topo);
    // send packet down to the destination RH
    // cout << "[AISwitch::addAvailableHops][RWA] send packet down to the destination RH" << endl;
    auto         target_region = wan_topo->get_region_for_server(pkt.dst());
    unsigned int rh_group_idx =
        wan_topo->get_RH_group_idx_mapped_to_RWA_group(switch_info.rwa_group_idx);

    for (unsigned int rh_batch_idx = 0;
         rh_batch_idx < target_region->get_num_RH_switch_batches_per_RH_group();
         rh_batch_idx++) {
        for (unsigned int rh_switch_idx = 0;
             rh_switch_idx < target_region->get_num_RH_switches_per_RH_batch();
             rh_switch_idx++) {
            uint64_t target_rh_switch_id = target_region->getRHSwitchId(
                switch_info.region_idx, rh_group_idx, rh_batch_idx, rh_switch_idx);
            target_region->checkSwitchExists(target_rh_switch_id);

            for (unsigned int link_idx = 0; link_idx < wan_topo->get_num_links_per_RH_RWA();
                 link_idx++) {
                tuple<uint64_t, uint64_t, uint64_t> rwa_to_rh_tuple =
                    make_tuple(this->_id, target_rh_switch_id, link_idx);
                wan_topo->checkQueueExists(rwa_to_rh_tuple);
                wan_topo->checkPipeExists(rwa_to_rh_tuple);
                auto   queue_itr = wan_topo->wan_queue_map.find(rwa_to_rh_tuple);
                auto   pipe_itr  = wan_topo->wan_pipe_map.find(rwa_to_rh_tuple);
                Route* r         = new Route();
                r->push_back(queue_itr->second);
                assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                r->push_back(pipe_itr->second);
                r->push_back(queue_itr->second->getRemoteEndpoint());
                _fib->addRoute(pkt.dst(), r, 1, DOWN);
            }
        }
    }
}

void AISwitch::addAvailableHopsRWAUp(Packet&              pkt,
                                     BaseQueue*           ingress_port,
                                     AI_WAN_topology_idx& dst_idx) {
    assert(wan_topo);
    // send packet up to the OWR tier
    // cout << "[AISwitch::addAvailableHops][RWA] send packet up to the OWR tier" << endl;
    if (_uproutes)
        _fib->setRoutes(pkt.dst(), _uproutes);
    else {
        unsigned int owr_group_id =
            wan_topo->get_OWR_group_id_mapped_to_RWA_group(switch_info.rwa_group_idx);
        for (unsigned int owr_switch_idx = 0;
             owr_switch_idx < wan_topo->get_num_OWR_switches_per_OWR_group();
             owr_switch_idx++) {
            uint64_t target_owr_switch_id =
                wan_topo->getOWRSwitchId(switch_info.region_idx, owr_group_id, owr_switch_idx);
            wan_topo->checkSwitchExists(target_owr_switch_id);

            for (unsigned int link_idx = 0; link_idx < wan_topo->get_num_links_per_RWA_OWR();
                 link_idx++) {
                tuple<uint64_t, uint64_t, uint64_t> rwa_to_owr_tuple =
                    make_tuple(this->_id, target_owr_switch_id, link_idx);
                wan_topo->checkQueueExists(rwa_to_owr_tuple);
                wan_topo->checkPipeExists(rwa_to_owr_tuple);
                auto   queue_itr = wan_topo->wan_queue_map.find(rwa_to_owr_tuple);
                auto   pipe_itr  = wan_topo->wan_pipe_map.find(rwa_to_owr_tuple);
                Route* r         = new Route();
                r->push_back(queue_itr->second);
                assert(((BaseQueue*)r->at(0))->getSwitch() == this);

                r->push_back(pipe_itr->second);
                r->push_back(queue_itr->second->getRemoteEndpoint());
                _fib->addRoute(pkt.dst(), r, 1, UP);
            }
        }
        _uproutes = _fib->getRoutes(pkt.dst());
        permute_paths_deterministic(_uproutes);
    }
}

void AISwitch::addAvailableHopsOWRDown(Packet&              pkt,
                                       BaseQueue*           ingress_port,
                                       AI_WAN_topology_idx& dst_idx) {
    assert(wan_topo);
    // send packet down to the destination RWA
    // cout << "[AISwitch::addAvailableHops][OWR] send packet down to the destination RWA" << endl;

    unsigned int rwa_group_idx =
        wan_topo->get_RWA_group_idx_mapped_to_OWR_group(switch_info.owr_group_idx);

    for (unsigned int rwa_switch_idx = 0;
         rwa_switch_idx < wan_topo->get_num_RWA_switches_per_RWA_group();
         rwa_switch_idx++) {
        uint64_t target_rwa_switch_id =
            wan_topo->getRWASwitchId(switch_info.region_idx, rwa_group_idx, rwa_switch_idx);
        wan_topo->checkSwitchExists(target_rwa_switch_id);

        for (unsigned int link_idx = 0; link_idx < wan_topo->get_num_links_per_RWA_OWR();
             link_idx++) {
            tuple<uint64_t, uint64_t, uint64_t> owr_to_rwa_tuple =
                make_tuple(this->_id, target_rwa_switch_id, link_idx);
            wan_topo->checkQueueExists(owr_to_rwa_tuple);
            wan_topo->checkPipeExists(owr_to_rwa_tuple);
            auto   queue_itr = wan_topo->wan_queue_map.find(owr_to_rwa_tuple);
            auto   pipe_itr  = wan_topo->wan_pipe_map.find(owr_to_rwa_tuple);
            Route* r         = new Route();
            r->push_back(queue_itr->second);
            assert(((BaseQueue*)r->at(0))->getSwitch() == this);

            r->push_back(pipe_itr->second);
            r->push_back(queue_itr->second->getRemoteEndpoint());
            _fib->addRoute(pkt.dst(), r, 1, DOWN);
        }
    }
}

void AISwitch::addAvailableHopsOWRUp(Packet&              pkt,
                                     BaseQueue*           ingress_port,
                                     AI_WAN_topology_idx& dst_idx) {
    assert(wan_topo);
    // send packet up to another OWR
    // cout << "[AISwitch::addAvailableHops][OWR] send packet up to another OWR" << endl;
    if (_uproutes)
        _fib->setRoutes(pkt.dst(), _uproutes);
    else {
        auto target_region = wan_topo->get_region_for_server(pkt.dst());
        // TODO: currently only support all OWRs are connected directly to each other
        unsigned int target_owr_group_idx  = switch_info.owr_group_idx;
        unsigned int target_owr_switch_idx = switch_info.owr_switch_idx;
        uint64_t     target_owr_switch_id  = wan_topo->getOWRSwitchId(
            target_region->get_region_idx(), target_owr_group_idx, target_owr_switch_idx);

        wan_topo->checkSwitchExists(target_owr_switch_id);

        for (unsigned int link_idx = 0;
             link_idx < wan_topo->get_num_links_between_regions(switch_info.region_idx,
                                                                target_region->get_region_idx());
             link_idx++) {
            tuple<uint64_t, uint64_t, uint64_t> owr_to_owr_tuple =
                make_tuple(this->_id, target_owr_switch_id, link_idx);
            wan_topo->checkQueueExists(owr_to_owr_tuple);
            wan_topo->checkPipeExists(owr_to_owr_tuple);
            auto   queue_itr = wan_topo->wan_queue_map.find(owr_to_owr_tuple);
            auto   pipe_itr  = wan_topo->wan_pipe_map.find(owr_to_owr_tuple);
            Route* r         = new Route();
            r->push_back(queue_itr->second);
            assert(((BaseQueue*)r->at(0))->getSwitch() == this);
            r->push_back(pipe_itr->second);
            r->push_back(queue_itr->second->getRemoteEndpoint());
            _fib->addRoute(pkt.dst(), r, 1, UP);
        }
        _uproutes = _fib->getRoutes(pkt.dst());
        permute_paths_deterministic(_uproutes);
    }
}
