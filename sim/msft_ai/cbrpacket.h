// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef CBR_PACKET
#define CBR_PACKET

#include <list>

#include "packet.h"
#include "packet_db.h"
#include "packet_flow.h"

#define CBR_ACKSIZE 64

class CbrPacket : public Packet {
public:
    static PacketDB<CbrPacket> _packetdb;
    static Packet::PktPriority _priority;

    enum packet_type { DATA, PARITY, SACK, NACK, METRIC_ACK };

    inline static CbrPacket* newpkt(PacketFlow& flow,
                                    route_t&    route,
                                    int         id,
                                    int         size,
                                    PktPriority priority = PRIO_LO,
                                    uint32_t    dst      = UINT32_MAX) {
        CbrPacket* p = _packetdb.allocPacket();
        p->set_dst(dst);
        p->set_pathid(0);
        p->set_route(flow, route, size, id);
        p->_direction = NONE;
        setPriority(priority);
        return p;
    }

    inline static CbrPacket* newSACKpkt(PacketFlow&  flow,
                                        const Route& route,
                                        int          id,
                                        int          size,
                                        PktPriority  priority = PRIO_HI,
                                        uint32_t     dst      = UINT32_MAX) {
        CbrPacket* p = _packetdb.allocPacket();
        p->set_dst(dst);
        p->set_pathid(0);
        p->set_route(flow, route, size, id);
        p->_direction = NONE;
        setPriority(priority);
        p->missing_stripes.clear();
        p->num_missing_stripes = 0;
        p->pkt_type            = SACK;
        return p;
    }

    inline static CbrPacket* newNACKpkt(PacketFlow&  flow,
                                        const Route& route,
                                        int          id,
                                        int          size,
                                        PktPriority  priority = PRIO_HI,
                                        uint32_t     dst      = UINT32_MAX) {
        CbrPacket* p = _packetdb.allocPacket();
        p->set_dst(dst);
        p->set_pathid(0);
        p->set_route(flow, route, size, id);
        p->_direction = NONE;
        setPriority(priority);
        p->missing_stripes.clear();
        p->num_missing_stripes = 0;
        p->pkt_type            = NACK;
        return p;
    }

    inline static CbrPacket* newMetricACKpkt(PacketFlow&  flow,
                                             const Route& route,
                                             int          id,
                                             int          size,
                                             PktPriority  priority = PRIO_HI,
                                             uint32_t     dst      = UINT32_MAX) {
        CbrPacket* p = _packetdb.allocPacket();
        p->set_dst(dst);
        p->set_pathid(0);
        p->set_route(flow, route, size, id);
        p->_direction = NONE;
        setPriority(priority);
        p->missing_stripes.clear();
        p->num_missing_stripes = 0;
        p->pkt_type            = METRIC_ACK;
        p->num_bytes_acked     = 0;
        return p;
    }

    virtual inline void strip_payload(uint16_t trim_size = CBR_ACKSIZE) {
        Packet::strip_payload(trim_size);
        _size = CBR_ACKSIZE;
    };

    virtual Packet::PktPriority priority() const { return _priority; }

    inline static void setPriority(Packet::PktPriority priority) {
        switch (priority) {
            case Packet::PRIO_LO:
            case Packet::PRIO_MID:
            case Packet::PRIO_HI:
            case Packet::PRIO_NONE:
                _priority = priority;
                break;
            default:
                throw std::runtime_error("CbrPacket: setPriority: invalid packet priority");
        }
    }

    void free() { _packetdb.freePacket(this); }

    virtual ~CbrPacket(){};

    packetid_t id() const { return _id; }

    inline simtime_picosec ts() const { return _ts; }

    inline void set_ts(simtime_picosec ts) { _ts = ts; }

    inline void set_entropy_to_replace(uint32_t entropy_to_replace) {
        this->entropy_to_replace = entropy_to_replace;
    }

    inline void set_one_way_latencies(const vector<simtime_picosec>& latencies) {
        for (size_t i = 0; i < one_way_latencies.size(); ++i) {
            this->one_way_latencies[i] = latencies[i];
        }
    }

    inline void set_num_bytes_acked(uint64_t num_bytes_acked) {
        this->num_bytes_acked = num_bytes_acked;
    }

    inline void set_bytes_ecn_marked(uint64_t bytes) { this->bytes_ecn_marked = bytes; }

    simtime_picosec _ts = 0;

    // Erasure coding
    bool             last;
    uint32_t         msn;
    uint32_t         stripe_offset;
    uint32_t         pkt_offset;
    uint32_t         last_psn_in_msg;  // Last packet sequence number in the message
    enum packet_type pkt_type;
    uint32_t         original_msn;            // Original message sequence number for retransmission
    uint32_t         original_stripe_offset;  // Original stripe offset for retransmission

    // SACK fields
    uint32_t              num_missing_stripes = 0;
    std::vector<uint32_t> missing_stripes;  // List of missing stripes

    // METRIC_ACK fields
    uint32_t                entropy_to_replace = -1;
    vector<simtime_picosec> one_way_latencies;
    uint64_t                num_bytes_acked  = 0;
    uint32_t                bytes_ecn_marked = 0;
};

#endif
