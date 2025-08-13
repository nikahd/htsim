// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef AI_SIMPLE_WAN_TOPO_H
#define AI_SIMPLE_WAN_TOPO_H

#include <map>
#include <ostream>
#include <tuple>

#include "AI_DC_msoft.h"
#include "AI_WAN_types.h"
#include "eventlist.h"
#include "logfile.h"
#include "network.h"
#include "pipe.h"
#include "queue.h"
#include "switch.h"
#include "topology.h"

class SimpleWAN : public AIDCMsoft {
public:
    SimpleWAN(unsigned int             n_region,
              unsigned int             n_core_per_region,
              unsigned int             n_tor_per_region,
              unsigned int             n_servers_per_tor,
              linkspeed_bps            intra_region_link_speed,
              linkspeed_bps            inter_region_link_speed,
              mem_b                    queuesize,
              QueueLoggerFactory*      logger_factory,
              EventList*               ev,
              simtime_picosec          srv_tor_hop_latency,
              simtime_picosec          tor_core_hop_latency,
              vector<simtime_picosec>& core_core_hop_latency_half,
              queue_type               sender_qt               = FAIR_PRIO,
              queue_type               tor_qt                  = LOSSLESS,
              queue_type               spine_qt                = LOSSLESS,
              queue_type               core_qt                 = COMP_NO_ECN,
              double                   switch_random_drop_prob = 0.0,
              bool                     use_link_down           = false,
              uint32_t                 hash_salt               = 1);

    ~SimpleWAN() {}

private:
    unsigned int n_region;
    unsigned int n_core_per_region;
    unsigned int n_tor_per_region;
    unsigned int n_servers_per_tor;

    linkspeed_bps intra_region_link_speed = speedFromMbps((double)100000);
    linkspeed_bps inter_region_link_speed = speedFromMbps((double)400000);

    simtime_picosec         srv_tor_hop_latency  = timeFromUs((uint32_t)5);
    simtime_picosec         tor_core_hop_latency = timeFromUs((uint32_t)5);
    vector<simtime_picosec> core_core_hop_latency_half;

    mem_b queue_size;

    QueueLoggerFactory* _logger_factory;
    EventList*          _eventlist;
    queue_type          _sender_qt;
    queue_type          _tor_qt;
    queue_type          _spine_qt;
    queue_type          _core_qt;
};

#endif  // AI_SIMPLE_WAN_TOPO_H