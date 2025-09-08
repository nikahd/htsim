// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "1ec_cc.h"

#include <algorithm>
#include <iostream>

#include "cbrpacket.h"
#include "data_collector.h"
#include "math.h"
#include "uec_src.h"

RouteStrategy OneECCCSrc::_route_strategy  = NOT_SET;
RouteStrategy OneECCCSink::_route_strategy = NOT_SET;

bool           OneECCCSrc::use_erasure_coding  = false;
bool           OneECCCSink::use_erasure_coding = false;
ErasureCoding* OneECCCSrc::_erasure_coding     = NULL;
ErasureCoding* OneECCCSink::_erasure_coding    = NULL;

std::string OneECCCSrc::epoch_type    = "short";
std::string OneECCCSrc::qa_type       = "short";
double      OneECCCSrc::starting_cwnd = 1;

////////////////////////////////////////////////////////////////
//  CBR SOURCE
////////////////////////////////////////////////////////////////

OneECCCSrc::OneECCCSrc(EventList&          eventlist,
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

    // CC variables
    resetEpochParams();
    _consecutive_good_epochs = 0;
    _consecutive_decreases   = 0;
    pacing_delay             = 0;
}

void OneECCCSrc::connect(Route*          routeout,
                         Route*          routeback,
                         OneECCCSink&    sink,
                         simtime_picosec starttime) {
    _route = routeout;
    _sink  = &sink;
    _flow.set_id(get_id());  // identify the packet flow with the CBR source that generated it
    _sink->connect(*this, routeback);

    _sack_restart_timeout = eventlist().now() + _restart_timeout;
    eventlist().sourceIsPending(*this, starttime);
}

void OneECCCSrc::update_offsets_ec(uint32_t msn,
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

void OneECCCSrc::update_retrans_offsets_ec(uint32_t msn,
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

bool OneECCCSrc::more_data_available() const {
    if (message_list.empty() && received_msg_sacks.empty()) {
        // cout << "[CbrSrc] No packets to send, waiting for flow to complete"
        //      << " flow_id: " << _flow.flow_id() << endl;
        return false;
    }

    if (unfinished_message_list.empty()) {
        cerr << "[CbrSrc] flow_id: " << _flow.flow_id()
             << " unfinished_message_list is empty but it still has messages to send, "
                "aborting"
             << endl;
        abort();
    }
    return true;
}

void OneECCCSrc::send_packets_ec() {
    uint32_t c = _cwnd;
    // print out the condition

    if (get_unacked() + _erasure_coding->get_chunk_size() * _mss > c) {
        cout << "[CbrSrc] flow_id: " << _flow.flow_id() << " Sending packets, current cwnd: " << c
             << " get_unacked(): " << get_unacked() << " _mss: " << _mss << endl;
    }

    while (get_unacked() + _erasure_coding->get_chunk_size() * _mss <= c && more_data_available()) {
        send_next_packet_ec();

        if (!use_pacing) {
            eventlist().sourceIsPendingRel(*this, _period);
        } else {
            eventlist().sourceIsPendingRel(*this, pacing_delay);
        }
    }
}

void OneECCCSrc::send_next_packet_ec() {
    _cur_is_retrans = (original_packet_list.empty() && message_list.empty());
    // send all original messages first

    if (_cur_is_retrans && received_msg_sacks.empty()) {
        cout << "[CbrSrc] flow_id: " << _flow.flow_id() << " No retransmission packets to send"
             << endl;
        return;
    }

    // The sender is always active
    if (!_cur_is_retrans) {
        // sending an original packet

        if (original_packet_list.empty() && message_list.empty()) {
            cerr << "[CbrSrc] flow_id: " << _flow.flow_id()
                 << " original_packet_list is empty when trying to send original packet!" << endl;
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
            retransmission_packet_list.push_back(
                make_tuple(next_msn,
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
}

void OneECCCSrc::doNextEvent() {
    if (rand() % 100 < 1 && !_flow_finished) {
        // logging sending rate
        _statistics_outfile << "Flow " << _flow.flow_id()
                            << " time: " << timeAsUs(eventlist().now()) << " cwnd: " << _cwnd / _mss
                            << endl;
    }

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

        // send_packets_ec();

        if (get_unacked() + _erasure_coding->get_chunk_size() * _mss > _cwnd) {
            cout << "[CbrSrc] flow_id: " << _flow.flow_id()
                 << " Sending packets, current cwnd: " << _cwnd
                 << " get_unacked(): " << get_unacked() << " _mss: " << _mss << endl;
        }

        if (get_unacked() + _erasure_coding->get_chunk_size() * _mss <= _cwnd &&
            more_data_available()) {
            send_next_packet_ec();
        }

        if (!use_pacing) {
            eventlist().sourceIsPendingRel(*this, _period);
        } else {
            eventlist().sourceIsPendingRel(*this, pacing_delay);
        }

        if (!use_erasure_coding) {
            // Normal CBR
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
int OneECCCSrc::choose_route() {
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
            cerr << "routing strategy not set for OneECCCSrc " << _nodename << endl;
            abort();  // shouldn't be here at all
        default:
            cerr << "routing strategy not supported for OneECCCSrc " << _nodename << endl;
            abort();
            break;
    }

    return _crt_path;
}

void OneECCCSrc::start_flow() {
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

            epoch_counter             = _cwnd;
            epoch_enabled             = false;
            qa_enabled                = false;
            qa_measurement_period_end = eventlist().now();
            epoch_end_time            = 0;
        }
    }
}

void OneECCCSrc::setFlowSize(uint64_t flow_size_in_bytes) {
    _flow_size = flow_size_in_bytes;
}

void OneECCCSrc::reduce_unacked(uint64_t amount) {
    if (_unacked >= amount) {
        _unacked -= amount;
    } else {
        _unacked = 0;
    }
}

void OneECCCSrc::send_packet_ec(uint32_t msn,
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

    // if (get_unacked() + _erasure_coding->get_chunk_size() * _mss > _cwnd) {
    //     cout << "[CbrSrc] flow_id: " << _flow.flow_id() << " Not sending packet, cwnd: " << _cwnd
    //          << ", unacked: " << get_unacked() << ", mss: " << _mss
    //          << ", now: " << timeAsUs(eventlist().now()) << ", waiting for SACKs" << endl;
    //     return;
    // }

    if (get_unacked() + _mss > _cwnd) {
        cout << "[CbrSrc] flow_id: " << _flow.flow_id() << " Not sending packet, cwnd: " << _cwnd
             << ", unacked: " << get_unacked() << ", mss: " << _mss
             << ", now: " << timeAsUs(eventlist().now()) << ", waiting for SACKs" << endl;
        return;
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
    _unacked += p->size();

    // cout << "flow_id: " << _flow.flow_id() << " ts: " << eventlist().now() << " psn = " << psn <<
    // endl;

    p->sendOn();
    track_sending_rate();

    if (epoch_counter > 0) {
        epoch_counter -= p->data_packet_size();
        if (epoch_counter <= 0) {
            resetEpochParams();
            epoch_enabled = true;
        }
    }

    _sack_restart_timeout = eventlist().now() + _restart_timeout;
}

void OneECCCSrc::SACKTimeOut() {
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

        reduce_unacked(this_sack.num_missing_stripes * _erasure_coding->get_chunk_size() * _mss);

        _statistics_outfile << "ts: " << timeAsUs(eventlist().now())
                            << ", flow_id: " << _flow.flow_id() << " SACK Timeout" << endl;
    }
}

void OneECCCSrc::processECSACK(CbrPacket& pkt) {
    cout << timeAsUs(eventlist().now()) << " [CbrSrc] Received SACK for msn: " << pkt.msn
         << ", original_msn: " << pkt.original_msn
         << ", num_missing_stripes: " << pkt.num_missing_stripes << ", flow_id: " << _flow.flow_id()
         << ", unfinished_message_list.size(): " << unfinished_message_list.size() << endl;

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

void OneECCCSrc::restartFlow() {
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

    // should not reset CC variables

    reset_unacked();

    // reinitialize entropy values
    // for (size_t i = 0; i < _erasure_coding->entropy_list.size(); i++) {
    //     uint32_t new_ev = rand() % _erasure_coding->num_entropies;
    //     while (find(_erasure_coding->entropy_list.begin(),
    //                 _erasure_coding->entropy_list.begin() + i,
    //                 new_ev) != _erasure_coding->entropy_list.begin() + i) {
    //         new_ev = rand() % _erasure_coding->num_entropies;
    //     }
    //     _erasure_coding->entropy_list[i] = new_ev;
    // }

    eventlist().sourceIsPendingRel(*this, _period);
}

void OneECCCSrc::processECNACK(CbrPacket& pkt) {
    // Process message-level NACK packets for erasure coding, restart the flow by setting epsn to 0

    // cout << "[CbrSrc] Received NACK packet, restarting flow with msn: " << pkt.msn
    //      << ", flow_id: " << _flow.flow_id() << endl;

    cout << "[CbrSrc] Received NACK, restarting flow_id: " << _flow.flow_id()
         << ", number_of_messages: " << number_of_messages
         << ", unfinished_message_list.size(): " << unfinished_message_list.size() << endl;

    restartFlow();
}

void OneECCCSrc::receivePacket(Packet& pkt) {
    // handle SACK packets for retransmission
    if (use_erasure_coding) {
        if (static_cast<CbrPacket&>(pkt).pkt_type == CbrPacket::SACK) {
            processECSACK(static_cast<CbrPacket&>(pkt));
        } else if (static_cast<CbrPacket&>(pkt).pkt_type == CbrPacket::NACK) {
            processECNACK(static_cast<CbrPacket&>(pkt));
        } else if (static_cast<CbrPacket&>(pkt).pkt_type == CbrPacket::METRIC_ACK) {
            processECMetricACK(static_cast<CbrPacket&>(pkt));
        } else {
            cerr << "[CbrSrc] Received unknown packet type: "
                 << static_cast<CbrPacket&>(pkt).pkt_type << ", flow_id: " << _flow.flow_id()
                 << endl;
            abort();
        }
    }

    if (rand() % 100 < 1 && !_flow_finished) {
        // logging sending rate
        _statistics_outfile << "Flow " << _flow.flow_id()
                            << " time: " << timeAsUs(eventlist().now()) << " cwnd: " << _cwnd / _mss
                            << endl;
    }

    _sack_restart_timeout = eventlist().now() + _restart_timeout;

    pkt.free();  // Free the packet after processing
}

void OneECCCSrc::send_packet() {
    Packet* p = CbrPacket::newpkt(_flow, *_route, _crt_id++, _mss, _priority, _dstaddr);
    _sent_bytes += p->size();

    int crt = choose_route();
    p->set_pathid(crt);
    p->sendOn();

    eventlist().sourceIsPendingRel(*this, _period);
}

void OneECCCSrc::processECMetricACK(CbrPacket& pkt) {
    cout << "[CbrSrc] Received Metric ACK, flow_id: " << _flow.flow_id()
         << ", pkt.id(): " << pkt.id() << ", num_bytes_acked: " << pkt.num_bytes_acked
         << ", original data packet ts: " << timeAsUs(pkt.ts()) << endl;

    uint32_t        path_id          = pkt.id() % _erasure_coding->entropy_list.size();
    uint32_t        bytes_ecn_marked = pkt.bytes_ecn_marked;
    uint64_t        now_time         = eventlist().now();
    simtime_picosec ts               = pkt.ts();
    uint64_t        num_bytes_acked  = pkt.num_bytes_acked;
    uint64_t        ack_path_rtt     = now_time - ts;

    cout << "processECMetricACK flow_id: " << _flow.flow_id() << " now_time: " << timeAsUs(now_time)
         << ", ts: " << timeAsUs(ts) << ", ack_path_rtt: " << ack_path_rtt << endl;

    bool pkt_ecn_marked = (bytes_ecn_marked > 0);
    if (use_per_ack_ewma) {
        // pass 1 if ecn_maked and 0 otherwise
        ecn_fraction_ewma = computeEwma(ecn_fraction_ewma, pkt_ecn_marked, lcp_ecn_alpha);
    }

    // use metric ACK received bytes to update _unacked bytes;
    // sender tracks total bytes sent since last updated cwnd to track in-flight bytes
    // For debugging, print whether always in-flight > cwnd
    reduce_unacked(pkt.num_bytes_acked);

    vector<simtime_picosec> one_way_latency_delta, per_path_rtt;
    one_way_latency_delta.resize(_erasure_coding->entropy_list.size(), 0);
    per_path_rtt.resize(_erasure_coding->entropy_list.size(), ack_path_rtt);

    simtime_picosec max_per_path_rtt = 0;

    for (size_t i = 0; i < pkt.one_way_latencies.size(); i++) {
        int64_t delta   = pkt.one_way_latencies[i] >= pkt.one_way_latencies[path_id]
                              ? (pkt.one_way_latencies[i] - pkt.one_way_latencies[path_id])
                              : (-(pkt.one_way_latencies[path_id] - pkt.one_way_latencies[i]));
        per_path_rtt[i] = delta + ack_path_rtt;
        cout << "flow_id: " << _flow.flow_id() << ", pkt.id(): " << pkt.id()
             << ", one_way_latencies[" << i << "]: " << pkt.one_way_latencies[i]
             << ", delta: " << delta << ", ack_path_rtt: " << ack_path_rtt << ", per_path_rtt[" << i
             << "]: " << per_path_rtt[i] << endl;
    }

    uint64_t        path_to_replace = -1;
    simtime_picosec max_rtt         = 0;
    // Use the new helper function to compute path metrics
    compute_path_metrics(per_path_rtt, &path_to_replace, &max_rtt);

    cout << "apply_mimd=" << apply_mimd << " cwnd_Before_adjust: " << _cwnd << endl;

    if (apply_mimd) {
        adjust_window_mimd(path_to_replace, max_rtt, pkt.entropy_to_replace);
    } else {
        adjust_window_aimd(num_bytes_acked, bytes_ecn_marked, ts, max_rtt, pkt.entropy_to_replace);
    }
}

void OneECCCSrc::updateParams(uint64_t      base_rtt,
                              linkspeed_bps network_linkspeed,
                              double        init_cwnd_ratio,
                              double        contract_scaling,
                              uint64_t      inter_queuesize) {
    _base_rtt          = base_rtt;
    _network_linkspeed = network_linkspeed;
    queuesize_bytes    = inter_queuesize;

    this->contract_scaling = contract_scaling;

    _bdp                    = (timeAsSec(_base_rtt) * _network_linkspeed / 8);
    _queue_size             = _bdp;  // Temporary
    _maxcwnd                = _maxcwnd_bdp_ratio * _bdp;
    starting_cwnd_bdp_ratio = init_cwnd_ratio;
    printf("BDP is %lu (Bytes) \n", _bdp);
    printf("Starting cwnd is %f\n", starting_cwnd);
    printf("Starting cwnd bdp ratio is %f\n", starting_cwnd_bdp_ratio);

    if (starting_cwnd == 1) {
        _cwnd = _bdp * starting_cwnd_bdp_ratio;
        printf("CWND3 HERE IS %f\n", _cwnd);
    } else {
        _cwnd = starting_cwnd;
        printf("CWND4 HERE IS %f\n", _cwnd);
    }
    check_limits_cwnd();
    target_window = _cwnd;

    cout << "_network_linkspeed: " << _network_linkspeed << endl;

    if (ai_bytes == 1 && ai_bytes_scale <= 0)
        ai_bytes = _bdp * 0.0006;  // compare to queue size, should be comparable to the queue size
    else if (ai_bytes_scale > 0) {
        if (ai_bytes_scale > 1) {
            cout << "ai_bytes_scale should be in (0, 1]" << endl;
            abort();
        }
        ai_bytes = _bdp * ai_bytes_scale;
    }

    baremetal_rtt = _base_rtt;
    assert(target_to_baremetal_ratio > 0);
    target_rtt                = baremetal_rtt * target_to_baremetal_ratio;
    qa_measurement_period_end = 2 * target_rtt;
    float queue_latency_ns    = (float)queuesize_bytes * 8 / (float)(INTER_LINK_SPEED_MODERN / 1e9);

    qa_trigger_rtt = QA_TRIGGER_RTT_FRACTION * queue_latency_ns * 1000.0 + baremetal_rtt;
    target_qdelay  = target_rtt - baremetal_rtt;

    // based on gemini, lcp_k and bdp are both in bytes
    if (lcp_k <= 0) {
        lcp_k = (_bdp * 1.0 / 7);
    }
    lcp_k /= lcp_k_scale;
    // ai_bytes /= lcp_k_scale;

    md_gain_ecn = 0.025;

    if (md_gain_ecn <= 0 || md_gain_ecn >= 1) {
        md_gain_ecn = lcp_k * 4.0 / (_bdp + lcp_k);
        md_gain_ecn = md_gain_ecn * 1;
    } else {
        cout << "md_gain_ecn is over-written to a constant value: " << md_gain_ecn
             << " but this should only happen for epoch_type == long" << endl;
    }

    if (md_factor_rtt <= 0 || md_factor_rtt >= 1)
        md_factor_rtt = target_qdelay * 1.0 / (target_qdelay + target_rtt);
    else {
        cout << "md_factor_rtt is over-written to a constant value: " << md_factor_rtt
             << " but this should only happen for epoch_type == short" << endl;
    }

    if (md_gain_ecn >= 1 || md_factor_rtt >= 1) {
        cout << "md_gain_ecn: " << md_gain_ecn << endl;
        cout << "md_factor_rtt: " << md_factor_rtt << endl;
        cout << "Invalid md_gain_ecn or md_factor_rtt" << endl;
        // abort();
    }

    // assert(qa_trigger_rtt > target_rtt);

    if (epoch_type != "short" && qa_type != "long") {
        cout << "When epoch_type != constant_timestamp, we cannot have qa_type != long" << endl;
        abort();
    }

    cout << "Link speed (Gbps), " << INTER_LINK_SPEED_MODERN / 1e9 << endl;
    cout << "Queue size (Bytes), " << queuesize_bytes << endl;
    cout << "Base RTT (us), " << timeAsUs(_base_rtt) << endl;
    cout << "Network link speed (bps), " << _network_linkspeed << endl;
    cout << "BDP (Bytes), " << _bdp << endl;
    cout << "Starting cwnd (Bytes), " << _cwnd << endl;
    cout << "Target cwnd (Bytes), " << target_window << endl;
    cout << "Max cwnd (Bytes), " << _maxcwnd << endl;
    cout << "ai_bytes (Bytes), " << ai_bytes << endl;
    cout << "ai_bytes_scale, " << ai_bytes_scale << endl;
    cout << "lcp_k (Bytes), " << lcp_k << endl;
    cout << "lcp_k_scale, " << lcp_k_scale << endl;
    cout << "md_gain_ecn, " << md_gain_ecn << endl;
    cout << "md_factor_rtt, " << md_factor_rtt << endl;
    cout << "target_to_baremetal_ratio, " << target_to_baremetal_ratio << endl;
    cout << "target_rtt (us), " << timeAsUs(target_rtt) << endl;
    cout << "baremetal_rtt (us), " << timeAsUs(baremetal_rtt) << endl;
    cout << "qa_trigger_rtt (us), " << timeAsUs(qa_trigger_rtt) << endl;
    cout << "target_qdelay (us), " << timeAsUs(target_qdelay) << endl;
    cout << "qa_measurement_period_end (us), " << timeAsUs(qa_measurement_period_end) << endl;
    cout << "contract_scaling, " << contract_scaling << endl;

    update_pacing_delay();
}

void OneECCCSrc::compute_path_metrics(vector<simtime_picosec>& per_path_rtt,
                                      uint64_t*                path_to_replace,
                                      simtime_picosec*         max_rtt) {
    double          mean_rtt       = 0;
    simtime_picosec second_max_rtt = 0;
    size_t          max_idx        = -1;
    *max_rtt                       = 0;
    *path_to_replace               = -1;

    // Calculate mean RTT and find max RTT
    for (size_t i = 0; i < per_path_rtt.size(); i++) {
        mean_rtt += per_path_rtt[i];
        if (per_path_rtt[i] > *max_rtt) {
            second_max_rtt = *max_rtt;
            *max_rtt       = per_path_rtt[i];
            max_idx        = i;
        } else if (per_path_rtt[i] > second_max_rtt && per_path_rtt[i] < *max_rtt) {
            second_max_rtt = per_path_rtt[i];
        }
    }
    mean_rtt /= per_path_rtt.size();

    // Find jittery path
    size_t jittery_path = -1;
    if (use_replace_path) {
        simtime_picosec rtt_threshold =
            jittery_path_replace_threshold_percentage * mean_rtt;  // 10% of the mean RTT
        for (size_t i = 0; i < per_path_rtt.size(); i++) {
            if (per_path_rtt[i] - mean_rtt > rtt_threshold) {
                jittery_path = i;
            }
        }
    }

    // Determine path to replace
    if (jittery_path != -1 && max_idx != -1) {
        if (max_idx == jittery_path) {
            *max_rtt         = second_max_rtt;
            *path_to_replace = jittery_path;
        }
    }
}

void OneECCCSrc::adjust_window_mimd(uint64_t        path_to_replace,
                                    simtime_picosec max_rtt,
                                    uint32_t        entropy_to_replace) {
    if (!apply_mimd) {
        cout << "How is adjust_window_mimd called in AIMD!" << endl;
        abort();
    }

    if (max_rtt < _base_rtt) {
        cout << "MIMD: max_rtt < _base_rtt, resetting _base_rtt to max_rtt" << endl;
        _base_rtt = max_rtt;
    }
    simtime_picosec delay        = max_rtt - _base_rtt;
    simtime_picosec now          = eventlist().now();
    simtime_picosec target_delay = contract_target_delay_vegas(_cwnd, max_rtt, delay);

    // MIMD
    double delay_ratio = 2;
    if (delay > 0)
        delay_ratio = (double)target_delay / (double)delay;
    delay_ratio = min(2.0, delay_ratio);

    double target_rtt   = _base_rtt + target_delay;
    double current_rate = (double)_cwnd / timeAsSec(max_rtt);    // bytes per second
    double target_cwnd  = current_rate * timeAsSec(target_rtt);  // bytes
    double cwnd_ratio   = (double)target_cwnd / (double)_cwnd;
    double rate         = (double)_cwnd * 8 / timeAsSec(max_rtt);  // bits per second
    double rate_ratio   = rate / (double)_network_linkspeed;

    // cout << "MIMD: delay: " << delay << ", target_delay: " << target_delay
    //      << ", now: " << timeAsUs(now) << ", t_last_update: " << timeAsUs(t_last_update)
    //      << ", max_rtt: " << max_rtt << ", _cwnd: " << _cwnd << ", target_cwnd: " << target_cwnd
    //      << ", delay_ratio: " << delay_ratio << ", cwnd_ratio: " << cwnd_ratio << ", rate: " <<
    //      rate
    //      << endl;

    // Every RTT we want:
    // To obtain rtt that has been produced due to cwnd change, we need to wait 1 rtt.
    if (now > t_last_update + max_rtt) {
        cout << "MIMD: Updating cwnd and replace path" << endl;

        // cwnd is mean of current and target_cwnd = _cwnd * ratio;
        double new_cwnd = ((double)_cwnd + target_cwnd) / 2;
        _cwnd           = min((mem_b)new_cwnd, (mem_b)(2 * _cwnd));
        t_last_update   = now;

        // For EC, _cwnd is multiply of stripes

        cout << "time: " << timeAsUs(now) << ", flow_id: " << _flow.flow_id()
             << ", _cwnd: " << _cwnd << endl;

        reset_unacked();

        // replace path
        // log and count the number of replacing due to jittery paths / loss count
        if (use_replace_path) {
            if (path_to_replace != -1 || entropy_to_replace != -1) {
                // randomly choose another entropy value for the path
                uint32_t new_ev = rand() % _erasure_coding->num_entropies;
                while (find(_erasure_coding->entropy_list.begin(),
                            _erasure_coding->entropy_list.end(),
                            new_ev) != _erasure_coding->entropy_list.end()) {
                    new_ev = rand() % _erasure_coding->num_entropies;
                }
                // Replace loss count based prior to rtt based
                if (entropy_to_replace != -1) {
                    _erasure_coding->entropy_list[entropy_to_replace] = new_ev;
                    num_replace_loss += 1;
                } else if (path_to_replace != -1) {
                    _erasure_coding->entropy_list[path_to_replace] = new_ev;
                    num_replace_jittery += 1;
                }
            }
        }
    }

    check_limits_cwnd();
}

void OneECCCSrc::check_limits_cwnd() {
    // Upper Limit
    // don't check upper limit for mimd

    if (!apply_mimd && _cwnd > _maxcwnd) {
        _cwnd = _maxcwnd;
    }

    // Lower Limit
    if (_cwnd < _erasure_coding->get_chunk_size() * _mss) {
        _cwnd = _erasure_coding->get_chunk_size() * _mss;
    }

    update_pacing_delay();
}

void OneECCCSrc::track_sending_rate() {
    if (eventlist().now() > last_track_ts + tracking_period) {
        double rate =
            (double)(tracking_bytes * 8.0 / (timeAsNs((eventlist().now() - last_track_ts))));
        list_sending_rate.push_back(std::make_pair(timeAsNs(eventlist().now()), rate));
        tracking_bytes = 0;
        last_track_ts  = eventlist().now();
    }
}

void OneECCCSrc::update_pacing_delay() {
    bool is_time_to_update =
        (last_pac_change == 0) || ((eventlist().now() - last_pac_change) > _base_rtt / 20);
    if (use_pacing && is_time_to_update) {
        simtime_picosec one_window_send_time = (_mss + CBR_ACKSIZE) * _base_rtt;
        pacing_delay                         = one_window_send_time * (1.0 / _cwnd);

        last_pac_change = eventlist().now();
    }
}

void OneECCCSrc::adjust_window_aimd(uint64_t        num_bytes_acked,
                                    uint64_t        bytes_ecn_marked,
                                    simtime_picosec ts,
                                    uint64_t        rtt,
                                    uint32_t        entropy_to_replace) {
    if (apply_mimd) {
        cout << "How is adjust_window_aimd called in MIMD!" << endl;
        abort();
    }

    if (rtt < _base_rtt) {
        cout << "AIMD: max_rtt < _base_rtt, resetting _base_rtt to rtt" << endl;
        cout << "new _base_rtt: " << timeAsUs(rtt) << endl;
        _base_rtt = rtt;
    }

    bool ecn_marking_true = (bytes_ecn_marked > 0);
    if (epoch_enabled) {
        total_new_bytes_acked_in_epoch += num_bytes_acked;
        if (ecn_marking_true) {
            _list_ecn_received.push_back(std::make_pair(timeAsNs(eventlist().now()), 1));
            ecn_marked_bytes_in_epoch += bytes_ecn_marked;
        }
    }

    cout << "[CbrSrc] adjust_window_aimd: ecn_marking_true: " << (bytes_ecn_marked > 0)
         << ", epoch_enabled: " << epoch_enabled << ", ts: " << timeAsUs(ts)
         << ", total_new_bytes_acked_in_epoch: " << total_new_bytes_acked_in_epoch
         << ", ecn_marked_bytes_in_epoch: " << ecn_marked_bytes_in_epoch
         << ", flow_id: " << _flow.flow_id() << endl;

    if (qa_enabled) {
        bytes_acked_in_qa_period += num_bytes_acked;
    }

    if (_flow_finished) {
        // flow is already finished, no need to check the extra packets
        return;
    }

    if (use_qa) {
        if (!qa_enabled && ts >= qa_measurement_period_end) {
            qa_enabled = true;
            if (qa_type == "long")
                qa_measurement_period_end = eventlist().now() + 2 * target_rtt;
            else if (qa_type == "short")
                qa_measurement_period_end =
                    eventlist().now() + (2 * target_rtt * 1.0 * constant_timestamp_epoch_ratio);
        } else if (qa_enabled && eventlist().now() >= qa_measurement_period_end) {
            processQaMeasurementEnd(rtt);
        }
    }

    bool ecn = bytes_ecn_marked > 0;
    if ((!use_ecn || !ecn) && (!use_rtt || rtt < target_rtt)) {
        if (!apply_ai_per_epoch) {
            additive_increase(num_bytes_acked);
        }
    } else {
        _consecutive_good_epochs = 0;
        fast_increase_round      = 0;
    }

    cout << "[CbrSrc] adjust_window_aimd: num_bytes_acked: " << num_bytes_acked
         << " bytes, ecn_marked_bytes: " << bytes_ecn_marked << ", ts: " << timeAsUs(ts)
         << ", rtt: " << timeAsUs(rtt) << ", target_rtt: " << timeAsUs(target_rtt)
         << ", base_rtt: " << timeAsUs(_base_rtt) << " use_rtt: " << use_rtt
         << "use_ecn: " << use_ecn << ", now_time: " << timeAsUs(eventlist().now())
         << ", flow_id: " << _flow.flow_id() << ", _cwnd: " << _cwnd
         << ", shouldtriggerepochend: " << shouldTriggerEpochEnd(ts)
         << ", unfinished_message_list_size: " << unfinished_message_list.size() << endl;

    if (shouldTriggerEpochEnd(ts))
        processEpochEnd(rtt);

    check_limits_cwnd();
}

void OneECCCSrc::additive_increase(uint64_t num_bytes_acked) {
    if (use_fi && _consecutive_good_epochs >= fi_threshold) {
        // Fast increase
        //_cwnd += num_bytes_acked;

        _statistics_outfile << "before increase: _cwnd: " << _cwnd << endl;
        double amount = ((ai_bytes * num_bytes_acked / _cwnd) * fast_increase_round);
        _cwnd += amount;
        _statistics_outfile << "Doing Additive Increase " << _name.c_str() << " - Time "
                            << timeAsUs(eventlist().now()) << " - Amount " << amount << " - Cwnd "
                            << _cwnd << " - Actual Cwnd "
                            << floor(_cwnd / (_erasure_coding->get_chunk_size() * _mss)) *
                                   (_erasure_coding->get_chunk_size() * _mss)
                            << " - Ai Bytes " << ai_bytes << " - Fast Increase Round "
                            << fast_increase_round << endl;

    } else {
        // Additive increase

        _statistics_outfile << "before increase: _cwnd: " << _cwnd << endl;
        double amount = (ai_bytes * num_bytes_acked / _cwnd);
        _cwnd += amount;
        _statistics_outfile << "Doing Additive Increase " << _name.c_str() << " - Time "
                            << timeAsUs(eventlist().now()) << " - Amount " << amount << " - Cwnd "
                            << _cwnd << " - Actual Cwnd "
                            << floor(_cwnd / (_erasure_coding->get_chunk_size() * _mss)) *
                                   (_erasure_coding->get_chunk_size() * _mss)
                            << " - Ai Bytes " << ai_bytes << endl;
    }
}

bool OneECCCSrc::shouldTriggerEpochEnd(simtime_picosec acked_pkt_ts) {
    cout << "epoch_enabled: " << epoch_enabled << endl;
    if (!epoch_enabled)
        return false;

    cout << "shouldTriggerEpochEnd: acked_pkt_ts: " << timeAsUs(acked_pkt_ts)
         << ", epoch_end_time: " << timeAsUs(epoch_end_time) << " epoch_type: " << epoch_type
         << ", now: " << timeAsUs(eventlist().now()) << endl;

    if (epoch_type == "long" || epoch_type == "short") {
        return (acked_pkt_ts >= epoch_end_time);
    }
    cout << "Uknown epoch type:" << epoch_type << endl;
    exit(0);
}

void OneECCCSrc::resetEpochParams() {
    ecn_marked_bytes_in_epoch      = 0;
    total_new_bytes_acked_in_epoch = 0;
    // epoch_end_time = eventlist().now() + most_recent_rtt;
    // epoch_end_time   = eventlist().now();
    epoch_start_cwnd = _cwnd;
}

void OneECCCSrc::processQaMeasurementEnd(simtime_picosec rtt) {
    // if use_rtt is false, it should not work for QA either
    bool trigger_qa_rtt = use_rtt && (rtt > qa_trigger_rtt);

    bool trigger_qa_bytes_acked;

    if (qa_type == "long")
        trigger_qa_bytes_acked = (bytes_acked_in_qa_period < _cwnd * QA_CWND_RATIO_THRESHOLD);
    else if (qa_type == "short")
        trigger_qa_bytes_acked =
            (bytes_acked_in_qa_period <
             (_cwnd * QA_CWND_RATIO_THRESHOLD * 1.0 * constant_timestamp_epoch_ratio));

    bool trigger_qa = trigger_qa_rtt || trigger_qa_bytes_acked;
    if (trigger_qa) {
        // Reduce cwnd to be equal to the bytes received in this QA period.
        // _cwnd = bytes_acked_in_qa_period * 0.96 / 2;
        _cwnd = bytes_acked_in_qa_period / 2;

        // Reset epoch info
        resetEpochParams();
        _consecutive_good_epochs = 0;

        cout << "in processQaMesurementEnd, trigger_qa_rtt: " << trigger_qa_rtt
             << ", trigger_qa_bytes_acked: " << trigger_qa_bytes_acked
             << ", bytes_acked_in_qa_period: " << bytes_acked_in_qa_period << ", _cwnd: " << _cwnd
             << endl;
        epoch_enabled       = false;
        fast_increase_round = 0;
        epoch_counter       = _cwnd;

        // reset qa info
        qa_enabled                = false;
        qa_measurement_period_end = eventlist().now();
    } else {
        if (qa_type == "long")
            qa_measurement_period_end = eventlist().now() + 2 * target_rtt;
        else if (qa_type == "short")
            qa_measurement_period_end =
                eventlist().now() + (2 * target_rtt * 1.0 * constant_timestamp_epoch_ratio);
    }
    bytes_acked_in_qa_period = 0;
    check_limits_cwnd();
}

float OneECCCSrc::computeEwma(double ewma_estimate, double new_value, double alpha) {
    // if (ewma_estimate != (ewma_estimate * (1 - alpha)) + (new_value * alpha)) {
    //     cout << "old ewma_estimate: " << ewma_estimate << ", new_value: " << new_value << ", new
    //     ewma_estimate: " <<
    //         (ewma_estimate * (1 - alpha)) + (new_value * alpha) << endl;
    //     cout << "------------------------" << endl;
    // }
    return (ewma_estimate * (1 - alpha)) + (new_value * alpha);
}

void OneECCCSrc::processEpochEnd(simtime_picosec rtt) {
    if (apply_mimd) {
        cout << "processEpochEnd while MIMD is activated!!" << endl;
        abort();
    }

    double ecn_fraction =
        (double)ecn_marked_bytes_in_epoch * 1.0 / (double)total_new_bytes_acked_in_epoch;
    if (!use_per_ack_ewma)
        ecn_fraction_ewma = computeEwma(ecn_fraction_ewma, ecn_fraction, lcp_ecn_alpha);

    average_online += ecn_fraction_ewma;
    average_count++;

    if (eventlist().now() - last_average_update > _base_rtt / 2) {
        old_average_online     = current_average_online;
        current_average_online = average_online / average_count;
        average_online         = 0;
        average_count          = 0;
        last_average_update    = eventlist().now();
    }

    bool ecn_congested = use_ecn && (ecn_fraction > 0);
    bool rtt_congested = use_rtt && (rtt > target_rtt);

    cout << "[CbrSrc] flow_id: " << _flow.flow_id() << " Epoch End: ecn_fraction: " << ecn_fraction
         << ", ecn_fraction_ewma: " << ecn_fraction_ewma
         << ", ecn_marked_bytes_in_epoch: " << ecn_marked_bytes_in_epoch
         << ", total_new_bytes_acked_in_epoch: " << total_new_bytes_acked_in_epoch
         << ", rtt: " << timeAsUs(rtt) << ", target_rtt: " << timeAsUs(target_rtt)
         << ", ecn_congested: " << ecn_congested << ", rtt_congested: " << rtt_congested
         << ", use_rtt: " << use_rtt << ", use_ecn: " << use_ecn
         << ", epoch_end_time: " << timeAsUs(epoch_end_time) << endl;

    if (ecn_congested || rtt_congested) {
        // Multiplicative decrease using worse of ECN and RTT decrease factor values.
        double ecn_reduction_factor = (ecn_fraction_ewma * md_gain_ecn);  // gain = lp_kx4/bdp+lp_k
        double rtt_reduction_factor = (rtt_congested ? md_factor_rtt : 0);
        assert(ecn_reduction_factor >= 0 && ecn_reduction_factor < 1);
        assert(rtt_reduction_factor >= 0 && rtt_reduction_factor < 1);

        _statistics_outfile << "[CbrSrc] Multiplicative decrease: ecn_reduction_factor: "
                            << ecn_reduction_factor
                            << ", rtt_reduction_factor: " << rtt_reduction_factor
                            << ", ecn_fraction_ewma: " << ecn_fraction_ewma
                            << ", md_gain_ecn: " << md_gain_ecn
                            << ", md_factor_rtt: " << md_factor_rtt
                            << ", _cwnd before update: " << _cwnd << endl;

        // _cwnd *= sqrt(sqrt(1 - max(ecn_reduction_factor, rtt_reduction_factor)));
        _cwnd *= pow(1 - max(ecn_reduction_factor, rtt_reduction_factor), 1 / epoch_period_factor);

        _statistics_outfile << "[CbrSrc] Multiplicative decrease: _cwnd after update: " << _cwnd
                            << endl;

        _consecutive_good_epochs = 0;
        fast_increase_round      = 0;
    } else {
        // Declare epoch as uncongested
        _consecutive_good_epochs++;
        fast_increase_round++;

        if (apply_ai_per_epoch)
            additive_increase(total_new_bytes_acked_in_epoch);
    }
    check_limits_cwnd();
    old_ecn_ewma = ecn_fraction_ewma;

    simtime_picosec previous_epoch_end_time = epoch_end_time;
    resetEpochParams();
    if (epoch_type == "short") {
        // epoch_end_time = previous_epoch_end_time + (rtt * 1.0 * constant_timestamp_epoch_ratio);
        epoch_end_time = previous_epoch_end_time +
                         (rtt / epoch_period_factor * 1.0 * constant_timestamp_epoch_ratio);
        cout << "[CbrSrc] Short epoch: epoch_end_time: " << timeAsUs(epoch_end_time)
             << ", previous_epoch_end_time: " << timeAsUs(previous_epoch_end_time)
             << ", epoch_period: " << timeAsUs(rtt / epoch_period_factor) << endl;
    }
}

simtime_picosec OneECCCSrc::contract_target_delay_vegas(mem_b           cwnd,
                                                        simtime_picosec rtt,
                                                        simtime_picosec delay) {
    // With 2 flows the delay is 0.1 rtprop.
    double rate = (double)cwnd * 8 / timeAsSec(rtt);  // bits per second
    // For Internet CC, the linkspeed is not known, so some constant should be used.
    double          ratio        = rate / (double)_network_linkspeed;
    simtime_picosec target_delay = ((double)_base_rtt / contract_scaling) * (1 / ratio);
    // cout << "Cwnd: " << cwnd << " RTT: " << rtt << " Delay: " << delay << endl;
    // cout << "Rate: " << rate << " Ratio: " << ratio << " Target Delay: " << target_delay << endl;
    return max((simtime_picosec)0, target_delay);
}

void OneECCCSrc::finish_flow() {
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

            _statistics_outfile << "[CbrSrc] num_replace_loss: " << num_replace_loss
                                << ", num_replace_jittery: " << num_replace_jittery << endl;

            _statistics_outfile
                << "[CbrSrc] Logging statistics: num_received_messages: "
                << _sink->statistics.num_received_messages
                << ", num_received_chunks: " << _sink->statistics.num_received_chunks
                << ", num_recovered_chunks: " << _sink->statistics.num_recovered_chunks
                << ", max_size_bitmap: " << _sink->statistics.max_size_bitmap
                << ", num_bitmap_overflow_drops: " << _sink->statistics.num_bitmap_overflow_drops
                << ", num_bitmap_underflow_drops: " << _sink->statistics.num_bitmap_underflow_drops
                << ", num_trivial_skips: " << _sink->statistics.num_trivial_skips
                << ", num_recoverable_skips: " << _sink->statistics.num_recoverable_skips
                << ", num_lossy_skips: " << _sink->statistics.num_lossy_skips
                << ", num_nack_sent: " << _sink->statistics.num_nack_sent << endl;

            cout << "[CbrSrc] flow_id: " << _flow.flow_id()
                 << " flow_start_time(us): " << timeAsUs(_flow_start_time)
                 << " flow_end_time(us): " << timeAsUs(eventlist().now())
                 << " flow_completion_time(us): " << timeAsUs(eventlist().now() - _flow_start_time)
                 << " total_bytes_sent: " << _sent_bytes << endl;
            cout << "[CbrSrc] Logging statistics: num_received_messages: "
                 << _sink->statistics.num_received_messages
                 << ", num_received_chunks: " << _sink->statistics.num_received_chunks
                 << ", num_recovered_chunks: " << _sink->statistics.num_recovered_chunks
                 << ", max_size_bitmap: " << _sink->statistics.max_size_bitmap
                 << ", num_bitmap_overflow_drops: " << _sink->statistics.num_bitmap_overflow_drops
                 << ", num_bitmap_underflow_drops: " << _sink->statistics.num_bitmap_underflow_drops
                 << ", num_trivial_skips: " << _sink->statistics.num_trivial_skips
                 << ", num_recoverable_skips: " << _sink->statistics.num_recoverable_skips
                 << ", num_lossy_skips: " << _sink->statistics.num_lossy_skips
                 << ", num_nack_sent: " << _sink->statistics.num_nack_sent << endl;

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

void OneECCCSrc::set_paths(uint32_t num_paths) {
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

void OneECCCSrc::restart_flow_timer_hook(simtime_picosec now, simtime_picosec period) {
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

OneECCCSink::OneECCCSink(EventList& eventlist, ofstream& statistics_outfile)
    : EventSource(eventlist, "cbrsink"),
      DataReceiver("cbr"),
      _statistics_outfile(statistics_outfile) {
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

        per_path_loss_counts.resize(_erasure_coding->entropy_list.size(), 0);
        per_path_last_loss.resize(_erasure_coding->entropy_list.size(), false);
        per_path_one_way_latencies.resize(_erasure_coding->entropy_list.size(), 0);

        epsn              = 0;
        max_psn           = 0;
        max_stripe_offset = 0;
        bitmap.reset();
    }
}

void OneECCCSink::connect(OneECCCSrc& src, const Route* route) {
    _src                = &src;
    _route              = route;
    _srcaddr            = src._srcaddr;
    _mss                = src._mss;
    _priority           = src._priority;
    flow_active         = true;
    _metric_ack_timeout = eventlist().now() + _metric_ack_period;

    registerMetrics();
}

// Register all the metrics that are gonna be collected for this object.
void OneECCCSink::registerMetrics() {
    _sink_stats = DataCollector::RegisterCsvMetric("CbrSinkStats",
                                                   {"srcNode_dstNode_flowId",
                                                    "num_received_messages",
                                                    "num_received_chunks",
                                                    "num_recovered_chunks",
                                                    "max_size_bitmap",
                                                    "num_bitmap_overflow_drops",
                                                    "num_bitmap_underflow_drops",
                                                    "num_trivial_skips",
                                                    "num_recoverable_skips",
                                                    "num_lossy_skips",
                                                    "num_nack_sent"});
}

// At the end of the experiment, log sink metrics
void OneECCCSink::logMetricSink() {
    _sink_stats->LogData({to_string(_src->flow().flow_id()),
                          to_string(statistics.num_received_messages),
                          to_string(statistics.num_received_chunks),
                          to_string(statistics.num_recovered_chunks),
                          to_string(statistics.max_size_bitmap),
                          to_string(statistics.num_bitmap_overflow_drops),
                          to_string(statistics.num_bitmap_underflow_drops),
                          to_string(statistics.num_trivial_skips),
                          to_string(statistics.num_recoverable_skips),
                          to_string(statistics.num_lossy_skips),
                          to_string(statistics.num_nack_sent)});
}

inline size_t OneECCCSink::find_highest_bit(const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap) {
    for (size_t i = bitmap.size() - 1; i > 0; i--) {
        if (bitmap[i]) {
            return i;
        }
    }
    return 0;
}

void OneECCCSink::update_bitmap_statistics() {
    if (use_erasure_coding) {
        statistics.max_size_bitmap = max(statistics.max_size_bitmap, find_highest_bit(bitmap));
    }
}

// Note: _cumulative_ack is the last byte we've ACKed.
// seqno is the first byte of the new packet.
void OneECCCSink::receivePacket(Packet& pkt) {
    _received++;
    _cumulative_ack += pkt.size();
    _last_id    = pkt.id();
    flow_active = true;
    received_since_last_log += pkt.size();

    simtime_picosec logging_period = timeFromUs((uint32_t)30000);

    cout << "[CbrSink] receivePacket: flow_id: " << pkt.flow().flow_id()
         << ", time: " << timeAsUs(eventlist().now()) << ", pkt.id: " << pkt.id()
         << ", pkt.size: " << pkt.size() << ", _cumulative_ack: " << _cumulative_ack
         << ", _last_id: " << _last_id << ", received_since_last_log: " << received_since_last_log
         << endl;

    if (eventlist().now() > last_logging + logging_period) {
        // logging receiving rate
        _statistics_outfile << received_since_last_log << " " << (double)timeAsUs(logging_period)
                            << endl;
        _statistics_outfile << "Flow " << pkt.flow().flow_id()
                            << " time: " << timeAsUs(eventlist().now()) << " receiving_rate: "
                            << received_since_last_log * 1.0 * 1000000 / timeAsUs(logging_period)
                            << " bytes/sec" << endl;
        received_since_last_log = 0;
        last_logging            = eventlist().now();
    }

    if (use_erasure_coding) {
        _nack_restart_timeout = eventlist().now() + _restart_timeout;

        simtime_picosec ts  = static_cast<CbrPacket&>(pkt).ts();
        simtime_picosec now = eventlist().now();
        max_stripe_offset =
            max(max_stripe_offset, pkt.id() - static_cast<CbrPacket&>(pkt).pkt_offset);
        max_psn = max(max_psn, pkt.id());

        // For Metric ACK
        uint32_t path_idx    = pkt.id() % _erasure_coding->entropy_list.size();
        last_path_id         = path_idx;
        last_original_pkt_ts = ts;
        this->per_path_one_way_latencies.at(path_idx) = now - ts;
        _acked_bytes_per_metric_ack += pkt.size();
        bool ecn = (bool)(pkt.flags() & ECN_CE);
        if (ecn) {
            _bytes_ecn_marked_per_metric_ack += pkt.size();
        }

        _can_send_metric_ack = true;

        update_mbl(static_cast<CbrPacket&>(pkt));

        bool     check_mbl               = false;
        uint32_t ret_pkt_skip            = 0;
        uint32_t ret_trivial_skip        = 0;
        uint32_t ret_lossy_skip          = 0;
        uint32_t ret_recoverable_skip    = 0;
        uint32_t ret_full_skip           = 0;
        bool     do_full_skip            = false;
        uint32_t ret_check_pkt_full_skip = 0;

        // cout << "flow_id: " << pkt.flow().flow_id()
        //      << " bitmap: " << bitmap.to_string() << endl;

        // Below updates the bitmap, return chunks can be skipped and new epsn for mbl
        tie(ret_pkt_skip, do_full_skip) = check_pkt(static_cast<CbrPacket&>(pkt));
        if (ret_pkt_skip) {
            check_mbl = true;
            cout << "check_pkt true flow_id: " << pkt.flow().flow_id() << endl;
        }

        if (_use_full_skip && do_full_skip) {
            // stripe checking across bitmaps for full skipping
            // 1. iterate all bitmaps first, and do whatever 3 types of skipping
            // 2. shift all bitmaps to the this per-path-psn with full skipping

            cout << "full_skip: do_full_skip true flow_id: " << _src->flow().flow_id()
                 << " target_psn: " << pkt.id() << endl;

            while (do_full_skip) {
                if (ret_trivial_skip = check_trivial_skip()) {
                    check_mbl = true;
                    // cout << " flow_id: " << pkt.flow().flow_id() << "trivial skip: epsn: " <<
                    // epsn << endl;
                    cout << "full_skip trivial_skip true flow_id: " << pkt.flow().flow_id() << endl;
                } else if (ret_recoverable_skip = check_recoverable_skip(do_full_skip)) {
                    check_mbl = true;
                    statistics.num_recoverable_skips++;
                    cout << "full_skip recoverable_skip true flow_id: " << pkt.flow().flow_id()
                         << endl;
                    // cout << " flow_id: " << pkt.flow().flow_id() << "recoverable skip: epsn: " <<
                    // epsn
                    //      << endl;
                } else if (ret_lossy_skip =
                               check_lossy_skip(do_full_skip)) {  // Check lossy skip before
                                                                  // recoverable skip, because
                    // lossy_skip_threshold is usually larger than
                    // recoverable_skip_threshold.
                    check_mbl = true;
                    statistics.num_lossy_skips++;
                    cout << "full_skip lossy_skip true flow_id: " << pkt.flow().flow_id()
                         << " pkt.path_id: " << pkt.pathid() << endl;
                    cout << "[CbrSink] check_lossy_skip: flow_id: " << pkt.flow().flow_id()
                         << " psn: " << pkt.id() << " epsn: " << epsn << " max_psn: " << max_psn
                         << " lossy_skip_threshold: " << lossy_skip_threshold
                         << " pkt.msn: " << static_cast<CbrPacket&>(pkt).msn << endl;
                }

                tie(ret_check_pkt_full_skip, do_full_skip) =
                    check_pkt_full_skip(static_cast<CbrPacket&>(pkt));
                if (ret_check_pkt_full_skip) {
                    check_mbl = true;
                    cout << "check_pkt_full_skip true flow_id: " << pkt.flow().flow_id()
                         << " epsn: " << epsn << endl;
                }
            }

            tie(ret_pkt_skip, do_full_skip) = check_pkt(static_cast<CbrPacket&>(pkt));
            if (ret_pkt_skip) {
                check_mbl = true;
                cout << "check_pkt after full_skip true flow_id: " << pkt.flow().flow_id() << endl;
            }

            if (do_full_skip != false) {
                cerr << "full skip should be false" << endl;
                abort();
            }

            check_mbl = true;
            statistics.num_full_skips++;
        }

        if (ret_trivial_skip = check_trivial_skip()) {
            check_mbl = true;
            // cout << " flow_id: " << pkt.flow().flow_id() << "trivial skip: epsn: " << epsn <<
            // endl;
            cout << "trivial_skip true flow_id: " << pkt.flow().flow_id() << endl;
        } else if (ret_recoverable_skip = check_recoverable_skip(do_full_skip)) {
            check_mbl = true;
            statistics.num_recoverable_skips++;
            cout << "recoverable_skip true flow_id: " << pkt.flow().flow_id() << endl;
            // cout << " flow_id: " << pkt.flow().flow_id() << "recoverable skip: epsn: " << epsn
            //      << endl;
        } else if (ret_lossy_skip = check_lossy_skip(
                       do_full_skip)) {  // Check lossy skip before recoverable skip, because
                                         // lossy_skip_threshold is usually larger than
                                         // recoverable_skip_threshold.
            check_mbl = true;
            statistics.num_lossy_skips++;
            cout << "lossy_skip true flow_id: " << pkt.flow().flow_id()
                 << " pkt.path_id: " << pkt.pathid() << endl;
            cout << "[CbrSink] check_lossy_skip: flow_id: " << pkt.flow().flow_id()
                 << " psn: " << pkt.id() << " epsn: " << epsn << " max_psn: " << max_psn
                 << " lossy_skip_threshold: " << lossy_skip_threshold
                 << " pkt.msn: " << static_cast<CbrPacket&>(pkt).msn << endl;
        }

        statistics.num_received_chunks += ret_pkt_skip + ret_trivial_skip + ret_recoverable_skip;

        update_bitmap_statistics();

        // check_mbl == true if some leading chunks can be skipped, but either
        // completed/recoverable/unsuccessfully completed; in whichever case, we update the message
        // boundary list.
        if (check_mbl) {
            // epsn here is updated after checking the bitmap
            complete_mbl(eventlist().now(), epsn);
        }
    }

    pkt.free();
}

// TODO: timeout for msg completion: init timeout = INF, when receiver get a packet, start a timer

// packet receive timer: no need
// when receiver get another packet, timer = timer + rto
// when timer > threshold, trigger skipping, reset timer = INF or rto if bitmap not empty
// reset timer when receive a packet at sink; threshold = RTT && bitmap not empty, trigger skipping

// msg completion timer: send NACK and clear bitmap and mbl
// if mbl_head is not updated for a long time, NACK

void OneECCCSink::send_sack_packet_ec(simtime_picosec         ts,
                                      uint32_t                msn,
                                      uint32_t                num_missing_stripes,
                                      const vector<uint32_t>& missing_stripes) {
    if (!use_erasure_coding) {
        cerr << "[CbrSink] send_sack_packet_ec called without erasure coding enabled!" << endl;
        abort();
    }

    CbrPacket* p = CbrPacket::newSACKpkt(_src->flow(), *_route, epsn, _mss, _priority, _srcaddr);
    p->set_pathid(_crt_path);
    p->msn                 = msn;
    p->num_missing_stripes = num_missing_stripes;
    p->missing_stripes.insert(
        p->missing_stripes.begin(), missing_stripes.begin(), missing_stripes.end());
    p->set_ts(ts);

    p->sendOn();
}

void OneECCCSink::set_paths(uint32_t num_paths) {
    switch (_route_strategy) {
        case SCATTER_PERMUTE:
        case PULL_BASED:
        case SCATTER_ECMP:
        case NOT_SET:
            cerr << "routing strategy not supported for OneECCCSink " << _nodename << endl;
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
        default:
            break;
    }
}

inline bool OneECCCSink::is_bitmap_full(const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap) {
    return find_highest_bit(bitmap) > _erasure_coding->BITMAP_SIZE_1EC * _bitmap_full_percent;
}

bool OneECCCSink::is_lossy_skip_condition() {
    return (_use_bitmap_full_condition
                ? is_bitmap_full(bitmap)
                : max_stripe_offset >= epsn &&
                      ceil(max_stripe_offset / _erasure_coding->get_chunk_size()) -
                              floor(1.0 * epsn / _erasure_coding->get_chunk_size()) >
                          lossy_skip_threshold);
}

bool OneECCCSink::is_full_skip_condition() {
    return is_bitmap_full(bitmap) && is_lossy_skip_condition();
}

// retransmission logic
uint32_t OneECCCSink::check_lossy_skip(bool do_full_skip) {
    // cout << " lossy skip checking bitmap: " << bitmap.to_string() << endl;
    cout << " statistics.num_bitmap_overflow_drops: " << statistics.num_bitmap_overflow_drops
         << " is_bitmap_full(bitmap): " << is_bitmap_full(bitmap) << " max_psn: " << max_psn
         << " epsn: " << epsn << " lossy_skip_threshold: " << lossy_skip_threshold
         << " max_stripe_offset: " << max_stripe_offset << " message_boundary_list[mbl_head]: "
         << (mbl_size > 0 ? message_boundary_list[mbl_head] : UINT32_MAX) << endl;

    if (do_full_skip || is_lossy_skip_condition()) {
        uint32_t this_msn = epsn / _erasure_coding->get_message_size_pkts();
        uint32_t this_stripe_offset =
            (epsn % _erasure_coding->get_message_size_pkts()) / _erasure_coding->get_chunk_size();
        missing_stripes_map[this_msn].push_back(this_stripe_offset);
        epsn += 1 * _erasure_coding->get_chunk_size();
        shift_stripe();
        // _acked_bytes_per_metric_ack += _erasure_coding->get_chunk_size() * _mss;
        // Note: increase per received packet, instead of per stripe skipped
        // ECN marked bytes is also increased per received packet
        return 1;
    } else {
        return 0;
    }
}

void OneECCCSink::update_per_path_loss_count(size_t bitmap_idx) {
    uint32_t weight = 0;
    if (per_path_last_loss.at(bitmap_idx)) {
        weight = 2;
    } else {
        weight = 1;
    }
    per_path_loss_counts.at(bitmap_idx) += weight;
}

inline uint32_t OneECCCSink::count_consecutive_ones(
    const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap) {
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

inline uint32_t OneECCCSink::count_ones(const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap,
                                        uint32_t                                      size) {
    uint32_t count = 0;
    for (size_t i = 0; i < size; i++) {
        count += bitmap[i];
        // cout << "(" << bitmap[i] << " " << count << ") ";
    }
    // cout << endl;
    return count;
}

inline bool OneECCCSink::is_num_consequtive_ones(
    const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap, uint32_t k) {
    uint32_t count = count_consecutive_ones(bitmap);
    return (count >= k);
}

uint32_t OneECCCSink::check_stripe_bitmap_update() {
    uint32_t count = floor(count_consecutive_ones(bitmap) / _erasure_coding->get_chunk_size());
    epsn += count * _erasure_coding->get_chunk_size();
    bitmap >>= count * _erasure_coding->get_chunk_size();
    statistics.num_trivial_skips += count;
    // _acked_bytes_per_metric_ack += count * _erasure_coding->get_chunk_size() * _mss;
    return count;
}

// shift_stripe: shift the bitmap to the right by one chunk size.
void OneECCCSink::shift_stripe() {
    bitmap >>= _erasure_coding->get_chunk_size();
}

// Trivial skip: if we have k consecutive ones, we can skip the stripe and don't need to do
// recovery. Return true: if we can skip the stripe. Return false: if we cannot skip any stripe.
uint32_t OneECCCSink::check_trivial_skip() {
    // cout << "check_trivial_skip checking bitmap: " << bitmap.to_string() << endl;
    // cout << " epsn: " << epsn << endl;

    bool     check          = true;
    uint32_t shifted_chunks = 0;
    while (check) {
        bool is_k = is_num_consequtive_ones(bitmap, _erasure_coding->get_k());
        if (is_k) {
            epsn += _erasure_coding->get_k() + _erasure_coding->get_m();
            shift_stripe();
            shifted_chunks += 1;
            statistics.num_trivial_skips++;
        } else {
            check = false;
        }
    }
    // _acked_bytes_per_metric_ack += shifted_chunks * _erasure_coding->get_chunk_size() * _mss;
    return shifted_chunks;
}

// Recoverable skip: if we have k of m packets, we can skip the stripe and recover the missing
// packets. Return true: if we can skip the stripe and recover the missing packets. Return false: if
// we cannot skip any stripe.
uint32_t OneECCCSink::check_recoverable_skip(bool do_full_skip) {
    // cout <<  " recoverable skip checking bitmap: " << bitmap.to_string() << endl;
    // cout << " max_stripe_offset: " << max_stripe_offset
    //      << " epsn: " << epsn
    //      << " recoverable_skip_threshold: " << recoverable_skip_threshold
    //      << " max_psn: " << max_psn
    //      << " message_boundary_list[mbl_head]: "
    //      << (mbl_size > 0 ? message_boundary_list[mbl_head] : UINT32_MAX) << endl;
    // cout << (mbl_size == 0)  << " " <<  (max_stripe_offset >= message_boundary_list[mbl_head]) <<
    // " " <<
    //     (max_stripe_offset - message_boundary_list[mbl_head] < recoverable_skip_threshold) <<
    //     endl;

    uint32_t shifted_chunks = 0;

    if (max_stripe_offset >= epsn &&
        ceil((double)max_stripe_offset / (double)_erasure_coding->get_chunk_size()) -
                floor(1.0 * epsn / (double)_erasure_coding->get_chunk_size()) >
            recoverable_skip_threshold) {
        bool check = true;
        while (check) {
            bool is_k_of_m =
                (count_ones(bitmap, _erasure_coding->get_chunk_size()) >= _erasure_coding->get_k());
            if (is_k_of_m) {
                epsn += _erasure_coding->get_k() + _erasure_coding->get_m();
                shift_stripe();
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

    // _acked_bytes_per_metric_ack += shifted_chunks * _erasure_coding->get_chunk_size() * _mss;
    return shifted_chunks;
}

// Return true if there are chunks received and we can shift right
// Return false otherwise
tuple<uint32_t, bool> OneECCCSink::check_pkt(CbrPacket& pkt) {
    uint32_t last_psn_in_msg         = pkt.last_psn_in_msg;
    uint32_t last_virtual_psn_in_msg = (pkt.msn + 1) * _erasure_coding->get_message_size_pkts() - 1;
    for (uint32_t iter_psn = last_psn_in_msg + 1; iter_psn <= last_virtual_psn_in_msg; iter_psn++) {
        if (iter_psn >= epsn && iter_psn - epsn < ErasureCoding::BITMAP_SIZE_1EC) {
            bitmap.set(iter_psn - epsn);
        }
    }

    // print bitmap
    // cout << "epsn: " << epsn << " Bitmap: " << bitmap.to_string() << endl;

    if (pkt.id() == epsn) {
        bitmap.set(pkt.id() - epsn);
        return make_tuple(check_stripe_bitmap_update(), false);
    } else if (pkt.id() > epsn) {
        if (pkt.id() - epsn >= ErasureCoding::BITMAP_SIZE_1EC) {
            if (_use_full_skip && is_full_skip_condition()) {
                return make_tuple(0, true);
            } else {
                statistics.num_bitmap_overflow_drops++;
                // drop packet, bitmap is full
                return make_tuple(0, false);
            }
        }
        bitmap.set(pkt.id() - epsn);
        return make_tuple(0, false);
    } else {
        // drop packet due to underflow
        statistics.num_bitmap_underflow_drops++;
        // pkt.free();
        return make_tuple(0, false);
    }
}

tuple<uint32_t, bool> OneECCCSink::check_pkt_full_skip(CbrPacket& pkt) {
    uint32_t ret = check_stripe_bitmap_update();
    if (pkt.id() == epsn) {
        // _acked_bytes_per_metric_ack += ret * _erasure_coding->get_chunk_size() * _mss;
        return make_tuple(ret, false);
    } else if (pkt.id() > epsn) {
        if (pkt.id() - epsn >= ErasureCoding::BITMAP_SIZE_1EC) {
            return make_tuple(0, true);
        }
        // _acked_bytes_per_metric_ack += ret * _erasure_coding->get_chunk_size() * _mss;
        return make_tuple(ret, false);
    }
}

void OneECCCSink::update_mbl(CbrPacket& pkt) {
    uint32_t pkt_iter = mbl_head;
    if (pkt.msn < mbl_base_msn) {
        return;
    }
    uint32_t relative_msn = pkt.msn - mbl_base_msn;

    if (relative_msn < mbl_size) {
        pkt_iter = (mbl_head + relative_msn) % ErasureCoding::MAX_MESSAGE_BOUNDARIES;
        message_boundary_list[pkt_iter] = pkt.last_psn_in_msg;
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
                    message_boundary_list[last_iter] = pkt.last_psn_in_msg;
                    pkt_iter                         = last_iter;
                }
            }
            mbl_size = relative_msn + 1;
        } else {
            cout << "[CbrSink] Message boundary list is full, cannot add new entry! relative_msn: "
                 << relative_msn << " mbl_base_msn: " << mbl_base_msn << endl;
            // do nothing and wait for NACK timeout
        }
    }

    uint32_t iter = mbl_head;
    while (iter != pkt_iter && mbl_size > 0) {
        uint32_t message_start =
            pkt.id() - pkt.stripe_offset * _erasure_coding->get_chunk_size() - pkt.pkt_offset;
        if (message_boundary_list[iter] > message_start - 1) {
            message_boundary_list[iter] = message_start - 1;
        }
        iter = (iter + 1) % ErasureCoding::MAX_MESSAGE_BOUNDARIES;
    }
}

void OneECCCSink::complete_mbl(simtime_picosec ts, uint32_t epsn) {
    if (mbl_size == 0) {
        return;  // no message boundaries to complete
    }

    // complete message boundary list from head
    uint32_t iter         = mbl_head;
    uint32_t relative_msn = 0;

    while (iter != mbl_tail && mbl_size > 0) {
        if (epsn > message_boundary_list[iter]) {
            // send SACK for the completed message
            uint32_t this_msn = mbl_base_msn + relative_msn;
            statistics.num_received_messages++;
            if (missing_stripes_map.find(this_msn) == missing_stripes_map.end()) {
                cout << "[CbrSink] flow_id: " << _src->flow().flow_id()
                     << " Sending SACK for msn: " << this_msn << " with "
                     << missing_stripes_map[this_msn].size() << " missing stripes." << endl;
                send_sack_packet_ec(ts, this_msn, 0, vector<uint32_t>());
            } else {
                cout << "[CbrSink] flow_id: missing" << _src->flow().flow_id()
                     << " Sending SACK for msn: " << this_msn << " with "
                     << missing_stripes_map[this_msn].size() << " missing stripes." << endl;
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
    if (relative_msn > 0) {
        _mbl_change           = true;
        _nack_restart_timeout = eventlist().now() + _restart_timeout;
    }
}

bool OneECCCSink::check_flow_active() {
    // return (bitmap.any());
    return true;
}

void OneECCCSink::restart_flow_timer_hook(simtime_picosec now, simtime_picosec period) {
    cout << "[CbrSink] restart_flow_timer_hook called at time: " << timeAsUs(now)
         << ", flow_active: " << flow_active
         << ", _restart_timeout_pending: " << _restart_timeout_pending
         << ", _mbl_change: " << _mbl_change << ", now: " << timeAsUs(now)
         << ", _nack_restart_timeout: " << timeAsUs(_nack_restart_timeout)
         << ", _restart_timeout: " << timeAsUs(_restart_timeout)
         << ", flow_id: " << _src->flow().flow_id() << endl;

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

void OneECCCSink::metric_ack_timer_hook(simtime_picosec now, simtime_picosec period) {
    cout << "[CbrSink] metric_ack_timer_hook called at time: " << timeAsUs(now)
         << ", flow_active: " << flow_active
         << ", _metric_ack_timeout_pending: " << _metric_ack_timeout_pending
         << ", now: " << timeAsUs(now) << ", _metric_ack_timeout: " << timeAsUs(_metric_ack_timeout)
         << ", _metric_ack_period: " << timeAsUs(_metric_ack_period)
         << ", flow_id: " << _src->flow().flow_id() << endl;

    if (!flow_active || _acked_bytes_per_metric_ack == 0) {
        return;
    }

    if (now <= _metric_ack_timeout || _metric_ack_timeout == timeInf)
        return;

    // Timeout and send NACK for restarting the flow
    if (!_metric_ack_timeout_pending) {
        _metric_ack_timeout_pending = true;

        // check the timer difference between the event and the real value
        simtime_picosec too_late = now - (_metric_ack_timeout);

        // careful: we might calculate a negative value
        // to prevent overflow but keep randomness we just divide until we are within the limit
        while (too_late > period)
            too_late >>= 1;

        simtime_picosec ack_off = (period - too_late);

        eventlist().sourceIsPendingRel(*this, ack_off);

        _metric_ack_timeout = now + _metric_ack_period;
    }
}

void OneECCCSink::send_nack_packet_ec(simtime_picosec ts) {
    if (!use_erasure_coding) {
        cerr << "[CbrSink] send_nack_packet_ec called without erasure coding enabled!" << endl;
        abort();
    }

    statistics.num_nack_sent++;

    cout << "[CbrSink] flow_id: " << _src->flow().flow_id() << " Sending NACK for epsn: " << epsn
         << " at time: " << timeAsUs(ts) << endl;

    // Send a NACK packet to restart the flow
    CbrPacket* p = CbrPacket::newNACKpkt(_src->flow(), *_route, epsn, _mss, _priority, _srcaddr);
    p->set_pathid(_crt_path);
    p->set_ts(ts);
    p->sendOn();
}

void OneECCCSink::restartFlow() {
    // Restart the flow by sending a NACK packet
    send_nack_packet_ec(eventlist().now());

    // Clear bitmap, reset the epsn, max_psn
    // Restart the flow by sending a NACK packet
    send_nack_packet_ec(eventlist().now());

    for (size_t i = 0; i < _erasure_coding->entropy_list.size(); i++) {
        per_path_loss_counts.at(i)       = 0;
        per_path_last_loss.at(i)         = false;
        per_path_one_way_latencies.at(i) = 0;
    }

    // Clear bitmap and mbl
    bitmap.reset();
    mbl_size     = 0;
    mbl_head     = 0;
    mbl_tail     = 0;
    mbl_base_msn = 0;

    for (size_t i = 0; i < message_boundary_list.size(); i++) {
        message_boundary_list[i] = UINT32_MAX;  // Initialize to a large value
    }

    missing_stripes_map.clear();
    // Reset the epsn
    epsn = 0;
    // Reset the max_psn and max_stripe_offset
    max_psn           = 0;
    max_stripe_offset = 0;

    _can_send_metric_ack = true;
    flow_active          = true;

    _metric_ack_timeout         = eventlist().now() + _metric_ack_period;
    _metric_ack_timeout_pending = false;

    _statistics_outfile << "ts: " << timeAsUs(eventlist().now())
                        << ", flow_id: " << _src->flow().flow_id() << " NACK Timeout" << endl;
}

void OneECCCSink::send_metric_ack_packet_ec(simtime_picosec ts, uint32_t last_path_id) {
    if (!use_erasure_coding) {
        cerr << "[CbrSink] send_metric_ack_packet_ec called without erasure coding enabled!"
             << endl;
        abort();
    }

    cout << "[CbrSink] flow_id: " << _src->flow().flow_id() << " Sending Metric ACK: "
         << " original packet timestamp: " << timeAsUs(ts) << endl;

    // When receiving a packet, if can_send == true, send the ACK and reset the timer
    // after sending metric ACK, set to false, restart the timer.

    _statistics_outfile << "Metric ACK: flow_id: " << _src->flow().flow_id()
                        << ", bytes_acked: " << _acked_bytes_per_metric_ack
                        << ", bytes_ecn_marked: " << _bytes_ecn_marked_per_metric_ack
                        << ", ts: " << timeAsUs(ts) << endl;

    // Send a Metric ACK packet
    CbrPacket* p =
        CbrPacket::newMetricACKpkt(_src->flow(), *_route, epsn, _mss, _priority, _srcaddr);
    p->set_pathid(0);
    p->set_ts(ts);
    p->set_num_bytes_acked(_acked_bytes_per_metric_ack);
    p->set_bytes_ecn_marked(_bytes_ecn_marked_per_metric_ack);
    _acked_bytes_per_metric_ack      = 0;
    _bytes_ecn_marked_per_metric_ack = 0;
    uint32_t entropy_to_replace      = -1;
    uint32_t max_loss_count          = 0;
    for (size_t i = 0; i < per_path_loss_counts.size(); i++) {
        if (per_path_loss_counts.at(i) > loss_path_replace_threshold) {
            if (max_loss_count < per_path_loss_counts.at(i)) {
                max_loss_count     = per_path_loss_counts.at(i);
                entropy_to_replace = i;
            }
        }
    }
    p->set_entropy_to_replace(entropy_to_replace);
    p->set_one_way_latencies(per_path_one_way_latencies);
    p->sendOn();

    _can_send_metric_ack = false;
    _metric_ack_timeout  = eventlist().now() + _metric_ack_period;

    for (size_t i = 0; i < per_path_one_way_latencies.size(); i++) {
        per_path_loss_counts.at(i)       = 0;  // Reset the loss counts
        per_path_last_loss.at(i)         = false;
        per_path_one_way_latencies.at(i) = 0;
    }
}

void OneECCCSink::doNextEvent() {
    cout << "[CbrSink] doNextEvent called at time: " << timeAsUs(eventlist().now())
         << ", flow_active: " << flow_active
         << ", _restart_timeout_pending: " << _restart_timeout_pending
         << ", _mbl_change: " << _mbl_change << ", now: " << timeAsUs(eventlist().now())
         << ", _nack_restart_timeout: " << timeAsUs(_nack_restart_timeout)
         << ", _restart_timeout: " << timeAsUs(_restart_timeout)
         << ", flow_id: " << _src->flow().flow_id() << endl;

    if (_restart_timeout_pending) {
        _restart_timeout_pending = false;

        flow_active = check_flow_active();

        restartFlow();

        _nack_restart_timeout = eventlist().now() + _restart_timeout;
    }

    if (_metric_ack_timeout_pending) {
        _metric_ack_timeout_pending = false;

        // Send metric ACK packet

        if (_can_send_metric_ack) {
            send_metric_ack_packet_ec(last_original_pkt_ts, last_path_id);
        } else {
            _metric_ack_timeout = eventlist().now() + _metric_ack_period;
        }
    }
}

////////////////////////////////////////////////////////////////
//  CBR SRC FLOW RESTART TIMER
////////////////////////////////////////////////////////////////

OneECCCSrcRestartTimerScanner::OneECCCSrcRestartTimerScanner(simtime_picosec scanPeriod,
                                                             EventList&      eventlist)
    : EventSource(eventlist, "RestartScanner"), _scanPeriod(scanPeriod) {
    eventlist.sourceIsPendingRel(*this, _scanPeriod);
}

void OneECCCSrcRestartTimerScanner::registerCbr(OneECCCSrc& cbrsrc) {
    _cbrs.push_back(&cbrsrc);
}

void OneECCCSrcRestartTimerScanner::doNextEvent() {
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

OneECCCSinkRestartTimerScanner::OneECCCSinkRestartTimerScanner(simtime_picosec scanPeriod,
                                                               EventList&      eventlist)
    : EventSource(eventlist, "RestartScanner"), _scanPeriod(scanPeriod) {
    eventlist.sourceIsPendingRel(*this, _scanPeriod);
}

void OneECCCSinkRestartTimerScanner::registerCbr(OneECCCSink& cbrsink) {
    _cbrs.push_back(&cbrsink);
}

void OneECCCSinkRestartTimerScanner::doNextEvent() {
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

////////////////////////////////////////////////////////////////
//  CBR SINK METRIC ACK TIMER
////////////////////////////////////////////////////////////////

OneECCCSinkMetricACKTimerScanner::OneECCCSinkMetricACKTimerScanner(simtime_picosec scanPeriod,
                                                                   EventList&      eventlist)
    : EventSource(eventlist, "MetricAckScanner"), _scanPeriod(scanPeriod) {
    eventlist.sourceIsPendingRel(*this, _scanPeriod);
}

void OneECCCSinkMetricACKTimerScanner::registerCbr(OneECCCSink& cbrsink) {
    _cbrs.push_back(&cbrsink);
}

void OneECCCSinkMetricACKTimerScanner::doNextEvent() {
    simtime_picosec  now = eventlist().now();
    cbrs_t::iterator i;
    bool             is_active = false;
    for (i = _cbrs.begin(); i != _cbrs.end(); i++) {
        (*i)->metric_ack_timer_hook(now, _scanPeriod);
        is_active = is_active || (*i)->flow_active;
    }

    cout << "[CbrSink] OneECCCSinkMetricACKTimerScanner: is_active: " << is_active
         << ", scanPeriod: " << timeAsUs(_scanPeriod) << endl;
    if (is_active)
        eventlist().sourceIsPendingRel(*this, _scanPeriod);
}
