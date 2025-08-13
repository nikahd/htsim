// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef MTCP_SUB_CTRL_H
#define MTCP_SUB_CTRL_H

#include <math.h>

#include <list>

#include "event_source.h"
#include "eventlist.h"
#include "helpers.h"
#include "mtcp.h"
#include "packet.h"
#include "packet_flow.h"
#include "tcp.h"
#include "types.h"

class MultipathTcpSubCtrl : public MultipathTcpSrc {
public:
    MultipathTcpSubCtrl(char                cc_type,
                        EventList&          ev,
                        MultipathTcpLogger* logger,
                        double              epsilon = 0.1);

    // run as normal. Every timestep check to see if a subflow needs adding, deleting, etc.
    void doNextEvent();
};

#endif
