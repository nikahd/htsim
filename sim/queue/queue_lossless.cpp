// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "queue_lossless.h"

#include <math.h>

#include <iostream>

#include "AI_switch.h"
#include "cbrpacket.h"
#include "switch.h"

int LosslessQueue::_ecn_enabled = true;

LosslessQueue::LosslessQueue(linkspeed_bps bitrate,
                             mem_b         maxsize,
                             EventList&    eventlist,
                             QueueLogger*  logger,
                             Switch*       sw,
                             double        switch_drop_event_prob,
                             double        switch_random_drop_prob,
                             bool enable_pfc)
    : Queue(bitrate, maxsize, eventlist, logger), _state_send(READY), _state_recv(READY) {
    // assume worst case: PAUSE frame waits for one MSS packet to be sent to other switch, and there
    // is an MSS just beginning to be sent when PAUSE frame arrives; this means 2 packets per
    // incoming port, and we must have buffering for all ports except this one (assuming no one hop
    // cycles!)

    setSwitch(sw);

    if (sw)
        sw->addPort(this);

    _sending        = 0;
    _high_threshold = maxsize * 2;
    _low_threshold  = 0;

    _ecn_maxthresh = static_cast<AISwitch*>(sw)->getECNKMax() * _bitrate / 8000000;
    _ecn_minthresh = static_cast<AISwitch*>(sw)->getECNKMin() * _bitrate / 8000000;

    _ecn_pmin = static_cast<AISwitch*>(sw)->getECNPMin();
    _ecn_pmax = static_cast<AISwitch*>(sw)->getECNPMax();

    _switch_drop_event_prob  = switch_drop_event_prob;
    _switch_random_drop_prob = switch_random_drop_prob;

    if (enable_pfc) 
        initThresholds(); // This will enable or disable PFC

    // cout << "LOSSLESS queue Config: " << nodename() << " bitrate " << bitrate
    //      << " maxsize " << maxsize << " ecn_maxthresh " << _ecn_maxthresh
    //      << " ecn_minthresh " << _ecn_minthresh << " ecn_pmin " << _ecn_pmin
    //      << " ecn_pmax " << _ecn_pmax
    //      << " static_cast<AISwitch*>(sw)->getECNKMax(): " <<
    //      static_cast<AISwitch*>(sw)->getECNKMax()
    //      << " static_cast<AISwitch*>(sw)->getECNKMin(): " <<
    //      static_cast<AISwitch*>(sw)->getECNKMin() << endl;
}

void LosslessQueue::initThresholds() {
    _high_threshold = _maxsize - (_switch->portCount()) * Packet::data_packet_size() * 3;

    assert(_high_threshold > 0);

    _low_threshold = 3 * Packet::data_packet_size();
    assert(_high_threshold > _low_threshold);
}

bool LosslessQueue::isDrop() {
    bool is_drop_event  = (drand() <= _switch_drop_event_prob);
    bool is_random_drop = (drand() <= _switch_random_drop_prob);
    return (is_drop_event && is_random_drop);
}

bool LosslessQueue::isControlPacket(Packet& pkt) {
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

void LosslessQueue::receivePacket(Packet& pkt) {
    //  cout << timeAsUs(eventlist().now()) << " name " << nodename() << " arrive "
    //      << " switch " << _switch->getID() << " flowid " << pkt.flow_id() << " pathid: "
    //      << pkt.pathid() << " pkt.type(): " << pkt.type() <<
    //      "(_queuesize + pkt.size() > _maxsize) " << (_queuesize + pkt.size() > _maxsize) << endl;

    if (nodename() == "queue(100000Mb/s,10500000bytes)Queue--Reg1-DC0-LeafGroup0ToRGroup15ToR0->Serv15(0)") {
        cout << "LOSSLESS queue, name: " << nodename() << " " << timeAsMs(eventlist().now()) << " queue_size: " << _queuesize << endl;
    }

    
    // is this a PAUSE frame?
    if (pkt.type() == ETH_PAUSE) {
        EthPausePacket* p = (EthPausePacket*)&pkt;

        if (p->sleepTime() > 0) {
            // remote end is telling us to shut up.
            // assert(_state_send == READY);
            if (_sending)
                // we have a packet in flight
                _state_send = PAUSE_RECEIVED;
            else
                _state_send = PAUSED;

            cout << "LOSSLESS queue, name: " << nodename() << " " << timeAsMs(eventlist().now())
                 << " " << _name << " PAUSED " << endl;
        } else {
            // we are allowed to send!
            _state_send = READY;
            // cout << timeAsMs(eventlist().now()) << " " << _name << " GO "<<endl;

            // start transmission if we have packets to send!
            if (_enqueued.size() > 0 && !_sending)
                beginService();
        }

        pkt.free();
        return;
    }

    /* normal packet, enqueue it */

    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_ARRIVE);
    bool queueWasEmpty = _enqueued.empty();
    _total_received_pkt += 1;

    Packet* pkt_p = &pkt;

    bool is_drop = isDrop();
    if (is_drop) {
        // if we are over the maxsize, we must drop the packet
        // but fake it for CBR/DCQCN(RoCE) control packets
        bool is_control_pkt = isControlPacket(pkt);

        if (!is_control_pkt) {
            cout << "LOSSLESS queue drops this packet: "
                 << " nodename: " << nodename() << " flow " << pkt.flow().flow_id() << " pkt_type "
                 << pkt.type() << " pkt_sz " << pkt.size() << " queuesize: " << _queuesize
                 << " maxsize: " << _maxsize
                 << " drop_prob: " << _total_dropped_pkt / _total_received_pkt
                 << " drop_due_to_background_traffic: " << 1
                 << endl;
            _total_dropped_pkt += 1;
            pkt.free();
            return;
        }
    }

    if (_queuesize + pkt.size() > _maxsize) {
        // if we are over the maxsize, we must drop the packet
        // but fake it for CBR/DCQCN(RoCE) control packets
        // We  are going to drop an existing packet from the queue
        Packet* booted_pkt = _enqueued.pop_front();
        _queuesize -= booted_pkt->size();
        bool is_control_pkt = isControlPacket(*booted_pkt);

        if (!is_control_pkt) {
            cout << "LOSSLESS queue drops this packet: "
                 << " nodename: " << nodename() << " flow " << pkt.flow().flow_id() << " pkt_type "
                 << pkt.type() << " pkt_sz " << pkt.size() << " queuesize: " << _queuesize
                 << " maxsize: " << _maxsize
                 << " drop_prob: " << _total_dropped_pkt / _total_received_pkt
                 << " drop_due_to_switch_overflow: " << 1
                 << endl;
            _total_dropped_pkt += 1;
            booted_pkt->free();
            return;
        }
    }

    _enqueued.push(pkt_p);
    _queuesize += pkt.size();

    // send PAUSE notifications if that is the case!
    if (_queuesize > _high_threshold && _state_recv != PAUSED) {
        _state_recv = PAUSED;
        _switch->sendPause(this, 1000);
    }

    // if (_state_recv==PAUSED)
    // cout << timeAsMs(eventlist().now()) << " queue " << _name << " switch (" << _switch->_name <<
    // ") "<< " recv when paused pkt " << pkt.type() << " sz " << _queuesize << endl;

    if (_queuesize > _maxsize) {
        cout << " Queue " << _name << " switch (" << _switch->nodename() << ") "
             << " LOSSLESS not working! I should have dropped this packet" << endl;
    }

    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);

    if (queueWasEmpty && _state_send == READY) {
        /* schedule the dequeue event */
        assert(_enqueued.size() == 1);
        beginService();
    }
}

void LosslessQueue::beginService() {
    assert(_state_send == READY && !_sending);
    Queue::beginService();
    _sending = 1;
}

bool LosslessQueue::decide_ECN() {
    if (_queuesize > _ecn_maxthresh) {
        return true;
    } else if (_queuesize > _ecn_minthresh) {
        uint64_t p = (0x7FFFFFFF * (_queuesize - _ecn_minthresh)) /
                         (_ecn_maxthresh - _ecn_minthresh) * _ecn_pmax +
                     _ecn_pmin;
        if ((uint64_t)random() < p) {
            return true;
        }
    }
    return false;
}

void LosslessQueue::completeService() {
    /* dequeue the packet */
    assert(!_enqueued.empty());
    // Packet* pkt = _enqueued.back();
    //_enqueued.pop_back();
    Packet* pkt = _enqueued.pop();

    // mark on deque
    if (_ecn_enabled && decide_ECN()) {
        cout << "LOSSLESS queue set ECN_CE, nodename: " << nodename()
             << " flow_id: " << pkt->flow().flow_id() << endl;
        pkt->set_flags(pkt->flags() | ECN_CE);
    }

    _queuesize -= pkt->size();

    pkt->flow().logTraffic(*pkt, *this, TrafficLogger::PKT_DEPART);

    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_SERVICE, *pkt);

    /* tell the packet to move on to the next pipe */
    pkt->sendOn();

    _sending = 0;

    if (_state_send == PAUSE_RECEIVED)
        _state_send = PAUSED;

    // unblock if that is the case
    if (_queuesize < _low_threshold && _state_recv == PAUSED) {
        _switch->sendPause(this, 0);
        _state_recv = READY;
    }

    if (!_enqueued.empty()) {
        if (_state_send == READY)
            /* start packet transmission, schedule the next dequeue event */
            beginService();
    }
}
