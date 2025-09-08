// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-

#ifndef DCQCN_H
#define DCQCN_H

/*
 * A DCQCN source and sink with tracing
 */

#include <list>
#include <map>
#include <fstream>
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

    virtual void receivePacket(Packet& pkt);
    virtual void processCNP(const CNPPacket& cnp);
    virtual void increaseRate();
    virtual void doNextEvent();

    // should really be private, but loggers want to see:
    uint32_t _cnps_received;

    static simtime_picosec _cc_update_period;
    static double          _alpha, _g;
    static uint32_t        _F;
    static linkspeed_bps   _RAI, _RHAI;
    static uint64_t        _B;

private:
    simtime_picosec _last_cc_update, _last_alpha_update;
    linkspeed_bps   _RC, _RT, _link;

    uint16_t _T, _BC;
    uint64_t _byte_counter;
    uint64_t _old_highest_sent;

    int _hi_cooldown_epochs;
};

class DCQCNSink : public RoceSink {
    friend class DCQCNSrc;

public:
    DCQCNSink(EventList& eventlist, ofstream& statistics_outfile);
    virtual void doNextEvent();
    virtual void receivePacket(Packet& pkt);

    static simtime_picosec _cnp_interval;

    inline id_t get_id() const { return EventSource::get_id(); }

    // CSV tracing
    static std::ofstream pkt_csv;
    static void open_csv();

private:
    simtime_picosec _last_cnp_sent_time;

    uint32_t _marked_packets_since_last_cnp;
    uint32_t _packets_since_last_cnp;

    void send_cnp();
};

#endif
