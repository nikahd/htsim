// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "cbr.h"

#include <algorithm>
#include <iostream>

#include "cbrpacket.h"
#include "data_collector.h"
#include "math.h"
#include "uec_src.h"

RouteStrategy CbrSrc::_route_strategy  = NOT_SET;
RouteStrategy CbrSink::_route_strategy = NOT_SET;

bool           CbrSrc::use_erasure_coding  = false;
bool           CbrSink::use_erasure_coding = false;
ErasureCoding* CbrSrc::_erasure_coding     = NULL;
ErasureCoding* CbrSink::_erasure_coding    = NULL;

////////////////////////////////////////////////////////////////
//  CBR SOURCE
////////////////////////////////////////////////////////////////

CbrSrc::CbrSrc(EventList&          eventlist,
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

void CbrSrc::connect(Route* routeout, Route* routeback, CbrSink& sink, simtime_picosec starttime) {
    _route = routeout;
    _sink  = &sink;
    _flow.set_id(get_id());  // identify the packet flow with the CBR source that generated it
    _sink->connect(*this, routeback);

    _sack_restart_timeout = eventlist().now() + _restart_timeout;
    eventlist().sourceIsPending(*this, starttime);
}

void CbrSrc::doNextEvent() {
    start_flow();

    bool trigger_flow_finished = (_sent_bytes >= _flow_size);
    if (trigger_flow_finished) {
        finish_flow();
        return;
    }

    send_packet();
}

void CbrSrc::start_flow() {
    if (!_flow_started) {
        _flow_start_time = eventlist().now();
        _flow_started    = true;
        _sent_bytes      = 0;
    }
}

void CbrSrc::setFlowSize(uint64_t flow_size_in_bytes) {
    _flow_size = flow_size_in_bytes;
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

void CbrSrc::set_paths(uint32_t num_paths) {
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

/* Choose a route for a particular packet */
int CbrSrc::choose_route() {
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
            cerr << "routing strategy not set for CbrSrc " << _nodename << endl;
            abort();  // shouldn't be here at all
        default:
            cerr << "routing strategy not supported for CbrSrc " << _nodename << endl;
            abort();
            break;
    }

    return _crt_path;
}

void CbrSrc::receivePacket(Packet& pkt) {
    // CBR sources don't expect inbound packets; free defensively.
    pkt.free();
}

void CbrSrc::send_packet() {
    Packet* p = CbrPacket::newpkt(_flow, *_route, _crt_id++, _mss, _priority, _dstaddr);
    _sent_bytes += p->size();

    int crt = choose_route();
    p->set_pathid(crt);
    p->sendOn();

    eventlist().sourceIsPendingRel(*this, _period);
}

void CbrSrc::finish_flow() {
    if (_flow_finished) {
        return;  // already finished
    } else {
        _flow_finished = true;
        eventlist().cancelPendingSource(*this);
    }
}

////////////////////////////////////////////////////////////////
//  Cbr SINK
////////////////////////////////////////////////////////////////

CbrSink::CbrSink(EventList& eventlist) : EventSource(eventlist, "cbrsink"), DataReceiver("cbr") {
    _nodename       = "cbrsink";
    _received       = 0;
    _last_id        = 0;
    _cumulative_ack = 0;
}

void CbrSink::connect(CbrSrc& src, const Route* route) {
    _src        = &src;
    _route      = route;
    _srcaddr    = src._srcaddr;
    _mss        = src._mss;
    _priority   = src._priority;
    flow_active = true;
}

// Note: _cumulative_ack is the last byte we've ACKed.
// seqno is the first byte of the new packet.
void CbrSink::receivePacket(Packet& pkt) {
    _received++;
    _cumulative_ack += pkt.size();
    _last_id    = pkt.id();
    flow_active = true;

    cout << "flow_id: " << _src->flow().flow_id() << " pkt_id: " << pkt.id() << endl;

    pkt.free();
}

void CbrSink::send_sack_packet_ec(simtime_picosec         ts,
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

void CbrSink::doNextEvent() {}

void CbrSink::set_paths(uint32_t num_paths) {
    switch (_route_strategy) {
        case SCATTER_PERMUTE:
        case PULL_BASED:
        case SCATTER_ECMP:
        case NOT_SET:
            cerr << "routing strategy not supported for CbrSink " << _nodename << endl;
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
