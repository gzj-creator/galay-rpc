/**
 * @file RpcBase.h
 * @brief RPC基础定义
 * @author galay-rpc
 * @version 1.0.0
 */

#ifndef GALAY_RPC_BASE_H
#define GALAY_RPC_BASE_H

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <bit>

namespace galay::rpc
{

/**
 * @brief 跨平台字节序转换（使用编译器内置，GCC/Clang/AppleClang 均支持）
 */
inline uint32_t rpcBswap32(uint32_t v) {
    if constexpr (std::endian::native == std::endian::big) {
        return v;
    } else {
        return __builtin_bswap32(v);
    }
}

inline uint16_t rpcBswap16(uint16_t v) {
    if constexpr (std::endian::native == std::endian::big) {
        return v;
    } else {
        return __builtin_bswap16(v);
    }
}

// 网络字节序 <-> 主机字节序（对称操作）
inline uint32_t rpcHtonl(uint32_t host) { return rpcBswap32(host); }
inline uint32_t rpcNtohl(uint32_t net)  { return rpcBswap32(net); }
inline uint16_t rpcHtons(uint16_t host) { return rpcBswap16(host); }
inline uint16_t rpcNtohs(uint16_t net)  { return rpcBswap16(net); }

/**
 * @brief RPC消息类型
 */
enum class RpcMessageType : uint8_t {
    REQUEST = 0x01,          ///< 普通请求
    RESPONSE = 0x02,         ///< 普通响应
    HEARTBEAT = 0x03,        ///< 心跳
    ERROR = 0x04,            ///< 错误
    STREAM_INIT = 0x10,      ///< 流初始化请求
    STREAM_INIT_ACK = 0x11,  ///< 流初始化确认
    STREAM_DATA = 0x12,      ///< 流数据
    STREAM_END = 0x13,       ///< 流结束
    STREAM_CANCEL = 0x14,    ///< 流取消
};

/**
 * @brief RPC调用模式
 */
enum class RpcCallMode : uint8_t {
    UNARY = 0,            ///< 一元调用（1 req -> 1 resp）
    CLIENT_STREAMING = 1, ///< 客户端流（N req-frame -> 1 resp）
    SERVER_STREAMING = 2, ///< 服务端流（1 req -> N resp-frame）
    BIDI_STREAMING = 3,   ///< 双向流（N req-frame <-> N resp-frame）
};

/**
 * @brief 头部 flags 布局
 *
 * bit[0..1]: RpcCallMode
 * bit[2]:    END_STREAM
 */
constexpr uint8_t RPC_FLAG_MODE_MASK = 0x03;
constexpr uint8_t RPC_FLAG_END_STREAM = 0x04;

inline uint8_t rpcEncodeFlags(RpcCallMode mode, bool end_stream) {
    uint8_t flags = static_cast<uint8_t>(mode) & RPC_FLAG_MODE_MASK;
    if (end_stream) {
        flags |= RPC_FLAG_END_STREAM;
    }
    return flags;
}

inline RpcCallMode rpcDecodeCallMode(uint8_t flags) {
    return static_cast<RpcCallMode>(flags & RPC_FLAG_MODE_MASK);
}

inline bool rpcIsEndStream(uint8_t flags) {
    return (flags & RPC_FLAG_END_STREAM) != 0;
}

inline uint8_t rpcSetEndStreamFlag(uint8_t flags, bool end_stream) {
    if (end_stream) {
        return static_cast<uint8_t>(flags | RPC_FLAG_END_STREAM);
    }
    return static_cast<uint8_t>(flags & static_cast<uint8_t>(~RPC_FLAG_END_STREAM));
}

/**
 * @brief RPC错误码
 */
enum class RpcErrorCode : uint16_t {
    OK = 0,                      ///< 成功
    UNKNOWN_ERROR = 1,           ///< 未知错误
    SERVICE_NOT_FOUND = 2,       ///< 服务未找到
    METHOD_NOT_FOUND = 3,        ///< 方法未找到
    INVALID_REQUEST = 4,         ///< 无效请求
    INVALID_RESPONSE = 5,        ///< 无效响应
    REQUEST_TIMEOUT = 6,         ///< 超时
    CONNECTION_CLOSED = 7,       ///< 连接关闭
    SERIALIZATION_ERROR = 8,     ///< 序列化错误
    DESERIALIZATION_ERROR = 9,   ///< 反序列化错误
    INTERNAL_ERROR = 10,         ///< 内部错误
};

/**
 * @brief 获取错误码描述
 */
inline const char* rpcErrorCodeToString(RpcErrorCode code) {
    switch (code) {
        case RpcErrorCode::OK: return "OK";
        case RpcErrorCode::UNKNOWN_ERROR: return "Unknown error";
        case RpcErrorCode::SERVICE_NOT_FOUND: return "Service not found";
        case RpcErrorCode::METHOD_NOT_FOUND: return "Method not found";
        case RpcErrorCode::INVALID_REQUEST: return "Invalid request";
        case RpcErrorCode::INVALID_RESPONSE: return "Invalid response";
        case RpcErrorCode::REQUEST_TIMEOUT: return "Request timeout";
        case RpcErrorCode::CONNECTION_CLOSED: return "Connection closed";
        case RpcErrorCode::SERIALIZATION_ERROR: return "Serialization error";
        case RpcErrorCode::DESERIALIZATION_ERROR: return "Deserialization error";
        case RpcErrorCode::INTERNAL_ERROR: return "Internal error";
        default: return "Unknown";
    }
}

/**
 * @brief RPC协议魔数
 */
constexpr uint32_t RPC_MAGIC = 0x47525043;  // "GRPC" in hex

/**
 * @brief RPC协议版本
 */
constexpr uint8_t RPC_VERSION = 0x01;

/**
 * @brief RPC消息头大小
 */
constexpr size_t RPC_HEADER_SIZE = 16;

/**
 * @brief RPC最大消息体大小 (16MB)
 */
constexpr size_t RPC_MAX_BODY_SIZE = 16 * 1024 * 1024;

} // namespace galay::rpc

#endif // GALAY_RPC_BASE_H
