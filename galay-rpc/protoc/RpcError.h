/**
 * @file RpcError.h
 * @brief RPC错误处理
 * @author galay-rpc
 * @version 1.0.0
 */

#ifndef GALAY_RPC_ERROR_H
#define GALAY_RPC_ERROR_H

#include "RpcBase.h"
#include <string>

namespace galay::rpc
{

/**
 * @brief RPC错误类
 */
class RpcError {
public:
    RpcError() = default;

    RpcError(RpcErrorCode code)
        : m_code(code)
        , m_message(rpcErrorCodeToString(code)) {}

    RpcError(RpcErrorCode code, std::string_view message)
        : m_code(code)
        , m_message(message) {}

    RpcErrorCode code() const { return m_code; }
    const std::string& message() const { return m_message; }

    bool isOk() const { return m_code == RpcErrorCode::OK; }

    explicit operator bool() const { return !isOk(); }

    std::string toString() const {
        return std::string(rpcErrorCodeToString(m_code)) + ": " + m_message;
    }

private:
    RpcErrorCode m_code = RpcErrorCode::OK;
    std::string m_message;
};

} // namespace galay::rpc

#endif // GALAY_RPC_ERROR_H
