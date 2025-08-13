// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "compositequeue.h"

#include <math.h>

#include <iostream>
#include <sstream>

#include "cbrpacket.h"
#include "helpers.h"
#include "types.h"
#include "uec_packet.h"
#include "uec_src.h"

static int  global_queue_id    = 0;
static bool print_switch_trace = true;
#define DEBUG_QUEUE_ID -1  // set to queue ID to enable debugging

CompositeQueue::CompositeQueue(linkspeed_bps bitrate,
                               mem_b         maxsize,
                               EventList&    eventlist,
                               QueueLogger*  logger,
                               uint16_t      trim_size,
                               bool          no_ecn,
                               bool          disable_trim,
                               bool          low_priority_trim,
                               bool          no_droping_low_header,
                               double        switch_drop_event_prob,
                               double        switch_random_drop_prob,
                               string        precompute_link_down_time_filename)
    : Queue(bitrate, maxsize, eventlist, logger) {
    _no_ecn                  = no_ecn;
    _disable_trim            = disable_trim;
    _low_priority_trim       = disable_trim && low_priority_trim;
    _no_droping_low_header   = no_droping_low_header;
    _switch_drop_event_prob  = switch_drop_event_prob;
    _switch_random_drop_prob = switch_random_drop_prob;
    _trim_size               = trim_size;
    _ratio_high              = 100000;
    _ratio_low               = 1;
    _crt                     = 0;
    _num_headers             = 0;
    _num_packets             = 0;
    _num_acks                = 0;
    _num_nacks               = 0;
    _num_pulls               = 0;
    _num_drops               = 0;
    _num_stripped            = 0;
    _num_bounced             = 0;
    _ecn_minthresh           = maxsize * 2;  // don't set ECN by default
    _ecn_maxthresh           = maxsize * 2;  // don't set ECN by default

    _return_to_sender = false;

    _queuesize_high = _queuesize_low = 0;
    _serv                            = QUEUE_INVALID;
    stringstream ss;
    ss << "compqueue(" << bitrate / 1000000 << "Mb/s," << maxsize << "bytes)";
    _nodename = ss.str();
    _queue_id = global_queue_id++;
    if (_queue_id == DEBUG_QUEUE_ID)
        cout << "queueid " << _queue_id << " bitrate " << bitrate / 1000000 << "Mb/s," << endl;

    _link_down_index = 0;
    load_link_down_time(precompute_link_down_time_filename);
}

void CompositeQueue::beginService() {
    if (!_enqueued_high.empty() && !_enqueued_low.empty()) {
        _crt++;

        if (_crt >= (_ratio_high + _ratio_low))
            _crt = 0;

        if (_crt < _ratio_high) {
            _serv = QUEUE_HIGH;
            eventlist().sourceIsPendingRel(*this, drainTime(_enqueued_high.back()));
        } else {
            assert(_crt < _ratio_high + _ratio_low);
            _serv = QUEUE_LOW;
            eventlist().sourceIsPendingRel(*this, drainTime(_enqueued_low.back()));
        }
        return;
    }

    if (!_enqueued_high.empty()) {
        _serv = QUEUE_HIGH;
        eventlist().sourceIsPendingRel(*this, drainTime(_enqueued_high.back()));
    } else if (!_enqueued_low.empty()) {
        _serv = QUEUE_LOW;
        eventlist().sourceIsPendingRel(*this, drainTime(_enqueued_low.back()));
    } else {
        throw std::runtime_error("CompositeQueue: beginService: no packets in queue");
    }
}

bool CompositeQueue::decide_ECN() {
    assert(!_no_ecn);  // ECN must be enabled for this to happen
    if (_queuesize_low > _ecn_maxthresh) {
        return true;
    } else if (_queuesize_low > _ecn_minthresh) {
        uint64_t p =
            (0x7FFFFFFF * (_queuesize_low - _ecn_minthresh)) / (_ecn_maxthresh - _ecn_minthresh);
        if ((uint64_t)random() < p) {
            return true;
        }
    }
    return false;
}

void CompositeQueue::completeService() {
    Packet* pkt;
    if (_serv == QUEUE_LOW) {
        assert(!_enqueued_low.empty());
        pkt = _enqueued_low.pop();
        if (_monitorQueue) {
            _queue_deq_metric->LogData({std::to_string(_queuesize_low),
                                        std::to_string(pkt->size()),
                                        std::to_string(pkt->type()),
                                        to_string(pkt->pathid()),
                                        to_string(pkt->dst())});
        }
        _queuesize_low -= pkt->size();

        // ECN mark on deque
        if (!_no_ecn && decide_ECN()) {  // only mark if ECN is enabled
            cout << "COMPOSITE queue set ECN_CE, nodename: " << nodename() << endl;
            pkt->set_flags(pkt->flags() | ECN_CE);
        }
        if (_queue_id == DEBUG_QUEUE_ID) {
            cout << timeAsUs(eventlist().now()) << " name " << _nodename << " _queuesize_low "
                 << _queuesize_low * 8 / ((_bitrate / 1000000.0)) << " _queueid " << _queue_id
                 << " switch " << _switch->getID() << " ecn " << decide_ECN() << " _queuesize_high "
                 << _queuesize_high * 8 / ((_bitrate / 1000000.0)) << endl;
        }
        if (_logger)
            _logger->logQueue(*this, QueueLogger::PKT_SERVICE, *pkt);
        _num_packets++;
    } else if (_serv == QUEUE_HIGH) {
        assert(!_enqueued_high.empty());
        pkt = _enqueued_high.pop();
        _queuesize_high -= pkt->size();
        if (_logger)
            _logger->logQueue(*this, QueueLogger::PKT_SERVICE, *pkt);
        if (pkt->type() == NDPACK)
            _num_acks++;
        else if (pkt->type() == NDPNACK)
            _num_nacks++;
        else if (pkt->type() == NDPPULL)
            _num_pulls++;
        else {
            // cout << "Hdr: type=" << pkt->type() << endl;
            _num_headers++;
            // ECN mark on deque of a header, if low priority queue is still over threshold
            //            if (decide_ECN()) {
            //                pkt->set_flags(pkt->flags() | ECN_CE);
            //            }
        }
    } else {
        throw std::runtime_error("CompositeQueue: completeService: invalid service");
    }

    pkt->flow().logTraffic(*pkt, *this, TrafficLogger::PKT_DEPART);
    pkt->sendOn();

    //_virtual_time += drainTime(pkt);

    _serv = QUEUE_INVALID;

    if (!_enqueued_high.empty() || !_enqueued_low.empty())
        beginService();
}

void CompositeQueue::doNextEvent() {
    completeService();
}

void printUecDataPacketAction(simtime_picosec now, string action_name, Packet* pkt) {
    if (pkt->type() == UECDATA) {
        auto   uec_data_pkt     = (UecDataPacket*)pkt;
        string packet_type_name = "data";
        if (pkt->header_low_only()) {
            packet_type_name = "trim";
        } else if (uec_data_pkt->is_probe_packet()) {
            packet_type_name = "probe";
        } else if (uec_data_pkt->retransmitted()) {
            packet_type_name = "rtx";
        } else if (uec_data_pkt->packet_type() == UecBasePacket::DATA_PROBE) {
            packet_type_name = "sleek probe";
        } else {
            packet_type_name = "data";
        }
        cout << timeAsUs(now) << " flow " << uec_data_pkt->flow_id() << " " << action_name << " "
             << packet_type_name;
        cout << " packet for psn " << uec_data_pkt->epsn() << " ev " << uec_data_pkt->path_id()
             << endl;
    }
}

void printQueueSize(
    simtime_picosec now, int queue_id, string queue_name, mem_b queue_size, mem_b max_size) {
    if (true) {
        cout << timeAsUs(now) << " queue " << queue_id << " " << queue_name << " size "
             << queue_size << " max " << max_size << endl;
    }
}

bool CompositeQueue::isLinkDown(simtime_picosec ts) {
    while (_link_down_index < _link_down_time.size() &&
           ts > _link_down_time[_link_down_index].second) {
        // move to next link down time
        _link_down_index++;
    }

    if (_link_down_index >= _link_down_time.size()) {
        return false;  // no link down time
    }

    if (ts >= _link_down_time[_link_down_index].first &&
        ts <= _link_down_time[_link_down_index].second) {
        // link is down
        if (print_switch_trace) {
            cout << timeAsUs(eventlist().now()) << " name " << _nodename << " link down at " << ts
                 << endl;
        }
        return true;
    }

    return false;
}

bool CompositeQueue::isDrop() {
    bool is_drop_event  = (drand() <= _switch_drop_event_prob);
    bool is_random_drop = (drand() <= _switch_random_drop_prob);
    return (is_drop_event && is_random_drop);
}

bool CompositeQueue::isControlPacket(Packet& pkt) {
    // TODO(zeying): currently assume no drops for CBR control packets; but should implement
    // reliable SACK/NACK later
    bool  is_control_packet = false;
    auto* cbr_pkt           = dynamic_cast<CbrPacket*>(&pkt);
    if (cbr_pkt) {
        is_control_packet =
            (cbr_pkt->pkt_type == CbrPacket::SACK || cbr_pkt->pkt_type == CbrPacket::NACK ||
             cbr_pkt->pkt_type == CbrPacket::METRIC_ACK);
    }

    is_control_packet |= (pkt.type() == ROCENACK || pkt.type() == ROCEACK || pkt.type() == CNP);
    return is_control_packet;
}

void CompositeQueue::receivePacket(Packet& pkt) {
    //  cout << timeAsUs(eventlist().now()) << " name " << _nodename << " arrive "
    //          << _queuesize_low * 8 / ((_bitrate / 1000000.0)) << " _queueid " << _queue_id
    //          << " switch " << _switch->getID() << " flowid " << pkt.flow_id() << " ev "
    //          << pkt.pathid() << endl;

    if (pkt.type() == ETH_PAUSE) {
        // Pause packet, just ignore it
        if (_logger)
            _logger->logQueue(*this, QueueLogger::PKT_ARRIVE, pkt);
        pkt.free();
        return;
    }

    if (_monitorQueue) {
        _queue_enq_metric->LogData({std::to_string(_queuesize_low),
                                    std::to_string(pkt.size()),
                                    std::to_string(pkt.type()),
                                    to_string(pkt.pathid()),
                                    to_string(pkt.dst())});
    }
    if (_queue_id == DEBUG_QUEUE_ID) {
        cout << timeAsUs(eventlist().now()) << " name " << _nodename << " arrive "
             << _queuesize_low * 8 / ((_bitrate / 1000000.0)) << " _queueid " << _queue_id
             << " switch " << _switch->getID() << " flowid " << pkt.flow_id() << " ev "
             << pkt.pathid() << endl;
    }
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_ARRIVE);
    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_ARRIVE, pkt);

    bool is_control_packet = isControlPacket(pkt);

    if (!is_control_packet) {
        // link down
        if (isLinkDown(eventlist().now())) {
            pkt.free();
            _num_drops++;
            return;
        }

        // random drop
        if ((!pkt.header_only()) && isDrop()) {
            if (print_switch_trace) {
                printUecDataPacketAction(eventlist().now(), "drop", &pkt);
            }
            cout << "COMPOSITE queue drops this packet: "
                 << " nodename: " << nodename() << " flow " << pkt.flow().flow_id() << " pkt_type "
                 << pkt.type() << " pkt_sz " << pkt.size() << " queuesize: " << _queuesize
                 << " maxsize: " << _maxsize << endl;
            pkt.free();
            _num_drops++;
            return;
        }
    }

    // low priority trim, only trim incomming packet
    // then drop as normal packet
    if ((!pkt.header_low_only()) && _low_priority_trim) {
        assert(_disable_trim);
        if (_queuesize_low + pkt.size() > _maxsize) {
            if (print_switch_trace) {
                printUecDataPacketAction(eventlist().now(), "trim", &pkt);
            }
            // trim pkt and treated as a small data packet
            pkt.strip_payload_low(_trim_size);
            // cout << "CQ trim at " << _nodename << endl;
            _num_stripped++;
            pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_TRIM);
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_TRIM, pkt);
        }
    }

    //@Ahmad: We hit a boundary condition if packet size == buffersize!!!
    if (!pkt.header_only()) {
        bool condition = (_queuesize_low + pkt.size() <= _maxsize) ||
                         ((drand() < 0.5) && !_enqueued_low.empty());
        if (_no_droping_low_header) {
            // if not dropping low priority header pkt(probe, low trim)
            // discard the random choosing step
            condition = _queuesize_low + pkt.size() <= _maxsize;
        }
        if (condition) {  // condition is true at Boundary of size = max_size
            // regular packet; don't drop the arriving packet

            // we are here because either the queue isn't full or,
            // it might be full and we randomly chose an
            // enqueued packet to trim

            if (_queuesize_low + pkt.size() > _maxsize) {
                // we're going to drop an existing packet from the queue
                if (_enqueued_low.empty()) {
                    // cout << "QUeuesize " << _queuesize_low << " packetsize " << pkt.size() << "
                    // maxsize " << _maxsize << endl;
                    throw std::runtime_error(
                        "CompositeQueue: receivePacket: low priority queue is empty");
                }
                // take last packet from low prio queue, make it a header and place it in the high
                // prio queue
                Packet* booted_pkt = _enqueued_low.pop_front();
                _queuesize_low -= booted_pkt->size();
                if (_logger)
                    _logger->logQueue(*this, QueueLogger::PKT_UNQUEUE, *booted_pkt);

                if (_disable_trim) {
                    if (print_switch_trace) {
                        printUecDataPacketAction(eventlist().now(), "drop", booted_pkt);
                    }
                    booted_pkt->free();
                    _num_drops++;
                    cout << "A [ " << _enqueued_low.size() << " " << _enqueued_high.size()
                         << " ] DROP  flowid " << booted_pkt->flow_id() << endl;
                } else {
                    // cout << "A [ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ]
                    // STRIP" << endl; cout << "booted_pkt->size(): " << booted_pkt->size();
                    if (print_switch_trace) {
                        printUecDataPacketAction(eventlist().now(), "trim", booted_pkt);
                    }
                    booted_pkt->strip_payload(_trim_size);
                    // cout << "CQ trim at " << _nodename << endl;
                    _num_stripped++;
                    booted_pkt->flow().logTraffic(*booted_pkt, *this, TrafficLogger::PKT_TRIM);
                    if (_logger)
                        _logger->logQueue(*this, QueueLogger::PKT_TRIM, pkt);

                    if (_queuesize_high + booted_pkt->size() > 2 * _maxsize) {
                        if (_return_to_sender && booted_pkt->reverse_route() &&
                            booted_pkt->bounced() == false) {
                            // return the packet to the sender
                            if (_logger)
                                _logger->logQueue(*this, QueueLogger::PKT_BOUNCE, *booted_pkt);
                            booted_pkt->flow().logTraffic(pkt, *this, TrafficLogger::PKT_BOUNCE);
                            // XXX what to do with it now?
#if 0
                            printf("Bounce2 at %s\n", _nodename.c_str());
                            printf("Fwd route:\n");
                            print_route(*(booted_pkt->route()));
                            printf("nexthop: %d\n", booted_pkt->nexthop());
#endif
                            booted_pkt->bounce();
#if 0
                            printf("\nRev route:\n");
                            print_route(*(booted_pkt->reverse_route()));
                            printf("nexthop: %d\n", booted_pkt->nexthop());
#endif
                            _num_bounced++;
                            booted_pkt->sendOn();
                        } else {
                            booted_pkt->flow().logTraffic(
                                *booted_pkt, *this, TrafficLogger::PKT_DROP);
                            booted_pkt->free();
                            if (_logger)
                                _logger->logQueue(*this, QueueLogger::PKT_DROP, pkt);
                        }
                    } else {
                        _enqueued_high.push(booted_pkt);
                        _queuesize_high += booted_pkt->size();
                        if (_logger)
                            _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, *booted_pkt);
                    }
                }
            }

            // assert(_queuesize_low+pkt.size()<= _maxsize);
            Packet* pkt_p = &pkt;
            _enqueued_low.push(pkt_p);
            _queuesize_low += pkt.size();
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);

            if (_serv == QUEUE_INVALID) {
                beginService();
            }

            // cout << "BL[ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ]" <<
            // endl;
            printQueueSize(eventlist().now(), _queue_id, "low", _queuesize_low, _maxsize);
            return;
        } else {
            if (_disable_trim) {
                // if not dropping low priority header pkt
                if (_no_droping_low_header) {
                    // first see if it is probe/low trim
                    bool is_low_trim = _low_priority_trim && pkt.header_low_only();
                    bool is_probe    = false;
                    if (pkt.type() == UECDATA) {
                        auto uec_data_pkt = (UecDataPacket&)pkt;
                        if (uec_data_pkt.is_probe_packet()) {
                            is_probe = true;
                        }
                    }
                    if (is_low_trim || is_probe) {
                        // no dropping, so push the packet anyway
                        Packet* pkt_p = &pkt;
                        _enqueued_low.push(pkt_p);
                        _queuesize_low += pkt.size();
                        if (_logger)
                            _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);

                        if (_serv == QUEUE_INVALID) {
                            beginService();
                        }
                        printQueueSize(
                            eventlist().now(), _queue_id, "low", _queuesize_low, _maxsize);
                        return;
                    }
                }
                if (_queue_id == DEBUG_QUEUE_ID) {
                    cout << timeAsUs(eventlist().now()) << "B[ " << _enqueued_low.size() << " "
                         << _enqueued_high.size() << " ] DROP flowid " << pkt.flow().flow_id()
                         << " queue " << str() << " pathid " << pkt.pathid() << " queueid "
                         << _queue_id << " size " << pkt.size() << endl;
                }
                if (print_switch_trace) {
                    printUecDataPacketAction(eventlist().now(), "drop", &pkt);
                }
                pkt.free();
                _num_drops++;
                return;
            }
            // strip packet the arriving packet - low priority queue is full
            // cout << "B [ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ] STRIP"
            // << endl;
            if (print_switch_trace) {
                printUecDataPacketAction(eventlist().now(), "trim", &pkt);
            }
            pkt.strip_payload(_trim_size);
            // cout << "CQ trim at " << _nodename << endl;
            _num_stripped++;
            pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_TRIM);
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_TRIM, pkt);
        }
    }
    assert(pkt.header_only());

    if (_queuesize_high + pkt.size() > 2 * _maxsize) {
        // drop header
        // cout << "drop!\n";
        if (_return_to_sender && pkt.reverse_route() && pkt.bounced() == false) {
            // return the packet to the sender
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_BOUNCE, pkt);
            pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_BOUNCE);
            // XXX what to do with it now?
#if 0
            printf("Bounce1 at %s\n", _nodename.c_str());
            printf("Fwd route:\n");
            print_route(*(pkt.route()));
            printf("nexthop: %d\n", pkt.nexthop());
#endif
            pkt.bounce();
#if 0
            printf("\nRev route:\n");
            print_route(*(pkt.reverse_route()));
            printf("nexthop: %d\n", pkt.nexthop());
#endif
            _num_bounced++;
            pkt.sendOn();
            return;
        } else {
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_DROP, pkt);
            pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_DROP);
            cout << "B[ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ] DROP "
                 << pkt.flow().flow_id() << endl;
            pkt.free();
            _num_drops++;
            return;
        }
    }

    // if (pkt.type()==NDP)
    //   cout << "H " << pkt.flow().str() << endl;
    Packet* pkt_p = &pkt;
    _enqueued_high.push(pkt_p);
    _queuesize_high += pkt.size();
    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);

    // cout << "BH[ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ]" << endl;

    if (_serv == QUEUE_INVALID) {
        beginService();
    }
}

mem_b CompositeQueue::queuesize() const {
    return _queuesize_low + _queuesize_high;
}

void CompositeQueue::load_link_down_time(const string& precompute_filename) {
    // Load precomputed link down times from a file
    if (precompute_filename == "") {
        return;  // No precomputed file provided
    }

    std::ifstream infile(precompute_filename);
    if (!infile.is_open()) {
        throw std::runtime_error("CompositeQueue: Unable to open link down time file: " +
                                 precompute_filename);
    }

    simtime_picosec start, end;
    while (infile >> start >> end) {
        _link_down_time.emplace_back(start, end);
    }
    infile.close();
}