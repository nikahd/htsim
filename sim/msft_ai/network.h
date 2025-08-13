// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef NETWORK_H
#define NETWORK_H

enum RouteStrategy {
    NOT_SET,
    SINGLE_PATH,
    SCATTER_PERMUTE,
    SCATTER_RANDOM,
    PULL_BASED,
    SCATTER_ECMP,
    ECMP_FIB,
    ECMP_FIB_ECN,
    REACTIVE_ECN,
    ECMP_FIB2_ECN,
    ECMP_RANDOM2_ECN,
    ECMP_RANDOM_ECN,
    SIMPLE_SUBFLOW
};

#define MIX3(a, b, c)   \
    do {                \
        a -= b;         \
        a -= c;         \
        a ^= (c >> 13); \
        b -= c;         \
        b -= a;         \
        b ^= (a << 8);  \
        c -= a;         \
        c -= b;         \
        c ^= (b >> 13); \
        a -= b;         \
        a -= c;         \
        a ^= (c >> 12); \
        b -= c;         \
        b -= a;         \
        b ^= (a << 16); \
        c -= a;         \
        c -= b;         \
        c ^= (b >> 5);  \
        a -= b;         \
        a -= c;         \
        a ^= (c >> 3);  \
        b -= c;         \
        b -= a;         \
        b ^= (a << 10); \
        c -= a;         \
        c -= b;         \
        c ^= (b >> 15); \
    } while (/*CONSTCOND*/ 0)

static inline uint32_t mFreeBSDHash(uint32_t target1, uint32_t target2 = 0, uint32_t target3 = 0) {
    uint32_t a = 0x9e3779b9, b = 0x9e3779b9, c = 0;  // hask key

    b += target3;
    c += target2;
    a += target1;
    MIX3(a, b, c);
    return c;
}

#undef MIX3

#endif
