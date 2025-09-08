cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# -------------------------------
#      Project Configuration
# -------------------------------
project(htsim_project LANGUAGES CXX)

# Set C++ standard and compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Ensures GNU extensions are disabled

# Set compiler flags
add_compile_options(
    -Wall
    -g
    -Wsign-compare
    -Wuninitialized
    -fPIE
    -O3
)
# Optionally enable sanitizers
# -fsanitize=address -fno-omit-frame-pointer -fsanitize=undefined"

# Set SPDLOG_ACTIVE_LEVEL globally based on build type.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
else()
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()

# -------------------------------
#     Fetching Dependencies
# -------------------------------
# Fetch spdlog
include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.1
)
FetchContent_MakeAvailable(spdlog)

# Fetch GoogleTest
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)
enable_testing()

# Fetch nlohmann/json
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3 # Replace with the desired version
)
FetchContent_MakeAvailable(json)

# -------------------------------
#       Helper Functions
# -------------------------------
# Function to add a symlink creation step for an executable.
# Used to create a symlink in the source directory to the executable
# in the build directory for easier access and backward compatibility.
function(add_symlink_target EXECUTABLE TARGET_DIR)
    add_custom_command(
        TARGET ${EXECUTABLE}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove ${TARGET_DIR}/${EXECUTABLE} # Remove existing symlink
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            $<TARGET_FILE:${EXECUTABLE}>  # Path to the executable
            ${TARGET_DIR}/${EXECUTABLE}  # Path for the symlink
        COMMENT "Removing and recreating symlink for ${EXECUTABLE} in ${TARGET_DIR}"
    )
endfunction()

# -------------------------------
#          Libraries
# -------------------------------
# Define source files
set(SOURCE_FILES
    core/callback_pipe.cpp
    core/connection_matrix.cpp
    core/eventlist.cpp
    core/pipe.cpp
    core/route.cpp
    core/trigger.cpp
     # DCQCN exps
    dcqcn/cnppacket.cpp
    dcqcn/dcqcn.cpp
    dcqcn/dcqcn_logger.cpp
    dcqcn/roce.cpp
    dcqcn/rocepacket.cpp
    host/oversubscribed_cc.cpp
    host/pcie_model.cpp
    host/uec_nic.cpp
    host/uec_sink.cpp
    host/uec_src.cpp
    host/uec_src_port.cpp
    host/uec_sink_port.cpp
    host/uec_pull_pacer.cpp
    logging/data_collector.cpp
    logging/logfile.cpp
    logging/loggers.cpp
    logging/metric.cpp
    logging/uec_logger.cpp
    # MSFT WAN effort
    msft_ai/common.cpp
    msft_ai/config.cpp
    msft_ai/kecbr.cpp
    msft_ai/kec_cc.cpp
    msft_ai/1ecbr.cpp
    msft_ai/1ec_cc.cpp 
    msft_ai/cbr.cpp
    msft_ai/cbrpacket.cpp 
    msft_ai/AI_switch.cpp
    msft_ai/AI_DC_msoft.cpp
    msft_ai/AI_region_msoft.cpp
    msft_ai/AI_WAN_msoft.cpp
    # msft_ai/AI_simple_wan.cpp

    packet/eth_pause_packet.cpp
    packet/packet_flow.cpp
    packet/packet.cpp
    packet/uec_packet.cpp
    queue/aeolusqueue.cpp
    queue/compositeprioqueue.cpp
    queue/compositequeue.cpp
    queue/cpqueue.cpp
    queue/ecnprioqueue.cpp
    queue/ecnqueue.cpp
    queue/exoqueue.cpp
    queue/fairpullqueue.cpp
    queue/priopullqueue.cpp
    queue/prioqueue.cpp
    queue/qos_scheduler.cpp
    queue/queue_lossless_input.cpp
    queue/queue_lossless_output.cpp
    queue/queue_lossless.cpp
    queue/queue.cpp
    queue/randomqueue.cpp
    queue/shared_queue.cpp
    switch/fat_tree_switch.cpp
    switch/route_table.cpp
    switch/switch_buffer.cpp
    switch/switch.cpp
    topology/fat_tree_topology.cpp
    utils/helpers.cpp
)

# Create static library
add_library(htsim STATIC ${SOURCE_FILES})
target_include_directories(htsim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/datacenter
    ${CMAKE_CURRENT_SOURCE_DIR}/dcqcn
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/logging
    ${CMAKE_CURRENT_SOURCE_DIR}/msft_ai
    ${CMAKE_CURRENT_SOURCE_DIR}/packet
    ${CMAKE_CURRENT_SOURCE_DIR}/queue
    ${CMAKE_CURRENT_SOURCE_DIR}/switch
    ${CMAKE_CURRENT_SOURCE_DIR}/topology
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)
target_link_libraries(htsim PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog)

# -------------------------------
#         Executables
# -------------------------------
# Define executables and link with htsim library
add_executable(htsim_uec main_uec.cpp)
add_executable(msft_ai_wan_1ec_cc main_msft_ai_wan_1ec_cc.cpp)
add_executable(msft_ai_wan_kec_cc main_msft_ai_wan_kec_cc.cpp)
add_executable(msft_ai_wan_1ecbr main_msft_ai_wan_1ecbr.cpp)
add_executable(msft_ai_wan_kecbr main_msft_ai_wan_kecbr.cpp)
add_executable(msft_ai_wan_cbr main_msft_ai_wan_cbr.cpp)
add_executable(msft_ai_wan_dcqcn main_msft_ai_wan_dcqcn.cpp)
add_executable(msft_ai_wan_single_dcqcn main_msft_ai_wan_single_dcqcn.cpp)

# Linking Executables with Libraries
set (ALL_EXECUTABLES
    htsim_uec
    msft_ai_wan_1ec_cc
    msft_ai_wan_kec_cc
    msft_ai_wan_dcqcn
    msft_ai_wan_single_dcqcn
    msft_ai_wan_1ecbr
    msft_ai_wan_kecbr
    msft_ai_wan_cbr
)
foreach(EXECUTABLE ${ALL_EXECUTABLES})
    # Link each executable with the 'htsim' library defined in the parent directory.
    target_link_libraries(${EXECUTABLE} PRIVATE htsim)
    # Add a post-build command to explicitly remove and recreate the symlink for all previous executables.
    add_symlink_target(${EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# -------------------------------
#          Unit Tests
# -------------------------------
set(UNIT_TEST_FILES
    core/callback_pipe_test
    core/pipe_test
    core/route_test
    logging/metric_test
    switch/switch_buffer_test
    queue/qos_scheduler_test
    queue/shared_queue_test
    queue/compositequeue_test
    msft_ai/AI_region_msoft_test
    msft_ai/AI_WAN_msoft_test
)

foreach(UNIT_TEST_FILE ${UNIT_TEST_FILES})
    get_filename_component(TEST_NAME ${UNIT_TEST_FILE} NAME)
    add_executable(${TEST_NAME} ${UNIT_TEST_FILE}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE htsim gtest_main gmock_main)
    gtest_discover_tests(${TEST_NAME}
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )
endforeach()
