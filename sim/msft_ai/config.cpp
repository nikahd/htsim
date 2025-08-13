// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "config.h"

#include <math.h>

#include <iostream>

void initializeLoggingFolders() {
    // Setup Paths and initialize folders
    std::string           desiredRootDirectoryName = "";  // Change this to the desired folder name.
    std::filesystem::path rootPath                 = findRootPath(desiredRootDirectoryName);

    if (!rootPath.empty()) {
        std::filesystem::path dataPath = rootPath;
        PROJECT_ROOT_PATH              = dataPath;
        std::cout << "Project Path " << PROJECT_ROOT_PATH << std::endl;
    } else {
        std::cout << "Root directory '" << desiredRootDirectoryName << "' not found." << std::endl;
        abort();
    }

    // Add the FOLDER_NAME to the root path.
    printf("Folder Name is %s\n", FOLDER_NAME.c_str());
    fflush(stdout);
    PROJECT_ROOT_PATH /= FOLDER_NAME;

    if (!std::filesystem::exists(PROJECT_ROOT_PATH)) {
        if (!std::filesystem::create_directory(PROJECT_ROOT_PATH)) {
            std::cerr << "Failed to create project root directory: " << PROJECT_ROOT_PATH
                      << std::endl;
            abort();
        }
    }

    if (COLLECT_DATA) {
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/rtt/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/ecn/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/cwd/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/queue/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/sent/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/retrans/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/timeout/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/nack/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/bts/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/ls_to_us/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/us_to_cs/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/fasti/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/fastd/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/mediumi/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/ecn_rtt/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/qa_free/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/trimmed_rtt/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/case1/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/case2/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/case3/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/case4/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/sending_rate/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/ecn_rate/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/ecn_ewma/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/queue_phantom/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/status/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/out_bw_paced/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/out_bw/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/current_rtt_ewma/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/target_rtt_low/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/target_rtt_high/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/baremetal_latency/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/params/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/cmd/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/unacked/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/fct/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/flow_size/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/dual_congested/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/ecn_congested/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/rtt_congested/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/epoch_ended/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/cwnd_reduction_factor/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/qa_ended/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/retrans_cwnd_reduce/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/qa_rtt_triggerred/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/qa_bytes_acked_triggerred/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/bytes_acked/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/queue_overflow/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/rto_info/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/bytes_sent/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/bytes_rcvd/");
        std::filesystem::remove_all(PROJECT_ROOT_PATH / "output/agg_bytes_sent/");

        bool ret_val = std::filesystem::create_directory(PROJECT_ROOT_PATH / "output");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/rtt");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/ecn");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/cwd");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/queue");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/queue_phantom");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/ecn_ewma");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/sent");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/retrans");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/timeout");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/nack");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/bts");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/ls_to_us");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/us_to_cs");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/fastd");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/fasti");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/qa_free");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/mediumi");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/ecn_rtt");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/trimmed_rtt");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/case1");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/case2");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/case3");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/case4");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/sending_rate");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/ecn_rate");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/out_bw");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/out_bw_paced");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/status");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/current_rtt_ewma");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/target_rtt_low");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/target_rtt_high");
        ret_val &=
            std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/baremetal_latency");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/params");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/cmd");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/unacked");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/fct");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/flow_size");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/dual_congested");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/ecn_congested");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/rtt_congested");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/epoch_ended");
        ret_val &=
            std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/cwnd_reduction_factor");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/qa_ended");
        ret_val &=
            std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/retrans_cwnd_reduce");
        ret_val &=
            std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/qa_rtt_triggerred");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH /
                                                     "output/qa_bytes_acked_triggerred");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/bytes_acked");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/queue_overflow");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/rto_info");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/bytes_sent");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/bytes_rcvd");
        ret_val &= std::filesystem::create_directory(PROJECT_ROOT_PATH / "output/agg_bytes_sent");
    }
}

// Path
std::filesystem::path findRootPath(const std::string& rootDirectoryName) {
    std::filesystem::path currentPath = std::filesystem::current_path();  // Get the current working
                                                                          // directory.

    // Iterate up the directory hierarchy until we find a directory with the
    // desired name.
    while (!currentPath.empty() && !std::filesystem::exists(currentPath / rootDirectoryName)) {
        std::filesystem::path parent_path = currentPath.parent_path();
        if (currentPath.compare(parent_path) == 0) {
            currentPath.clear();
            break;
        }
        currentPath = parent_path;
    }

    if (!currentPath.empty()) {
        return currentPath / rootDirectoryName;  // Found the root directory with
                                                 // the desired name.
    } else {
        printf("Error");
        abort();
    }
}
