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
#include "galay-kernel/kernel/Coroutine.h"
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
using RpcMethodHandler = std::function<kernel::Coroutine(RpcContext&)>;

/**
 * @brief RPC服务基类
 */
class RpcService {
public:
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
        auto it = m_methods.find(method);
        if (it != m_methods.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /**
     * @brief 获取所有方法名
     */
    std::vector<std::string> methodNames() const {
        std::vector<std::string> names;
        names.reserve(m_methods.size());
        for (const auto& [name, _] : m_methods) {
            names.push_back(name);
        }
        return names;
    }

protected:
    /**
     * @brief 注册方法
     * @param name 方法名
     * @param handler 处理函数
     */
    void registerMethod(std::string_view name, RpcMethodHandler handler) {
        m_methods[std::string(name)] = std::move(handler);
    }

    /**
     * @brief 注册成员方法
     * @tparam T 服务类型
     * @param name 方法名
     * @param method 成员函数指针
     */
    template<typename T>
    void registerMethod(std::string_view name, kernel::Coroutine (T::*method)(RpcContext&)) {
        m_methods[std::string(name)] = [this, method](RpcContext& ctx) -> kernel::Coroutine {
            return (static_cast<T*>(this)->*method)(ctx);
        };
    }

private:
    std::string m_name;
    std::unordered_map<std::string, RpcMethodHandler> m_methods;
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

private:
    RpcRequest& m_request;
    RpcResponse& m_response;
};

} // namespace galay::rpc

#endif // GALAY_RPC_SERVICE_H
