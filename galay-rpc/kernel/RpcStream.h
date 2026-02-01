/**
 * @file RpcStream.h
 * @brief RPC双向流支持
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供双向流式RPC调用支持。
 *
 * 流模式：
 * - 客户端流：客户端发送多个消息，服务端返回一个响应
 * - 服务端流：客户端发送一个请求，服务端返回多个消息
 * - 双向流：客户端和服务端可以同时发送多个消息
 *
 * @example
 * @code
 * // 客户端双向流
 * Coroutine biStreamExample(RpcClient& client) {
 *     auto stream = co_await client.openStream("ChatService", "chat");
 *     if (!stream) co_return;
 *
 *     // 发送消息
 *     co_await stream->send("Hello");
 *
 *     // 接收消息
 *     while (true) {
 *         auto msg = co_await stream->recv();
 *         if (!msg || msg->isEnd()) break;
 *         std::cout << "Received: " << msg->payloadStr() << "\n";
 *     }
 *
 *     co_await stream->close();
 * }
 *
 * // 服务端流处理
 * Coroutine chatHandler(RpcStreamContext& ctx) {
 *     while (true) {
 *         auto msg = co_await ctx.recv();
 *         if (!msg || msg->isEnd()) break;
 *
 *         // Echo back
 *         co_await ctx.send(msg->payload());
 *     }
 *     co_await ctx.end();
 * }
 * @endcode
 */

#ifndef GALAY_RPC_STREAM_H
#define GALAY_RPC_STREAM_H

#include "RpcConn.h"
#include "galay-rpc/protoc/RpcMessage.h"
#include "galay-rpc/protoc/RpcBase.h"
#include "galay-kernel/kernel/Coroutine.h"
#include <atomic>
#include <queue>
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

    /**
     * @brief 序列化流消息
     */
    std::vector<char> serialize(RpcMessageType type) const {
        size_t body_size = m_payload.size();
        std::vector<char> buffer(RPC_HEADER_SIZE + body_size);

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(type);
        header.m_request_id = m_stream_id;  // 复用 request_id 作为 stream_id
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
    const std::string& serviceName() const { return m_service_name; }
    const std::string& methodName() const { return m_method_name; }

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

        uint16_t service_len = htons(static_cast<uint16_t>(m_service_name.size()));
        std::memcpy(body + offset, &service_len, 2);
        offset += 2;
        std::memcpy(body + offset, m_service_name.data(), m_service_name.size());
        offset += m_service_name.size();

        uint16_t method_len = htons(static_cast<uint16_t>(m_method_name.size()));
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
        service_len = ntohs(service_len);
        offset += 2;

        if (offset + service_len > length) return false;
        m_service_name.assign(body + offset, service_len);
        offset += service_len;

        if (offset + 2 > length) return false;
        uint16_t method_len;
        std::memcpy(&method_len, body + offset, 2);
        method_len = ntohs(method_len);
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
template<typename SocketType>
class RpcStreamImpl;

/**
 * @brief 发送流数据等待体
 */
template<typename SocketType>
class SendStreamDataAwaitable {
public:
    SendStreamDataAwaitable(RpcWriterImpl<SocketType>& writer, StreamMessage&& msg, RpcMessageType type)
        : m_writer(writer)
        , m_message(std::move(msg))
        , m_type(type)
        , m_send_awaitable(std::nullopt) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        auto data = m_message.serialize(m_type);
        m_send_awaitable.emplace(m_writer.sendRaw(data.data(), data.size()));
        return m_send_awaitable->await_suspend(handle);
    }

    std::expected<bool, RpcError> await_resume() {
        auto result = m_send_awaitable->await_resume();
        if (!result) {
            return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR, result.error().message()));
        }
        return result.value();
    }

private:
    RpcWriterImpl<SocketType>& m_writer;
    StreamMessage m_message;
    RpcMessageType m_type;
    std::optional<typename RpcWriterImpl<SocketType>::SendRawAwaitable> m_send_awaitable;
};

/**
 * @brief 接收流数据等待体
 */
template<typename SocketType>
class RecvStreamDataAwaitable {
public:
    RecvStreamDataAwaitable(RpcReaderImpl<SocketType>& reader, StreamMessage& msg)
        : m_reader(reader)
        , m_message(msg)
        , m_recv_awaitable(std::nullopt) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        m_recv_awaitable.emplace(m_reader.getStreamMessage(m_message));
        return m_recv_awaitable->await_suspend(handle);
    }

    std::expected<std::optional<StreamMessage>, RpcError> await_resume() {
        auto result = m_recv_awaitable->await_resume();
        if (!result) {
            return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR, result.error().message()));
        }
        if (!result.value()) {
            return std::nullopt;
        }
        return m_message;
    }

private:
    RpcReaderImpl<SocketType>& m_reader;
    StreamMessage& m_message;
    std::optional<typename RpcReaderImpl<SocketType>::GetStreamMessageAwaitable> m_recv_awaitable;
};

/**
 * @brief RPC双向流
 */
template<typename SocketType>
class RpcStreamImpl {
public:
    RpcStreamImpl(SocketType& socket, RingBuffer& ring_buffer,
                  uint32_t stream_id, const RpcReaderSetting& reader_setting,
                  const RpcWriterSetting& writer_setting)
        : m_socket(socket)
        , m_ring_buffer(ring_buffer)
        , m_stream_id(stream_id)
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_closed(false) {}

    uint32_t streamId() const { return m_stream_id; }

    bool isClosed() const { return m_closed.load(); }

    /**
     * @brief 发送流数据
     */
    Coroutine send(const char* data, size_t len) {
        if (m_closed.load()) co_return;

        StreamMessage msg(m_stream_id, data, len);
        auto serialized = msg.serialize(RpcMessageType::STREAM_DATA);

        RpcWriterImpl<SocketType> writer(m_writer_setting, m_socket);
        while (true) {
            auto result = co_await writer.sendRaw(serialized.data(), serialized.size());
            if (!result) co_return;
            if (result.value()) break;
        }
        co_return;
    }

    Coroutine send(const std::string& data) {
        return send(data.data(), data.size());
    }

    /**
     * @brief 接收流数据
     */
    Coroutine recv(StreamMessage& msg) {
        if (m_closed.load()) {
            msg.setEnd(true);
            co_return;
        }

        RpcReaderImpl<SocketType> reader(m_ring_buffer, m_reader_setting, m_socket);

        RpcHeader header;
        while (true) {
            auto result = co_await reader.getHeader(header);
            if (!result) {
                msg.setEnd(true);
                co_return;
            }
            if (result.value()) break;
        }

        // 检查消息类型
        auto type = static_cast<RpcMessageType>(header.m_type);
        if (type == RpcMessageType::STREAM_END || type == RpcMessageType::STREAM_CANCEL) {
            msg.setEnd(true);
            m_closed.store(true);
            co_return;
        }

        if (type != RpcMessageType::STREAM_DATA) {
            msg.setEnd(true);
            co_return;
        }

        msg.streamId(header.m_request_id);

        // 读取消息体
        std::vector<char> body(header.m_body_length);
        while (true) {
            auto result = co_await reader.getBody(body.data(), header.m_body_length);
            if (!result) {
                msg.setEnd(true);
                co_return;
            }
            if (result.value()) break;
        }

        msg.deserializeBody(body.data(), body.size());
        co_return;
    }

    /**
     * @brief 结束流
     */
    Coroutine end() {
        if (m_closed.exchange(true)) co_return;

        StreamMessage msg(m_stream_id, nullptr, 0);
        auto serialized = msg.serialize(RpcMessageType::STREAM_END);

        RpcWriterImpl<SocketType> writer(m_writer_setting, m_socket);
        while (true) {
            auto result = co_await writer.sendRaw(serialized.data(), serialized.size());
            if (!result) co_return;
            if (result.value()) break;
        }
        co_return;
    }

    /**
     * @brief 取消流
     */
    Coroutine cancel() {
        if (m_closed.exchange(true)) co_return;

        StreamMessage msg(m_stream_id, nullptr, 0);
        auto serialized = msg.serialize(RpcMessageType::STREAM_CANCEL);

        RpcWriterImpl<SocketType> writer(m_writer_setting, m_socket);
        while (true) {
            auto result = co_await writer.sendRaw(serialized.data(), serialized.size());
            if (!result) co_return;
            if (result.value()) break;
        }
        co_return;
    }

private:
    SocketType& m_socket;
    RingBuffer& m_ring_buffer;
    uint32_t m_stream_id;
    RpcReaderSetting m_reader_setting;
    RpcWriterSetting m_writer_setting;
    std::atomic<bool> m_closed;
};

// 类型别名
using RpcStream = RpcStreamImpl<TcpSocket>;

/**
 * @brief 服务端流上下文
 */
template<typename SocketType>
class RpcStreamContextImpl {
public:
    RpcStreamContextImpl(RpcStreamImpl<SocketType>& stream,
                         const StreamInitRequest& init_request)
        : m_stream(stream)
        , m_service_name(init_request.serviceName())
        , m_method_name(init_request.methodName()) {}

    uint32_t streamId() const { return m_stream.streamId(); }
    const std::string& serviceName() const { return m_service_name; }
    const std::string& methodName() const { return m_method_name; }

    Coroutine send(const char* data, size_t len) {
        return m_stream.send(data, len);
    }

    Coroutine send(const std::string& data) {
        return m_stream.send(data);
    }

    Coroutine recv(StreamMessage& msg) {
        return m_stream.recv(msg);
    }

    Coroutine end() {
        return m_stream.end();
    }

    Coroutine cancel() {
        return m_stream.cancel();
    }

private:
    RpcStreamImpl<SocketType>& m_stream;
    std::string m_service_name;
    std::string m_method_name;
};

using RpcStreamContext = RpcStreamContextImpl<TcpSocket>;

} // namespace galay::rpc

#endif // GALAY_RPC_STREAM_H
