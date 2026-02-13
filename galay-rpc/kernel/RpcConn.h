/**
 * @file RpcConn.h
 * @brief RPC连接封装
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 封装TCP连接，使用RingBuffer配合readv/writev提供高效的RPC消息读写。
 */

#ifndef GALAY_RPC_CONN_H
#define GALAY_RPC_CONN_H

#include "galay-rpc/protoc/RpcMessage.h"
#include "galay-rpc/protoc/RpcCodec.h"
#include "galay-rpc/protoc/RpcError.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/kernel/IOScheduler.hpp"
#include "galay-kernel/kernel/Timeout.hpp"
#include "galay-kernel/common/Buffer.h"
#include <expected>
#include <optional>

namespace galay::rpc
{

using namespace galay::kernel;
using namespace galay::async;

// 前向声明
template<typename SocketType> class RpcConnImpl;
template<typename SocketType> class RpcReaderImpl;
template<typename SocketType> class RpcWriterImpl;

/**
 * @brief RPC读取器配置
 */
struct RpcReaderSetting {
    size_t max_message_size = RPC_MAX_BODY_SIZE;  ///< 最大消息大小
};

/**
 * @brief RPC写入器配置
 */
struct RpcWriterSetting {
    // 预留扩展
};

/**
 * @brief RPC请求读取等待体（使用readv）
 */
template<typename SocketType>
class GetRpcRequestAwaitable : public TimeoutSupport<GetRpcRequestAwaitable<SocketType>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetRpcRequestAwaitable(RingBuffer& ring_buffer,
                           const RpcReaderSetting& setting,
                           RpcRequest& request,
                           SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_request(request)
        , m_socket(socket)
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_readv_awaitable) {
            m_readv_awaitable.emplace(m_socket.readv(m_ring_buffer.getWriteIovecs()));
        }
        return m_readv_awaitable->await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto readv_result = m_readv_awaitable->await_resume();
        m_readv_awaitable.reset();

        if (!readv_result) {
            if (IOError::contains(readv_result.error().code(), kDisconnectError)) {
                return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed by peer"));
            }
            return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR, readv_result.error().message()));
        }

        ssize_t bytes_read = readv_result.value();

        if (bytes_read == 0) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed"));
        }

        m_ring_buffer.produce(bytes_read);
        m_total_received += bytes_read;

        // 获取可读数据
        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return false;  // 需要继续读取
        }

        // 计算总可读长度
        size_t total_readable = 0;
        for (const auto& iov : read_iovecs) {
            total_readable += iov.iov_len;
        }

        // 检查是否有足够的头部数据
        if (total_readable < RPC_HEADER_SIZE) {
            return false;  // 需要继续读取
        }

        // 将数据拷贝到连续缓冲区进行解析
        std::vector<char> linear_buffer(total_readable);
        size_t offset = 0;
        for (const auto& iov : read_iovecs) {
            std::memcpy(linear_buffer.data() + offset, iov.iov_base, iov.iov_len);
            offset += iov.iov_len;
        }

        // 获取完整消息长度
        size_t msg_len = RpcCodec::messageLength(linear_buffer.data(), linear_buffer.size());
        if (msg_len == 0) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid message header"));
        }

        // 检查消息大小限制
        if (msg_len > m_setting.max_message_size + RPC_HEADER_SIZE) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Message too large"));
        }

        // 检查是否有完整消息
        if (total_readable < msg_len) {
            return false;  // 需要继续读取
        }

        // 解码请求
        auto result = RpcCodec::decodeRequest(linear_buffer.data(), msg_len);
        if (!result) {
            return std::unexpected(result.error());
        }

        m_request = std::move(result.value());
        m_ring_buffer.consume(msg_len);

        return true;  // 解析完成
    }

private:
    RingBuffer& m_ring_buffer;
    const RpcReaderSetting& m_setting;
    RpcRequest& m_request;
    SocketType& m_socket;
    size_t m_total_received;
    std::optional<ReadvAwaitableType> m_readv_awaitable;
};

/**
 * @brief RPC响应读取等待体（使用readv）
 */
template<typename SocketType>
class GetRpcResponseAwaitable : public TimeoutSupport<GetRpcResponseAwaitable<SocketType>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetRpcResponseAwaitable(RingBuffer& ring_buffer,
                            const RpcReaderSetting& setting,
                            RpcResponse& response,
                            SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_response(response)
        , m_socket(socket)
        , m_total_received(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_readv_awaitable) {
            m_readv_awaitable.emplace(m_socket.readv(m_ring_buffer.getWriteIovecs()));
        }
        return m_readv_awaitable->await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto readv_result = m_readv_awaitable->await_resume();
        m_readv_awaitable.reset();

        if (!readv_result) {
            if (IOError::contains(readv_result.error().code(), kDisconnectError)) {
                return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed by peer"));
            }
            return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR, readv_result.error().message()));
        }

        ssize_t bytes_read = readv_result.value();

        if (bytes_read == 0) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed"));
        }

        m_ring_buffer.produce(bytes_read);
        m_total_received += bytes_read;

        // 获取可读数据
        auto read_iovecs = m_ring_buffer.getReadIovecs();
        if (read_iovecs.empty()) {
            return false;
        }

        // 计算总可读长度
        size_t total_readable = 0;
        for (const auto& iov : read_iovecs) {
            total_readable += iov.iov_len;
        }

        if (total_readable < RPC_HEADER_SIZE) {
            return false;
        }

        // 线性化数据
        std::vector<char> linear_buffer(total_readable);
        size_t offset = 0;
        for (const auto& iov : read_iovecs) {
            std::memcpy(linear_buffer.data() + offset, iov.iov_base, iov.iov_len);
            offset += iov.iov_len;
        }

        size_t msg_len = RpcCodec::messageLength(linear_buffer.data(), linear_buffer.size());
        if (msg_len == 0) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Invalid message header"));
        }

        if (msg_len > m_setting.max_message_size + RPC_HEADER_SIZE) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Message too large"));
        }

        if (total_readable < msg_len) {
            return false;
        }

        auto result = RpcCodec::decodeResponse(linear_buffer.data(), msg_len);
        if (!result) {
            return std::unexpected(result.error());
        }

        m_response = std::move(result.value());
        m_ring_buffer.consume(msg_len);

        return true;
    }

private:
    RingBuffer& m_ring_buffer;
    const RpcReaderSetting& m_setting;
    RpcResponse& m_response;
    SocketType& m_socket;
    size_t m_total_received;
    std::optional<ReadvAwaitableType> m_readv_awaitable;
};

/**
 * @brief RPC发送请求等待体（使用writev）
 */
template<typename SocketType>
class SendRpcRequestAwaitable : public TimeoutSupport<SendRpcRequestAwaitable<SocketType>>
{
public:
    using WritevAwaitableType = decltype(std::declval<SocketType>().writev(std::declval<std::vector<iovec>>()));

    SendRpcRequestAwaitable(const RpcRequest& request, SocketType& socket)
        : m_socket(socket)
        , m_data(request.serialize())
        , m_sent(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_writev_awaitable) {
            std::vector<iovec> iovecs(1);
            iovecs[0].iov_base = m_data.data() + m_sent;
            iovecs[0].iov_len = m_data.size() - m_sent;
            m_writev_awaitable.emplace(m_socket.writev(std::move(iovecs)));
        }
        return m_writev_awaitable->await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto writev_result = m_writev_awaitable->await_resume();
        m_writev_awaitable.reset();

        if (!writev_result) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Send failed"));
        }

        m_sent += writev_result.value();

        if (m_sent >= m_data.size()) {
            return true;  // 发送完成
        }

        return false;  // 需要继续发送
    }

private:
    SocketType& m_socket;
    std::vector<char> m_data;
    size_t m_sent;
    std::optional<WritevAwaitableType> m_writev_awaitable;
};

/**
 * @brief RPC发送响应等待体（使用writev）
 */
template<typename SocketType>
class SendRpcResponseAwaitable : public TimeoutSupport<SendRpcResponseAwaitable<SocketType>>
{
public:
    using WritevAwaitableType = decltype(std::declval<SocketType>().writev(std::declval<std::vector<iovec>>()));

    SendRpcResponseAwaitable(const RpcResponse& response, SocketType& socket)
        : m_socket(socket)
        , m_data(response.serialize())
        , m_sent(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_writev_awaitable) {
            std::vector<iovec> iovecs(1);
            iovecs[0].iov_base = m_data.data() + m_sent;
            iovecs[0].iov_len = m_data.size() - m_sent;
            m_writev_awaitable.emplace(m_socket.writev(std::move(iovecs)));
        }
        return m_writev_awaitable->await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto writev_result = m_writev_awaitable->await_resume();
        m_writev_awaitable.reset();

        if (!writev_result) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Send failed"));
        }

        m_sent += writev_result.value();

        if (m_sent >= m_data.size()) {
            return true;
        }

        return false;
    }

private:
    SocketType& m_socket;
    std::vector<char> m_data;
    size_t m_sent;
    std::optional<WritevAwaitableType> m_writev_awaitable;
};

/**
 * @brief 发送原始数据等待体（用于流式传输）
 */
template<typename SocketType>
class SendRawDataAwaitable : public TimeoutSupport<SendRawDataAwaitable<SocketType>>
{
public:
    using WritevAwaitableType = decltype(std::declval<SocketType>().writev(std::declval<std::vector<iovec>>()));

    SendRawDataAwaitable(std::vector<char>&& data, SocketType& socket)
        : m_socket(socket)
        , m_data(std::move(data))
        , m_sent(0)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_writev_awaitable) {
            std::vector<iovec> iovecs(1);
            iovecs[0].iov_base = m_data.data() + m_sent;
            iovecs[0].iov_len = m_data.size() - m_sent;
            m_writev_awaitable.emplace(m_socket.writev(std::move(iovecs)));
        }
        return m_writev_awaitable->await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto writev_result = m_writev_awaitable->await_resume();
        m_writev_awaitable.reset();

        if (!writev_result) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Send failed"));
        }

        m_sent += writev_result.value();

        if (m_sent >= m_data.size()) {
            return true;
        }

        return false;
    }

private:
    SocketType& m_socket;
    std::vector<char> m_data;
    size_t m_sent;
    std::optional<WritevAwaitableType> m_writev_awaitable;
};

/**
 * @brief 读取消息头等待体（用于流式传输）
 */
template<typename SocketType>
class GetRpcHeaderAwaitable : public TimeoutSupport<GetRpcHeaderAwaitable<SocketType>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetRpcHeaderAwaitable(RingBuffer& ring_buffer, RpcHeader& header, SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_header(header)
        , m_socket(socket)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_readv_awaitable) {
            m_readv_awaitable.emplace(m_socket.readv(m_ring_buffer.getWriteIovecs()));
        }
        return m_readv_awaitable->await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto readv_result = m_readv_awaitable->await_resume();
        m_readv_awaitable.reset();

        if (!readv_result) {
            if (IOError::contains(readv_result.error().code(), kDisconnectError)) {
                return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed by peer"));
            }
            return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR, readv_result.error().message()));
        }

        ssize_t bytes_read = readv_result.value();
        if (bytes_read == 0) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed"));
        }

        m_ring_buffer.produce(bytes_read);

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        size_t total_readable = 0;
        for (const auto& iov : read_iovecs) {
            total_readable += iov.iov_len;
        }

        if (total_readable < RPC_HEADER_SIZE) {
            return false;  // 需要继续读取
        }

        // 线性化头部数据
        char header_buf[RPC_HEADER_SIZE];
        size_t copied = 0;
        for (const auto& iov : read_iovecs) {
            size_t to_copy = std::min(iov.iov_len, RPC_HEADER_SIZE - copied);
            std::memcpy(header_buf + copied, iov.iov_base, to_copy);
            copied += to_copy;
            if (copied >= RPC_HEADER_SIZE) break;
        }

        if (!m_header.deserialize(header_buf)) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid header"));
        }

        m_ring_buffer.consume(RPC_HEADER_SIZE);
        return true;
    }

private:
    RingBuffer& m_ring_buffer;
    RpcHeader& m_header;
    SocketType& m_socket;
    std::optional<ReadvAwaitableType> m_readv_awaitable;
};

/**
 * @brief 读取消息体等待体（用于流式传输）
 */
template<typename SocketType>
class GetRpcBodyAwaitable : public TimeoutSupport<GetRpcBodyAwaitable<SocketType>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetRpcBodyAwaitable(RingBuffer& ring_buffer, char* body, size_t body_len, SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_body(body)
        , m_body_len(body_len)
        , m_socket(socket)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_readv_awaitable) {
            m_readv_awaitable.emplace(m_socket.readv(m_ring_buffer.getWriteIovecs()));
        }
        return m_readv_awaitable->await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto readv_result = m_readv_awaitable->await_resume();
        m_readv_awaitable.reset();

        if (!readv_result) {
            if (IOError::contains(readv_result.error().code(), kDisconnectError)) {
                return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed by peer"));
            }
            return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR, readv_result.error().message()));
        }

        ssize_t bytes_read = readv_result.value();
        if (bytes_read == 0) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed"));
        }

        m_ring_buffer.produce(bytes_read);

        auto read_iovecs = m_ring_buffer.getReadIovecs();
        size_t total_readable = 0;
        for (const auto& iov : read_iovecs) {
            total_readable += iov.iov_len;
        }

        if (total_readable < m_body_len) {
            return false;  // 需要继续读取
        }

        // 拷贝消息体
        size_t copied = 0;
        for (const auto& iov : read_iovecs) {
            size_t to_copy = std::min(iov.iov_len, m_body_len - copied);
            std::memcpy(m_body + copied, iov.iov_base, to_copy);
            copied += to_copy;
            if (copied >= m_body_len) break;
        }

        m_ring_buffer.consume(m_body_len);
        return true;
    }

private:
    RingBuffer& m_ring_buffer;
    char* m_body;
    size_t m_body_len;
    SocketType& m_socket;
    std::optional<ReadvAwaitableType> m_readv_awaitable;
};

/**
 * @brief RPC读取器模板类
 */
template<typename SocketType>
class RpcReaderImpl
{
public:
    using GetHeaderAwaitable = GetRpcHeaderAwaitable<SocketType>;
    using GetBodyAwaitable = GetRpcBodyAwaitable<SocketType>;

    RpcReaderImpl(RingBuffer& ring_buffer, const RpcReaderSetting& setting, SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_socket(socket)
    {
    }

    /**
     * @brief 获取RPC请求
     * @param request 输出请求对象
     * @return 等待体，返回true表示解析完成，false表示需要继续读取
     */
    GetRpcRequestAwaitable<SocketType> getRequest(RpcRequest& request) {
        return GetRpcRequestAwaitable<SocketType>(m_ring_buffer, m_setting, request, m_socket);
    }

    /**
     * @brief 获取RPC响应
     * @param response 输出响应对象
     * @return 等待体
     */
    GetRpcResponseAwaitable<SocketType> getResponse(RpcResponse& response) {
        return GetRpcResponseAwaitable<SocketType>(m_ring_buffer, m_setting, response, m_socket);
    }

    /**
     * @brief 获取消息头（用于流式传输）
     */
    GetHeaderAwaitable getHeader(RpcHeader& header) {
        return GetHeaderAwaitable(m_ring_buffer, header, m_socket);
    }

    /**
     * @brief 获取消息体（用于流式传输）
     */
    GetBodyAwaitable getBody(char* body, size_t body_len) {
        return GetBodyAwaitable(m_ring_buffer, body, body_len, m_socket);
    }

private:
    RingBuffer& m_ring_buffer;
    const RpcReaderSetting& m_setting;
    SocketType& m_socket;
};

/**
 * @brief RPC写入器模板类
 */
template<typename SocketType>
class RpcWriterImpl
{
public:
    using SendRawAwaitable = SendRawDataAwaitable<SocketType>;

    RpcWriterImpl(const RpcWriterSetting& setting, SocketType& socket)
        : m_setting(setting)
        , m_socket(socket)
    {
    }

    /**
     * @brief 发送RPC请求
     * @param request 请求对象
     * @return 等待体
     */
    SendRpcRequestAwaitable<SocketType> sendRequest(const RpcRequest& request) {
        return SendRpcRequestAwaitable<SocketType>(request, m_socket);
    }

    /**
     * @brief 发送RPC响应
     * @param response 响应对象
     * @return 等待体
     */
    SendRpcResponseAwaitable<SocketType> sendResponse(const RpcResponse& response) {
        return SendRpcResponseAwaitable<SocketType>(response, m_socket);
    }

    /**
     * @brief 发送原始数据（用于流式传输）
     */
    SendRawAwaitable sendRaw(const char* data, size_t len) {
        std::vector<char> buf(data, data + len);
        return SendRawAwaitable(std::move(buf), m_socket);
    }

private:
    const RpcWriterSetting& m_setting;
    SocketType& m_socket;
};

// 类型别名
using RpcReader = RpcReaderImpl<TcpSocket>;
using RpcWriter = RpcWriterImpl<TcpSocket>;

/**
 * @brief RPC连接模板类
 *
 * @details 封装TCP连接，使用RingBuffer配合readv/writev提供高效IO。
 */
template<typename SocketType>
class RpcConnImpl
{
public:
    static constexpr size_t kDefaultRingBufferSize = 8192;

    /**
     * @brief 从已有socket构造（服务端使用）
     */
    explicit RpcConnImpl(GHandle handle, const RpcReaderSetting& reader_setting = {},
                         const RpcWriterSetting& writer_setting = {},
                         size_t ring_buffer_size = kDefaultRingBufferSize)
        : m_socket(handle)
        , m_ring_buffer(normalizeRingBufferSize(ring_buffer_size))
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
    {
        m_socket.option().handleNonBlock();
    }

    /**
     * @brief 创建新连接（客户端使用）
     */
    explicit RpcConnImpl(IPType type = IPType::IPV4,
                         const RpcReaderSetting& reader_setting = {},
                         const RpcWriterSetting& writer_setting = {},
                         size_t ring_buffer_size = kDefaultRingBufferSize)
        : m_socket(type)
        , m_ring_buffer(normalizeRingBufferSize(ring_buffer_size))
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
    {
        m_socket.option().handleNonBlock();
    }

    ~RpcConnImpl() = default;

    // 禁止拷贝
    RpcConnImpl(const RpcConnImpl&) = delete;
    RpcConnImpl& operator=(const RpcConnImpl&) = delete;

    // 允许移动
    RpcConnImpl(RpcConnImpl&&) = default;
    RpcConnImpl& operator=(RpcConnImpl&&) = default;

    /**
     * @brief 连接到服务器
     */
    ConnectAwaitable connect(const Host& host) {
        return m_socket.connect(host);
    }

    /**
     * @brief 获取读取器
     */
    RpcReaderImpl<SocketType> getReader() {
        return RpcReaderImpl<SocketType>(m_ring_buffer, m_reader_setting, m_socket);
    }

    /**
     * @brief 获取写入器
     */
    RpcWriterImpl<SocketType> getWriter() {
        return RpcWriterImpl<SocketType>(m_writer_setting, m_socket);
    }

    /**
     * @brief 获取底层socket
     */
    SocketType& socket() { return m_socket; }

    /**
     * @brief 关闭连接
     */
    CloseAwaitable close() {
        return m_socket.close();
    }

private:
    static size_t normalizeRingBufferSize(size_t ring_buffer_size) {
        return ring_buffer_size == 0 ? kDefaultRingBufferSize : ring_buffer_size;
    }

private:
    SocketType m_socket;
    RingBuffer m_ring_buffer;
    RpcReaderSetting m_reader_setting;
    RpcWriterSetting m_writer_setting;
};

// 类型别名
using RpcConn = RpcConnImpl<TcpSocket>;

} // namespace galay::rpc

#endif // GALAY_RPC_CONN_H
