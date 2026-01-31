/**
 * @file ServiceDiscovery.h
 * @brief 服务发现模块
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供服务发现的抽象接口，使用C++20 concept约束。
 * 支持本地服务发现和未来的etcd等注册中心。
 */

#ifndef GALAY_RPC_SERVICE_DISCOVERY_H
#define GALAY_RPC_SERVICE_DISCOVERY_H

#include "RpcService.h"
#include "galay-kernel/concurrency/AsyncMutex.h"
#include "galay-kernel/kernel/Coroutine.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <expected>
#include <concepts>
#include <functional>

namespace galay::rpc
{

/**
 * @brief 服务端点信息
 */
struct ServiceEndpoint {
    std::string host;           ///< 主机地址
    uint16_t port;              ///< 端口
    std::string service_name;   ///< 服务名
    std::string instance_id;    ///< 实例ID
    uint32_t weight = 100;      ///< 权重（用于负载均衡）

    std::string address() const {
        return host + ":" + std::to_string(port);
    }
};

/**
 * @brief 服务发现错误
 */
struct DiscoveryError {
    enum Code {
        OK = 0,
        NOT_FOUND,
        CONNECTION_ERROR,
        LOCK_TIMEOUT,
        INTERNAL_ERROR
    };

    Code code = OK;
    std::string message;

    DiscoveryError() = default;
    DiscoveryError(Code c, std::string msg = "") : code(c), message(std::move(msg)) {}

    bool isOk() const { return code == OK; }
    explicit operator bool() const { return !isOk(); }
};

/**
 * @brief 服务变更事件类型
 */
enum class ServiceEventType {
    ADDED,      ///< 服务添加
    REMOVED,    ///< 服务移除
    UPDATED     ///< 服务更新
};

/**
 * @brief 服务变更事件
 */
struct ServiceEvent {
    ServiceEventType type;
    ServiceEndpoint endpoint;
};

/**
 * @brief 服务变更回调
 */
using ServiceWatchCallback = std::function<void(const ServiceEvent&)>;

/**
 * @brief 服务注册中心 Concept
 *
 * @details 定义服务注册中心必须实现的接口。
 * 支持本地注册、etcd、consul等多种实现。
 */
template<typename T>
concept ServiceRegistry = requires(T registry,
                                   const std::string& service_name,
                                   const ServiceEndpoint& endpoint,
                                   ServiceWatchCallback callback) {
    // 注册服务
    { registry.registerService(endpoint) } -> std::same_as<std::expected<void, DiscoveryError>>;

    // 注销服务
    { registry.deregisterService(endpoint) } -> std::same_as<std::expected<void, DiscoveryError>>;

    // 发现服务（获取所有实例）
    { registry.discoverService(service_name) } -> std::same_as<std::expected<std::vector<ServiceEndpoint>, DiscoveryError>>;

    // 监听服务变更
    { registry.watchService(service_name, callback) } -> std::same_as<std::expected<void, DiscoveryError>>;

    // 取消监听
    { registry.unwatchService(service_name) } -> std::same_as<void>;
};

/**
 * @brief 服务选择器 Concept
 *
 * @details 定义负载均衡策略必须实现的接口。
 */
template<typename T>
concept ServiceSelector = requires(T selector,
                                   const std::vector<ServiceEndpoint>& endpoints) {
    // 选择一个服务实例
    { selector.select(endpoints) } -> std::same_as<const ServiceEndpoint*>;

    // 更新服务列表
    { selector.update(endpoints) } -> std::same_as<void>;
};

/**
 * @brief 本地服务注册中心（非线程安全）
 *
 * @details 简单的内存服务注册，用于单线程环境。
 * 如需多线程/协程环境，请使用 AsyncLocalServiceRegistry。
 *
 * @note 非线程安全，仅适用于单线程环境
 */
class LocalServiceRegistry {
public:
    LocalServiceRegistry() = default;
    ~LocalServiceRegistry() = default;

    /**
     * @brief 注册服务
     */
    std::expected<void, DiscoveryError> registerService(const ServiceEndpoint& endpoint) {
        m_services[endpoint.service_name].push_back(endpoint);

        // 触发回调
        if (auto it = m_watchers.find(endpoint.service_name); it != m_watchers.end()) {
            ServiceEvent event{ServiceEventType::ADDED, endpoint};
            for (auto& callback : it->second) {
                callback(event);
            }
        }

        return {};
    }

    /**
     * @brief 注销服务
     */
    std::expected<void, DiscoveryError> deregisterService(const ServiceEndpoint& endpoint) {
        auto it = m_services.find(endpoint.service_name);
        if (it == m_services.end()) {
            return std::unexpected(DiscoveryError(DiscoveryError::NOT_FOUND, "Service not found"));
        }

        auto& endpoints = it->second;
        auto ep_it = std::find_if(endpoints.begin(), endpoints.end(),
            [&](const ServiceEndpoint& ep) {
                return ep.instance_id == endpoint.instance_id;
            });

        if (ep_it == endpoints.end()) {
            return std::unexpected(DiscoveryError(DiscoveryError::NOT_FOUND, "Instance not found"));
        }

        ServiceEndpoint removed = *ep_it;
        endpoints.erase(ep_it);

        // 触发回调
        if (auto wit = m_watchers.find(endpoint.service_name); wit != m_watchers.end()) {
            ServiceEvent event{ServiceEventType::REMOVED, removed};
            for (auto& callback : wit->second) {
                callback(event);
            }
        }

        return {};
    }

    /**
     * @brief 发现服务
     */
    std::expected<std::vector<ServiceEndpoint>, DiscoveryError> discoverService(const std::string& service_name) {
        auto it = m_services.find(service_name);
        if (it == m_services.end()) {
            return std::vector<ServiceEndpoint>{};
        }

        return it->second;
    }

    /**
     * @brief 监听服务变更
     */
    std::expected<void, DiscoveryError> watchService(const std::string& service_name, ServiceWatchCallback callback) {
        m_watchers[service_name].push_back(std::move(callback));
        return {};
    }

    /**
     * @brief 取消监听
     */
    void unwatchService(const std::string& service_name) {
        m_watchers.erase(service_name);
    }

private:
    std::unordered_map<std::string, std::vector<ServiceEndpoint>> m_services;
    std::unordered_map<std::string, std::vector<ServiceWatchCallback>> m_watchers;
};

// 验证LocalServiceRegistry满足ServiceRegistry concept
static_assert(ServiceRegistry<LocalServiceRegistry>, "LocalServiceRegistry must satisfy ServiceRegistry concept");

/**
 * @brief 异步服务注册中心 Concept
 *
 * @details 定义异步服务注册中心必须实现的接口，用于协程环境。
 * 所有方法返回Awaitable引用，支持co_await，减少等待体创建开销。
 */
template<typename T, typename RegisterAwaitable, typename DeregisterAwaitable,
         typename DiscoverAwaitable, typename WatchAwaitable, typename UnwatchAwaitable>
concept AsyncServiceRegistry = requires(T registry,
                                        const std::string& service_name,
                                        const ServiceEndpoint& endpoint,
                                        ServiceWatchCallback callback) {
    // 异步注册服务
    { registry.registerServiceAsync(endpoint) } -> std::same_as<RegisterAwaitable&>;

    // 异步注销服务
    { registry.deregisterServiceAsync(endpoint) } -> std::same_as<DeregisterAwaitable&>;

    // 异步发现服务
    { registry.discoverServiceAsync(service_name) } -> std::same_as<DiscoverAwaitable&>;

    // 异步监听服务变更
    { registry.watchServiceAsync(service_name, callback) } -> std::same_as<WatchAwaitable&>;

    // 异步取消监听
    { registry.unwatchServiceAsync(service_name) } -> std::same_as<UnwatchAwaitable&>;
};

// 前向声明
class AsyncLocalServiceRegistry;

/**
 * @brief 服务注册等待体基类
 */
class ServiceRegistryAwaitableBase {
public:
    virtual ~ServiceRegistryAwaitableBase() = default;

    bool await_ready() const noexcept { return false; }

    DiscoveryError lastError() const { return m_error; }

    void reset() {
        m_state = State::AcquireLock;
        m_error = DiscoveryError();
        m_lock_awaitable.reset();
    }

protected:
    enum class State {
        AcquireLock,
        Execute,
        Done
    };

    State m_state = State::AcquireLock;
    DiscoveryError m_error;
    std::optional<kernel::AsyncMutexAwaitable> m_lock_awaitable;
};

/**
 * @brief 注册服务等待体
 */
class RegisterServiceAwaitable : public ServiceRegistryAwaitableBase {
public:
    RegisterServiceAwaitable(AsyncLocalServiceRegistry& registry) : m_registry(registry) {}

    RegisterServiceAwaitable& setEndpoint(const ServiceEndpoint& endpoint) {
        m_endpoint = endpoint;
        return *this;
    }

    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<void, DiscoveryError> await_resume();

private:
    AsyncLocalServiceRegistry& m_registry;
    ServiceEndpoint m_endpoint;
};

/**
 * @brief 注销服务等待体
 */
class DeregisterServiceAwaitable : public ServiceRegistryAwaitableBase {
public:
    DeregisterServiceAwaitable(AsyncLocalServiceRegistry& registry) : m_registry(registry) {}

    DeregisterServiceAwaitable& setEndpoint(const ServiceEndpoint& endpoint) {
        m_endpoint = endpoint;
        return *this;
    }

    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<void, DiscoveryError> await_resume();

private:
    AsyncLocalServiceRegistry& m_registry;
    ServiceEndpoint m_endpoint;
};

/**
 * @brief 发现服务等待体
 */
class DiscoverServiceAwaitable : public ServiceRegistryAwaitableBase {
public:
    DiscoverServiceAwaitable(AsyncLocalServiceRegistry& registry) : m_registry(registry) {}

    DiscoverServiceAwaitable& setServiceName(const std::string& name) {
        m_service_name = name;
        return *this;
    }

    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<std::vector<ServiceEndpoint>, DiscoveryError> await_resume();

private:
    AsyncLocalServiceRegistry& m_registry;
    std::string m_service_name;
    std::vector<ServiceEndpoint> m_result;
};

/**
 * @brief 监听服务等待体
 */
class WatchServiceAwaitable : public ServiceRegistryAwaitableBase {
public:
    WatchServiceAwaitable(AsyncLocalServiceRegistry& registry) : m_registry(registry) {}

    WatchServiceAwaitable& set(const std::string& name, ServiceWatchCallback cb) {
        m_service_name = name;
        m_callback = std::move(cb);
        return *this;
    }

    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<void, DiscoveryError> await_resume();

private:
    AsyncLocalServiceRegistry& m_registry;
    std::string m_service_name;
    ServiceWatchCallback m_callback;
};

/**
 * @brief 取消监听等待体
 */
class UnwatchServiceAwaitable : public ServiceRegistryAwaitableBase {
public:
    UnwatchServiceAwaitable(AsyncLocalServiceRegistry& registry) : m_registry(registry) {}

    UnwatchServiceAwaitable& setServiceName(const std::string& name) {
        m_service_name = name;
        return *this;
    }

    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<void, DiscoveryError> await_resume();

private:
    AsyncLocalServiceRegistry& m_registry;
    std::string m_service_name;
};

/**
 * @brief 异步本地服务注册中心
 *
 * @details 使用AsyncMutex实现的协程友好服务注册中心。
 * 适用于协程环境，不会阻塞线程。
 * 所有异步方法返回Awaitable引用，减少等待体创建开销。
 */
class AsyncLocalServiceRegistry {
public:
    AsyncLocalServiceRegistry()
        : m_register_awaitable(*this)
        , m_deregister_awaitable(*this)
        , m_discover_awaitable(*this)
        , m_watch_awaitable(*this)
        , m_unwatch_awaitable(*this)
    {}
    ~AsyncLocalServiceRegistry() = default;

    /**
     * @brief 异步注册服务
     */
    RegisterServiceAwaitable& registerServiceAsync(const ServiceEndpoint& endpoint) {
        m_register_awaitable.reset();
        return m_register_awaitable.setEndpoint(endpoint);
    }

    /**
     * @brief 异步注销服务
     */
    DeregisterServiceAwaitable& deregisterServiceAsync(const ServiceEndpoint& endpoint) {
        m_deregister_awaitable.reset();
        return m_deregister_awaitable.setEndpoint(endpoint);
    }

    /**
     * @brief 异步发现服务
     */
    DiscoverServiceAwaitable& discoverServiceAsync(const std::string& service_name) {
        m_discover_awaitable.reset();
        return m_discover_awaitable.setServiceName(service_name);
    }

    /**
     * @brief 异步监听服务变更
     */
    WatchServiceAwaitable& watchServiceAsync(const std::string& service_name, ServiceWatchCallback callback) {
        m_watch_awaitable.reset();
        return m_watch_awaitable.set(service_name, std::move(callback));
    }

    /**
     * @brief 异步取消监听
     */
    UnwatchServiceAwaitable& unwatchServiceAsync(const std::string& service_name) {
        m_unwatch_awaitable.reset();
        return m_unwatch_awaitable.setServiceName(service_name);
    }

    // 内部访问
    kernel::AsyncMutex& mutex() { return m_mutex; }
    std::unordered_map<std::string, std::vector<ServiceEndpoint>>& services() { return m_services; }
    std::unordered_map<std::string, std::vector<ServiceWatchCallback>>& watchers() { return m_watchers; }

private:
    kernel::AsyncMutex m_mutex;
    std::unordered_map<std::string, std::vector<ServiceEndpoint>> m_services;
    std::unordered_map<std::string, std::vector<ServiceWatchCallback>> m_watchers;

    RegisterServiceAwaitable m_register_awaitable;
    DeregisterServiceAwaitable m_deregister_awaitable;
    DiscoverServiceAwaitable m_discover_awaitable;
    WatchServiceAwaitable m_watch_awaitable;
    UnwatchServiceAwaitable m_unwatch_awaitable;
};

// Awaitable 实现

inline bool RegisterServiceAwaitable::await_suspend(std::coroutine_handle<> handle) {
    if (m_state == State::AcquireLock) {
        m_lock_awaitable.emplace(m_registry.mutex().lock());
        m_state = State::Execute;
        return m_lock_awaitable->await_suspend(
            std::coroutine_handle<kernel::Coroutine::promise_type>::from_address(handle.address()));
    }
    return false;
}

inline std::expected<void, DiscoveryError> RegisterServiceAwaitable::await_resume() {
    auto lock_result = m_lock_awaitable->await_resume();
    if (!lock_result) {
        m_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
        reset();
        return std::unexpected(m_error);
    }

    m_registry.services()[m_endpoint.service_name].push_back(m_endpoint);

    if (auto it = m_registry.watchers().find(m_endpoint.service_name); it != m_registry.watchers().end()) {
        ServiceEvent event{ServiceEventType::ADDED, m_endpoint};
        for (auto& callback : it->second) {
            callback(event);
        }
    }

    m_registry.mutex().unlock();
    reset();
    return {};
}

inline bool DeregisterServiceAwaitable::await_suspend(std::coroutine_handle<> handle) {
    if (m_state == State::AcquireLock) {
        m_lock_awaitable.emplace(m_registry.mutex().lock());
        m_state = State::Execute;
        return m_lock_awaitable->await_suspend(
            std::coroutine_handle<kernel::Coroutine::promise_type>::from_address(handle.address()));
    }
    return false;
}

inline std::expected<void, DiscoveryError> DeregisterServiceAwaitable::await_resume() {
    auto lock_result = m_lock_awaitable->await_resume();
    if (!lock_result) {
        m_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
        reset();
        return std::unexpected(m_error);
    }

    auto it = m_registry.services().find(m_endpoint.service_name);
    if (it == m_registry.services().end()) {
        m_registry.mutex().unlock();
        m_error = DiscoveryError(DiscoveryError::NOT_FOUND, "Service not found");
        reset();
        return std::unexpected(m_error);
    }

    auto& endpoints = it->second;
    auto ep_it = std::find_if(endpoints.begin(), endpoints.end(),
        [&](const ServiceEndpoint& ep) { return ep.instance_id == m_endpoint.instance_id; });

    if (ep_it == endpoints.end()) {
        m_registry.mutex().unlock();
        m_error = DiscoveryError(DiscoveryError::NOT_FOUND, "Instance not found");
        reset();
        return std::unexpected(m_error);
    }

    ServiceEndpoint removed = *ep_it;
    endpoints.erase(ep_it);

    if (auto wit = m_registry.watchers().find(m_endpoint.service_name); wit != m_registry.watchers().end()) {
        ServiceEvent event{ServiceEventType::REMOVED, removed};
        for (auto& callback : wit->second) {
            callback(event);
        }
    }

    m_registry.mutex().unlock();
    reset();
    return {};
}

inline bool DiscoverServiceAwaitable::await_suspend(std::coroutine_handle<> handle) {
    if (m_state == State::AcquireLock) {
        m_lock_awaitable.emplace(m_registry.mutex().lock());
        m_state = State::Execute;
        return m_lock_awaitable->await_suspend(
            std::coroutine_handle<kernel::Coroutine::promise_type>::from_address(handle.address()));
    }
    return false;
}

inline std::expected<std::vector<ServiceEndpoint>, DiscoveryError> DiscoverServiceAwaitable::await_resume() {
    auto lock_result = m_lock_awaitable->await_resume();
    if (!lock_result) {
        m_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
        reset();
        return std::unexpected(m_error);
    }

    auto it = m_registry.services().find(m_service_name);
    if (it == m_registry.services().end()) {
        m_result.clear();
    } else {
        m_result = it->second;
    }

    m_registry.mutex().unlock();
    auto result = std::move(m_result);
    reset();
    return result;
}

inline bool WatchServiceAwaitable::await_suspend(std::coroutine_handle<> handle) {
    if (m_state == State::AcquireLock) {
        m_lock_awaitable.emplace(m_registry.mutex().lock());
        m_state = State::Execute;
        return m_lock_awaitable->await_suspend(
            std::coroutine_handle<kernel::Coroutine::promise_type>::from_address(handle.address()));
    }
    return false;
}

inline std::expected<void, DiscoveryError> WatchServiceAwaitable::await_resume() {
    auto lock_result = m_lock_awaitable->await_resume();
    if (!lock_result) {
        m_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
        reset();
        return std::unexpected(m_error);
    }

    m_registry.watchers()[m_service_name].push_back(std::move(m_callback));

    m_registry.mutex().unlock();
    reset();
    return {};
}

inline bool UnwatchServiceAwaitable::await_suspend(std::coroutine_handle<> handle) {
    if (m_state == State::AcquireLock) {
        m_lock_awaitable.emplace(m_registry.mutex().lock());
        m_state = State::Execute;
        return m_lock_awaitable->await_suspend(
            std::coroutine_handle<kernel::Coroutine::promise_type>::from_address(handle.address()));
    }
    return false;
}

inline std::expected<void, DiscoveryError> UnwatchServiceAwaitable::await_resume() {
    auto lock_result = m_lock_awaitable->await_resume();
    if (!lock_result) {
        m_error = DiscoveryError(DiscoveryError::LOCK_TIMEOUT, "Lock timeout");
        reset();
        return std::unexpected(m_error);
    }

    m_registry.watchers().erase(m_service_name);

    m_registry.mutex().unlock();
    reset();
    return {};
}

/**
 * @brief 轮询选择器
 */
class RoundRobinSelector {
public:
    const ServiceEndpoint* select(const std::vector<ServiceEndpoint>& endpoints) {
        if (endpoints.empty()) {
            return nullptr;
        }
        size_t idx = m_index.fetch_add(1, std::memory_order_relaxed) % endpoints.size();
        return &endpoints[idx];
    }

    void update(const std::vector<ServiceEndpoint>& /*endpoints*/) {
        // 轮询不需要特殊处理
    }

private:
    std::atomic<size_t> m_index{0};
};

/**
 * @brief 随机选择器
 */
class RandomSelector {
public:
    const ServiceEndpoint* select(const std::vector<ServiceEndpoint>& endpoints) {
        if (endpoints.empty()) {
            return nullptr;
        }
        size_t idx = std::rand() % endpoints.size();
        return &endpoints[idx];
    }

    void update(const std::vector<ServiceEndpoint>& /*endpoints*/) {
        // 随机不需要特殊处理
    }
};

/**
 * @brief 加权轮询选择器
 */
class WeightedRoundRobinSelector {
public:
    const ServiceEndpoint* select(const std::vector<ServiceEndpoint>& endpoints) {
        if (endpoints.empty()) {
            return nullptr;
        }

        // 简单实现：按权重展开后轮询
        std::vector<size_t> expanded;
        for (size_t i = 0; i < endpoints.size(); ++i) {
            for (uint32_t w = 0; w < endpoints[i].weight / 10; ++w) {
                expanded.push_back(i);
            }
        }

        if (expanded.empty()) {
            return &endpoints[0];
        }

        size_t idx = m_index.fetch_add(1, std::memory_order_relaxed) % expanded.size();
        return &endpoints[expanded[idx]];
    }

    void update(const std::vector<ServiceEndpoint>& /*endpoints*/) {
        // 可以在这里预计算权重
    }

private:
    std::atomic<size_t> m_index{0};
};

// 验证选择器满足ServiceSelector concept
static_assert(ServiceSelector<RoundRobinSelector>, "RoundRobinSelector must satisfy ServiceSelector concept");
static_assert(ServiceSelector<RandomSelector>, "RandomSelector must satisfy ServiceSelector concept");
static_assert(ServiceSelector<WeightedRoundRobinSelector>, "WeightedRoundRobinSelector must satisfy ServiceSelector concept");

/**
 * @brief 服务发现客户端
 *
 * @tparam Registry 服务注册中心类型，必须满足ServiceRegistry concept
 * @tparam Selector 服务选择器类型，必须满足ServiceSelector concept
 */
template<ServiceRegistry Registry, ServiceSelector Selector = RoundRobinSelector>
class ServiceDiscoveryClient {
public:
    explicit ServiceDiscoveryClient(Registry& registry)
        : m_registry(registry) {}

    /**
     * @brief 获取服务实例
     */
    std::expected<ServiceEndpoint, DiscoveryError> getServiceEndpoint(const std::string& service_name) {
        auto result = m_registry.discoverService(service_name);
        if (!result) {
            return std::unexpected(result.error());
        }

        auto& endpoints = result.value();
        const ServiceEndpoint* selected = m_selector.select(endpoints);

        if (!selected) {
            return std::unexpected(DiscoveryError(DiscoveryError::NOT_FOUND, "No available instance"));
        }

        return *selected;
    }

    /**
     * @brief 监听服务变更
     */
    std::expected<void, DiscoveryError> watch(const std::string& service_name, ServiceWatchCallback callback) {
        return m_registry.watchService(service_name, std::move(callback));
    }

    /**
     * @brief 取消监听
     */
    void unwatch(const std::string& service_name) {
        m_registry.unwatchService(service_name);
    }

private:
    Registry& m_registry;
    Selector m_selector;
};

} // namespace galay::rpc

#endif // GALAY_RPC_SERVICE_DISCOVERY_H
