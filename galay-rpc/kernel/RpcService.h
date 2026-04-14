/**
 * @file RpcService.h
 * @brief RPC服务定义
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供RPC服务的基类和方法注册机制。
 *
 * @example
 * @code
 * class EchoService : public RpcService {
 * public:
 *     EchoService() : RpcService("EchoService") {
 *         registerMethod("echo", &EchoService::echo);
 *     }
 *
 *     Coroutine echo(RpcContext& ctx) {
 *         auto& req = ctx.request();
 *         ctx.response().payload(req.payload().data(), req.payload().size());
 *         co_return;
 *     }
 * };
 * @endcode
 */

#ifndef GALAY_RPC_SERVICE_H
#define GALAY_RPC_SERVICE_H

#include "galay-rpc/protoc/RpcMessage.h"
#include "galay-rpc/protoc/RpcError.h"
#include "RpcStream.h"
#include <array>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

namespace galay::rpc
{

class RpcContext;

/**
 * @brief RPC方法处理函数类型
 */
using RpcMethodHandler = std::function<Coroutine(RpcContext&)>;
using RpcStreamHandler = std::function<Coroutine(RpcStream&)>;

/**
 * @brief RPC服务基类
 */
class RpcService {
public:
    static constexpr size_t kStreamRpcModeCount = 3;

    /**
     * @brief 构造函数
     * @param name 服务名称
     */
    explicit RpcService(std::string_view name)
        : m_name(name) {}

    virtual ~RpcService() = default;

    /**
     * @brief 获取服务名称
     */
    const std::string& name() const { return m_name; }

    /**
     * @brief 查找方法处理器
     * @param method 方法名
     * @return 方法处理器，未找到返回nullptr
     */
    RpcMethodHandler* findMethod(const std::string& method) {
        auto it = m_unary_methods.find(method);
        if (it != m_unary_methods.end()) {
            return &it->second;
        }
        return nullptr;
    }

    RpcMethodHandler* findMethod(const std::string& method, RpcCallMode mode) {
        if (mode == RpcCallMode::UNARY) {
            return findMethod(method);
        }

        auto it = m_stream_methods.find(method);
        if (it == m_stream_methods.end()) {
            return nullptr;
        }

        const size_t mode_idx = streamModeIndex(mode);
        if (!it->second.registered[mode_idx]) {
            return nullptr;
        }
        return &it->second.handlers[mode_idx];
    }

    RpcStreamHandler* findStreamMethod(const std::string& method) {
        auto it = m_stream_session_methods.find(method);
        if (it != m_stream_session_methods.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /**
     * @brief 获取所有方法名
     */
    std::vector<std::string> methodNames() const {
        std::vector<std::string> names;
        names.reserve(m_unary_methods.size() + m_stream_methods.size());
        for (const auto& [name, _] : m_unary_methods) {
            names.push_back(name);
        }
        for (const auto& [name, _] : m_stream_methods) {
            if (m_unary_methods.find(name) == m_unary_methods.end()) {
                names.push_back(name);
            }
        }
        for (const auto& [name, _] : m_stream_session_methods) {
            if (m_unary_methods.find(name) == m_unary_methods.end() &&
                m_stream_methods.find(name) == m_stream_methods.end()) {
                names.push_back(name);
            }
        }
        return names;
    }

protected:
    /**
     * @brief 注册一元方法（兼容旧接口）
     */
    void registerMethod(std::string_view name, RpcMethodHandler handler) {
        registerUnaryMethod(name, std::move(handler));
    }

    /**
     * @brief 注册一元成员方法（兼容旧接口）
     * @tparam T 服务类型
     * @param name 方法名
     * @param method 成员函数指针
     */
    template<typename T>
    void registerMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerUnaryMethod(name, method);
    }

    void registerUnaryMethod(std::string_view name, RpcMethodHandler handler) {
        registerMethodByMode(name, RpcCallMode::UNARY, std::move(handler));
    }

    void registerClientStreamingMethod(std::string_view name, RpcMethodHandler handler) {
        registerMethodByMode(name, RpcCallMode::CLIENT_STREAMING, std::move(handler));
    }

    void registerServerStreamingMethod(std::string_view name, RpcMethodHandler handler) {
        registerMethodByMode(name, RpcCallMode::SERVER_STREAMING, std::move(handler));
    }

    void registerBidiStreamingMethod(std::string_view name, RpcMethodHandler handler) {
        registerMethodByMode(name, RpcCallMode::BIDI_STREAMING, std::move(handler));
    }

    template<typename T>
    void registerUnaryMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerMemberMethod(name, RpcCallMode::UNARY, method);
    }

    template<typename T>
    void registerClientStreamingMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerMemberMethod(name, RpcCallMode::CLIENT_STREAMING, method);
    }

    template<typename T>
    void registerServerStreamingMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerMemberMethod(name, RpcCallMode::SERVER_STREAMING, method);
    }

    template<typename T>
    void registerBidiStreamingMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerMemberMethod(name, RpcCallMode::BIDI_STREAMING, method);
    }

    void registerStreamMethod(std::string_view name, RpcStreamHandler handler) {
        m_stream_session_methods[std::string(name)] = std::move(handler);
    }

    template<typename T>
    void registerStreamMethod(std::string_view name, Coroutine (T::*method)(RpcStream&)) {
        m_stream_session_methods[std::string(name)] =
            [this, method](RpcStream& stream) -> Coroutine {
                return (static_cast<T*>(this)->*method)(stream);
            };
    }

private:
    struct RpcMethodSlots {
        std::array<RpcMethodHandler, kStreamRpcModeCount> handlers{};
        std::array<bool, kStreamRpcModeCount> registered{false, false, false};
    };

    static size_t streamModeIndex(RpcCallMode mode) {
        switch (mode) {
            case RpcCallMode::CLIENT_STREAMING:
                return 0;
            case RpcCallMode::SERVER_STREAMING:
                return 1;
            case RpcCallMode::BIDI_STREAMING:
                return 2;
            default:
                return 0;
        }
    }

    void registerMethodByMode(std::string_view name, RpcCallMode mode, RpcMethodHandler handler) {
        const std::string method_name(name);
        if (mode == RpcCallMode::UNARY) {
            m_unary_methods[method_name] = std::move(handler);
            return;
        }

        auto& slots = m_stream_methods[method_name];
        const size_t mode_idx = streamModeIndex(mode);
        slots.handlers[mode_idx] = std::move(handler);
        slots.registered[mode_idx] = true;
    }

    template<typename T>
    void registerMemberMethod(std::string_view name,
                              RpcCallMode mode,
                              Coroutine (T::*method)(RpcContext&)) {
        registerMethodByMode(name,
                             mode,
                             [this, method](RpcContext& ctx) -> Coroutine {
                                 return (static_cast<T*>(this)->*method)(ctx);
                             });
    }

private:
    std::string m_name;
    std::unordered_map<std::string, RpcMethodHandler> m_unary_methods;
    std::unordered_map<std::string, RpcMethodSlots> m_stream_methods;
    std::unordered_map<std::string, RpcStreamHandler> m_stream_session_methods;
};

/**
 * @brief RPC上下文
 *
 * @details 封装请求和响应，提供给服务方法使用。
 */
class RpcContext {
public:
    RpcContext(RpcRequest& request, RpcResponse& response)
        : m_request(request)
        , m_response(response) {}

    /**
     * @brief 获取请求
     */
    RpcRequest& request() { return m_request; }
    const RpcRequest& request() const { return m_request; }

    /**
     * @brief 获取响应
     */
    RpcResponse& response() { return m_response; }
    const RpcResponse& response() const { return m_response; }

    /**
     * @brief 设置错误
     */
    void setError(RpcErrorCode code) {
        m_response.errorCode(code);
    }

    /**
     * @brief 设置响应数据
     */
    void setPayload(const char* data, size_t len) {
        m_response.payload(data, len);
    }

    void setPayload(const std::string& data) {
        m_response.payload(data.data(), data.size());
    }

    void setPayload(std::vector<char>&& data) {
        m_response.payload(std::move(data));
    }

    // 借用模式：仅传递payload视图，不复制数据。
    // 需确保view引用的数据在响应发送完成前有效。
    void setPayload(const RpcPayloadView& view) {
        m_response.payloadView(view);
    }

private:
    RpcRequest& m_request;
    RpcResponse& m_response;
};

} // namespace galay::rpc

#endif // GALAY_RPC_SERVICE_H
