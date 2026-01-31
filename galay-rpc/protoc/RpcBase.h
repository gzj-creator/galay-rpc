/**
 * @file RpcBase.h
 * @brief RPC基础定义
 * @author galay-rpc
 * @version 1.0.0
 */

#ifndef GALAY_RPC_BASE_H
#define GALAY_RPC_BASE_H

#include <cstdint>
#include <string>
#include <string_view>

namespace galay::rpc
{

/**
 * @brief RPC消息类型
 */
enum class RpcMessageType : uint8_t {
    REQUEST = 0x01,      ///< 请求
    RESPONSE = 0x02,     ///< 响应
    HEARTBEAT = 0x03,    ///< 心跳
    ERROR = 0x04,        ///< 错误
};

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
