// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef CBR_H
#define CBR_H

/*
 * A non responsive flow, source and sink
 */

#include <algorithm>
#include <bitset>
#include <deque>
#include <list>
#include <set>
#include <tuple>
#include <vector>

#include "cbrpacket.h"
#include "data_collector.h"
#include "data_receiver.h"
#include "erasure_coding.h"
#include "event_source.h"
#include "eventlist.h"
#include "helpers.h"
#include "network.h"
#include "packet.h"
#include "packet_flow.h"
#include "packet_sink.h"
#include "trigger.h"
#include "types.h"

class CbrSink;

#define timeInf 0

class CbrSrc : public EventSource, public PacketSink {
    friend class CbrSink;

public:
    CbrSrc(EventList&          eventlist,
           linkspeed_bps       rate,
           ofstream&           statistics_outfile,
           Packet::PktPriority priority = Packet::PRIO_NONE);

    void connect(Route* routeout, Route* routeback, CbrSink& sink, simtime_picosec startTime);

    void receivePacket(Packet& pkt) override;

    void setNumberEntropies(int num_entropies) { _num_entropies = num_entropies; }

    void set_src(uint32_t src) { _srcaddr = src; }

    void set_dst(uint32_t dst) { _dstaddr = dst; }

    mem_b flowsize() { return _flow_size; }

    void doNextEvent() override;
    void send_packet();

    inline void set_flowid(flowid_t flow_id) { _flow.set_flowid(flow_id); }

    inline PacketFlow& flow() { return _flow; }

    inline flowid_t flow_id() const { return _flow.flow_id(); }

    static void setRouteStrategy(RouteStrategy strat) { _route_strategy = strat; }

    void set_paths(uint32_t num_paths);

    int choose_route();

    void setFlowSize(uint64_t flow_size_in_bytes);
    void start_flow();
    void finish_flow();

    void set_hash_salt(uint32_t salt) { _hash_salt = salt; }

    const string& nodename() override { return _nodename; }

    deque<tuple<uint32_t, uint32_t, uint32_t, vector<uint32_t>>>
        unfinished_message_list;  // tuple<msn, original_msn (for retransmitted message),
                                  // stripe_num, vector<stripe_ids>>

    uint32_t _srcaddr;
    uint32_t _dstaddr;
    uint32_t _hash_salt;
    mem_b    _flow_size;
    mem_b    _sent_bytes;  // total bytes sent by this flow
    uint64_t _flow_start_time;
    bool     _flow_started  = false;
    bool     _flow_finished = false;

    int _num_entropies = -1;

    // should really be private, but loggers want to see:
    linkspeed_bps        _bitrate;
    int                  _crt_id;
    int                  _mss;
    simtime_picosec      _period;
    Packet::PktPriority  _priority;
    static RouteStrategy _route_strategy;

    uint16_t             _crt_path = 0;
    vector<const Route*> _paths;
    vector<int>          _path_ids;        // path IDs to be used for ECMP FIB.
    vector<const Route*> _original_paths;  // paths in original permutation order

    uint32_t total_sack = 0;

    simtime_picosec _restart_timeout =
        timeFromUs((uint32_t)14 * 256 *
                   1000);  // The period to wait before sending a NACK if no progress of mbl is made
    simtime_picosec _sack_restart_timeout =
        timeInf;  // the absolute simulation time when the next SACK/restart event should fire
    bool _restart_timeout_pending = false;

private:
    string _nodename;
    // Connectivity
    PacketFlow _flow;
    CbrSink*   _sink;
    Route*     _route;

    static bool           use_erasure_coding;
    static ErasureCoding* _erasure_coding;
    unsigned int          message_size_bytes = 0;
    uint64_t              number_of_messages = 0;
    uint32_t              _next_msn          = 0;
    uint32_t              _next_retrans_msn  = 0;  // next message to retransmit

    // States for sending the original packets
    deque<uint32_t> message_list;  // maintain message ids to send -- when message_list is empty,
                                   // the flow is completed.
    deque<tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> original_packet_list;
    // tuple<msn, stripe_offset, pkt_offset, stripe_num_in_message, last_psn_in_msg>

    // states for sending retransmission packets
    deque<tuple<uint32_t, ErasureCoding::message_sack>>  // tuple<msn, sack_info>
        received_msg_sacks;  // maintain received message SACKs, so that we know what to retransmit
    deque<tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>>
        retransmission_packet_list;  // maintain retransmission packets to send
    // tuple<msn, stripe_offset, pkt_offset, stripe_num, last_psn_in_msg, original_msn,
    // original_stripe_offset>

    bool _cur_is_retrans = false;  // whether the current packet is a retransmission

    ofstream& _statistics_outfile;

    // Mechanism
    void send_packets();
};

class CbrSink : public EventSource, public PacketSink, public DataReceiver {
public:
    struct logging_statistics {
        uint32_t num_received_messages      = 0;
        uint32_t num_received_chunks        = 0;
        uint32_t num_recovered_chunks       = 0;
        size_t   max_size_bitmap            = 0;
        uint32_t num_bitmap_overflow_drops  = 0;
        uint32_t num_bitmap_underflow_drops = 0;
        uint32_t num_trivial_skips          = 0;
        uint32_t num_recoverable_skips      = 0;
        uint32_t num_lossy_skips            = 0;
        uint32_t num_full_skips             = 0;
        uint32_t num_nack_sent              = 0;
    } statistics;

    void registerMetrics();
    void logMetricSink();

    void update_bitmap_statistics();

    CbrSink(EventList& eventlist);

    ~CbrSink() {}

    void receivePacket(Packet& pkt);

    uint64_t cumulative_ack() { return _cumulative_ack; }

    uint32_t drops() {
        throw std::runtime_error("CbrSink::drops() should not be called");
        return 1;
    }

    void send_sack_packet_ec(simtime_picosec         ts,
                             uint32_t                msn,
                             uint32_t                num_missing_stripes,
                             const vector<uint32_t>& missing_stripes);

    void set_hash_salt(uint32_t salt) { _hash_salt = salt; }

    void set_use_full_skip(bool use) { _use_full_skip = use; }

    void set_src(uint32_t s) { _srcaddr = s; }

    void connect(CbrSrc& src, const Route* route);

    static void setRouteStrategy(RouteStrategy strat) { _route_strategy = strat; }

    void set_paths(uint32_t num_paths);

    const string& nodename() { return _nodename; }

    void doNextEvent() override;

    void set_use_bitmap_full_condition_lossy_skip(bool use) { _use_bitmap_full_condition = use; }

    uint32_t _last_id;         // the id of the last packet we have received
    uint32_t _received;        // number of packets received;_last_id-_received = dropped packets
    uint64_t _cumulative_ack;  //_received * 1000 - this is for loggers

    uint32_t _srcaddr;
    uint32_t _hash_salt;

    bool flow_active = false;

private:
    string _nodename;

    CbrSrc*         _src;
    simtime_picosec _flow_start_time = 0;

    uint16_t             _crt_path = 0;
    vector<const Route*> _paths;
    vector<int>          _path_ids;        // path IDs to be used for ECMP FIB.
    vector<const Route*> _original_paths;  // paths in original permutation order

    static bool           use_erasure_coding;
    static ErasureCoding* _erasure_coding;

    int                  _mss;
    Packet::PktPriority  _priority;
    static RouteStrategy _route_strategy;
    const Route*         _route;

    // retransmission logic
    uint32_t epsn                       = 0;
    uint32_t max_psn                    = 0;
    uint32_t full_skip_threshold        = 256;  // in terms of stripes,
    uint32_t lossy_skip_threshold       = 256;  // in terms of stripes
    uint32_t recoverable_skip_threshold = 20;   // in terms of stripes; recoverable_skip_threshold
                                                // is smaller than lossy_skip_threshold

    map<uint32_t, vector<uint32_t>> missing_stripes_map;  // for constructing SACKs
    simtime_picosec                 _restart_timeout = timeFromUs(
        (uint32_t)10000);  // The period to wait before sending a NACK if no progress of mbl is made
    simtime_picosec _nack_restart_timeout =
        timeInf;  // the absolute simulation time when the next NACK/restart event should fire
    bool _restart_timeout_pending = false;
    bool _mbl_change              = false;

    bool _use_full_skip = false;
    bool _use_as_fast_as_possible =
        false;  // whether to use as fast as possible for recoverable skip
    bool _use_bitmap_full_condition = false;  // whether to use bitmap full condition for lossy skip
    double _bitmap_full_percent     = 0.9;
    bool   _mbl_overflow            = false;

    CsvMetric* _sink_stats;
};

#endif