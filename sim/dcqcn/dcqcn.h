// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef DCQCN_H
#define DCQCN_H

#include "config.h"
#include "roce.h"      // RoceSrc/RoceSink base classes
#include "eventlist.h"
#include <fstream>

class DCQCNSink;

class DCQCNSrc : public RoceSrc {
public:
    DCQCNSrc(RoceLogger* logger, TrafficLogger* pktlogger, EventList& eventlist,
             linkspeed_bps linkspeed, linkspeed_bps rate, std::ofstream& statistics_outfile);

    void doNextEvent() override;
    void receivePacket(Packet& pkt) override;

    // ---- DCQCN global knobs (defaults compiled-in; may be overridden by env) ----
    static simtime_picosec _cc_update_period; // epoch
    static double          _g;                // alpha EWMA decay
    static uint64_t        _B;                // bytes per BC epoch
    static uint32_t        _F;                // epochs before HI eligible
    static linkspeed_bps   _RAI;              // base AI step (set per instance)
    static linkspeed_bps   _RHAI;             // HI step (set per instance)

    // Additional tunables (env-overridable)
    static double    _md_cap;              // per-CNP MD cap
    static double    _floor_line_frac;     // RC floor: fraction of line
    static double    _floor_rt_frac;       // RC floor: fraction of previous RT
    static uint32_t  _hi_cooldown_default; // epochs of AI-only after MD
    static uint32_t  _hi_cap_div;          // HI cap limiter (link / div)

    // one-time env override (safe re-entry)
    static void apply_env_overrides_once();

protected:
    // Take Packet& here so header doesn’t need CNPPacket declaration
    void processCNP(const Packet& cnp);
    void increaseRate();

    // state
    linkspeed_bps   _link;
    linkspeed_bps   _RC;      // current/pacing rate (what we actually pace at)
    linkspeed_bps   _RT;      // target/aux rate used by the controller
    simtime_picosec _last_cc_update;
    simtime_picosec _last_alpha_update;

    uint32_t        _T;            // epoch counter (time-based)
    uint32_t        _BC;           // epoch counter (bytes-based)
    uint64_t        _byte_counter;
    uint64_t        _old_highest_sent;

    uint32_t        _hi_cooldown_epochs;

    // α is per-source (not static); _g is static
    double          _alpha;

    // diagnostics
    uint64_t        _cnps_received = 0;

    // ---- NEW: sender-current-rate sampling (for plots) ----
    // Cumulative bytes sent at last sample and timestamp of that sample.
    // Used in doNextEvent() to log "current_rate:" as bytes/delta_t.
    uint64_t        _sample_last_bytes = 0;
    simtime_picosec _sample_last_ts    = 0;

private:
    static bool     _env_applied;
};

class DCQCNSink : public RoceSink {
public:
    DCQCNSink(EventList& eventlist, std::ofstream& statistics_outfile);

    void receivePacket(Packet& pkt) override;
    void doNextEvent() override;

    static simtime_picosec _cnp_interval;

    static std::ofstream pkt_csv;  // packet observations
    static std::ofstream cnp_csv;  // CNP events (sent/rcvd)

    static void open_csv();
    static void open_cnp_csv();

protected:
    void send_cnp();

    simtime_picosec _last_cnp_sent_time;
    uint64_t        _marked_packets_since_last_cnp;
    uint64_t        _packets_since_last_cnp;
};

#endif // DCQCN_H
