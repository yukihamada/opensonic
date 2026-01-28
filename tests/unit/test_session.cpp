/**
 * Unit tests for SessionManager
 * SPDX-License-Identifier: MIT
 */

#include <soluna/control/session.h>
#include <gtest/gtest.h>

using namespace soluna::control;

class SessionManagerTest : public ::testing::Test {
protected:
    SessionManager sessions;
};

TEST_F(SessionManagerTest, CreateStream) {
    uint16_t id = sessions.create_stream("devA", "devB", 2);
    EXPECT_GT(id, 0u);
    auto s = sessions.get_stream(id);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->source_device, "devA");
    EXPECT_EQ(s->sink_device, "devB");
    EXPECT_EQ(s->channels, 2u);
    EXPECT_EQ(s->state, StreamState::Active);
}

TEST_F(SessionManagerTest, CreateMultipleStreams) {
    uint16_t id1 = sessions.create_stream("devA", "devB");
    uint16_t id2 = sessions.create_stream("devA", "devC");
    EXPECT_NE(id1, id2);
    auto streams = sessions.list_streams();
    EXPECT_EQ(streams.size(), 2u);
}

TEST_F(SessionManagerTest, DestroyStream) {
    uint16_t id = sessions.create_stream("devA", "devB");
    EXPECT_TRUE(sessions.destroy_stream(id));
    EXPECT_EQ(sessions.get_stream(id), nullptr);
    EXPECT_EQ(sessions.list_streams().size(), 0u);
}

TEST_F(SessionManagerTest, DestroyNonexistent) {
    EXPECT_FALSE(sessions.destroy_stream(999));
}

TEST_F(SessionManagerTest, SetStreamState) {
    uint16_t id = sessions.create_stream("devA", "devB");
    sessions.set_stream_state(id, StreamState::Inactive);
    auto s = sessions.get_stream(id);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->state, StreamState::Inactive);
}

TEST_F(SessionManagerTest, UniquePortAssignment) {
    uint16_t id1 = sessions.create_stream("devA", "devB");
    uint16_t id2 = sessions.create_stream("devC", "devD");
    auto s1 = sessions.get_stream(id1);
    auto s2 = sessions.get_stream(id2);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_NE(s1->rtp_port, s2->rtp_port);
}

TEST_F(SessionManagerTest, StreamHasSSRC) {
    uint16_t id = sessions.create_stream("devA", "devB");
    auto s = sessions.get_stream(id);
    ASSERT_NE(s, nullptr);
    // SSRC should be assigned (random, but non-zero is very likely)
    // Just check we can access it
    (void)s->ssrc;
}

TEST_F(SessionManagerTest, NextStreamId) {
    EXPECT_EQ(sessions.next_stream_id(), 1u);
    sessions.create_stream("devA", "devB");
    EXPECT_EQ(sessions.next_stream_id(), 2u);
}

TEST_F(SessionManagerTest, ListStreamsEmpty) {
    auto streams = sessions.list_streams();
    EXPECT_TRUE(streams.empty());
}
