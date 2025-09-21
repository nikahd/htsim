// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dcqcn.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>   // getenv
#include <algorithm>
#include <iostream>
#include <assert.h>

#include "queue.h"
#include "switch.h"
#include "trigger.h"

// RoCE packet types (adjust includes if your tree differs)
#include "roce.h"
#include "cnppacket.h"

using namespace std;

////////////////////////////////////////////////////////////////
// helpers: env parsing
////////////////////////////////////////////////////////////////
namespace {
inline bool get_env_double(const char* k, double& out) {
    if (const char* s = getenv(k)) { char* e=nullptr; double v=strtod(s,&e); if(e!=s){ out=v; return true; } }
    return false;
}
inline bool get_env_ll(const char* k, long long& out) {
    if (const char* s = getenv(k)) { char* e=nullptr; long long v=strtoll(s,&e,10); if(e!=s){ out=v; return true; } }
    return false;
}
} // namespace

////////////////////////////////////////////////////////////////
//  DCQCN SOURCE/SINK STATICS (defaults)
////////////////////////////////////////////////////////////////

simtime_picosec DCQCNSink::_cnp_interval      = timeFromUs(50.0);          // conservative pacing
simtime_picosec DCQCNSrc::_cc_update_period   = timeFromUs(15000.0);       // WAN epoch (~15ms)

double          DCQCNSrc::_g                  = 1.0/256;                   // α EWMA decay
uint64_t        DCQCNSrc::_B                  = 64ULL * 1024ULL * 1024ULL; // bytes per BC epoch
uint32_t        DCQCNSrc::_F                  = 20;                         // epochs before HI
linkspeed_bps   DCQCNSrc::_RAI                = 0;                          // set per instance
linkspeed_bps   DCQCNSrc::_RHAI               = 0;

// env-tunable extras
double          DCQCNSrc::_md_cap             = 0.35;
double          DCQCNSrc::_floor_line_frac    = 0.50;
double          DCQCNSrc::_floor_rt_frac      = 0.75;
uint32_t        DCQCNSrc::_hi_cooldown_default= 2;
uint32_t        DCQCNSrc::_hi_cap_div         = 256;

// one-time flag
bool            DCQCNSrc::_env_applied        = false;

// single (correct) definition of the CSV streams
std::ofstream DCQCNSink::pkt_csv;
std::ofstream DCQCNSink::cnp_csv;

////////////////////////////////////////////////////////////////
//  DCQCN SOURCE
////////////////////////////////////////////////////////////////

void DCQCNSrc::apply_env_overrides_once() {
    if (_env_applied) return;
    _env_applied = true;

    // sink pacing
    { double us=0.0; if (get_env_double("DCQCN_CNP_INTERVAL_US", us))
        DCQCNSink::_cnp_interval = timeFromUs(us); }
    // epoch
    { double us=0.0; if (get_env_double("DCQCN_EPOCH_US", us))
        DCQCNSrc::_cc_update_period = timeFromUs(us); }
    // alpha decay
    { double g=_g; if (get_env_double("DCQCN_G", g)) _g = g; }
    // bytes-per-epoch
    { long long B= (long long)_B; if (get_env_ll("DCQCN_B_BYTES", B)) _B = (uint64_t)B; }
    // F epochs
    { long long F=(long long)_F; if (get_env_ll("DCQCN_F_EPOCHS", F)) _F=(uint32_t)F; }
    // MD cap
    { double v=_md_cap; if (get_env_double("DCQCN_MD_CAP", v)) _md_cap=v; }
    // floors
    { double v=_floor_line_frac; if (get_env_double("DCQCN_FLOOR_LINE_FRAC", v)) _floor_line_frac=v; }
    { double v=_floor_rt_frac;   if (get_env_double("DCQCN_FLOOR_RT_FRAC", v))   _floor_rt_frac=v; }
    // HI cooldown
    { long long v=_hi_cooldown_default; if (get_env_ll("DCQCN_HI_COOLDOWN", v)) _hi_cooldown_default=(uint32_t)v; }
    // HI cap divisor
    { long long v=_hi_cap_div; if (get_env_ll("DCQCN_HI_CAP_DIV", v)) _hi_cap_div=(uint32_t)v; }
}

DCQCNSrc::DCQCNSrc(RoceLogger* logger,
                   TrafficLogger* pktlogger,
                   EventList& eventlist,
                   linkspeed_bps linkspeed,
                   linkspeed_bps rate,
                   ofstream& statistics_outfile)
    : RoceSrc(logger, pktlogger, eventlist, rate, statistics_outfile)
{
    // Make sure env overrides are applied before using any static knobs
    apply_env_overrides_once();

    _link = linkspeed;
    _RC   = rate;
    _RT   = rate;

    // Initial alpha (overridable via env)
    _alpha = 1.0;
    get_env_double("DCQCN_ALPHA_INIT", _alpha);

    // AI/HI steps (WAN-friendly)
    _RAI  = std::max<linkspeed_bps>(_link / 192, rate / 48);
    _RHAI = std::max<linkspeed_bps>(_link / 128, rate / 32);

    _last_cc_update    = 0;
    _last_alpha_update = 0;

    _T                = 0;
    _BC               = 0;
    _byte_counter     = 0;
    _old_highest_sent = 0;

    _hi_cooldown_epochs = 0;

    // init true-rate sampling state (per flow)
    _sample_last_ts    = 0;
    _sample_last_bytes = 0;
}

void DCQCNSrc::processCNP(const Packet& /*cnp*/) {
    _RT = _RC;

    // Floor
    const linkspeed_bps floor_line = (linkspeed_bps)(_link * _floor_line_frac);
    const linkspeed_bps floor_rt   = (linkspeed_bps)(_RT   * _floor_rt_frac);
    const linkspeed_bps floor_rate = std::max<linkspeed_bps>(floor_line, floor_rt);

    // Clamp MD
    const double md_alpha = std::min(_md_cap, _alpha);
    _RC = std::max<linkspeed_bps>(
        (linkspeed_bps)(_RC * (1.0 - md_alpha / 2.0)),
        floor_rate);

    _alpha = (1 - _g) * _alpha + _g;

    _T = 0;
    _BC = 0;
    _byte_counter = 0;
    _old_highest_sent = _highest_sent;
    _hi_cooldown_epochs = _hi_cooldown_default;

    _pacing_rate = _RC;
    update_spacing();

    _last_cc_update    = eventlist().now();
    _last_alpha_update = eventlist().now();

    // Log CNP reception (for analysis)
    DCQCNSink::open_cnp_csv();
    if (DCQCNSink::cnp_csv.is_open()) {
        DCQCNSink::cnp_csv << timeAsUs(eventlist().now()) << ","
                           << flow_id() << ",rcvd\n";
    }

    eventlist().sourceIsPendingRel(*this, _cc_update_period);
}

void DCQCNSrc::increaseRate() {
    if (_RC >= _link) return;

    const bool in_cooldown = (_hi_cooldown_epochs > 0);

    if (std::max(_T, _BC) <= _F) {
        _RC = (_RT + _RC) / 2;
    } else if (!in_cooldown && std::min(_T, _BC) > _F) {
        linkspeed_bps steps = (std::min(_T, _BC) - _F);
        linkspeed_bps delta = steps * _RHAI;
        linkspeed_bps cap   = _link / std::max<uint32_t>(_hi_cap_div, 1);
        if (delta > cap) delta = cap;
        _RT  += delta;
        _RC   = (_RT + _RC) / 2;
    } else {
        _RT += _RAI;
        _RC  = (_RT + _RC) / 2;
    }

    if (_RC > _link) _RC = _link;

    _pacing_rate = _RC;
    update_spacing();
}

void DCQCNSrc::doNextEvent() {
    bool reschedule = false;

    // opportunistic sampling (target vs current)
    if (rand() % 1000 < 5) {
        if (_state_send == PAUSED) {
            _statistics_outfile << "Flow " << _name
                                << " time: " << timeAsUs(eventlist().now())
                                << " sending_rate: " << 0
                                << " current_rate: " << 0
                                << endl;
        } else {
            _statistics_outfile << "Flow " << _name
                                << " time: " << timeAsUs(eventlist().now())
                                << " sending_rate: " << _pacing_rate
                                << " current_rate: " << _pacing_rate
                                << endl;
        }
    }

    if (_done) return;

    RoceSrc::doNextEvent();

    _byte_counter += (_highest_sent - _old_highest_sent) * _mss;
    _old_highest_sent = _highest_sent;

    // ----- true sender current rate (bytes actually sent / elapsed time) -----
    {
        simtime_picosec now   = eventlist().now();
        uint64_t cum_bytes    = _highest_sent * _mss;               // bytes ever sent
        uint64_t delta_bytes  = cum_bytes - _sample_last_bytes;
        simtime_picosec dt    = now - _sample_last_ts;

        // sample roughly every ~1ms of sim time
        if (_sample_last_ts == 0 || dt >= timeFromUs(1000.0)) {
            double bps = (dt > 0) ? (double)delta_bytes * 1e12 / (double)dt : 0.0;
            _statistics_outfile << "Flow " << _name
                                << " time: " << timeAsUs(now)
                                << " current_rate: " << (linkspeed_bps)bps
                                << std::endl;
            _sample_last_ts    = now;
            _sample_last_bytes = cum_bytes;
        }
    }

    if (_byte_counter >= _B) {
        _byte_counter = 0;
        _BC++;
        increaseRate();
        reschedule = true;
    }

    if (eventlist().now() - _last_alpha_update >= _cc_update_period) {
        _alpha = (1 - _g) * _alpha;
        _last_alpha_update = eventlist().now();
        reschedule = true;
    }

    if (eventlist().now() - _last_cc_update >= _cc_update_period) {
        _last_cc_update = eventlist().now();
        _T++;
        if (_hi_cooldown_epochs > 0) _hi_cooldown_epochs--;
        increaseRate();
        reschedule = true;
    }

    if (reschedule)
        eventlist().sourceIsPendingRel(*this, _cc_update_period);
}

void DCQCNSrc::receivePacket(Packet& pkt) {
    if (!_flow_started) {
        assert(pkt.type() == ETH_PAUSE);
        return;
    }
    if (_stop_time && eventlist().now() >= _stop_time) {
        _flow_size = _highest_sent * _mss + _mss;
        _stop_time = 0;
    }
    if (_done) return;

    switch (pkt.type()) {
        case ETH_PAUSE:
            processPause((const EthPausePacket&)pkt); pkt.free(); return;
        case ROCENACK:
            _nacks_received++; processNack((const RoceNack&)pkt); pkt.free(); return;
        case ROCEACK:
            _acks_received++;  processAck((const RoceAck&)pkt);   pkt.free(); return;
        case CNP:
            _cnps_received++;  processCNP(pkt);                   pkt.free(); return;
        default: abort();
    }
}

////////////////////////////////////////////////////////////////
//  DCQCN SINK
////////////////////////////////////////////////////////////////

DCQCNSink::DCQCNSink(EventList& eventlist, ofstream& statistics_outfile)
    : RoceSink(eventlist, statistics_outfile)
{
    _last_cnp_sent_time            = UINT64_MAX;
    _marked_packets_since_last_cnp = 0;
    _packets_since_last_cnp        = 0;

    // ensure env overrides are pulled once
    DCQCNSrc::apply_env_overrides_once();

    if (!pkt_csv.is_open()) open_csv();
    if (!cnp_csv.is_open()) open_cnp_csv();
}

void DCQCNSink::open_csv() {
    if (!pkt_csv.is_open()) {
        pkt_csv.open("trace_packets.csv", std::ios::out | std::ios::trunc);
        if (pkt_csv.is_open()) {
            pkt_csv << "ts_us,flow,seq,ecn_marked,src_id,sink_tag\n";
        } else {
            std::cerr << "WARNING: could not open trace_packets.csv for writing\n";
        }
    }
}

void DCQCNSink::open_cnp_csv() {
    if (!cnp_csv.is_open()) {
        cnp_csv.open("cnp_events.csv", std::ios::out | std::ios::trunc);
        if (cnp_csv.is_open()) {
            cnp_csv << "ts_us,flow,event\n"; // event in {"sent","rcvd"}
        } else {
            std::cerr << "WARNING: could not open cnp_events.csv for writing\n";
        }
    }
}

// Receive a packet.
void DCQCNSink::receivePacket(Packet& pkt) {
    const bool ecn_marked = ((pkt.flags() & ECN_CE) != 0);

    // extract seq for data packets (0 if not RocePacket)
    uint64_t seq = 0;
    if (auto* rp = dynamic_cast<RocePacket*>(&pkt)) {
        seq = rp->seqno();
    }

    // Log observation
    if (pkt_csv.is_open()) {
        auto sid = static_cast<const EventSource*>(this)->get_id();
        pkt_csv << timeAsUs(eventlist().now()) << ","
                << (_src ? _src->flow_id() : -1) << ","
                << static_cast<long long>(seq) << ","
                << (ecn_marked ? 1 : 0) << ","
                << static_cast<long long>(_srcaddr) << ","
                << static_cast<long long>(sid) << "\n";
    }

    RoceSink::receivePacket(pkt);

    if (ecn_marked) {
        if (_last_cnp_sent_time == UINT64_MAX ||
            eventlist().now() - _last_cnp_sent_time >= _cnp_interval) {
            send_cnp();
            eventlist().sourceIsPendingRel(*this, _cnp_interval);
        } else {
            _marked_packets_since_last_cnp++;
        }
    }
    _packets_since_last_cnp++;
}

void DCQCNSink::doNextEvent() {
    if (eventlist().now() - _last_cnp_sent_time >= _cnp_interval &&
        _marked_packets_since_last_cnp > 0) {
        send_cnp();
        eventlist().sourceIsPendingRel(*this, _cnp_interval);
    }
}

void DCQCNSink::send_cnp() {
    // Build & send a CNP back to the source.
    CNPPacket* cnp = CNPPacket::newpkt(_src->_flow, *_route, _cumulative_ack, _srcaddr);
    cnp->set_pathid(0);
    cnp->sendOn();

    _last_cnp_sent_time            = eventlist().now();
    _packets_since_last_cnp        = 0;
    _marked_packets_since_last_cnp = 0;

    // Log CNP sent
    open_cnp_csv();
    if (cnp_csv.is_open()) {
        cnp_csv << timeAsUs(_last_cnp_sent_time) << ","
                << (_src ? _src->flow_id() : -1) << ",sent\n";
    }
}
