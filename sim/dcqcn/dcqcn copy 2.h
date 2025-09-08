// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-

#ifndef DCQCN_H
#define DCQCN_H

/*
 * A DCQCN source and sink
 */

#include <list>
#include <map>

#include "cnppacket.h"
#include "eth_pause_packet.h"
#include "event_source.h"
#include "eventlist.h"
#include "helpers.h"
#include "math.h"
#include "packet.h"
#include "packet_flow.h"
#include "queue.h"
#include "roce.h"
#include "rocepacket.h"
#include "trigger.h"
#include "types.h"

#define timeInf 0

class DCQCNSink;
class Switch;

class DCQCNSrc : public RoceSrc {
    friend class DCQCNSink;

public:
    DCQCNSrc(RoceLogger*    logger,
             TrafficLogger* pktlogger,
             EventList&     eventlist,
             linkspeed_bps  linkspeed,
             linkspeed_bps  rate,
             ofstream&      statistics_outfile);

    // datapath handlers
    virtual void receivePacket(Packet& pkt);

    // CC core
    virtual void processCNP(const CNPPacket& cnp);
    virtual void increaseRate();
    virtual void doNextEvent();

    // visible to loggers
    uint32_t _cnps_received;

    // --- Static knobs / shared params (same across all DCQCN sources) ---
    static simtime_picosec _cc_update_period;  // control-loop epoch (t and k timers)
    static double          _alpha;             // DCQCN alpha
    static double          _g;                 // EWMA gain for alpha
    static uint32_t        _F;                 // number of epochs before HI eligible
    static linkspeed_bps   _RAI;               // additive increase step
    static linkspeed_bps   _RHAI;              // hyper-additive increase step
    static uint64_t        _B;                 // bytes per epoch to bump BC

    // WAN-safety extensions (not in original paper)
    static double          _rate_floor_frac;            // min fraction of line after MD
    static double          _md_alpha_cap;               // cap α used per MD
    static uint32_t        _hi_cooldown_epochs_default; // AI-only epochs after MD
    static bool            _one_md_per_epoch;           // coalesce multiple CNPs in one epoch

private:
    // time bookkeeping
    simtime_picosec _last_cc_update, _last_alpha_update;

    // rate state
    linkspeed_bps   _RC, _RT, _link;

    // Stage-2 counters
    uint16_t        _T;              // epochs since last CNP (t timer)
    uint16_t        _BC;             // byte-counter epochs
    uint64_t        _byte_counter;   // bytes accrued in current epoch
    uint64_t        _old_highest_sent;

    // Stability helpers
    int32_t         _hi_cooldown_epochs;  // remaining epochs of AI-only after MD
    int32_t         _last_md_epoch;       // last epoch index when we applied MD
    uint64_t        _cnps_received_total; // cumulative CNPs (for logging)
};

class DCQCNSink : public RoceSink {
    friend class DCQCNSrc;

public:
    DCQCNSink(EventList& eventlist, ofstream& statistics_outfile);

    virtual void doNextEvent();
    virtual void receivePacket(Packet& pkt);

    static simtime_picosec _cnp_interval;  // min spacing between sent CNPs

    inline id_t get_id() const { return EventSource::get_id(); }

private:
    // CNP pacing
    simtime_picosec _last_cnp_sent_time;
    uint32_t        _marked_packets_since_last_cnp;
    uint32_t        _packets_since_last_cnp;

    // simple instrumentation
    uint64_t        _ecn_marked_total;
    uint64_t        _cnp_sent_total;

    // Mechanism
    void send_cnp();
};

#endif  // DCQCN_H
