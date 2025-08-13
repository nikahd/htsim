// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

class ErasureCoding {
public:
    struct message_sack {
        uint32_t              msn;
        uint32_t              num_missing_stripes = 0;
        std::vector<uint32_t> missing_stripes;  // List of missing stripes
    };

    static const uint32_t BITMAP_SIZE_KEC        = 1024;
    static const uint32_t BITMAP_SIZE_1EC        = 1024 * 8;
    static const uint32_t MAX_MESSAGE_BOUNDARIES = 1024 * 8;

    ErasureCoding(unsigned int k, unsigned int m, unsigned int stripe_num, uint32_t num_entropies)
        : k(k), m(m), stripe_num(stripe_num) {
        if (k == 0 || m == 0 || k + m > MAX_BLOCKS) {
            throw std::invalid_argument("Invalid parameters for Erasure Coding");
        }

        message_size_pkts = stripe_num * (k + m);

        this->num_entropies = num_entropies;
        if (num_entropies < k + m) {
            throw std::invalid_argument("Number of entropies must be at least k + m");
        }
        entropy_list.resize(k + m);
        for (unsigned int i = 0; i < k + m; ++i)
            entropy_list[i] = i;
    }

    virtual ~ErasureCoding() {}

    unsigned int get_k() const { return k; }

    unsigned int get_m() const { return m; }

    unsigned int get_chunk_size() { return k + m; }

    unsigned int get_stripe_num() const { return stripe_num; }

    unsigned int get_message_size_pkts() const { return message_size_pkts; }

    std::vector<uint32_t> entropy_list;
    uint32_t              num_entropies = 0;  // Number of entropies available

    static const unsigned int MAX_BLOCKS = 255;  // Maximum number of blocks (k + m)

private:
    unsigned int k;  // Number of data blocks
    unsigned int m;  // Number of parity blocks

    unsigned int stripe_num        = 0;
    unsigned int message_size_pkts = 0;  // size of each message in packet MTUs
};