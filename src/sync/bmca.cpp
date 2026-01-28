/**
 * BMCA — Best Master Clock Algorithm (IEEE 1588 Section 9.3)
 *
 * Determines which node should be the grandmaster clock based on:
 * 1. Priority1 (lower is better)
 * 2. Clock class (lower is better)
 * 3. Clock accuracy (lower enum value is better)
 * 4. Offset scaled log variance (lower is better)
 * 5. Priority2 (lower is better)
 * 6. Clock identity (tiebreaker, lower is better)
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/sync/ptp_engine.h>
#include <cstring>

namespace soluna::sync {

bool PtpEngine::bmca_compare(
    const PtpAnnounceBody& a, const PtpPortIdentity& a_id,
    const PtpAnnounceBody& b, const PtpPortIdentity& b_id)
{
    // Step 1: Priority1
    if (a.grandmaster_priority1 != b.grandmaster_priority1)
        return a.grandmaster_priority1 < b.grandmaster_priority1;

    // Step 2: Clock class
    auto a_class = static_cast<uint8_t>(a.grandmaster_clock_quality.clock_class);
    auto b_class = static_cast<uint8_t>(b.grandmaster_clock_quality.clock_class);
    if (a_class != b_class)
        return a_class < b_class;

    // Step 3: Clock accuracy
    auto a_acc = static_cast<uint8_t>(a.grandmaster_clock_quality.clock_accuracy);
    auto b_acc = static_cast<uint8_t>(b.grandmaster_clock_quality.clock_accuracy);
    if (a_acc != b_acc)
        return a_acc < b_acc;

    // Step 4: Offset scaled log variance
    if (a.grandmaster_clock_quality.offset_scaled_log_variance !=
        b.grandmaster_clock_quality.offset_scaled_log_variance)
        return a.grandmaster_clock_quality.offset_scaled_log_variance <
               b.grandmaster_clock_quality.offset_scaled_log_variance;

    // Step 5: Priority2
    if (a.grandmaster_priority2 != b.grandmaster_priority2)
        return a.grandmaster_priority2 < b.grandmaster_priority2;

    // Step 6: Clock identity (tiebreaker)
    int cmp = std::memcmp(a.grandmaster_identity.data(),
                           b.grandmaster_identity.data(), 8);
    if (cmp != 0) return cmp < 0;

    // Step 7: Port number (final tiebreaker)
    return a_id.port_number < b_id.port_number;
}

} // namespace soluna::sync
