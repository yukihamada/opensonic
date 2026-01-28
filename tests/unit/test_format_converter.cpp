#include <soluna/pipeline/pipeline.h>
#include <gtest/gtest.h>
#include <vector>
#include <cmath>

using namespace soluna::pipeline;

TEST(FormatConverter, FloatToS24Roundtrip) {
    std::vector<float> input = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.001f, -0.001f};
    std::vector<int32_t> s24(input.size());
    std::vector<float> output(input.size());

    float_to_s24(input.data(), s24.data(), input.size());
    s24_to_float(s24.data(), output.data(), input.size());

    for (size_t i = 0; i < input.size(); i++) {
        EXPECT_NEAR(output[i], input[i], 1.0f / 8388607.0f)
            << "Mismatch at index " << i;
    }
}

TEST(FormatConverter, Clamping) {
    float input[] = {2.0f, -2.0f, 100.0f};
    int32_t s24[3];

    float_to_s24(input, s24, 3);

    // Should be clamped to ±1.0
    EXPECT_EQ(s24[0], 8388607);
    EXPECT_EQ(s24[1], -8388607);
    EXPECT_EQ(s24[2], 8388607);
}

TEST(FormatConverter, Zero) {
    float input = 0.0f;
    int32_t s24;
    float output;

    float_to_s24(&input, &s24, 1);
    EXPECT_EQ(s24, 0);

    s24_to_float(&s24, &output, 1);
    EXPECT_FLOAT_EQ(output, 0.0f);
}

TEST(FormatConverter, SignExtension) {
    // Negative value stored in 24-bit
    int32_t s24 = static_cast<int32_t>(0x00FF0000); // large positive in 24-bit → negative
    float output;

    s24_to_float(&s24, &output, 1);
    EXPECT_LT(output, 0.0f); // should be negative after sign extension
}
