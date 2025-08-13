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

class OneECbrSink;

#define timeInf 0

class OneECbrSrc : public EventSource, public PacketSink {
    friend class OneECbrSink;

public:
    OneECbrSrc(EventList&          eventlist,
               linkspeed_bps       rate,
               ofstream&           statistics_outfile,
               Packet::PktPriority priority = Packet::PRIO_NONE);

    void connect(Route* routeout, Route* routeback, OneECbrSink& sink, simtime_picosec startTime);

    void receivePacket(Packet& pkt) override;

    void setNumberEntropies(int num_entropies) { _num_entropies = num_entropies; }

    void set_src(uint32_t src) { _srcaddr = src; }

    void set_dst(uint32_t dst) { _dstaddr = dst; }

    mem_b flowsize() { return _flow_size; }

    void doNextEvent() override;
    void send_packet();

    // retransmission logic
    void send_packet_ec(
        uint32_t msn,              // message sequence number
        uint32_t stripe_offset,    // stripe offset in a message
        uint32_t pkt_offset,       // packet offset in a stripe
        uint32_t stripe_num,       // number of stripes in a message
        uint32_t last_psn_in_msg,  // last psn in the message
        uint32_t original_msn,     // original message sequence number for retransmitted packets
        uint32_t original_stripe_offset);  // original stripe offset for retransmitted packets
    void update_offsets_ec(uint32_t msn,
                           uint32_t stripe_offset,
                           uint32_t pkt_offset,
                           uint32_t stripe_num);
    void update_retrans_offsets_ec(uint32_t msn,
                                   uint32_t stripe_offset,
                                   uint32_t pkt_offset,
                                   uint32_t stripe_num);
    void processECSACK(CbrPacket& pkt);
    void processECNACK(CbrPacket& pkt);
    void restart_flow_timer_hook(simtime_picosec now, simtime_picosec period);
    void restartFlow();
    void SACKTimeOut();

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

    static void set_use_erasure_coding(bool use) { use_erasure_coding = use; }

    static bool get_use_erasure_coding() { return use_erasure_coding; }

    static void set_erasure_coding(ErasureCoding* ec) { _erasure_coding = ec; }

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
    PacketFlow   _flow;
    OneECbrSink* _sink;
    Route*       _route;

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

class OneECbrSink : public EventSource, public PacketSink, public DataReceiver {
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

    OneECbrSink(EventList& eventlist);

    ~OneECbrSink() {}

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

    void connect(OneECbrSrc& src, const Route* route);

    static void setRouteStrategy(RouteStrategy strat) { _route_strategy = strat; }

    void set_paths(uint32_t num_paths);

    const string& nodename() { return _nodename; }

    static void set_use_erasure_coding(bool use) { use_erasure_coding = use; }

    static void set_erasure_coding(ErasureCoding* ec) { _erasure_coding = ec; }

    // retransmission logic
    uint32_t check_lossy_skip(bool do_full_skip = false);

    void update_mbl(CbrPacket& pkt);

    inline bool     is_bitmap_full(const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap);
    inline uint32_t count_consecutive_ones(const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap);

    inline uint32_t count_ones(const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap, uint32_t size);

    inline bool is_num_consequtive_ones(const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap,
                                        uint32_t                                      k);

    inline size_t find_highest_bit(const bitset<ErasureCoding::BITMAP_SIZE_1EC>& bitmap);

    uint32_t check_stripe_bitmap_update();

    void shift_stripe();

    uint32_t check_trivial_skip();

    uint32_t              check_recoverable_skip(bool do_full_skip = false);
    tuple<uint32_t, bool> check_pkt(CbrPacket& pkt);
    tuple<uint32_t, bool> check_pkt_full_skip(CbrPacket& pkt);

    bool is_full_skip_condition();
    bool is_lossy_skip_condition();
    bool is_recoverable_skip_condition();

    void complete_mbl(simtime_picosec ts, uint32_t epsn);

    void send_nack_packet_ec(simtime_picosec ts);

    void restart_flow_timer_hook(simtime_picosec now, simtime_picosec period);
    void doNextEvent() override;
    bool check_flow_active();

    void set_recoverable_threshold(uint32_t threshold) { recoverable_skip_threshold = threshold; }

    void set_lossy_threshold(uint32_t threshold) { lossy_skip_threshold = threshold; }

    void set_use_as_fast_as_possible_recoverable_skip(bool use) { _use_as_fast_as_possible = use; }

    void set_use_bitmap_full_percentage(double percent) {
        if (percent < 0.0 || percent > 1.0) {
            throw std::runtime_error(
                "CbrSink::set_use_bitmap_full_percentage: "
                "percentage must be in [0, 1]");
        }
        _bitmap_full_percent = percent;
    }

    void set_use_bitmap_full_condition_lossy_skip(bool use) { _use_bitmap_full_condition = use; }

    uint32_t _last_id;         // the id of the last packet we have received
    uint32_t _received;        // number of packets received;_last_id-_received = dropped packets
    uint64_t _cumulative_ack;  //_received * 1000 - this is for loggers

    uint32_t _srcaddr;
    uint32_t _hash_salt;

    bool flow_active = false;

private:
    string _nodename;

    OneECbrSrc*     _src;
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
    uint32_t         max_stripe_offset = 0;
    uint32_t         mbl_base_msn      = 0;  // base msn for the message boundary list at head
    vector<uint32_t> message_boundary_list;  // tuple<psn>
    int              mbl_head = 0;           // head of the message boundary list
    int              mbl_tail = 0;           // tail of the message boundary list
    int              mbl_size = 0;
    bitset<ErasureCoding::BITMAP_SIZE_1EC> bitmap;
    map<uint32_t, vector<uint32_t>>        missing_stripes_map;  // for constructing SACKs
    simtime_picosec                        _restart_timeout = timeFromUs(
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

class OneECbrSrcRestartTimerScanner : public EventSource {
public:
    OneECbrSrcRestartTimerScanner(simtime_picosec scanPeriod, EventList& eventlist);
    void doNextEvent();
    void registerCbr(OneECbrSrc& cbrsrc);

private:
    simtime_picosec           _scanPeriod;
    typedef list<OneECbrSrc*> cbrs_t;
    cbrs_t                    _cbrs;
};

class OneECbrSinkRestartTimerScanner : public EventSource {
public:
    OneECbrSinkRestartTimerScanner(simtime_picosec scanPeriod, EventList& eventlist);
    void doNextEvent();
    void registerCbr(OneECbrSink& cbrsink);

private:
    simtime_picosec            _scanPeriod;
    typedef list<OneECbrSink*> cbrs_t;
    cbrs_t                     _cbrs;
};

#endif