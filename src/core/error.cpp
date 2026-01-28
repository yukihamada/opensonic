/**
 * Soluna — Error implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <soluna/core/error.h>
#include <sstream>

namespace soluna {

const char* error_name(ErrorCode code) {
    switch (code) {
        // General
        case ErrorCode::OK: return "OK";
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::NotImplemented: return "NotImplemented";
        case ErrorCode::OutOfMemory: return "OutOfMemory";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::PermissionDenied: return "PermissionDenied";

        // Audio
        case ErrorCode::AudioDeviceNotFound: return "AudioDeviceNotFound";
        case ErrorCode::AudioDeviceOpenFailed: return "AudioDeviceOpenFailed";
        case ErrorCode::AudioDeviceBusy: return "AudioDeviceBusy";
        case ErrorCode::AudioFormatNotSupported: return "AudioFormatNotSupported";
        case ErrorCode::AudioBufferUnderrun: return "AudioBufferUnderrun";
        case ErrorCode::AudioBufferOverrun: return "AudioBufferOverrun";
        case ErrorCode::AudioDriverError: return "AudioDriverError";

        // Network
        case ErrorCode::SocketError: return "SocketError";
        case ErrorCode::SocketBindFailed: return "SocketBindFailed";
        case ErrorCode::SocketConnectFailed: return "SocketConnectFailed";
        case ErrorCode::SocketSendFailed: return "SocketSendFailed";
        case ErrorCode::SocketRecvFailed: return "SocketRecvFailed";
        case ErrorCode::AddressInvalid: return "AddressInvalid";
        case ErrorCode::MulticastJoinFailed: return "MulticastJoinFailed";
        case ErrorCode::NetworkUnreachable: return "NetworkUnreachable";
        case ErrorCode::ConnectionRefused: return "ConnectionRefused";
        case ErrorCode::ConnectionReset: return "ConnectionReset";

        // Security
        case ErrorCode::AuthenticationFailed: return "AuthenticationFailed";
        case ErrorCode::AuthenticationRequired: return "AuthenticationRequired";
        case ErrorCode::TokenExpired: return "TokenExpired";
        case ErrorCode::TokenInvalid: return "TokenInvalid";
        case ErrorCode::AccessDenied: return "AccessDenied";
        case ErrorCode::CertificateError: return "CertificateError";
        case ErrorCode::EncryptionError: return "EncryptionError";

        // Config
        case ErrorCode::ConfigParseError: return "ConfigParseError";
        case ErrorCode::ConfigValidationError: return "ConfigValidationError";
        case ErrorCode::ConfigFileNotFound: return "ConfigFileNotFound";
        case ErrorCode::ConfigWriteError: return "ConfigWriteError";
        case ErrorCode::ConfigKeyNotFound: return "ConfigKeyNotFound";
        case ErrorCode::ConfigTypeMismatch: return "ConfigTypeMismatch";

        // Protocol
        case ErrorCode::ProtocolError: return "ProtocolError";
        case ErrorCode::ProtocolVersionMismatch: return "ProtocolVersionMismatch";
        case ErrorCode::PacketMalformed: return "PacketMalformed";
        case ErrorCode::PacketTooLarge: return "PacketTooLarge";
        case ErrorCode::SequenceError: return "SequenceError";
        case ErrorCode::SyncError: return "SyncError";

        // Codec
        case ErrorCode::CodecNotFound: return "CodecNotFound";
        case ErrorCode::CodecInitFailed: return "CodecInitFailed";
        case ErrorCode::CodecEncodeFailed: return "CodecEncodeFailed";
        case ErrorCode::CodecDecodeFailed: return "CodecDecodeFailed";
        case ErrorCode::CodecUnsupportedFormat: return "CodecUnsupportedFormat";
    }
    return "Unknown";
}

std::string Error::to_string() const {
    if (code_ == ErrorCode::OK) {
        return "OK";
    }

    std::ostringstream ss;
    ss << "[" << error_category(code_) << "::" << error_name(code_) << "]";

    if (!message_.empty()) {
        ss << " " << message_;
    }

    if (!context_.empty()) {
        ss << " (" << context_ << ")";
    }

    return ss.str();
}

} // namespace soluna
