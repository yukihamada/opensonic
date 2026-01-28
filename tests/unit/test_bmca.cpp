#include <soluna/sync/ptp_engine.h>
#include <gtest/gtest.h>

using namespace soluna::sync;

static PtpAnnounceBody make_announce(uint8_t p1, PtpClockClass cls,
    PtpClockAccuracy acc, uint16_t var, uint8_t p2, uint8_t id_byte)
{
    PtpAnnounceBody body;
    body.grandmaster_priority1 = p1;
    body.grandmaster_clock_quality.clock_class = cls;
    body.grandmaster_clock_quality.clock_accuracy = acc;
    body.grandmaster_clock_quality.offset_scaled_log_variance = var;
    body.grandmaster_priority2 = p2;
    body.grandmaster_identity.fill(id_byte);
    return body;
}

static PtpPortIdentity make_pid(uint8_t id_byte, uint16_t port = 1) {
    PtpPortIdentity pid;
    pid.clock_id.fill(id_byte);
    pid.port_number = port;
    return pid;
}

TEST(BMCA, Priority1Wins) {
    auto a = make_announce(100, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0xFFFF, 128, 0x01);
    auto b = make_announce(200, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0xFFFF, 128, 0x02);

    EXPECT_TRUE(PtpEngine::bmca_compare(a, make_pid(0x01), b, make_pid(0x02)));
    EXPECT_FALSE(PtpEngine::bmca_compare(b, make_pid(0x02), a, make_pid(0x01)));
}

TEST(BMCA, ClockClassWins) {
    auto a = make_announce(128, PtpClockClass::PrimarySyncRef, PtpClockAccuracy::Unknown,
                           0xFFFF, 128, 0x01);
    auto b = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0xFFFF, 128, 0x02);

    EXPECT_TRUE(PtpEngine::bmca_compare(a, make_pid(0x01), b, make_pid(0x02)));
}

TEST(BMCA, AccuracyWins) {
    auto a = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Within1us,
                           0xFFFF, 128, 0x01);
    auto b = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Within1ms,
                           0xFFFF, 128, 0x02);

    EXPECT_TRUE(PtpEngine::bmca_compare(a, make_pid(0x01), b, make_pid(0x02)));
}

TEST(BMCA, VarianceWins) {
    auto a = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0x1000, 128, 0x01);
    auto b = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0xFFFF, 128, 0x02);

    EXPECT_TRUE(PtpEngine::bmca_compare(a, make_pid(0x01), b, make_pid(0x02)));
}

TEST(BMCA, Priority2Wins) {
    auto a = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0xFFFF, 100, 0x01);
    auto b = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0xFFFF, 200, 0x02);

    EXPECT_TRUE(PtpEngine::bmca_compare(a, make_pid(0x01), b, make_pid(0x02)));
}

TEST(BMCA, ClockIdentityTiebreaker) {
    auto a = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0xFFFF, 128, 0x01);
    auto b = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0xFFFF, 128, 0x02);

    EXPECT_TRUE(PtpEngine::bmca_compare(a, make_pid(0x01), b, make_pid(0x02)));
    EXPECT_FALSE(PtpEngine::bmca_compare(b, make_pid(0x02), a, make_pid(0x01)));
}

TEST(BMCA, PortNumberTiebreaker) {
    auto a = make_announce(128, PtpClockClass::Default, PtpClockAccuracy::Unknown,
                           0xFFFF, 128, 0x01);
    auto b = a; // identical

    EXPECT_TRUE(PtpEngine::bmca_compare(a, make_pid(0x01, 1), b, make_pid(0x01, 2)));
    EXPECT_FALSE(PtpEngine::bmca_compare(b, make_pid(0x01, 2), a, make_pid(0x01, 1)));
}
