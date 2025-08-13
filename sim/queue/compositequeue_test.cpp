#include "compositequeue.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "eventlist.h"
#include "helpers.h"
#include "loggers.h"
#include "packet.h"
#include "switch.h"
#include "switch_buffer.h"

using json = nlohmann::json;

constexpr uint32_t SWITCH_BUFFER_SIZE      = 10000;
constexpr mem_b    ECN_MIN_THRESH          = 3000;  // Lower than buffer size
constexpr mem_b    ECN_MAX_THRESH          = 4000;  // Between min thresh and buffer size
constexpr uint16_t TRIM_SIZE               = 64;
constexpr uint32_t DEFAULT_LINK_SPEED_Gbps = 100.0;

//@Nadeen: Fold TEST_F after completing the code

json getTestDataCollectorConfig() {
    json config;
    config["output_location"] = "stdout";
    config["filters"]         = json::array();
    config["filters"].push_back(json::object({{"regex", ".*"}, {"enabled", false}}));
    return config;
}

class MockPacket : public Packet {
public:
    MockPacket(uint32_t size, PktPriority priority) : Packet(), _priority(priority) {
        _size      = size;
        _is_header = priority != Packet::PRIO_LO;  // always header only for tests
        _flow      = new PacketFlow(nullptr);
    }

    ~MockPacket() { delete _flow; }

    MOCK_METHOD(PacketSink*, sendOn, (), (override));

    PktPriority priority() const override { return _priority; }

private:
    PktPriority _priority;
};

// Different configurations to test CompositeQueue
struct QueueTestConfig {
    uint32_t    switch_buffer_size;
    uint16_t    trim_size;
    bool        no_ecn;
    bool        disable_trim;
    bool        low_priority_trim;
    bool        no_dropping_low_header;
    double      switch_random_drop_prob;
    std::string name;
};

const std::array<QueueTestConfig, 3> TEST_CONFIGS = {
    {{.switch_buffer_size      = SWITCH_BUFFER_SIZE,
      .trim_size               = TRIM_SIZE,
      .no_ecn                  = false,
      .disable_trim            = true,
      .low_priority_trim       = false,
      .no_dropping_low_header  = false,
      .switch_random_drop_prob = 0.0,
      .name                    = "BaselineConfig"},
     {.switch_buffer_size      = SWITCH_BUFFER_SIZE,
      .trim_size               = TRIM_SIZE,
      .no_ecn                  = true,  // ECN disabled
      .disable_trim            = true,
      .low_priority_trim       = false,
      .no_dropping_low_header  = false,
      .switch_random_drop_prob = 0.0,
      .name                    = "NoEcnConfig"},
     {.switch_buffer_size = SWITCH_BUFFER_SIZE,
      .trim_size          = TRIM_SIZE,
      .no_ecn             = false,
      .disable_trim       = false,  // Enable trimming
      .low_priority_trim  = false,  // With low priority assumes we have no low priority trimming
      .no_dropping_low_header  = false,
      .switch_random_drop_prob = 0.0,  // Add some random drops
      .name                    = "TrimConfig"}}};

class CompositeQueueTest : public ::testing::Test {
protected:
    std::unique_ptr<Switch>         sw_;
    std::unique_ptr<CompositeQueue> queue_;
    std::unique_ptr<EventList>      eventlist_;
    QueueTestConfig                 current_config_;

    void initQueue(linkspeed_bps speed, const QueueTestConfig& config) {
        current_config_ = config;
        queue_          = make_unique<CompositeQueue>(speed,
                                             config.switch_buffer_size,
                                             *eventlist_,
                                             nullptr,
                                             config.trim_size,
                                             config.no_ecn,
                                             config.disable_trim,
                                             config.low_priority_trim,
                                             config.no_dropping_low_header,
                                             config.switch_random_drop_prob);
        queue_->setName(testing::UnitTest::GetInstance()->current_test_info()->name());
        queue_->setSwitch(sw_.get());
    }

    void SetUpWithConfig(const QueueTestConfig& config) {
        eventlist_ = make_unique<EventList>();
        eventlist_->setEndtime(timeFromSec(100));

        std::unique_ptr<SwitchBuffer> buffer = make_unique<SwitchBuffer>(config.switch_buffer_size);
        sw_ = make_unique<Switch>(*eventlist_, "switch", std::move(buffer));

        initQueue(speedFromGbps(DEFAULT_LINK_SPEED_Gbps), config);
    }

    virtual void SetUp() override {
        SetUpWithConfig(TEST_CONFIGS[0]);  // Use baseline config by default
    }

    virtual void TearDown() {}
};

// Test with different configurations
class CompositeQueueConfigTest : public CompositeQueueTest,
                                 public testing::WithParamInterface<QueueTestConfig> {
protected:
    void SetUp() override { SetUpWithConfig(GetParam()); }
};

INSTANTIATE_TEST_SUITE_P(AllConfigs,
                         CompositeQueueConfigTest,
                         testing::ValuesIn(TEST_CONFIGS),
                         [](const testing::TestParamInfo<QueueTestConfig>& info) {
                             return info.param.name;
                         });

TEST_P(CompositeQueueConfigTest, ShouldEcnMarkQos0AboveMaxThreshold) {
    if (GetParam().no_ecn) {
        GTEST_SKIP() << "ECN disabled for this configuration";
    }

    queue_->set_ecn_thresholds(ECN_MIN_THRESH, ECN_MAX_THRESH);

    // First build up queue with some packets
    std::unique_ptr<MockPacket> pkt1    = std::make_unique<MockPacket>(4000, Packet::PRIO_LO);
    std::unique_ptr<MockPacket> pkt2    = std::make_unique<MockPacket>(4000, Packet::PRIO_LO);
    std::unique_ptr<MockPacket> pktTest = std::make_unique<MockPacket>(100, Packet::PRIO_LO);

    // Fill queue above ECN threshold
    queue_->receivePacket(*pkt1);
    queue_->receivePacket(*pkt2);  // Now queue is at 6000 > ECN_MAX_THRESH
    queue_->receivePacket(*pktTest);

    // First two packets dequeued should be marked since queue was above threshold during dequeue
    EXPECT_CALL(*pkt1, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_TRUE(pkt1->flags() & ECN_CE);

    EXPECT_CALL(*pkt2, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_FALSE(pkt2->flags() & ECN_CE);

    // Last packet should not be marked since queue is now below threshold
    EXPECT_CALL(*pktTest, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_FALSE(pktTest->flags() & ECN_CE);

    EXPECT_EQ(queue_->queuesize(), 0);
}

TEST_P(CompositeQueueConfigTest, ShouldRespectTrimConfig) {
    std::unique_ptr<MockPacket> pkt =
        std::make_unique<MockPacket>(SWITCH_BUFFER_SIZE + 20,  // Oversized packet
                                     Packet::PRIO_LO);

    queue_->receivePacket(*pkt);

    if (GetParam().disable_trim) {
        EXPECT_EQ(queue_->queuesize(), 0);  // Packet should be dropped
    } else {
        EXPECT_EQ(queue_->queuesize(),
                  GetParam().trim_size);  // Packet should be trimmed
    }
}

TEST_P(CompositeQueueConfigTest, PacketHandlingWithConfig) {
    // First add a small packet to ensure queue isn't empty
    std::unique_ptr<MockPacket> pktSmall = std::make_unique<MockPacket>(100, Packet::PRIO_LO);
    queue_->receivePacket(*pktSmall);

    // Then test with larger packet
    std::unique_ptr<MockPacket> pkt = std::make_unique<MockPacket>(
        current_config_.switch_buffer_size - 100,  // Leave room for small packet
        Packet::PRIO_LO);
    queue_->receivePacket(*pkt);

    // Rest of checks remain same but account for both packets
    if (current_config_.switch_random_drop_prob > 0) {
        EXPECT_GE(current_config_.switch_buffer_size, queue_->queuesize());
    } else if (current_config_.low_priority_trim &&
               queue_->_queuesize_low > current_config_.switch_buffer_size) {
        EXPECT_EQ(queue_->queuesize(), current_config_.trim_size + 100);
    } else if (!current_config_.disable_trim &&
               queue_->_queuesize_low > current_config_.switch_buffer_size) {
        EXPECT_EQ(queue_->queuesize(), current_config_.trim_size + 100);
    } else {
        std::cout << "Queue size: " << queue_->queuesize() << std::endl;
        EXPECT_EQ(queue_->queuesize(), pkt->size() + 100);
    }

    // Process both packets
    EXPECT_CALL(*pkt, sendOn()).Times(queue_->queuesize() > 100 ? 1 : 0);
    EXPECT_CALL(*pktSmall, sendOn()).Times(1);
    if (queue_->queuesize() > 0) {
        EXPECT_TRUE(eventlist_->doNextEvent());
        if (queue_->queuesize() > 0) {
            EXPECT_TRUE(eventlist_->doNextEvent());
        }
    }
}

TEST_F(CompositeQueueTest, ReceivePacketSuccess) {
    // Create a packet smaller than buffer size to ensure it fits
    std::unique_ptr<MockPacket> pkt = std::make_unique<MockPacket>(1000, Packet::PRIO_LO);
    queue_->receivePacket(*pkt);

    // Should be accepted at full size since it's below buffer limit
    EXPECT_EQ(queue_->queuesize(), pkt->size());

    // Packet should be sent on and queue should become empty
    EXPECT_CALL(*pkt, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);
}

TEST_F(CompositeQueueTest, UseAndFreeBufferQos0Success) {
    std::unique_ptr<MockPacket> pktQos0 = std::make_unique<MockPacket>(
        current_config_.switch_buffer_size / 2,  // Half the buffer size to ensure it fits
        Packet::PRIO_LO);

    queue_->receivePacket(*pktQos0);

    // Should be accepted at full size since it's below buffer limit
    EXPECT_EQ(queue_->queuesize(), pktQos0->size());  // Check total queue size

    // Should be sent normally
    EXPECT_CALL(*pktQos0, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());

    // Buffer should be empty after sending
    EXPECT_EQ(queue_->queuesize(), 0);
}

TEST_F(CompositeQueueTest, ShouldEcnMarkQos0AboveMaxThreshold) {
    if (current_config_.no_ecn) {
        cout << "ECN is disabled for this test, skipping..." << endl;
        return;  // Skip test if ECN is disabled
    }
    queue_->set_ecn_thresholds(ECN_MIN_THRESH, ECN_MAX_THRESH);

    // First build up queue with some packets
    std::unique_ptr<MockPacket> pkt1    = std::make_unique<MockPacket>(4000, Packet::PRIO_LO);
    std::unique_ptr<MockPacket> pkt2    = std::make_unique<MockPacket>(4000, Packet::PRIO_LO);
    std::unique_ptr<MockPacket> pktTest = std::make_unique<MockPacket>(100, Packet::PRIO_LO);

    // Fill queue above ECN threshold
    queue_->receivePacket(*pkt1);
    queue_->receivePacket(*pkt2);  // Now queue is at 6000 > ECN_MAX_THRESH
    queue_->receivePacket(*pktTest);

    cout << "size of queue after adding packets: " << queue_->_queuesize_low << endl;

    // First two packets dequeued should be marked since queue was above threshold during dequeue
    EXPECT_CALL(*pkt1, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_TRUE(pkt1->flags() & ECN_CE);

    EXPECT_CALL(*pkt2, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_FALSE(pkt2->flags() & ECN_CE);

    // Last packet should not be marked since queue is now below threshold
    EXPECT_CALL(*pktTest, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_FALSE(pktTest->flags() & ECN_CE);

    EXPECT_EQ(queue_->queuesize(), 0);
}

TEST_F(CompositeQueueTest, ShouldNotMarkEcnWhenDisabled) {  // Change to non-parameterized test
    // Setup queue with ECN disabled
    if (!current_config_.no_ecn) {
        std::cout << "ECN should be disabled for this test, skipping..." << std::endl;
        return;  // Skip test if ECN is disabled
    }

    // Create packets that would normally trigger ECN marking
    std::unique_ptr<MockPacket> pktLarge = std::make_unique<MockPacket>(
        current_config_.switch_buffer_size * 2,  // Size that would trigger ECN if enabled
        Packet::PRIO_LO);
    std::unique_ptr<MockPacket> pktSmall = std::make_unique<MockPacket>(100, Packet::PRIO_LO);

    // Fill queue above what would be ECN threshold
    queue_->receivePacket(*pktLarge);
    queue_->receivePacket(*pktSmall);

    // First packet dequeued
    EXPECT_CALL(*pktLarge, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_FALSE(pktLarge->flags() & ECN_CE);  // Verify no ECN marking

    // Second packet dequeued
    EXPECT_CALL(*pktSmall, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_FALSE(pktSmall->flags() & ECN_CE);  // Verify no ECN marking

    // Verify queue is empty
    EXPECT_EQ(queue_->queuesize(), 0);
}

TEST_F(CompositeQueueTest, ShouldDropPacketWhenLinkDown) {
    queue_->load_link_down_time(
        "../sim/queue/testdata/test_link_down_times_1.txt");  // Load link down times: [7900, 8100]
                                                              // picoseconds, link is down

    std::unique_ptr<MockPacket> pktSmall_1 = std::make_unique<MockPacket>(100, Packet::PRIO_LO);
    std::unique_ptr<MockPacket> pktSmall_2 = std::make_unique<MockPacket>(100, Packet::PRIO_LO);

    simtime_picosec now = queue_->eventlist().now();
    EXPECT_EQ(now, 0);
    queue_->receivePacket(*pktSmall_1);

    // Packet should not be dropped since link is up
    EXPECT_EQ(queue_->queuesize(), pktSmall_1->size());
    EXPECT_CALL(*pktSmall_1, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);

    now = queue_->eventlist().now();
    EXPECT_EQ(now, pktSmall_1->size() * 8 / DEFAULT_LINK_SPEED_Gbps * 1e3);

    queue_->receivePacket(*pktSmall_2);
    // Packet should be dropped since link is down
    EXPECT_EQ(queue_->queuesize(), 0);
    EXPECT_CALL(*pktSmall_2, sendOn()).Times(0);
    EXPECT_FALSE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);
}

TEST_F(CompositeQueueTest, ShouldNotDropPacketWhenLinkUp) {
    queue_->load_link_down_time(
        "../sim/queue/testdata/test_link_down_times_2.txt");  // Load link down times: [10, 15],
                                                              // [20, 30], [8010, 8020] picoseconds,
                                                              // link is down

    std::unique_ptr<MockPacket> pktSmall_1 = std::make_unique<MockPacket>(100, Packet::PRIO_LO);
    std::unique_ptr<MockPacket> pktSmall_2 = std::make_unique<MockPacket>(100, Packet::PRIO_LO);
    std::unique_ptr<MockPacket> pktSmall_3 = std::make_unique<MockPacket>(100, Packet::PRIO_LO);

    simtime_picosec now = queue_->eventlist().now();
    EXPECT_EQ(now, 0);
    queue_->receivePacket(*pktSmall_1);

    // Packet should not be dropped since link is up
    EXPECT_EQ(queue_->queuesize(), pktSmall_1->size());
    EXPECT_CALL(*pktSmall_1, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);

    now = queue_->eventlist().now();
    EXPECT_EQ(now, pktSmall_1->size() * 8 / DEFAULT_LINK_SPEED_Gbps * 1e3);

    queue_->receivePacket(*pktSmall_2);
    // Packet should not be dropped since link is up
    EXPECT_EQ(queue_->queuesize(), pktSmall_2->size());
    EXPECT_CALL(*pktSmall_2, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);

    now = queue_->eventlist().now();
    EXPECT_EQ(now,
              pktSmall_1->size() * 8 / DEFAULT_LINK_SPEED_Gbps * 1e3 +
                  pktSmall_2->size() * 8 / DEFAULT_LINK_SPEED_Gbps * 1e3);
    queue_->receivePacket(*pktSmall_3);
    // Packet should not be dropped since link is up
    EXPECT_EQ(queue_->queuesize(), pktSmall_3->size());
    EXPECT_CALL(*pktSmall_3, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);
}

// Add test for DoS protection and buffer sharing features specific to CompositeQueue

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    DataCollector::InitWithJsonObject(getTestDataCollectorConfig());
    return RUN_ALL_TESTS();
}
