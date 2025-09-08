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

class OneECCCSink;
class OneECSmarttPacer;

#define timeInf 0

class OneECCCSrc : public EventSource, public PacketSink {
    friend class OneECCCSink;

public:
    OneECCCSrc(EventList&          eventlist,
               linkspeed_bps       rate,
               ofstream&           statistics_outfile,
               Packet::PktPriority priority = Packet::PRIO_NONE);

    void connect(Route* routeout, Route* routeback, OneECCCSink& sink, simtime_picosec startTime);

    void receivePacket(Packet& pkt) override;

    void setNumberEntropies(int num_entropies) { _num_entropies = num_entropies; }

    void set_src(uint32_t src) { _srcaddr = src; }

    void set_dst(uint32_t dst) { _dstaddr = dst; }

    mem_b flowsize() { return _flow_size; }

    void doNextEvent() override;
    void send_packet();
    void send_packets_ec();
    void send_next_packet_ec();

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

    // Congestion Control
    static void set_starting_cwnd(double value) { starting_cwnd = value; }

    void set_cc_algorithm_type(const string& type) { cc_algorithm_type = type; }

    void set_starting_cwnd_bdp_ratio(float value) { starting_cwnd_bdp_ratio = value; }

    // Helper function to compute path metrics
    void compute_path_metrics(vector<simtime_picosec>& per_path_rtt,
                              uint64_t*                path_to_replace,
                              simtime_picosec*         max_rtt);

    void adjust_window_mimd(uint64_t        path_to_replace,
                            simtime_picosec max_rtt,
                            uint32_t        entropy_to_replace);
    void adjust_window_aimd(uint64_t        num_bytes_acked,
                            uint64_t        bytes_ecn_marked,
                            simtime_picosec ts,
                            uint64_t        last_rtt,
                            uint32_t        entropy_to_replace);
    void check_limits_cwnd();
    void update_pacing_delay();
    void track_sending_rate();
    bool shouldTriggerEpochEnd(simtime_picosec acked_pkt_ts);
    void resetEpochParams();
    bool more_data_available() const;

    void  additive_increase(uint64_t num_bytes_acked);
    void  processEpochEnd(simtime_picosec rtt);
    void  processQaMeasurementEnd(simtime_picosec rtt);
    float computeEwma(double ewma_estimate, double new_value, double alpha);

    void processECMetricACK(CbrPacket& pkt);

    uint64_t get_unacked() const { return _unacked; }

    void reset_unacked() { _unacked = 0; }

    void reduce_unacked(uint64_t amount);

    void updateParams(uint64_t      base_rtt,
                      linkspeed_bps _network_linkspeed,
                      double        init_cwnd_ratio,
                      double        contract_scaling,
                      uint64_t      inter_queuesize);

    void set_jittery_path_replace_threshold_percentage(double percent) {
        if (percent < 0.0 || percent > 1.0) {
            throw std::runtime_error(
                "OneECCCSrc::set_jittery_path_replace_threshold_percentage: "
                "percentage must be in [0, 1]");
        }
        jittery_path_replace_threshold_percentage = percent;
    }

    void set_use_replace_path(bool use) { use_replace_path = use; }

    void set_apply_mimd(bool mimd) { apply_mimd = mimd; }

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

    // CC variables
    string          cc_algorithm_type;
    double          _cwnd;
    simtime_picosec _base_rtt;
    static double   starting_cwnd;
    uint64_t        _bdp;
    uint64_t        _queue_size;
    uint64_t        last_pac_change     = 0;
    uint64_t        previous_window_end = 0;
    bool            _target_based_received;
    uint64_t        _maxcwnd;
    uint32_t        target_window;
    bool            apply_mimd         = true;  // congestion control type, default is mimd
    float           _maxcwnd_bdp_ratio = 1.0;
    uint64_t        _unacked           = 0;

    linkspeed_bps   _network_linkspeed;
    double          max_cwnd_scaling = 10;
    simtime_picosec t_last_update    = 0;
    double          contract_scaling = 10;
    float           starting_cwnd_bdp_ratio =
        0.7;  // if starting_cwnd = 1 then cwnd becomes _bdp * starting_cwnd_bdp_ratio

    bool              use_pacing = true;
    simtime_picosec   pacing_delay;
    bool              _paced_packet   = false;
    OneECSmarttPacer* generic_pacer   = NULL;
    bool              pause_send      = false;
    simtime_picosec   tracking_period = 0;
    simtime_picosec   qa_period       = 0;
    simtime_picosec   last_track_ts   = 0;
    uint64_t          tracking_bytes  = 0;

    simtime_picosec target_rtt = 0;  // Low threshold to perform reductions for RTT.

    uint32_t _consecutive_decreases;

    // This is the part for various epoch end techniques
    simtime_picosec    epoch_end_time = 0;
    static std::string epoch_type;
    bool               qa_enabled                     = false;
    double             constant_timestamp_epoch_ratio = 1.0;
    static std::string qa_type;
    float              epoch_counter;
    bool               epoch_enabled    = true;
    bool               use_per_ack_ewma = false;
    simtime_picosec    epoch_period     = timeAsUs((uint32_t)1000);
    double epoch_period_factor = 400.0;  // epoch_period_factor is used to scale down starting from
                                         // rtt, epoch_period = rtt / epoch_period_factor

    double                                  epoch_start_cwnd = 0;
    vector<pair<simtime_picosec, int>>      list_ecn_rate;
    vector<pair<simtime_picosec, double>>   list_sending_rate;
    vector<pair<simtime_picosec, uint64_t>> _list_ecn_received;

    uint64_t        total_new_bytes_acked_in_epoch = 0;
    uint64_t        ecn_marked_bytes_in_epoch;
    uint32_t        bytes_acked_in_qa_period = 0;
    simtime_picosec qa_period_time =
        0;  // Keeps track of the measurement period for received bytes.

    simtime_picosec baremetal_rtt  = 0;   // Baremetal latency.
    double          ai_bytes       = 1;   // Additive increase constant for LCP.
    double          ai_bytes_scale = -1;  // ai_bytes / bdp = ai_bytes_scale
    float md_gain_ecn = -1;  // Multiplicative decrease gain for ECN-based reduction md_gan should
                             // be >= 0 and <= 1 so 10
    float           md_factor_rtt = -1;  // Multiplicative decrease gain for RTT-based reduction
    simtime_picosec target_qdelay = 0;
    bool            use_qa        = true;
    bool            use_fi        = false;
    bool            use_rtt       = true;
    bool            use_ecn       = true;
    bool            use_rto_cwnd_reduction = true;
    bool            use_trimming           = false;
    bool            apply_ai_per_epoch     = false;  // default apply ai is per ACK
    double          lcp_k                  = 0;
    double          lcp_k_scale            = 1.0;
    float           target_to_baremetal_ratio =
        1.05;  // what target_rtt = baremetal_rtt * target_to_baremetal_ratio
    uint32_t fi_threshold        = 3;  // fast increase threshold
    int      fast_increase_round = 0;
    float    QA_TRIGGER_RTT_FRACTION =
        0.9;  // Fraction of queueing latency to add to the baremetal RTT.
    // LCP.
    uint64_t      queuesize_bytes         = 0;
    linkspeed_bps INTER_LINK_SPEED_MODERN = 400000000;  // 400Gbps

    float           old_ecn_ewma     = 0;
    float           old_ecn_ewma_ago = 0;
    simtime_picosec last_ewma_update = 0;
    float           ecn_fraction_ewma;
    float           _ecn_count_this_window;
    float           _good_count_this_window;
    uint32_t        _consecutive_good_epochs;  // Counts number of epochs without congestion.
    uint64_t _next_qa_sn;  // Next sequence number that must be received before window changes are
                           // allowed.
    simtime_picosec qa_measurement_period_end;  // Time that current QA ends

    int             average_count          = 0;
    double          average_online         = 0;
    simtime_picosec last_average_update    = 0;
    double          old_average_online     = 0;
    double          current_average_online = 0;

    float           lcp_ecn_alpha  = 1.0;
    simtime_picosec qa_trigger_rtt = 0;

    float QA_CWND_RATIO_THRESHOLD = 0.9;

    double jittery_path_replace_threshold_percentage = 0.1;
    bool   use_replace_path                          = false;

    uint64_t num_replace_loss    = 0;
    uint64_t num_replace_jittery = 0;

private:
    string _nodename;
    // Connectivity
    PacketFlow   _flow;
    OneECCCSink* _sink;
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

    simtime_picosec contract_target_delay_vegas(mem_b           cwnd,
                                                simtime_picosec rtt,
                                                simtime_picosec delay);
};

class OneECCCSink : public EventSource, public PacketSink, public DataReceiver {
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

    OneECCCSink(EventList& eventlist, ofstream& statistics_outfile);

    ~OneECCCSink() {}

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

    void connect(OneECCCSrc& src, const Route* route);

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
    void send_metric_ack_packet_ec(simtime_picosec ts, uint32_t last_path_id);

    void restart_flow_timer_hook(simtime_picosec now, simtime_picosec period);
    void doNextEvent() override;
    void restartFlow();
    bool check_flow_active();

    void metric_ack_timer_hook(simtime_picosec now, simtime_picosec period);

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

    void set_loss_path_replace_threshold(double percent) {
        if (percent < 0.0 || percent > 1.0) {
            throw std::runtime_error(
                "CbrSink::set_loss_path_replace_threshold: "
                "percentage must be in [0, 1]");
        }
        loss_path_replace_threshold = percent * ErasureCoding::BITMAP_SIZE_1EC;
    }

    void set_use_bitmap_full_condition_lossy_skip(bool use) { _use_bitmap_full_condition = use; }

    uint32_t _last_id;         // the id of the last packet we have received
    uint32_t _received;        // number of packets received;_last_id-_received = dropped packets
    uint64_t _cumulative_ack;  //_received * 1000 - this is for loggers
    uint64_t received_since_last_log = 0;  // number of bytes received since last log

    uint32_t _srcaddr;
    uint32_t _hash_salt;

    bool flow_active = false;

private:
    ofstream&       _statistics_outfile;
    simtime_picosec last_logging = 0;
    string          _nodename;

    OneECCCSrc*     _src;
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
        (uint32_t)40000);  // The period to wait before sending a NACK if no progress of mbl is made
    simtime_picosec _nack_restart_timeout =
        timeInf;  // the absolute simulation time when the next NACK/restart event should fire
    bool _restart_timeout_pending = false;
    bool _mbl_change              = false;

    // metric timeout
    bool            _metric_ack_timeout_pending = false;
    simtime_picosec _metric_ack_timeout = timeInf;  // the absolute simulation time when the next
                                                    // metric ACK event should fire
    simtime_picosec _metric_ack_period =
        timeFromUs((uint32_t)50);  // default metric ACK period, 50 us
    bool _can_send_metric_ack = true;
    // Metric ACK
    vector<uint32_t> per_path_loss_counts;
    uint64_t         _acked_bytes_per_metric_ack      = 0;
    uint32_t         _bytes_ecn_marked_per_metric_ack = 0;
    vector<bool>     per_path_last_loss;  // to track consecutive losses per path

    vector<simtime_picosec> per_path_one_way_latencies;  // one-way latencies for each path
    simtime_picosec         last_original_pkt_ts;
    uint32_t                last_path_id;
    uint32_t                loss_path_replace_threshold = 0.5 * ErasureCoding::BITMAP_SIZE_1EC;

    // configuration
    bool _use_full_skip = false;
    bool _use_as_fast_as_possible =
        false;  // whether to use as fast as possible for recoverable skip
    bool _use_bitmap_full_condition = false;  // whether to use bitmap full condition for lossy skip
    double _bitmap_full_percent     = 0.9;
    bool   _mbl_overflow            = false;

    CsvMetric* _sink_stats;

    void update_per_path_loss_count(size_t bitmap_idx);
};

class OneECCCSrcRestartTimerScanner : public EventSource {
public:
    OneECCCSrcRestartTimerScanner(simtime_picosec scanPeriod, EventList& eventlist);
    void doNextEvent();
    void registerCbr(OneECCCSrc& cbrsrc);

private:
    simtime_picosec           _scanPeriod;
    typedef list<OneECCCSrc*> cbrs_t;
    cbrs_t                    _cbrs;
};

class OneECCCSinkRestartTimerScanner : public EventSource {
public:
    OneECCCSinkRestartTimerScanner(simtime_picosec scanPeriod, EventList& eventlist);
    void doNextEvent();
    void registerCbr(OneECCCSink& cbrsink);

private:
    simtime_picosec            _scanPeriod;
    typedef list<OneECCCSink*> cbrs_t;
    cbrs_t                     _cbrs;
};

class OneECCCSinkMetricACKTimerScanner : public EventSource {
public:
    OneECCCSinkMetricACKTimerScanner(simtime_picosec scanPeriod, EventList& eventlist);
    void doNextEvent();
    void registerCbr(OneECCCSink& cbrsink);

private:
    simtime_picosec            _scanPeriod;
    typedef list<OneECCCSink*> cbrs_t;
    cbrs_t                     _cbrs;
};

#endif