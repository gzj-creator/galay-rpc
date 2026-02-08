/**
 * @file RpcStream.h
 * @brief RPC双向流支持
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供双向流式RPC调用支持，参考 galay-http WebSocket 的设计模式。
 *
 * 使用方式（循环发送/接收）：
 * @code
 * // 客户端双向流
 * Coroutine biStreamExample(RpcStreamConn& stream) {
 *     auto& writer = stream.getWriter();
 *     auto& reader = stream.getReader();
 *
 *     // 发送消息（循环直到完成）
 *     while (true) {
 *         auto result = co_await writer.sendData("Hello");
 *         if (!result) { // 错误
 *             break;
 *         }
 *         if (result.value()) { // 发送完成
 *             break;
 *         }
 *         // 继续发送
 *     }
 *
 *     // 接收消息（循环直到完成）
 *     StreamMessage msg;
 *     while (true) {
 *         auto result = co_await reader.getMessage(msg);
 *         if (!result) { // 错误
 *             break;
 *         }
 *         if (result.value()) { // 接收完成
 *             std::cout << "Received: " << msg.payloadStr() << "\n";
 *             break;
 *         }
 *         // 继续接收
 *     }
 *
 *     // 结束流
 *     while (true) {
 *         auto result = co_await writer.sendEnd();
 *         if (!result || result.value()) break;
 *     }
 * }
 * @endcode
 */

#ifndef GALAY_RPC_STREAM_H
#define GALAY_RPC_STREAM_H

#include "RpcConn.h"
#include "galay-rpc/protoc/RpcMessage.h"
#include "galay-rpc/protoc/RpcBase.h"
#include "galay-kernel/kernel/Coroutine.h"
#include "galay-kernel/kernel/Timeout.hpp"
#include <atomic>
#include <optional>

namespace galay::rpc
{

using namespace galay::kernel;

/**
 * @brief 流消息
 */
class StreamMessage {
public:
    StreamMessage() = default;

    StreamMessage(uint32_t stream_id, const char* data, size_t len)
        : m_stream_id(stream_id)
        , m_payload(data, data + len)
        , m_is_end(false) {}

    uint32_t streamId() const { return m_stream_id; }
    void streamId(uint32_t id) { m_stream_id = id; }

    const std::vector<char>& payload() const { return m_payload; }
    std::string payloadStr() const { return std::string(m_payload.begin(), m_payload.end()); }

    void payload(const char* data, size_t len) {
        m_payload.assign(data, data + len);
    }
    void payload(const std::string& data) {
        m_payload.assign(data.begin(), data.end());
    }

    bool isEnd() const { return m_is_end; }
    void setEnd(bool end = true) { m_is_end = end; }

    RpcMessageType messageType() const { return m_msg_type; }
    void messageType(RpcMessageType type) { m_msg_type = type; }

    /**
     * @brief 序列化流消息
     */
    std::vector<char> serialize(RpcMessageType type) const {
        size_t body_size = m_payload.size();
        std::vector<char> buffer(RPC_HEADER_SIZE + body_size);

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(type);
        header.m_request_id = m_stream_id;
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(buffer.data());

        if (!m_payload.empty()) {
            std::memcpy(buffer.data() + RPC_HEADER_SIZE, m_payload.data(), m_payload.size());
        }

        return buffer;
    }

    /**
     * @brief 反序列化流消息体
     */
    bool deserializeBody(const char* body, size_t length) {
        if (length > 0) {
            m_payload.assign(body, body + length);
        }
        return true;
    }

private:
    uint32_t m_stream_id = 0;
    std::vector<char> m_payload;
    bool m_is_end = false;
    RpcMessageType m_msg_type = RpcMessageType::STREAM_DATA;
};

/**
 * @brief 流初始化请求
 */
class StreamInitRequest {
public:
    StreamInitRequest() = default;

    StreamInitRequest(uint32_t stream_id, std::string_view service, std::string_view method)
        : m_stream_id(stream_id)
        , m_service_name(service)
        , m_method_name(method) {}

    uint32_t streamId() const { return m_stream_id; }
    void streamId(uint32_t id) { m_stream_id = id; }
    const std::string& serviceName() const { return m_service_name; }
    void serviceName(std::string_view name) { m_service_name = name; }
    const std::string& methodName() const { return m_method_name; }
    void methodName(std::string_view name) { m_method_name = name; }

    std::vector<char> serialize() const {
        size_t body_size = 2 + m_service_name.size() + 2 + m_method_name.size();
        std::vector<char> buffer(RPC_HEADER_SIZE + body_size);

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::STREAM_INIT);
        header.m_request_id = m_stream_id;
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(buffer.data());

        char* body = buffer.data() + RPC_HEADER_SIZE;
        size_t offset = 0;

        uint16_t service_len = rpcHtons(static_cast<uint16_t>(m_service_name.size()));
        std::memcpy(body + offset, &service_len, 2);
        offset += 2;
        std::memcpy(body + offset, m_service_name.data(), m_service_name.size());
        offset += m_service_name.size();

        uint16_t method_len = rpcHtons(static_cast<uint16_t>(m_method_name.size()));
        std::memcpy(body + offset, &method_len, 2);
        offset += 2;
        std::memcpy(body + offset, m_method_name.data(), m_method_name.size());

        return buffer;
    }

    bool deserializeBody(const char* body, size_t length) {
        if (length < 4) return false;

        size_t offset = 0;

        uint16_t service_len;
        std::memcpy(&service_len, body + offset, 2);
        service_len = rpcNtohs(service_len);
        offset += 2;

        if (offset + service_len > length) return false;
        m_service_name.assign(body + offset, service_len);
        offset += service_len;

        if (offset + 2 > length) return false;
        uint16_t method_len;
        std::memcpy(&method_len, body + offset, 2);
        method_len = rpcNtohs(method_len);
        offset += 2;

        if (offset + method_len > length) return false;
        m_method_name.assign(body + offset, method_len);

        return true;
    }

private:
    uint32_t m_stream_id = 0;
    std::string m_service_name;
    std::string m_method_name;
};

// 前向声明
template<typename SocketType> class StreamReaderImpl;
template<typename SocketType> class StreamWriterImpl;

/**
 * @brief 流数据发送等待体
 */
template<typename SocketType>
class SendStreamDataAwaitable : public TimeoutSupport<SendStreamDataAwaitable<SocketType>>
{
public:
    using WritevAwaitableType = decltype(std::declval<SocketType>().writev(std::declval<std::vector<iovec>>()));

    SendStreamDataAwaitable(SocketType& socket, std::vector<char>&& data)
        : m_socket(socket)
        , m_data(std::move(data))
        , m_sent(0)
    {}

    bool await_ready() const noexcept { return false; }

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
        auto result = m_writev_awaitable->await_resume();
        m_writev_awaitable.reset();

        if (!result) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Send failed"));
        }

        m_sent += result.value();
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

public:
    std::expected<bool, IOError> m_result;  // TimeoutSupport 需要
};

/**
 * @brief 流消息接收等待体
 */
template<typename SocketType>
class GetStreamMessageAwaitable : public TimeoutSupport<GetStreamMessageAwaitable<SocketType>>
{
public:
    using ReadvAwaitableType = decltype(std::declval<SocketType>().readv(std::declval<std::vector<iovec>>()));

    GetStreamMessageAwaitable(RingBuffer& ring_buffer, SocketType& socket, StreamMessage& msg)
        : m_ring_buffer(ring_buffer)
        , m_socket(socket)
        , m_message(msg)
        , m_state(State::ReadHeader)
        , m_body_length(0)
    {}

    bool await_ready() const noexcept { return false; }

    template<typename Handle>
    auto await_suspend(Handle handle) {
        if (!m_readv_awaitable) {
            m_readv_awaitable.emplace(m_socket.readv(m_ring_buffer.getWriteIovecs()));
        }
        return m_readv_awaitable->await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto result = m_readv_awaitable->await_resume();
        m_readv_awaitable.reset();

        if (!result) {
            if (IOError::contains(result.error().code(), kDisconnectError)) {
                return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed"));
            }
            return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR, result.error().message()));
        }

        ssize_t bytes_read = result.value();
        if (bytes_read == 0) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed"));
        }

        m_ring_buffer.produce(bytes_read);

        // 获取可读数据
        auto read_iovecs = m_ring_buffer.getReadIovecs();
        size_t total_readable = 0;
        for (const auto& iov : read_iovecs) {
            total_readable += iov.iov_len;
        }

        if (m_state == State::ReadHeader) {
            if (total_readable < RPC_HEADER_SIZE) {
                return false;  // 继续读取
            }

            // 线性化头部
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
            m_body_length = m_header.m_body_length;
            m_message.streamId(m_header.m_request_id);

            auto msg_type = static_cast<RpcMessageType>(m_header.m_type);
            m_message.messageType(msg_type);

            if (msg_type == RpcMessageType::STREAM_END || msg_type == RpcMessageType::STREAM_CANCEL) {
                m_message.setEnd(true);
                return true;  // 流结束
            }

            if (m_body_length == 0) {
                return true;  // 无消息体
            }

            m_state = State::ReadBody;
            total_readable -= RPC_HEADER_SIZE;
        }

        if (m_state == State::ReadBody) {
            // 重新获取可读数据
            read_iovecs = m_ring_buffer.getReadIovecs();
            total_readable = 0;
            for (const auto& iov : read_iovecs) {
                total_readable += iov.iov_len;
            }

            if (total_readable < m_body_length) {
                return false;  // 继续读取
            }

            // 读取消息体
            std::vector<char> body(m_body_length);
            size_t copied = 0;
            for (const auto& iov : read_iovecs) {
                size_t to_copy = std::min(iov.iov_len, m_body_length - copied);
                std::memcpy(body.data() + copied, iov.iov_base, to_copy);
                copied += to_copy;
                if (copied >= m_body_length) break;
            }

            m_message.deserializeBody(body.data(), body.size());
            m_ring_buffer.consume(m_body_length);

            m_state = State::ReadHeader;  // 重置状态
            return true;  // 接收完成
        }

        return false;
    }

private:
    enum class State { ReadHeader, ReadBody };

    RingBuffer& m_ring_buffer;
    SocketType& m_socket;
    StreamMessage& m_message;
    State m_state;
    RpcHeader m_header;
    size_t m_body_length;
    std::optional<ReadvAwaitableType> m_readv_awaitable;

public:
    std::expected<bool, IOError> m_result;  // TimeoutSupport 需要
};

/**
 * @brief 流读取器
 */
template<typename SocketType>
class StreamReaderImpl {
public:
    StreamReaderImpl(RingBuffer& ring_buffer, SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_socket(socket)
    {}

    /**
     * @brief 获取流消息
     * @param msg 输出消息
     * @return 等待体，返回 true 表示接收完成，false 表示需要继续
     */
    GetStreamMessageAwaitable<SocketType> getMessage(StreamMessage& msg) {
        return GetStreamMessageAwaitable<SocketType>(m_ring_buffer, m_socket, msg);
    }

private:
    RingBuffer& m_ring_buffer;
    SocketType& m_socket;
};

/**
 * @brief 流写入器
 */
template<typename SocketType>
class StreamWriterImpl {
public:
    StreamWriterImpl(SocketType& socket, uint32_t stream_id)
        : m_socket(socket)
        , m_stream_id(stream_id)
    {}

    /**
     * @brief 发送流数据
     */
    SendStreamDataAwaitable<SocketType> sendData(const char* data, size_t len) {
        StreamMessage msg(m_stream_id, data, len);
        return SendStreamDataAwaitable<SocketType>(m_socket, msg.serialize(RpcMessageType::STREAM_DATA));
    }

    SendStreamDataAwaitable<SocketType> sendData(const std::string& data) {
        return sendData(data.data(), data.size());
    }

    /**
     * @brief 发送流初始化请求
     */
    SendStreamDataAwaitable<SocketType> sendInit(const std::string& service, const std::string& method) {
        StreamInitRequest init(m_stream_id, service, method);
        return SendStreamDataAwaitable<SocketType>(m_socket, init.serialize());
    }

    /**
     * @brief 发送流初始化确认
     */
    SendStreamDataAwaitable<SocketType> sendInitAck() {
        StreamMessage msg(m_stream_id, nullptr, 0);
        return SendStreamDataAwaitable<SocketType>(m_socket, msg.serialize(RpcMessageType::STREAM_INIT_ACK));
    }

    /**
     * @brief 发送流结束
     */
    SendStreamDataAwaitable<SocketType> sendEnd() {
        StreamMessage msg(m_stream_id, nullptr, 0);
        return SendStreamDataAwaitable<SocketType>(m_socket, msg.serialize(RpcMessageType::STREAM_END));
    }

    /**
     * @brief 发送流取消
     */
    SendStreamDataAwaitable<SocketType> sendCancel() {
        StreamMessage msg(m_stream_id, nullptr, 0);
        return SendStreamDataAwaitable<SocketType>(m_socket, msg.serialize(RpcMessageType::STREAM_CANCEL));
    }

private:
    SocketType& m_socket;
    uint32_t m_stream_id;
};

/**
 * @brief RPC流连接
 */
template<typename SocketType>
class RpcStreamConnImpl {
public:
    RpcStreamConnImpl(SocketType& socket, RingBuffer& ring_buffer, uint32_t stream_id)
        : m_socket(socket)
        , m_ring_buffer(ring_buffer)
        , m_stream_id(stream_id)
        , m_reader(ring_buffer, socket)
        , m_writer(socket, stream_id)
    {}

    uint32_t streamId() const { return m_stream_id; }

    StreamReaderImpl<SocketType>& getReader() { return m_reader; }
    StreamWriterImpl<SocketType>& getWriter() { return m_writer; }

    SocketType& socket() { return m_socket; }
    RingBuffer& ringBuffer() { return m_ring_buffer; }

private:
    SocketType& m_socket;
    RingBuffer& m_ring_buffer;
    uint32_t m_stream_id;
    StreamReaderImpl<SocketType> m_reader;
    StreamWriterImpl<SocketType> m_writer;
};

// 类型别名
using StreamReader = StreamReaderImpl<TcpSocket>;
using StreamWriter = StreamWriterImpl<TcpSocket>;
using RpcStreamConn = RpcStreamConnImpl<TcpSocket>;

} // namespace galay::rpc

#endif // GALAY_RPC_STREAM_H
