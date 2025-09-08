// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef _AISWITCH_H
#define _AISWITCH_H

#include <unordered_map>

#include "AI_DC_msoft.h"
#include "AI_WAN_msoft.h"
#include "AI_region_msoft.h"
#include "AI_switch_info.h"
#include "callback_pipe.h"
#include "fat_tree_switch.h"
#include "switch.h"
#include <unordered_set>
#include <fstream>
#include "roce.h"   // for flowid_t / RocePacket

class AIDCMsoft;
class AIRegionMsoft;
class AIWANMsoft;

class AISwitch : public Switch {
public:
    enum node_type { SERVER = 0, TOR, LEAF, SPINE, RH, RWA, OWR };

    enum routing_strategy {
        NIX              = 0,
        ECMP             = 1,
        ADAPTIVE_ROUTING = 2,
        ECMP_ADAPTIVE    = 3,
        RR               = 4,
        RR_ECMP          = 5,
        SINGLE           = 6
    };

    enum sticky_choices { PER_PACKET = 0, PER_FLOWLET = 1 };

    AISwitch(EventList&      eventlist,
             string          switch_name,
             node_type       type,
             uint64_t        switch_id,
             simtime_picosec delay,
             AIDCMsoft*      dc_topo,
             uint64_t        dc_id,
             uint64_t        region_id);
    AISwitch(EventList&      eventlist,
             string          switch_name,
             node_type       type,
             uint64_t        switch_id,
             simtime_picosec delay,
             AIRegionMsoft*  region_topo,
             uint64_t        region_id);
    AISwitch(EventList&      eventlist,
             string          switch_name,
             node_type       type,
             uint64_t        switch_id,
             simtime_picosec delay,
             AIWANMsoft*     wan_topo,
             uint64_t        region_id);

    void   receivePacket(Packet& pkt) override;
    Route* getNextHop(Packet& pkt, BaseQueue* ingress_port) override;
    Route* getNextAvailableHop(Packet&            pkt,
                               BaseQueue*         ingress_port,
                               vector<FibEntry*>* available_hops);
    void   addAvailableHops(Packet& pkt, BaseQueue* ingress_port, AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsToRUp(Packet& pkt, BaseQueue* ingress_port, AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsLeafDown(Packet&              pkt,
                                  BaseQueue*           ingress_port,
                                  AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsLeafUp(Packet& pkt, BaseQueue* ingress_port, AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsSpineDown(Packet&              pkt,
                                   BaseQueue*           ingress_port,
                                   AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsSpineUp(Packet&              pkt,
                                 BaseQueue*           ingress_port,
                                 AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsRHDown(Packet& pkt, BaseQueue* ingress_port, AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsRHUp(Packet& pkt, BaseQueue* ingress_port, AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsRWADown(Packet&              pkt,
                                 BaseQueue*           ingress_port,
                                 AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsRWAUp(Packet& pkt, BaseQueue* ingress_port, AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsOWRDown(Packet&              pkt,
                                 BaseQueue*           ingress_port,
                                 AI_WAN_topology_idx& dst_idx);
    void addAvailableHopsOWRUp(Packet& pkt, BaseQueue* ingress_port, AI_WAN_topology_idx& dst_idx);

    bool is_dst_region(struct AI_WAN_topology_idx* dst_idx, SwitchInfo* switchinfo) {
        assert(dst_idx->region_idx != UINT32_MAX);
        return (dst_idx->region_idx == switchinfo->region_idx);
    }

    bool is_dst_dc(struct AI_WAN_topology_idx* dst_idx, SwitchInfo* switchinfo) {
        assert(dst_idx->region_idx != UINT32_MAX);
        assert(dst_idx->dc_idx != UINT32_MAX);
        return (dst_idx->region_idx == switchinfo->region_idx &&
                dst_idx->dc_idx == switchinfo->datacenter_idx);
    }

    bool is_dst_leafgroup(struct AI_WAN_topology_idx* dst_idx, SwitchInfo* switchinfo) {
        assert(dst_idx->region_idx != UINT32_MAX);
        assert(dst_idx->dc_idx != UINT32_MAX);
        assert(dst_idx->leaf_group_idx != UINT32_MAX);

        return (dst_idx->region_idx == switchinfo->region_idx &&
                dst_idx->dc_idx == switchinfo->datacenter_idx &&
                dst_idx->leaf_group_idx == switchinfo->leaf_group_idx);
    }

    bool is_dst_torgroup(struct AI_WAN_topology_idx* dst_idx, SwitchInfo* switchinfo) {
        assert(dst_idx->region_idx != UINT32_MAX);
        assert(dst_idx->dc_idx != UINT32_MAX);
        assert(dst_idx->leaf_group_idx != UINT32_MAX);
        assert(dst_idx->tor_group_idx != UINT32_MAX);

        return (dst_idx->region_idx == switchinfo->region_idx &&
                dst_idx->dc_idx == switchinfo->datacenter_idx &&
                dst_idx->leaf_group_idx == switchinfo->leaf_group_idx &&
                dst_idx->tor_group_idx == switchinfo->tor_group_idx);
    }

    uint32_t getType() override { return _type; }

    uint32_t adaptive_route(vector<FibEntry*>* ecmp_set, int8_t (*cmp)(FibEntry*, FibEntry*));
    uint32_t replace_worst_choice(vector<FibEntry*>* ecmp_set,
                                  int8_t (*cmp)(FibEntry*, FibEntry*),
                                  uint32_t my_choice);
    uint32_t adaptive_route_p2c(vector<FibEntry*>* ecmp_set, int8_t (*cmp)(FibEntry*, FibEntry*));

    static int8_t compare_pause(FibEntry* l, FibEntry* r);
    static int8_t compare_bandwidth(FibEntry* l, FibEntry* r);
    static int8_t compare_queuesize(FibEntry* l, FibEntry* r);
    static int8_t compare_pqb(FibEntry* l,
                              FibEntry* r);              // compare pause,queue, bw.
    static int8_t compare_pq(FibEntry* l, FibEntry* r);  // compare pause, queue
    static int8_t compare_pb(FibEntry* l,
                             FibEntry* r);  // compare pause, bandwidth
    static int8_t compare_qb(FibEntry* l,
                             FibEntry* r);  // compare pause, bandwidth

    static int8_t (*fn)(FibEntry*, FibEntry*);

    virtual void addHostPort(int addr, int flowid, PacketSink* transport) override;

    virtual void permute_paths(vector<FibEntry*>* uproutes);
    virtual void permute_paths_deterministic(vector<FibEntry*>* uproutes);

    static void set_strategy(routing_strategy s) {
        assert(_strategy == NIX);
        _strategy = s;
    }

    static void set_ar_fraction(uint16_t f) {
        assert(f >= 1);
        _ar_fraction = f;
    }

    static void set_precision_ts(int ts) { precision_ts = ts; }

    static routing_strategy _strategy;
    static uint16_t         _ar_fraction;
    static uint16_t         _ar_sticky;
    static simtime_picosec  _sticky_delta;
    static double           _ecn_threshold_fraction;
    static int              precision_ts;

    void set_hash_salt(uint32_t salt) { _hash_salt = salt; }

    int64_t get_receive_packet_counter() { return receive_packet_counter; }

    void reset_receive_packet_counter() { receive_packet_counter = 0; }

    double getECNPMin() { return _ECNPmin; }

    double getECNPMax() { return _ECNPmax; }

    void setECNPMin(double min) { _ECNPmin = min; }

    void setECNPMax(double max) { _ECNPmax = max; }

    void setECNKMin(uint32_t min_us) { _ECNKmin_us = min_us; }

    uint32_t getECNKMin() { return _ECNKmin_us; }

    void setECNKMax(uint32_t max_us) { _ECNKmax_us = max_us; }

    uint32_t getECNKMax() { return _ECNKmax_us; }

    void breadcrumb_once_per_flow(flowid_t flow_id);

private:
    node_type      _type;
    Pipe*          _pipe;
    AIDCMsoft*     dc_topo     = nullptr;
    AIRegionMsoft* region_topo = nullptr;
    AIWANMsoft*    wan_topo    = nullptr;
    int            dc_id;
    int            region_id;
    SwitchInfo     switch_info;

    vector<pair<simtime_picosec, uint64_t>> _list_sent;

    // CAREFUL: can't always have a single FIB for all up destinations when
    // there are failures!
    vector<FibEntry*>* _uproutes;

    unordered_map<uint32_t, FlowletInfo*> _flowlet_maps;
    uint32_t                              _crt_route;
    uint32_t                              _hash_salt;
    simtime_picosec                       _last_choice;

    unordered_map<Packet*, bool> _packets;

    int64_t receive_packet_counter = 0;

    uint32_t _ECNKmin_us;  // in terms of microseconds
    uint32_t _ECNKmax_us;  // in terms of microseconds
    double   _ECNPmin = 0;
    double   _ECNPmax = 1;

    // ======== Breadcrumb (once per flow) ========
    static void open_crumb_csv_if_needed();
    static std::ofstream                _crumb_csv;
    static bool                         _crumb_opened;
    static std::unordered_set<uint64_t> _seen_pair;

    static inline uint64_t make_key(uint64_t sid, uint64_t fid) {
        return (sid << 32) ^ fid;
    }
    void        breadcrumb_once_per_flow(uint64_t flow_id);
    // ============================================
};

#endif
