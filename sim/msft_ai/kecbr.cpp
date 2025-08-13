// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "kecbr.h"

#include <algorithm>
#include <iostream>
#include <queue>

#include "cbrpacket.h"
#include "math.h"
#include "uec_src.h"

string KECbrSrc::_ec_algorithm  = "KECBR";
string KECbrSink::_ec_algorithm = "KECBR";

RouteStrategy KECbrSrc::_route_strategy  = NOT_SET;
RouteStrategy KECbrSink::_route_strategy = NOT_SET;

bool           KECbrSrc::use_erasure_coding  = false;
bool           KECbrSink::use_erasure_coding = false;
ErasureCoding* KECbrSrc::_erasure_coding     = NULL;
ErasureCoding* KECbrSink::_erasure_coding    = NULL;

////////////////////////////////////////////////////////////////
//  CBR SOURCE
////////////////////////////////////////////////////////////////

KECbrSrc::KECbrSrc(EventList&          eventlist,
                   linkspeed_bps       rate,
                   ofstream&           statistics_outfile,
                   Packet::PktPriority priority)
    : EventSource(eventlist, "cbrsrc"),
      _bitrate(rate),
      _crt_id(1),
      _mss(UecSrc::_mtu),
      _flow(NULL),
      _statistics_outfile(statistics_outfile) {
    _period     = (simtime_picosec)((pow(10.0, 12.0) * 8 * _mss) / _bitrate);
    _sink       = NULL;
    _route      = NULL;
    _priority   = priority;
    _dstaddr    = UINT32_MAX;
    _sent_bytes = 0;
    _hash_salt  = random();
}

void KECbrSrc::connect(Route*          routeout,
                       Route*          routeback,
                       KECbrSink&      sink,
                       simtime_picosec starttime) {
    _route = routeout;
    _sink  = &sink;
    _flow.set_id(get_id());  // identify the packet flow with the CBR source that generated it
    _sink->connect(*this, routeback);

    _sack_restart_timeout = eventlist().now() + _restart_timeout;
    eventlist().sourceIsPending(*this, starttime);
}

void KECbrSrc::update_offsets_ec(uint32_t msn,
                                 uint32_t stripe_offset,
                                 uint32_t pkt_offset,
                                 uint32_t stripe_num) {
    uint32_t next_pkt_offset      = pkt_offset + 1;
    uint32_t next_stripe_offset   = stripe_offset;
    uint32_t next_msn             = msn;
    uint32_t next_stripe_num      = stripe_num;
    uint32_t next_last_psn_in_msg = 0xFFFFFFF;
    if (next_pkt_offset >= _erasure_coding->get_chunk_size()) {
        next_pkt_offset = 0;
        next_stripe_offset += 1;
        if (next_stripe_offset >= stripe_num) {
            message_list.pop_front();
            if (message_list.empty()) {
                return;
            }
            next_msn           = message_list.front();
            next_stripe_offset = 0;
            next_stripe_num    = _erasure_coding->get_stripe_num();
        }
    }
    next_last_psn_in_msg = next_msn * _erasure_coding->get_message_size_pkts() +
                           next_stripe_num * _erasure_coding->get_chunk_size() - 1;
    original_packet_list.push_back(make_tuple(
        next_msn, next_stripe_offset, next_pkt_offset, next_stripe_num, next_last_psn_in_msg));
}

void KECbrSrc::update_retrans_offsets_ec(uint32_t msn,
                                         uint32_t stripe_offset,
                                         uint32_t pkt_offset,
                                         uint32_t stripe_num) {
    uint32_t next_pkt_offset      = pkt_offset + 1;
    uint32_t next_stripe_offset   = stripe_offset;
    uint32_t next_stripe_num      = stripe_num;
    uint32_t next_last_psn_in_msg = 0xFFFFFFF;

    ErasureCoding::message_sack this_sack;
    uint32_t                    next_msn;
    tie(next_msn, this_sack)             = received_msg_sacks.front();
    uint32_t next_original_msn           = this_sack.msn;
    uint32_t next_original_stripe_offset = stripe_offset;

    if (next_pkt_offset >= _erasure_coding->get_chunk_size()) {
        next_pkt_offset = 0;
        next_stripe_offset += 1;
        if (next_stripe_offset >= stripe_num) {
            received_msg_sacks.pop_front();
            if (received_msg_sacks.empty()) {
                _next_retrans_msn += 1;
                return;
            }

            next_stripe_offset = 0;
            uint32_t                    next_msn;
            ErasureCoding::message_sack next_sack;
            tie(next_msn, next_sack) = received_msg_sacks.front();
            next_stripe_num          = next_sack.num_missing_stripes;
            if (next_sack.num_missing_stripes > 0) {
                _next_retrans_msn = next_msn;
                // unfinished_message_list.push_back(_next_retrans_msn);
                next_original_msn           = next_sack.msn;
                next_original_stripe_offset = next_sack.missing_stripes[0];
            }
        }
    }
    next_last_psn_in_msg = _next_retrans_msn * _erasure_coding->get_message_size_pkts() +
                           next_stripe_num * _erasure_coding->get_chunk_size() - 1;
    if (next_stripe_num > 0) {
        retransmission_packet_list.push_back(make_tuple(_next_retrans_msn,
                                                        next_stripe_offset,
                                                        next_pkt_offset,
                                                        next_stripe_num,
                                                        next_last_psn_in_msg,
                                                        next_original_msn,
                                                        next_original_stripe_offset));
    }
}

void KECbrSrc::doNextEvent() {
    if (_restart_timeout_pending) {
        _restart_timeout_pending = false;

        cout << "[CbrSrc] Restarting flow due to timeout, flow_id: " << _flow.flow_id()
             << ", now: " << timeAsUs(eventlist().now())
             << ", _sack_restart_timeout: " << timeAsUs(_sack_restart_timeout)
             << ", _restart_timeout: " << timeAsUs(_restart_timeout) << endl;

        // treat it as sack
        SACKTimeOut();

        _sack_restart_timeout = eventlist().now() + _restart_timeout;
    } else {
        start_flow();

        if (use_erasure_coding) {
            if (message_list.empty() && received_msg_sacks.empty()) {
                cout << "[CbrSrc] No packets to send, waiting for flow to complete"
                     << " flow_id: " << _flow.flow_id() << endl;
                return;
            }

            if (unfinished_message_list.empty()) {
                cerr << "[CbrSrc] flow_id: " << _flow.flow_id()
                     << " unfinished_message_list is empty but it still has messages to send, "
                        "aborting"
                     << endl;
                abort();
            }

            _cur_is_retrans = (original_packet_list.empty() && message_list.empty());
            // send all original messages first

            if (_cur_is_retrans && received_msg_sacks.empty()) {
                cout << "[CbrSrc] flow_id: " << _flow.flow_id()
                     << " No retransmission packets to send" << endl;
                return;
            }

            // The sender is always active
            if (!_cur_is_retrans) {
                // sending an original packet

                if (original_packet_list.empty() && message_list.empty()) {
                    cerr << "[CbrSrc] flow_id: " << _flow.flow_id()
                         << " original_packet_list is empty when trying to send original packet!"
                         << endl;
                    abort();
                } else if (original_packet_list.empty()) {
                    // If original_packet_list is empty but message_list is not, we need to add the
                    // next packet to original_packet_list
                    uint32_t msn             = message_list.front();
                    uint32_t stripe_offset   = 0;
                    uint32_t pkt_offset      = 0;
                    uint32_t stripe_num      = _erasure_coding->get_stripe_num();
                    uint32_t last_psn_in_msg = msn * _erasure_coding->get_message_size_pkts() +
                                               stripe_num * _erasure_coding->get_chunk_size() - 1;

                    original_packet_list.push_back(
                        make_tuple(msn, stripe_offset, pkt_offset, stripe_num, last_psn_in_msg));
                }

                uint32_t msn, stripe_offset, pkt_offset, stripe_num, last_psn_in_msg;
                tie(msn, stripe_offset, pkt_offset, stripe_num, last_psn_in_msg) =
                    original_packet_list.front();

                send_packet_ec(msn,
                               stripe_offset,
                               pkt_offset,
                               stripe_num,
                               last_psn_in_msg,
                               0xFFFFFFFF,
                               0xFFFFFFFF);  // original_msn and original_stripe_offset are not used
                                             // for original packets
                update_offsets_ec(msn, stripe_offset, pkt_offset, stripe_num);
                original_packet_list.pop_front();
            } else {
                // sending a retransmission packet
                // cout << "[CbrSrc] flow_id: " << _flow.flow_id()
                //      << " Sending retransmission packet, "
                //      << " flow_id: " << _flow.flow_id()
                //      << " received_msg_sacks.size(): " << received_msg_sacks.size()
                //      << " unfinished_message_list.size(): " << unfinished_message_list.size()
                //      << endl;

                if (retransmission_packet_list.empty() && received_msg_sacks.empty()) {
                    cerr << "[CbrSrc] flow_id: " << _flow.flow_id()
                         << " retransmission_packet_list is empty when trying to send "
                            "retransmission "
                            "packet!"
                         << endl;
                    abort();
                } else if (retransmission_packet_list.empty() && !received_msg_sacks.empty()) {
                    // If retransmission_packet_list is empty but received_msg_sacks is not, we need
                    // to add the next retransmission packet to retransmission_packet_list
                    uint32_t                    next_msn;
                    ErasureCoding::message_sack sack;
                    tie(next_msn, sack) = received_msg_sacks.front();
                    retransmission_packet_list.push_back(make_tuple(
                        next_msn,
                        0,
                        0,
                        sack.num_missing_stripes,
                        next_msn * _erasure_coding->get_message_size_pkts() +
                            sack.num_missing_stripes * _erasure_coding->get_chunk_size() -
                            1,  // last_psn_in_msg
                        sack.msn,
                        sack.missing_stripes[0]));  // original_msn and
                                                    // original_stripe_offset are the same
                }

                uint32_t msn, stripe_offset, pkt_offset, stripe_num, last_psn_in_msg, original_msn,
                    original_stripe_offset;
                tie(msn,
                    stripe_offset,
                    pkt_offset,
                    stripe_num,
                    last_psn_in_msg,
                    original_msn,
                    original_stripe_offset) = retransmission_packet_list.front();

                send_packet_ec(msn,
                               stripe_offset,
                               pkt_offset,
                               stripe_num,
                               last_psn_in_msg,
                               original_msn,
                               original_stripe_offset);
                update_retrans_offsets_ec(msn, stripe_offset, pkt_offset, stripe_num);
                retransmission_packet_list.pop_front();
            }
        } else {
            bool trigger_flow_finished = (_sent_bytes >= _flow_size);
            if (trigger_flow_finished) {
                finish_flow();
                return;
            }

            send_packet();
        }
    }
}

/* Choose a route for a particular packet */
int KECbrSrc::choose_route() {
    switch (_route_strategy) {
        case ECMP_FIB:
            // Cycle through a permutation.  Generally gets better load balancing
            // than SCATTER_RANDOM.
            _crt_path++;
            if (_crt_path == _paths.size()) {
                // permute_paths();
                _crt_path = 0;
            }
            break;
        case ECMP_RANDOM_ECN:
            // Randomly choose a path from the list of paths?
            _crt_path = _srcaddr;
            break;
        case SINGLE_PATH:
            return _crt_path;
        case NOT_SET:
            cerr << "routing strategy not set for KECbrSrc " << _nodename << endl;
            abort();  // shouldn't be here at all
        default:
            cerr << "routing strategy not supported for KECbrSrc " << _nodename << endl;
            abort();
            break;
    }

    return _crt_path;
}

void KECbrSrc::start_flow() {
    if (!_flow_started) {
        _flow_start_time = eventlist().now();
        _flow_started    = true;
        _sent_bytes      = 0;

        if (use_erasure_coding) {
            message_size_bytes = _erasure_coding->get_message_size_pkts() * _mss;
            number_of_messages = ceil((double)_flow_size / message_size_bytes);
            for (uint32_t msg_id = 0; msg_id < number_of_messages; msg_id++) {
                message_list.push_back(msg_id);
                vector<uint32_t> stripes;
                for (size_t stripe_id = 0; stripe_id < _erasure_coding->get_stripe_num();
                     stripe_id++) {
                    stripes.push_back(stripe_id);
                }
                unfinished_message_list.push_back(make_tuple(
                    msg_id,
                    0xFFFFFFFF,
                    _erasure_coding->get_stripe_num(),
                    stripes));  // tuple<msn, original_msn, stripe_num, vector<stripe_ids>>
            }
            original_packet_list.push_back(make_tuple(
                0,
                0,
                0,
                _erasure_coding->get_stripe_num(),
                _erasure_coding->get_message_size_pkts() - 1));  // start with first packet
            _next_msn           = 0;
            _next_retrans_msn   = number_of_messages;
            double total_chunks = number_of_messages * _erasure_coding->get_stripe_num();
            double total_pkts   = number_of_messages * _erasure_coding->get_message_size_pkts();
            cout << "[CbrSrc] flow_id: " << _flow.flow_id()
                 << " flow_start_time(us): " << timeAsUs(eventlist().now())
                 << " number_of_messages: " << number_of_messages
                 << " message_list.size(): " << message_list.size()
                 << " total_chunks: " << total_chunks << " total_pkts: " << total_pkts << endl;
        }
    }
}

void KECbrSrc::setFlowSize(uint64_t flow_size_in_bytes) {
    _flow_size = flow_size_in_bytes;
}

void KECbrSrc::send_packet_ec(uint32_t msn,
                              uint32_t stripe_offset,
                              uint32_t pkt_offset,
                              uint32_t stripe_num,
                              uint32_t last_psn_in_msg,
                              uint32_t original_msn,
                              uint32_t original_stripe_offset) {
    if (!use_erasure_coding) {
        cerr << "[CbrSrc] send_packet_ec called without erasure coding enabled!" << endl;
        abort();
    }

    uint32_t entropy = _erasure_coding->entropy_list[pkt_offset];
    // bool     is_last_pkt =
    //     (stripe_offset == stripe_num - 1) && (pkt_offset == _erasure_coding->get_chunk_size() -
    //     1);
    uint32_t psn = msn * _erasure_coding->get_message_size_pkts() +
                   stripe_offset * _erasure_coding->get_chunk_size() +
                   pkt_offset;  // this assumes some psn are virtual packets

    CbrPacket* p = CbrPacket::newpkt(_flow, *_route, psn, _mss, _priority, _dstaddr);
    p->set_pathid(entropy);
    p->msn                    = msn;
    p->stripe_offset          = stripe_offset;
    p->pkt_offset             = pkt_offset;
    p->last_psn_in_msg        = last_psn_in_msg;
    p->original_msn           = original_msn;
    p->original_stripe_offset = original_stripe_offset;
    p->pkt_type = (pkt_offset < _erasure_coding->get_k() ? CbrPacket::DATA : CbrPacket::PARITY);
    p->set_ts(eventlist().now());
    _sent_bytes += p->size();

    p->sendOn();

    _sack_restart_timeout = eventlist().now() + _restart_timeout;

    eventlist().sourceIsPendingRel(*this, _period);
}

void KECbrSrc::SACKTimeOut() {
    cout << timeAsUs(eventlist().now()) << " [CbrSrc] SACK timeout: flow_id: " << _flow.flow_id()
         << " unfinished_message_list.size(): " << unfinished_message_list.size()
         << ", received_msg_sacks.size(): " << received_msg_sacks.size()
         << " message_list.size(): " << message_list.size()
         << " treat the first unfinished msn as SACKed" << endl;

    if (unfinished_message_list.empty()) {
        cout << "[CbrSrc][SACKTimeOut] flow_id: " << _flow.flow_id()
             << " No unfinished messages, nothing to do" << endl;
        return;
    }

    uint32_t                    msn;
    uint32_t                    original_msn = 0xFFFFFFFF;
    ErasureCoding::message_sack this_sack;
    this_sack.msn = msn;
    auto front    = unfinished_message_list.front();
    tie(msn, original_msn, this_sack.num_missing_stripes, this_sack.missing_stripes) = front;
    auto iter = unfinished_message_list.begin();
    if (iter != unfinished_message_list.end()) {
        unfinished_message_list.erase(iter);

        auto iter_msg_list = find(message_list.begin(), message_list.end(), msn);

        if (iter_msg_list != message_list.end()) {
            this_sack.msn                 = msn;
            this_sack.num_missing_stripes = _erasure_coding->get_stripe_num();
            this_sack.missing_stripes.clear();
            for (size_t i = 0; i < this_sack.num_missing_stripes; ++i) {
                this_sack.missing_stripes.push_back(i);
            }
            message_list.erase(iter_msg_list);
        }

        if (original_msn != 0xFFFFFFFF) {
            auto it_sack =
                std::find_if(received_msg_sacks.begin(),
                             received_msg_sacks.end(),
                             [&](const tuple<uint32_t, ErasureCoding::message_sack>& sack) {
                                 return get<0>(sack) == original_msn;
                             });

            if (it_sack != received_msg_sacks.end()) {
                this_sack.msn                 = msn;
                this_sack.num_missing_stripes = get<1>(*it_sack).num_missing_stripes;
                this_sack.missing_stripes.assign(get<1>(*it_sack).missing_stripes.begin(),
                                                 get<1>(*it_sack).missing_stripes.end());
                received_msg_sacks.erase(it_sack);
            }
        }

        cout << "SACKTimeout: this_sack.msn: " << this_sack.msn
             << ", num_missing_stripes: " << this_sack.num_missing_stripes
             << ", missing_stripes.size(): " << this_sack.missing_stripes.size()
             << ", flow_id: " << _flow.flow_id() << endl;

        if (message_list.empty() && received_msg_sacks.empty()) {
            // schedule next retransmission event
            uint32_t new_msn = get<0>(unfinished_message_list.back()) + 1;
            received_msg_sacks.push_back(make_tuple(new_msn, this_sack));
            unfinished_message_list.push_back(make_tuple(
                new_msn, this_sack.msn, this_sack.num_missing_stripes, this_sack.missing_stripes));
            eventlist().sourceIsPendingRel(*this, _period);
        } else {
            uint32_t new_msn = get<0>(unfinished_message_list.back()) + 1;
            received_msg_sacks.push_back(make_tuple(new_msn, this_sack));
            unfinished_message_list.push_back(make_tuple(
                new_msn, this_sack.msn, this_sack.num_missing_stripes, this_sack.missing_stripes));
        }
    }
}

void KECbrSrc::processECSACK(CbrPacket& pkt) {
    cout << timeAsUs(eventlist().now()) << " [CbrSrc] Received SACK for msn: " << pkt.msn
         << ", original_msn: " << pkt.original_msn
         << ", num_missing_stripes: " << pkt.num_missing_stripes << ", flow_id: " << _flow.flow_id()
         << endl;

    total_sack += 1;

    auto iter = find_if(
        unfinished_message_list.begin(), unfinished_message_list.end(), [&](const auto& tuple) {
            return get<0>(tuple) == static_cast<CbrPacket&>(pkt).msn;
        });

    if (iter != unfinished_message_list.end()) {
        unfinished_message_list.erase(iter);

        auto iter_msg_list =
            find(message_list.begin(), message_list.end(), static_cast<CbrPacket&>(pkt).msn);
        if (iter_msg_list != message_list.end()) {
            message_list.erase(iter_msg_list);
        }

        auto it_sack = std::find_if(received_msg_sacks.begin(),
                                    received_msg_sacks.end(),
                                    [&](const tuple<uint32_t, ErasureCoding::message_sack>& sack) {
                                        return get<0>(sack) == static_cast<CbrPacket&>(pkt).msn;
                                    });

        if (it_sack != received_msg_sacks.end()) {
            received_msg_sacks.erase(it_sack);
        }

    } else {
        cout << "[CbrSrc] Received SACK for unknown msn: " << pkt.msn
             << " original_msn: " << static_cast<CbrPacket&>(pkt).original_msn
             << " flow_id: " << _flow.flow_id() << endl;
    }

    ErasureCoding::message_sack sack;
    sack.msn                 = pkt.msn;
    sack.num_missing_stripes = pkt.num_missing_stripes;
    sack.missing_stripes.assign(pkt.missing_stripes.begin(), pkt.missing_stripes.end());

    if (sack.num_missing_stripes > 0) {
        if (message_list.empty() && received_msg_sacks.empty()) {
            // schedule next retransmission event
            uint32_t new_msn = get<0>(unfinished_message_list.back()) + 1;
            received_msg_sacks.push_back(make_tuple(new_msn, sack));
            unfinished_message_list.push_back(
                make_tuple(new_msn, sack.msn, sack.num_missing_stripes, sack.missing_stripes));
            eventlist().sourceIsPendingRel(*this, _period);
        } else {
            uint32_t new_msn = get<0>(unfinished_message_list.back()) + 1;
            received_msg_sacks.push_back(make_tuple(new_msn, sack));
            unfinished_message_list.push_back(
                make_tuple(new_msn, sack.msn, sack.num_missing_stripes, sack.missing_stripes));
        }
    }

    bool trigger_flow_finished = false;
    if (!use_erasure_coding)
        trigger_flow_finished = (_sent_bytes >= _flow_size);
    else {
        // If we are using erasure coding, we check if all messages have been sent
        // cout << "[CbrSrc] flow_id: " << _flow.flow_id()
        //      << " unfinished_message_list.size(): " << unfinished_message_list.size()
        //      << ", received_msg_sacks.size(): " << received_msg_sacks.size()
        //      << ", message_list.size(): " << message_list.size() << endl;
        trigger_flow_finished =
            (unfinished_message_list.empty() && received_msg_sacks.empty() && message_list.empty());
    }

    if (trigger_flow_finished) {
        finish_flow();
        return;
    }
}

void KECbrSrc::restartFlow() {
    if (unfinished_message_list.empty()) {
        bool trigger_flow_finished = false;
        if (!use_erasure_coding)
            trigger_flow_finished = (_sent_bytes >= _flow_size);
        else {
            // If we are using erasure coding, we check if all messages have been sent
            trigger_flow_finished = (unfinished_message_list.empty() &&
                                     received_msg_sacks.empty() && message_list.empty());
        }

        if (trigger_flow_finished) {
            finish_flow();
            return;
        }
        return;
    }

    vector<tuple<uint32_t, uint32_t, uint32_t, vector<uint32_t>>> new_unfinished_message_list;
    message_list.clear();

    for (size_t i = 0; i < unfinished_message_list.size(); i++) {
        uint32_t         num_stripes = get<2>(unfinished_message_list[i]);
        vector<uint32_t> stripes(get<3>(unfinished_message_list[i]));
        new_unfinished_message_list.push_back(make_tuple(i, 0xFFFFFFFF, num_stripes, stripes));
        message_list.push_back(i);
    }
    number_of_messages = new_unfinished_message_list.size();
    _next_retrans_msn  = number_of_messages;  // Reset the next retransmission msn
    _next_msn          = 0;                   // Reset the next msn to start from the beginning
    original_packet_list.clear();
    retransmission_packet_list.clear();
    received_msg_sacks.clear();

    unfinished_message_list.clear();
    unfinished_message_list.assign(new_unfinished_message_list.begin(),
                                   new_unfinished_message_list.end());

    original_packet_list.push_back(
        make_tuple(0,
                   0,
                   0,
                   _erasure_coding->get_stripe_num(),
                   _erasure_coding->get_message_size_pkts() - 1));  // start with first packet
    eventlist().sourceIsPendingRel(*this, _period);
}

void KECbrSrc::processECNACK(CbrPacket& pkt) {
    // Process message-level NACK packets for erasure coding, restart the flow by setting epsn to 0

    // cout << "[CbrSrc] Received NACK packet, restarting flow with msn: " << pkt.msn
    //      << ", flow_id: " << _flow.flow_id() << endl;

    cout << "[CbrSrc] Received NACK, restarting flow_id: " << _flow.flow_id()
         << ", number_of_messages: " << number_of_messages
         << ", unfinished_message_list.size(): " << unfinished_message_list.size() << endl;

    restartFlow();
}

void KECbrSrc::receivePacket(Packet& pkt) {
    // handle SACK packets for retransmission
    if (use_erasure_coding) {
        if (static_cast<CbrPacket&>(pkt).pkt_type == CbrPacket::SACK) {
            processECSACK(static_cast<CbrPacket&>(pkt));
        } else if (static_cast<CbrPacket&>(pkt).pkt_type == CbrPacket::NACK) {
            processECNACK(static_cast<CbrPacket&>(pkt));
        }
    }

    _sack_restart_timeout = eventlist().now() + _restart_timeout;

    pkt.free();  // Free the packet after processing
}

void KECbrSrc::send_packet() {
    Packet* p = CbrPacket::newpkt(_flow, *_route, _crt_id++, _mss, _priority, _dstaddr);
    _sent_bytes += p->size();

    int crt = choose_route();
    p->set_pathid(crt);
    p->sendOn();

    eventlist().sourceIsPendingRel(*this, _period);
}

void KECbrSrc::finish_flow() {
    if (_flow_finished) {
        return;  // already finished
    } else {
        if (use_erasure_coding) {
            _flow_finished = true;
            _statistics_outfile << "[CbrSrc] flow_id: " << _flow.flow_id()
                                << " flow_start_time(us): " << timeAsUs(_flow_start_time)
                                << " flow_end_time(us): " << timeAsUs(eventlist().now())
                                << " flow_completion_time(us): "
                                << timeAsUs(eventlist().now() - _flow_start_time)
                                << " total_bytes_sent: " << _sent_bytes << endl;

            _statistics_outfile << "total_received_sack: " << total_sack
                                << ", flow_id: " << _flow.flow_id() << endl;

            _statistics_outfile
                << "[CbrSrc] Logging statistics: num_received_messages: "
                << _sink->statistics.num_received_messages
                << ", num_received_chunks: " << _sink->statistics.num_received_chunks
                << ", num_recovered_chunks: " << _sink->statistics.num_recovered_chunks
                << ", num_bitmap_overflow_drops: " << _sink->statistics.num_bitmap_overflow_drops
                << ", num_bitmap_underflow_drops: " << _sink->statistics.num_bitmap_underflow_drops
                << ", num_trivial_skips: " << _sink->statistics.num_trivial_skips
                << ", num_recoverable_skips: " << _sink->statistics.num_recoverable_skips
                << ", num_lossy_skips: " << _sink->statistics.num_lossy_skips
                << ", num_nack_sent: " << _sink->statistics.num_nack_sent;
            for (size_t i = 0; i < _erasure_coding->entropy_list.size(); i++) {
                _statistics_outfile << ", max_size_bitmaps[" + to_string(i)
                                    << "]: " << _sink->statistics.max_size_bitmaps.at(i);
            }
            _statistics_outfile << endl;

            cout << "[CbrSrc] flow_id: " << _flow.flow_id()
                 << " flow_start_time(us): " << timeAsUs(_flow_start_time)
                 << " flow_end_time(us): " << timeAsUs(eventlist().now())
                 << " flow_completion_time(us): " << timeAsUs(eventlist().now() - _flow_start_time)
                 << " total_bytes_sent: " << _sent_bytes << endl;

            cout << "total_received_sack: " << total_sack << ", flow_id: " << _flow.flow_id()
                 << endl;

            cout << "[CbrSrc] Logging statistics: num_received_messages: "
                 << _sink->statistics.num_received_messages
                 << ", num_received_chunks: " << _sink->statistics.num_received_chunks
                 << ", num_recovered_chunks: " << _sink->statistics.num_recovered_chunks
                 << ", num_bitmap_overflow_drops: " << _sink->statistics.num_bitmap_overflow_drops
                 << ", num_bitmap_underflow_drops: " << _sink->statistics.num_bitmap_underflow_drops
                 << ", num_trivial_skips: " << _sink->statistics.num_trivial_skips
                 << ", num_recoverable_skips: " << _sink->statistics.num_recoverable_skips
                 << ", num_lossy_skips: " << _sink->statistics.num_lossy_skips
                 << ", num_nack_sent: " << _sink->statistics.num_nack_sent;
            for (size_t i = 0; i < _erasure_coding->entropy_list.size(); i++) {
                cout << ", max_size_bitmaps[" + to_string(i)
                     << "]: " << _sink->statistics.max_size_bitmaps.at(i);
            }
            cout << endl;
            eventlist().cancelPendingSource(*this);
        } else {
            _flow_finished = true;
            eventlist().cancelPendingSource(*this);
        }
    }
}

void permute_sequence_cbr(vector<int>& seq) {
    size_t len = seq.size();
    for (uint32_t i = 0; i < len; i++) {
        seq[i] = i;
    }
    for (uint32_t i = 0; i < len; i++) {
        int ix           = random() % (len - i);
        int tmpval       = seq[ix];
        seq[ix]          = seq[len - 1 - i];
        seq[len - 1 - i] = tmpval;
    }
}

void KECbrSrc::set_paths(uint32_t num_paths) {
    _path_ids.resize(num_paths);
    permute_sequence_cbr(_path_ids);

    _paths.resize(num_paths);
    _original_paths.resize(num_paths);

    _path_ids.resize(num_paths);
    _paths.resize(num_paths);

    for (size_t i = 0; i < num_paths; i++) {
        _paths[i]          = NULL;
        _original_paths[i] = NULL;
        _path_ids[i]       = i;
    }
}

void KECbrSrc::restart_flow_timer_hook(simtime_picosec now, simtime_picosec period) {
    if (_flow_finished) {
        return;
    }

    // cout << "[CbrSrc] restart_flow_timer_hook called at time: " << timeAsUs(now)
    //      << ", flow_finished: " << _flow_finished
    //      << ", _restart_timeout_pending: " << _restart_timeout_pending << ", now: " <<
    //      timeAsUs(now)
    //      << ", _sack_restart_timeout: " << timeAsUs(_sack_restart_timeout)
    //      << ", _restart_timeout: " << timeAsUs(_restart_timeout) << ", flow_id: " <<
    //      _flow.flow_id()
    //      << endl;

    if (now <= _sack_restart_timeout || _sack_restart_timeout == timeInf)
        return;

    // Timeout and restart the flow as received an NACK
    if (!_restart_timeout_pending) {
        _restart_timeout_pending = true;

        cout << "[CbrSrc] restart flow due to timeout flow_id: " << _flow.flow_id() << endl;

        // check the timer difference between the event and the real value
        simtime_picosec too_late = now - (_sack_restart_timeout);

        // careful: we might calculate a negative value
        // to prevent overflow but keep randomness we just divide until we are within the limit
        while (too_late > period)
            too_late >>= 1;

        simtime_picosec restart_off = (period - too_late);

        eventlist().sourceIsPendingRel(*this, restart_off);

        _sack_restart_timeout = now + _restart_timeout;
    }
}

////////////////////////////////////////////////////////////////
//  Cbr SINK
////////////////////////////////////////////////////////////////

KECbrSink::KECbrSink(EventList& eventlist)
    : EventSource(eventlist, "cbrsink"), DataReceiver("cbr") {
    _nodename       = "cbrsink";
    _received       = 0;
    _last_id        = 0;
    _cumulative_ack = 0;

    if (use_erasure_coding) {
        message_boundary_list.resize(ErasureCoding::MAX_MESSAGE_BOUNDARIES);
        mbl_head = 0;
        mbl_tail = 0;
        mbl_size = 0;
        for (size_t i = 0; i < message_boundary_list.size(); i++) {
            message_boundary_list[i] = UINT32_MAX;  // Initialize to a large value
        }

        per_path_epsns.resize(_erasure_coding->entropy_list.size(), 0);
        per_path_max_psns.resize(_erasure_coding->entropy_list.size(), 0);
        per_path_bitmaps.resize(_erasure_coding->entropy_list.size());
        for (size_t i = 0; i < _erasure_coding->entropy_list.size(); i++) {
            per_path_bitmaps[i].reset();
        }
        global_min_epsn = 0;
        statistics.max_size_bitmaps.resize(_erasure_coding->get_chunk_size(), 0);
    }
}

void KECbrSink::connect(KECbrSrc& src, const Route* route) {
    _src        = &src;
    _route      = route;
    _srcaddr    = src._srcaddr;
    _mss        = src._mss;
    _priority   = src._priority;
    flow_active = true;
}

inline size_t KECbrSink::find_highest_bit(const bitset<ErasureCoding::BITMAP_SIZE_KEC>& bitmap) {
    for (size_t i = bitmap.size() - 1; i > 0; i--) {
        if (bitmap[i]) {
            return i;
        }
    }
    return 0;
}

void KECbrSink::update_bitmap_statistics(size_t bitmap_idx) {
    if (use_erasure_coding) {
        statistics.max_size_bitmaps.at(bitmap_idx) =
            max(statistics.max_size_bitmaps.at(bitmap_idx),
                find_highest_bit(per_path_bitmaps[bitmap_idx]));
    }
}

// Note: _cumulative_ack is the last byte we've ACKed.
// seqno is the first byte of the new packet.
void KECbrSink::receivePacket(Packet& pkt) {
    _received++;
    _cumulative_ack += pkt.size();
    _last_id    = pkt.id();
    flow_active = true;

    if (use_erasure_coding) {
        simtime_picosec ts = static_cast<CbrPacket&>(pkt).ts();

        uint32_t this_msn = static_cast<CbrPacket&>(pkt).msn;

        uint32_t bitmap_idx   = pkt.id() % _erasure_coding->entropy_list.size();
        uint32_t per_path_psn = pkt.id() / _erasure_coding->entropy_list.size();
        uint32_t last_per_path_psn =
            static_cast<CbrPacket&>(pkt).last_psn_in_msg / _erasure_coding->entropy_list.size();
        uint32_t last_virtual_per_path_psn =
            (static_cast<CbrPacket&>(pkt).msn + 1) * _erasure_coding->get_stripe_num() - 1;

        //  per_path_max_psns.at(bitmap_idx) = max(per_path_max_psns.at(bitmap_idx), per_path_psn);
        global_max_psn = max(global_max_psn, per_path_psn);

        // cout << "[CbrSink] Received packet with id: " << pkt.id() << ", bitmap_idx: " <<
        // bitmap_idx
        //      << ", per_path_psn: " << per_path_psn << ", last_per_path_psn: " <<
        //      last_per_path_psn
        //      << ", flow_id: " << _src->flow().flow_id() << endl;

        update_mbl(static_cast<CbrPacket&>(pkt), per_path_psn, last_per_path_psn);

        bool     check_mbl               = false;
        uint32_t ret_check_pkt           = 0;
        uint32_t ret_trivial_skip        = 0;
        uint32_t ret_lossy_skip          = 0;
        uint32_t ret_recoverable_skip    = 0;
        uint32_t ret_virtual_epsns       = 0;
        uint32_t ret_full_skip           = 0;
        uint32_t ret_check_pkt_full_skip = 0;
        bool     do_full_skip            = false;

        //  cout << "bitmaps: flow_id: " << _src->flow().flow_id() << endl;
        // for (size_t i = 0; i < per_path_bitmaps.size(); i++) {
        //     cout << "bitmap[" << i << "]: per_path_epsn: " <<  per_path_epsns[i] <<  " " <<
        //     per_path_bitmaps[i] << endl;
        // }

        tie(ret_check_pkt, do_full_skip) = check_pkt(per_path_psn,
                                                     bitmap_idx,
                                                     _is_skip_bitmap_one_chunk,
                                                     last_per_path_psn,
                                                     last_virtual_per_path_psn);
        if (ret_check_pkt) {  // Update the corresponding bitmap
            check_mbl = true;
            cout << "check_pkt true flow_id: " << _src->flow().flow_id() << endl;
        }

        if (_use_full_skip && do_full_skip) {  // full skipping for all bitmaps
            // stripe checking across bitmaps for full skipping
            // 1. iterate all bitmaps first, and do whatever 3 types of skipping
            // 2. shift all bitmaps to the this per-path-psn with full skipping

            // cout << "full_skip: do_full_skip true flow_id: " << _src->flow().flow_id()
            //      << " target_per_path_psn: " << per_path_psn << endl;

            while (do_full_skip) {
                if ((ret_trivial_skip = check_trivial_skip(_is_skip_bitmap_one_chunk)) > 0) {
                    check_mbl = true;
                    // cout << "full_skip trivial_skip true flow_id: " << _src->flow().flow_id()
                    //      << endl;
                } else if (ret_recoverable_skip = check_recoverable_skip(
                               per_path_psn, _is_skip_bitmap_one_chunk, do_full_skip)) {
                    // cout << "full_skip recoverable_skip true flow_id: " << _src->flow().flow_id()
                    //      << endl;
                    check_mbl = true;
                    statistics.num_recoverable_skips++;
                } else if (ret_lossy_skip = check_lossy_skip(do_full_skip)) {
                    // cout << "full_skip lossy_skip true flow_id: " << _src->flow().flow_id() <<
                    // endl;
                    check_mbl = true;
                    statistics.num_lossy_skips++;
                }

                tie(ret_check_pkt_full_skip, do_full_skip) =
                    check_pkt_full_skip(per_path_psn,
                                        bitmap_idx,
                                        _is_skip_bitmap_one_chunk,
                                        last_per_path_psn,
                                        last_virtual_per_path_psn);
                if (ret_check_pkt_full_skip) {  // Update the corresponding bitmap
                    check_mbl = true;
                    // cout << "check_pkt_full_skip true flow_id: " << _src->flow().flow_id() <<
                    // endl;
                }
            }

            tie(ret_check_pkt, do_full_skip) = check_pkt(per_path_psn,
                                                         bitmap_idx,
                                                         _is_skip_bitmap_one_chunk,
                                                         last_per_path_psn,
                                                         last_virtual_per_path_psn);
            if (ret_check_pkt) {  // Update the corresponding bitmap
                check_mbl = true;
                cout << "check_pkt true flow_id: " << _src->flow().flow_id() << endl;
            }

            if (do_full_skip != false) {
                cerr << "full skip should be false" << endl;
                abort();
            }

            check_mbl = true;
            statistics.num_full_skips++;
        }

        if ((ret_trivial_skip = check_trivial_skip(_is_skip_bitmap_one_chunk)) > 0) {
            check_mbl = true;
            cout << "trivial_skip true flow_id: " << _src->flow().flow_id() << endl;
        } else if (ret_recoverable_skip = check_recoverable_skip(
                       per_path_psn, _is_skip_bitmap_one_chunk, do_full_skip)) {
            cout << "recoverable_skip true flow_id: " << _src->flow().flow_id() << endl;
            check_mbl = true;
            statistics.num_recoverable_skips++;
        } else if (ret_lossy_skip = check_lossy_skip(do_full_skip)) {
            cout << "lossy_skip true flow_id: " << _src->flow().flow_id() << endl;
            check_mbl = true;
            statistics.num_lossy_skips++;
        }

        // cout << "bitmaps: flow_id: " << _src->flow().flow_id() << endl;
        // for (size_t i = 0; i < per_path_bitmaps.size(); i++) {
        //     cout << "bitmap[" << i << "]: per_path_epsn: " <<  per_path_epsns[i] <<  " " <<
        //     per_path_bitmaps[i] << endl;
        // }
        // cout << "global_min_epsn: " << global_min_epsn << endl;

        statistics.num_received_chunks += ret_trivial_skip + ret_recoverable_skip;

        update_bitmap_statistics(bitmap_idx);

        // cout << "[CbrSink] check_mbl: " << check_mbl << " flow_id: " << _src->flow().flow_id()
        //      << " now: " << timeAsUs(eventlist().now()) << " flow_acitve: " << flow_active
        //      << " mbl_base_msn: " << mbl_base_msn << " mbl_size: " << mbl_size << endl;

        // check_mbl == true if some leading chunks can be skipped, but either
        // completed/recoverable/unsuccessfully completed; in whichever case, we update the message
        // boundary list.
        if (check_mbl) {
            complete_mbl(pkt, ts, global_min_epsn);
        }
    }

    pkt.free();
}

void KECbrSink::send_sack_packet_ec(simtime_picosec         ts,
                                    uint32_t                msn,
                                    uint32_t                num_missing_stripes,
                                    const vector<uint32_t>& missing_stripes) {
    if (!use_erasure_coding) {
        cerr << "[CbrSink] send_sack_packet_ec called without erasure coding enabled!" << endl;
        abort();
    }

    // cout << "[CbrSink] Sending SACK packet for msn: " << msn
    //      << ", num_missing_stripes: " << num_missing_stripes
    //      << ", missing_stripes: " << missing_stripes.size()
    //      << ", flow_id: " << _src->flow().flow_id() << ", time: " << ts << endl;

    CbrPacket* p =
        CbrPacket::newSACKpkt(_src->flow(), *_route, global_min_epsn, _mss, _priority, _srcaddr);
    p->set_pathid(_crt_path);
    p->msn                 = msn;
    p->num_missing_stripes = num_missing_stripes;
    p->missing_stripes.insert(
        p->missing_stripes.begin(), missing_stripes.begin(), missing_stripes.end());
    p->set_ts(ts);

    p->sendOn();
}

void KECbrSink::set_paths(uint32_t num_paths) {
    switch (_route_strategy) {
        case SCATTER_PERMUTE:
        case PULL_BASED:
        case SCATTER_ECMP:
        case NOT_SET:
            cerr << "routing strategy not supported for KECbrSink " << _nodename << endl;
            abort();
        case SCATTER_RANDOM:
        case SINGLE_PATH:
        case ECMP_FIB:
        case ECMP_FIB_ECN:
        case ECMP_RANDOM2_ECN:
        case SIMPLE_SUBFLOW:
        case REACTIVE_ECN:
            assert(_paths.size() == 0);
            _paths.resize(num_paths);
            _path_ids.resize(num_paths);
            for (unsigned int i = 0; i < num_paths; i++) {
                _paths[i]    = NULL;
                _path_ids[i] = i;
            }
            _crt_path = 0;
            break;
        case ECMP_RANDOM_ECN:
            assert(_paths.size() == 0);
            _paths.resize(num_paths);
            _path_ids.resize(num_paths);
            for (unsigned int i = 0; i < num_paths; i++) {
                _paths[i]    = NULL;
                _path_ids[i] = i;
            }
            _crt_path = 0;
            break;
        default:
            break;
    }
}

inline bool KECbrSink::is_bitmap_full() {
    for (const auto& bitmap : per_path_bitmaps) {
        if (find_highest_bit(bitmap) > _erasure_coding->BITMAP_SIZE_KEC * _bitmap_full_percent) {
            return true;  // If any bitmap has less than k bits set, it's not full
        }
    }
    return false;  // All bitmaps are full
}

bool KECbrSink::is_lossy_skip_condition() {
    return (_use_bitmap_full_condition ? is_bitmap_full()
                                       : (global_max_psn >= global_min_epsn &&
                                          global_max_psn - global_min_epsn > lossy_skip_threshold));
}

bool KECbrSink::is_full_skip_condition() {
    return is_bitmap_full() && is_lossy_skip_condition();
}

// retransmission logic
uint32_t KECbrSink::check_lossy_skip(bool do_full_skip) {
    // cout << "in check_lossy_skip, global_max_psn: " << global_max_psn
    //      << ", global_min_epsn: " << global_min_epsn
    //      << ", lossy_skip_threshold: " << lossy_skip_threshold
    //      << ", is_bitmap_full(): " << is_bitmap_full()
    //      << ", bitmap_overflow: " << statistics.num_bitmap_overflow_drops << endl;

    // for (size_t idx = 0; idx < per_path_bitmaps.size(); idx++) {
    //     cout << "per_path_bitmaps[" << idx << "]: " << per_path_epsns[idx] << " "
    //          << per_path_bitmaps[idx].to_string() << endl;
    // }

    if (do_full_skip || is_lossy_skip_condition()) {
        uint32_t this_msn           = global_min_epsn / _erasure_coding->get_stripe_num();
        uint32_t this_stripe_offset = (global_min_epsn % _erasure_coding->get_stripe_num());

        // cout << "flow_id: " << _src->flow().flow_id() << " adding missing stripes for msn: " <<
        // this_msn
        //      << ", stripe_offset: " << this_stripe_offset << endl;

        missing_stripes_map[this_msn].push_back(this_stripe_offset);

        // update both data and parity bitmaps
        for (size_t bitmap_idx = 0; bitmap_idx < per_path_bitmaps.size(); bitmap_idx++) {
            if (per_path_epsns.at(bitmap_idx) == global_min_epsn) {
                per_path_epsns.at(bitmap_idx) += 1;
                shift_stripe(bitmap_idx, 1);
            }
        }
        update_global_epsn();

        return 1;
    } else {
        return 0;
    }
}

inline uint32_t KECbrSink::count_consecutive_ones(
    const bitset<ErasureCoding::BITMAP_SIZE_KEC>& bitmap) {
    uint32_t count = 0;
    for (size_t i = 0; i < bitmap.size(); i++) {
        if (bitmap[i]) {
            count++;
        } else {
            break;  // stop counting at the first zero
        }
    }
    return count;
}

inline uint32_t KECbrSink::count_ones(const bitset<ErasureCoding::BITMAP_SIZE_KEC>& bitmap,
                                      uint32_t                                      size) {
    uint32_t count = 0;
    for (size_t i = 0; i < size; i++) {
        count += bitmap[i];
    }
    return count;
}

inline bool KECbrSink::is_num_consequtive_ones(const bitset<ErasureCoding::BITMAP_SIZE_KEC>& bitmap,
                                               uint32_t                                      k) {
    uint32_t count = 0;
    for (size_t i = 0; i < bitmap.size(); i++) {
        if (bitmap[i]) {
            count++;
        } else {
            if (count >= k) {
                return true;
            }
            count = 0;
        }
    }
    return (count >= k);
}

void KECbrSink::update_global_epsn() {
    global_min_epsn = per_path_epsns[0];
    for (size_t i = 1; i < per_path_epsns.size(); i++) {
        global_min_epsn = min(global_min_epsn, per_path_epsns[i]);
    }

    // global_max_psn = per_path_max_psns[0];
    // for (size_t i = 1; i < per_path_max_psns.size(); i++) {
    //     global_max_psn = max(global_max_psn, per_path_max_psns[i]);
    // }

    // cout << "flow_id: " << _src->flow().flow_id() << " global_min_epsn: " << global_min_epsn <<
    // ", global_max_psn: " << global_max_psn
    //      << endl;
}

uint32_t KECbrSink::check_stripe_bitmap_update(uint32_t bitmap_idx) {
    uint32_t count = count_consecutive_ones(per_path_bitmaps[bitmap_idx]);

    if (count > 0) {
        per_path_epsns[bitmap_idx] += count;
        shift_stripe(bitmap_idx, count);
    }

    return count;
}

// shift_stripe: shift the bitmap to the right by one chunk.
void KECbrSink::shift_stripe(uint32_t bitmap_idx, uint32_t shift_bits) {
    per_path_bitmaps.at(bitmap_idx) >>= shift_bits;
}

tuple<bool, uint32_t> KECbrSink::trivial_skip_condition(
    const vector<bitset<ErasureCoding::BITMAP_SIZE_KEC>>& bitmaps, uint32_t k) {
    uint32_t first_k_min_epsn = UINT32_MAX;
    for (size_t i = 0; i < k; i++) {
        first_k_min_epsn = min(first_k_min_epsn, per_path_epsns.at(i));
    }

    return make_tuple((first_k_min_epsn > global_min_epsn), first_k_min_epsn);
}

// Trivial skip: if we have k consecutive ones, we can skip the stripe and don't need to do
// recovery. Return true: if we can skip the stripe. Return false: if we cannot skip any stripe.
uint32_t KECbrSink::check_trivial_skip(bool _is_skip_bitmap_one_chunk) {
    // cout << "in check_trivial_skip, global_max_psn: " << global_max_psn
    //      << ", global_min_epsn: " << global_min_epsn << endl;

    // for (size_t idx = 0; idx < per_path_bitmaps.size(); idx++) {
    //     cout << "per_path_bitmaps[" << idx << "]: " << per_path_epsns[idx] << " "
    //          << per_path_bitmaps[idx].to_string() << endl;
    // }

    bool     check          = true;
    uint32_t shifted_chunks = 0;
    while (check) {
        bool     is_k;
        uint32_t first_k_min_epsn;
        tie(is_k, first_k_min_epsn) =
            trivial_skip_condition(per_path_bitmaps, _erasure_coding->get_k());
        // cout << "trivial skip: is_k = " << is_k << ", first_k_min_epsn = " << first_k_min_epsn
        //      << ", global_min_epsn = " << global_min_epsn << endl;
        if (is_k) {
            // update both data and parity bitmaps
            for (size_t bitmap_idx = 0; bitmap_idx < per_path_bitmaps.size(); bitmap_idx++) {
                if (per_path_epsns.at(bitmap_idx) == global_min_epsn) {
                    per_path_epsns.at(bitmap_idx) += 1;
                    shift_stripe(bitmap_idx, 1);
                }
            }
            update_global_epsn();

            shifted_chunks += 1;
            statistics.num_trivial_skips++;
            // cout << "trivial_skips incremented, trivial_skips=" << statistics.num_trivial_skips
            //      << ", flow_id: " << _src->flow().flow_id() << endl;
        } else {
            check = false;
            break;
        }

        if (_is_skip_bitmap_one_chunk && shifted_chunks == 1) {
            break;
        }
    }
    return shifted_chunks;
}

tuple<bool, uint32_t> KECbrSink::recoverable_skip_condition(
    const vector<bitset<ErasureCoding::BITMAP_SIZE_KEC>>& bitmaps, uint32_t k) {
    std::priority_queue<uint32_t> max_heap;
    for (size_t i = 0; i < per_path_epsns.size(); ++i) {
        max_heap.push(per_path_epsns[i]);
    }

    uint32_t kth_max = 0;
    for (uint32_t i = 0; i < k && !max_heap.empty(); ++i) {
        kth_max = max_heap.top();
        max_heap.pop();
    }

    return make_tuple((kth_max > global_min_epsn), kth_max);
}

bool KECbrSink::is_recoverable_skip_condition() {
    return (_use_as_fast_as_possible
                ? true
                : (global_max_psn >= global_min_epsn &&
                   global_max_psn - global_min_epsn > recoverable_skip_threshold));
}

// Recoverable skip: if we have k of m packets, we can skip the stripe and recover the missing
// packets. Return true: if we can skip the stripe and recover the missing packets. Return false: if
// we cannot skip any stripe.
uint32_t KECbrSink::check_recoverable_skip(uint32_t per_path_psn,
                                           bool     _is_skip_bitmap_one_chunk,
                                           bool     do_full_skip) {
    // cout << "in check_recoverable_skip, global_max_psn: " << global_max_psn
    //      << ", global_min_epsn: " << global_min_epsn
    //      << ", recoverable_skip_threshold: " << recoverable_skip_threshold << endl;

    // for (size_t idx = 0; idx < per_path_bitmaps.size(); idx++) {
    //     cout << "per_path_bitmaps[" << idx << "]: " << per_path_epsns[idx] << " "
    //          << per_path_bitmaps[idx].to_string() << endl;
    // }

    if (_use_recoverable_skip == false) {
        return 0;
    }

    uint32_t shifted_chunks = 0;

    if (do_full_skip || is_recoverable_skip_condition()) {
        bool check = true;
        while (check) {
            bool     is_k_of_m;
            uint32_t kth_max_epsn;
            tie(is_k_of_m, kth_max_epsn) =
                recoverable_skip_condition(per_path_bitmaps, _erasure_coding->get_k());
            if (is_k_of_m) {
                // update both data and parity bitmaps
                for (size_t bitmap_idx = 0; bitmap_idx < per_path_bitmaps.size(); bitmap_idx++) {
                    if (per_path_epsns.at(bitmap_idx) == global_min_epsn) {
                        per_path_epsns.at(bitmap_idx) += 1;
                        shift_stripe(bitmap_idx, 1);
                    }
                }
                update_global_epsn();
                shifted_chunks += 1;
                statistics.num_recovered_chunks++;
            } else {
                check = false;
            }

            if (shifted_chunks >= 1) {
                check = false;  // Due to the resource limitation for recovery, we only do one
                                // recoverable skip
            }
        }
    }

    return shifted_chunks;
}

uint32_t KECbrSink::check_virtual_epsns(uint32_t current_virtual_epsn) {
    // Check if there are any virtual PSNs that can be removed
    uint32_t removed_virtual_epsns = 0;
    bool     flag                  = true;
    while (global_min_epsn < current_virtual_epsn && flag) {
        flag = false;
        for (size_t bitmap_idx = 0; bitmap_idx < per_path_bitmaps.size(); bitmap_idx++) {
            if (per_path_epsns.at(bitmap_idx) == global_min_epsn &&
                per_path_bitmaps.at(bitmap_idx)[0] == 0) {
                per_path_epsns.at(bitmap_idx) += 1;
                shift_stripe(bitmap_idx, 1);
                flag = true;
            }
        }
        if (flag) {
            removed_virtual_epsns++;
            update_global_epsn();
        }
    }

    return removed_virtual_epsns;
}

tuple<uint32_t, bool> KECbrSink::check_pkt(uint32_t per_path_psn,
                                           uint32_t bitmap_idx,
                                           bool     _is_skip_bitmap_one_chunk,
                                           uint32_t last_per_path_psn,
                                           uint32_t last_virtual_per_path_psn) {
    // cout << "[CbrSink] check_pkt: per_path_psn: " << per_path_psn << ", bitmap_idx: " <<
    // bitmap_idx
    //      << ", per_path_epsns[bitmap_idx]: " << per_path_epsns[bitmap_idx] << ", flow_id: " <<
    //      _src->flow().flow_id()
    //      << endl;

    for (uint32_t iter_psn = last_per_path_psn + 1; iter_psn <= last_virtual_per_path_psn;
         iter_psn++) {
        if (iter_psn >= per_path_epsns[bitmap_idx] &&
            iter_psn - per_path_epsns[bitmap_idx] < ErasureCoding::BITMAP_SIZE_KEC) {
            per_path_bitmaps.at(bitmap_idx).set(iter_psn - per_path_epsns[bitmap_idx]);
        }
    }

    if (per_path_psn == per_path_epsns[bitmap_idx]) {
        per_path_bitmaps.at(bitmap_idx).set(per_path_psn - per_path_epsns[bitmap_idx]);

        uint32_t old_global_min_epsn = global_min_epsn;
        uint32_t ret                 = check_stripe_bitmap_update(bitmap_idx);
        update_global_epsn();
        if (old_global_min_epsn < global_min_epsn) {
            statistics.num_trivial_skips += global_min_epsn - old_global_min_epsn;
            statistics.num_received_chunks += global_min_epsn - old_global_min_epsn;
        }

        // sanity check
        // if (_mbl_overflow) {
        //     cout << "[CbrSink] check_pkt: mbl_overflow is true, but bitmap doesn't overflow!"
        //          << endl;
        //     abort();
        // }

        return make_tuple(ret, false);
    } else if (per_path_psn > per_path_epsns[bitmap_idx]) {
        if (per_path_psn - per_path_epsns[bitmap_idx] >= ErasureCoding::BITMAP_SIZE_KEC) {
            // if use_full_skipping == true && epsn_delta > lossy_threshold
            //     then use full skipping
            if (_use_full_skip && is_full_skip_condition()) {
                return make_tuple(0, true);
            } else {
                // drop packet, bitmap is full, and we don't do full skipping
                statistics.num_bitmap_overflow_drops++;
                return make_tuple(0, false);
            }
        }
        per_path_bitmaps.at(bitmap_idx).set(per_path_psn - per_path_epsns[bitmap_idx]);

        uint32_t old_global_min_epsn = global_min_epsn;
        uint32_t ret                 = check_stripe_bitmap_update(bitmap_idx);
        update_global_epsn();
        if (old_global_min_epsn < global_min_epsn) {
            statistics.num_trivial_skips += global_min_epsn - old_global_min_epsn;
            statistics.num_received_chunks += global_min_epsn - old_global_min_epsn;
        }

        // sanity check
        // if (_mbl_overflow) {
        //     cout << "[CbrSink] check_pkt: mbl_overflow is true, but bitmap doesn't overflow!"
        //          << endl;
        //     abort();
        // }

        return make_tuple(ret, false);
    } else {
        // drop packet due to underflow
        // cout << "[CbrSink] check_pkt: dropping packet due to underflow" << endl;
        statistics.num_bitmap_underflow_drops++;

        // sanity check
        // if (_mbl_overflow) {
        //     cout << "[CbrSink] check_pkt: mbl_overflow is true, but bitmap doesn't overflow!"
        //          << endl;
        //     abort();
        // }

        return make_tuple(0, false);
    }
}

tuple<uint32_t, bool> KECbrSink::check_pkt_full_skip(uint32_t per_path_psn,
                                                     uint32_t bitmap_idx,
                                                     bool     _is_skip_bitmap_one_chunk,
                                                     uint32_t last_per_path_psn,
                                                     uint32_t last_virtual_per_path_psn) {
    // cout << "[CbrSink] check_pkt: per_path_psn: " << per_path_psn << ", bitmap_idx: " <<
    // bitmap_idx
    //      << ", per_path_epsns[bitmap_idx]: " << per_path_epsns[bitmap_idx] << ", flow_id: " <<
    //      _src->flow().flow_id()
    //      << endl;

    for (uint32_t iter_psn = last_per_path_psn + 1; iter_psn <= last_virtual_per_path_psn;
         iter_psn++) {
        if (iter_psn >= per_path_epsns[bitmap_idx] &&
            iter_psn - per_path_epsns[bitmap_idx] < ErasureCoding::BITMAP_SIZE_KEC) {
            per_path_bitmaps.at(bitmap_idx).set(iter_psn - per_path_epsns[bitmap_idx]);
        }
    }

    // update this bitmap epsn
    uint32_t old_global_min_epsn = global_min_epsn;
    uint32_t ret                 = check_stripe_bitmap_update(bitmap_idx);
    update_global_epsn();
    if (old_global_min_epsn < global_min_epsn) {
        statistics.num_trivial_skips += global_min_epsn - old_global_min_epsn;
        statistics.num_received_chunks += global_min_epsn - old_global_min_epsn;
    }

    if (per_path_psn == per_path_epsns[bitmap_idx]) {
        return make_tuple(ret, false);
    } else if (per_path_psn > per_path_epsns[bitmap_idx]) {
        if (per_path_psn - per_path_epsns[bitmap_idx] >= ErasureCoding::BITMAP_SIZE_KEC) {
            return make_tuple(0, true);
        }

        return make_tuple(ret, false);
    }
}

void KECbrSink::update_mbl(CbrPacket& pkt, uint32_t per_path_psn, uint32_t last_per_path_psn) {
    uint32_t pkt_iter = mbl_head;
    if (pkt.msn < mbl_base_msn) {
        return;
    }
    uint32_t relative_msn = pkt.msn - mbl_base_msn;

    _mbl_overflow = false;  // Reset overflow flag

    if (relative_msn < mbl_size) {
        pkt_iter = (mbl_head + relative_msn) % ErasureCoding::MAX_MESSAGE_BOUNDARIES;
        message_boundary_list[pkt_iter] = last_per_path_psn;
    } else {
        if (relative_msn < ErasureCoding::MAX_MESSAGE_BOUNDARIES) {
            uint32_t iter      = mbl_tail;
            uint32_t last_iter = iter;
            mbl_tail =
                (mbl_tail + relative_msn + 1 - mbl_size) % ErasureCoding::MAX_MESSAGE_BOUNDARIES;
            while (iter != mbl_tail) {
                message_boundary_list[iter] = UINT32_MAX;
                last_iter                   = iter;
                iter                        = (iter + 1) % ErasureCoding::MAX_MESSAGE_BOUNDARIES;
                if (iter == mbl_tail) {
                    message_boundary_list[last_iter] = last_per_path_psn;
                    pkt_iter                         = last_iter;
                }
            }
            mbl_size = relative_msn + 1;
        } else {
            cout << "[CbrSink] Message boundary list is full, cannot add new entry! relative_msn: "
                 << relative_msn << " mbl_base_msn: " << mbl_base_msn << " pkt.msn: " << pkt.msn
                 << " flow_id: " << _src->flow().flow_id() << endl;

            // do nothing and wait for NACK timeout
            _mbl_overflow = true;
            // TODO: assert check_pkt will be overflowed as well!
        }
    }

    uint32_t iter = mbl_head;
    while (iter != pkt_iter && mbl_size > 0) {
        uint32_t message_start = per_path_psn - pkt.stripe_offset;
        if (message_boundary_list[iter] > message_start - 1) {
            message_boundary_list[iter] = message_start - 1;
        }
        iter = (iter + 1) % ErasureCoding::MAX_MESSAGE_BOUNDARIES;
    }
}

void KECbrSink::complete_mbl(Packet& pkt, simtime_picosec ts, uint32_t epsn) {
    if (mbl_size == 0) {
        return;  // no message boundaries to complete
    }

    // complete message boundary list from head
    uint32_t iter         = mbl_head;
    uint32_t relative_msn = 0;

    while (iter != mbl_tail && mbl_size > 0) {
        uint32_t this_msn = mbl_base_msn + relative_msn;

        if (epsn > message_boundary_list[iter]) {
            // send SACK for the completed message
            statistics.num_received_messages++;
            if (missing_stripes_map.find(this_msn) == missing_stripes_map.end()) {
                send_sack_packet_ec(ts, this_msn, 0, vector<uint32_t>());
            } else {
                // cout << "[CbrSink] flow_id: missing " << _src->flow().flow_id()
                //      << " Sending SACK for msn: " << this_msn << " with "
                //      << missing_stripes_map[this_msn].size() << " missing stripes." << endl;
                send_sack_packet_ec(ts,
                                    this_msn,
                                    missing_stripes_map[this_msn].size(),
                                    missing_stripes_map[this_msn]);
            }

            message_boundary_list[iter] = UINT32_MAX;  // Reset the message boundary, meaning this
                                                       // entry is not occupied anymore
            mbl_size--;
            mbl_head = (mbl_head + 1) % ErasureCoding::MAX_MESSAGE_BOUNDARIES;
            relative_msn++;
        }
        iter = (iter + 1) % ErasureCoding::MAX_MESSAGE_BOUNDARIES;
    }

    mbl_base_msn += relative_msn;
    if (relative_msn > 0)
        _mbl_change = true;

    // cout << "[CbrSink] Completed " << relative_msn
    //      << " message boundaries, new mbl_base_msn: " << mbl_base_msn << ", mbl_size: " <<
    //      mbl_size
    //      << endl;

    _nack_restart_timeout = eventlist().now() + _restart_timeout;
}

bool KECbrSink::check_flow_active() {
    uint32_t compare = per_path_epsns[0];
    for (size_t idx = 1; idx < _erasure_coding->entropy_list.size(); idx++) {
        if (compare != per_path_epsns[idx]) {
            return true;
        }
    }

    for (size_t idx = 0; idx < _erasure_coding->entropy_list.size(); idx++) {
        if (per_path_bitmaps[idx].any()) {
            // If any bitmap has a 1, the flow is still active
            return true;
        }
    }

    return false;
}

void KECbrSink::restart_flow_timer_hook(simtime_picosec now, simtime_picosec period) {
    // cout << "[CbrSink] restart_flow_timer_hook called at time: " << timeAsUs(now)
    //      << ", flow_active: " << flow_active
    //      << ", _restart_timeout_pending: " << _restart_timeout_pending
    //      << ", _mbl_change: " << _mbl_change << ", now: " << timeAsUs(now)
    //      << ", _nack_restart_timeout: " << timeAsUs(_nack_restart_timeout)
    //      << ", _restart_timeout: " << timeAsUs(_restart_timeout)
    //      << ", flow_id: " << _src->flow().flow_id() << endl;

    if (!flow_active) {
        return;
    }

    if (now <= _nack_restart_timeout || _nack_restart_timeout == timeInf)
        return;

    if (_mbl_change) {
        // reset the timer
        _nack_restart_timeout    = now + _restart_timeout;
        _restart_timeout_pending = false;
        _mbl_change              = false;  // reset the flag
        return;
    }

    // Timeout and send NACK for restarting the flow
    if (!_restart_timeout_pending) {
        _restart_timeout_pending = true;
        flow_active              = check_flow_active();

        // check the timer difference between the event and the real value
        simtime_picosec too_late = now - (_nack_restart_timeout);

        // careful: we might calculate a negative value
        // to prevent overflow but keep randomness we just divide until we are within the limit
        while (too_late > period)
            too_late >>= 1;

        simtime_picosec restart_off = (period - too_late);

        eventlist().sourceIsPendingRel(*this, restart_off);

        _nack_restart_timeout = now + _restart_timeout;
    }
}

void KECbrSink::send_nack_packet_ec(simtime_picosec ts) {
    if (!use_erasure_coding) {
        cerr << "[CbrSink] send_nack_packet_ec called without erasure coding enabled!" << endl;
        abort();
    }

    statistics.num_nack_sent++;

    cout << "[CbrSink] flow_id: " << _src->flow().flow_id() << " Sending NACK: "
         << " at time: " << timeAsUs(ts) << endl;

    // Send a NACK packet to restart the flow
    CbrPacket* p =
        CbrPacket::newNACKpkt(_src->flow(), *_route, global_min_epsn, _mss, _priority, _srcaddr);
    p->set_pathid(_crt_path);
    p->set_ts(ts);
    p->sendOn();
}

void KECbrSink::restartFlow() {
    // Restart the flow by sending a NACK packet
    send_nack_packet_ec(eventlist().now());

    // Clear bitmap, reset the epsn, max_psn
    for (size_t i = 0; i < per_path_bitmaps.size(); i++) {
        per_path_bitmaps.at(i).reset();
        per_path_epsns.at(i)    = 0;
        per_path_max_psns.at(i) = 0;
    }
    global_min_epsn = 0;
    global_max_psn  = 0;

    for (size_t i = 0; i < message_boundary_list.size(); i++) {
        message_boundary_list[i] = UINT32_MAX;  // Initialize to a large value
    }
    mbl_size     = 0;
    mbl_head     = 0;
    mbl_tail     = 0;
    mbl_base_msn = 0;
    missing_stripes_map.clear();
    _mbl_change   = false;
    _mbl_overflow = false;
}

void KECbrSink::doNextEvent() {
    // cout << "[CbrSink] doNextEvent called at time: " << timeAsUs(eventlist().now())
    //      << ", flow_active: " << flow_active
    //      << ", _restart_timeout_pending: " << _restart_timeout_pending
    //      << ", _mbl_change: " << _mbl_change << ", now: " << timeAsUs(eventlist().now())
    //      << ", _nack_restart_timeout: " << timeAsUs(_nack_restart_timeout)
    //      << ", _restart_timeout: " << timeAsUs(_restart_timeout)
    //      << ", flow_id: " << _src->flow().flow_id() << endl;

    if (_restart_timeout_pending) {
        _restart_timeout_pending = false;

        restartFlow();

        _nack_restart_timeout = eventlist().now() + _restart_timeout;
    }
}

////////////////////////////////////////////////////////////////
//  CBR SRC FLOW RESTART TIMER
////////////////////////////////////////////////////////////////

KECbrSrcRestartTimerScanner::KECbrSrcRestartTimerScanner(simtime_picosec scanPeriod,
                                                         EventList&      eventlist)
    : EventSource(eventlist, "RestartScanner"), _scanPeriod(scanPeriod) {
    eventlist.sourceIsPendingRel(*this, _scanPeriod);
}

void KECbrSrcRestartTimerScanner::registerCbr(KECbrSrc& cbrsrc) {
    _cbrs.push_back(&cbrsrc);
}

void KECbrSrcRestartTimerScanner::doNextEvent() {
    simtime_picosec  now = eventlist().now();
    cbrs_t::iterator i;
    bool             is_active = false;
    for (i = _cbrs.begin(); i != _cbrs.end(); i++) {
        (*i)->restart_flow_timer_hook(now, _scanPeriod);
        is_active = is_active || (!(*i)->_flow_finished);
    }
    if (is_active)
        eventlist().sourceIsPendingRel(*this, _scanPeriod);
}

////////////////////////////////////////////////////////////////
//  CBR SINK FLOW RESTART TIMER
////////////////////////////////////////////////////////////////

KECbrSinkRestartTimerScanner::KECbrSinkRestartTimerScanner(simtime_picosec scanPeriod,
                                                           EventList&      eventlist)
    : EventSource(eventlist, "RestartScanner"), _scanPeriod(scanPeriod) {
    eventlist.sourceIsPendingRel(*this, _scanPeriod);
}

void KECbrSinkRestartTimerScanner::registerCbr(KECbrSink& cbrsink) {
    _cbrs.push_back(&cbrsink);
}

void KECbrSinkRestartTimerScanner::doNextEvent() {
    simtime_picosec  now = eventlist().now();
    cbrs_t::iterator i;
    bool             is_active = false;
    for (i = _cbrs.begin(); i != _cbrs.end(); i++) {
        (*i)->restart_flow_timer_hook(now, _scanPeriod);
        is_active = is_active || (*i)->flow_active;
    }
    if (is_active)
        eventlist().sourceIsPendingRel(*this, _scanPeriod);
}