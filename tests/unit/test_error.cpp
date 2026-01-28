/**
 * Soluna — Error System Tests
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <soluna/core/error.h>

using namespace soluna;

TEST(ErrorTest, ErrorCodeCategories) {
    EXPECT_STREQ(error_category(ErrorCode::OK), "OK");
    EXPECT_STREQ(error_category(ErrorCode::InvalidArgument), "General");
    EXPECT_STREQ(error_category(ErrorCode::AudioDeviceNotFound), "Audio");
    EXPECT_STREQ(error_category(ErrorCode::SocketError), "Network");
    EXPECT_STREQ(error_category(ErrorCode::AuthenticationFailed), "Security");
    EXPECT_STREQ(error_category(ErrorCode::ConfigParseError), "Config");
    EXPECT_STREQ(error_category(ErrorCode::ProtocolError), "Protocol");
    EXPECT_STREQ(error_category(ErrorCode::CodecNotFound), "Codec");
}

TEST(ErrorTest, ErrorCodeNames) {
    EXPECT_STREQ(error_name(ErrorCode::OK), "OK");
    EXPECT_STREQ(error_name(ErrorCode::InvalidArgument), "InvalidArgument");
    EXPECT_STREQ(error_name(ErrorCode::AudioDeviceNotFound), "AudioDeviceNotFound");
    EXPECT_STREQ(error_name(ErrorCode::SocketBindFailed), "SocketBindFailed");
    EXPECT_STREQ(error_name(ErrorCode::AuthenticationFailed), "AuthenticationFailed");
    EXPECT_STREQ(error_name(ErrorCode::ConfigParseError), "ConfigParseError");
}

TEST(ErrorTest, DefaultError) {
    Error err;
    EXPECT_TRUE(err.ok());
    EXPECT_EQ(err.code(), ErrorCode::OK);
    EXPECT_FALSE(static_cast<bool>(err));
}

TEST(ErrorTest, ErrorWithCode) {
    Error err(ErrorCode::AudioDeviceNotFound);
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.code(), ErrorCode::AudioDeviceNotFound);
    EXPECT_TRUE(err.message().empty());
}

TEST(ErrorTest, ErrorWithMessage) {
    Error err(ErrorCode::SocketError, "Connection refused");
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.code(), ErrorCode::SocketError);
    EXPECT_EQ(err.message(), "Connection refused");
}

TEST(ErrorTest, ErrorWithContext) {
    Error err(ErrorCode::ConfigParseError, "Invalid syntax", "line 42");
    EXPECT_EQ(err.context(), "line 42");

    std::string str = err.to_string();
    EXPECT_NE(str.find("Config"), std::string::npos);
    EXPECT_NE(str.find("ConfigParseError"), std::string::npos);
    EXPECT_NE(str.find("Invalid syntax"), std::string::npos);
    EXPECT_NE(str.find("line 42"), std::string::npos);
}

TEST(ErrorTest, ErrorWithContextChain) {
    Error err(ErrorCode::AudioDeviceOpenFailed, "Permission denied");
    err.with_context("device: hw:0");
    err.with_context("opening input");

    std::string ctx = err.context();
    EXPECT_NE(ctx.find("device: hw:0"), std::string::npos);
    EXPECT_NE(ctx.find("opening input"), std::string::npos);
}

TEST(ResultTest, ResultWithValue) {
    Result<int> result(42);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 42);
}

TEST(ResultTest, ResultWithError) {
    Result<int> result(ErrorCode::InvalidArgument);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ResultTest, ResultWithErrorMessage) {
    Result<std::string> result(ErrorCode::NotFound, "File not found");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(result.error().message(), "File not found");
}

TEST(ResultTest, ResultValueOr) {
    Result<int> ok(42);
    Result<int> err(ErrorCode::Unknown);

    EXPECT_EQ(ok.value_or(0), 42);
    EXPECT_EQ(err.value_or(0), 0);
}

TEST(ResultTest, ResultMap) {
    Result<int> ok(10);
    auto doubled = ok.map([](int x) { return x * 2; });
    EXPECT_TRUE(doubled.ok());
    EXPECT_EQ(doubled.value(), 20);

    Result<int> err(ErrorCode::Unknown);
    auto mapped_err = err.map([](int x) { return x * 2; });
    EXPECT_FALSE(mapped_err.ok());
}

TEST(ResultTest, ResultAndThen) {
    auto parse_positive = [](int x) -> Result<int> {
        if (x > 0) return x;
        return ErrorCode::InvalidArgument;
    };

    Result<int> ok(10);
    auto chained = ok.and_then(parse_positive);
    EXPECT_TRUE(chained.ok());
    EXPECT_EQ(chained.value(), 10);

    Result<int> negative(-5);
    auto chained_fail = negative.and_then(parse_positive);
    EXPECT_FALSE(chained_fail.ok());
}

TEST(ResultTest, VoidResult) {
    VoidResult success = VoidResult::success();
    EXPECT_TRUE(success.ok());

    VoidResult failure(ErrorCode::PermissionDenied);
    EXPECT_FALSE(failure.ok());
    EXPECT_EQ(failure.error().code(), ErrorCode::PermissionDenied);
}

TEST(ResultTest, ResultWithString) {
    Result<std::string> result(std::string("hello"));
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "hello");
}

TEST(ResultTest, ResultWithVector) {
    std::vector<int> vec = {1, 2, 3};
    Result<std::vector<int>> result(vec);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value().size(), 3u);
}

// Test SOLUNA_TRY macro functionality (simulated without actual macro due to GTest)
Result<int> divide(int a, int b) {
    if (b == 0) {
        return Error(ErrorCode::InvalidArgument, "Division by zero");
    }
    return a / b;
}

Result<int> compute(int a, int b) {
    auto div_result = divide(a, b);
    if (!div_result.ok()) {
        return div_result.error();
    }
    return div_result.value() + 10;
}

TEST(ResultTest, ErrorPropagation) {
    auto ok = compute(20, 2);
    EXPECT_TRUE(ok.ok());
    EXPECT_EQ(ok.value(), 20);  // 20/2 + 10

    auto err = compute(10, 0);
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.error().code(), ErrorCode::InvalidArgument);
}
