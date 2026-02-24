/**
 * @file RpcStream.h
 * @brief RPC双向流支持
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供双向流式RPC调用支持，参考 galay-http WebSocket 的设计模式。
 *
 * 使用方式（单次co_await等待完整发送/接收）：
 * @code
 * // 客户端双向流
 * Coroutine biStreamExample(RpcStream& stream) {
 *     auto& writer = stream.getWriter();
 *     auto& reader = stream.getReader();
 *
 *     // 发送消息（内部会自动续发直到完成）
 *     auto send_result = co_await writer.sendData("Hello");
 *     if (!send_result) {
 *         co_return;
 *     }
 *
 *     // 接收消息（内部会自动续读直到完整消息）
 *     StreamMessage msg;
 *     auto recv_result = co_await reader.getMessage(msg);
 *     if (!recv_result) {
 *         co_return;
 *     }
 *     std::cout << "Received: " << msg.payloadStr() << "\n";
 *
 *     // 结束流
 *     auto end_result = co_await writer.sendEnd();
 *     if (!end_result) {
 *         co_return;
 *     }
 * }
 * @endcode
 */

#ifndef GALAY_RPC_STREAM_H
#define GALAY_RPC_STREAM_H

#include "RpcConn.h"
#include "galay-rpc/protoc/RpcMessage.h"
#include "galay-rpc/protoc/RpcBase.h"
#include <string>
#include <utility>

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
class SendStreamDataAwaitable
    : public ResumableWriteAwaitable<SendStreamDataAwaitable<SocketType>, SocketType>
{
    using Base = ResumableWriteAwaitable<SendStreamDataAwaitable<SocketType>, SocketType>;
public:
    SendStreamDataAwaitable(SocketType& socket, std::vector<char>&& data)
        : Base(socket)
        , m_data(std::move(data))
    {
        this->m_total_bytes = m_data.size();
        if (this->m_total_bytes > 0) {
            this->m_iovecs.push_back(iovec{m_data.data(), this->m_total_bytes});
        }
    }

    bool await_ready() const noexcept { return m_data.empty(); }

    std::expected<bool, RpcError> await_resume() {
        if (m_data.empty()) {
            return true;
        }
        return Base::await_resume();
    }

private:
    std::vector<char> m_data;
};

/**
 * @brief 流消息接收等待体
 */
template<typename SocketType>
class GetStreamMessageAwaitable
    : public RingBufferReadAwaitable<GetStreamMessageAwaitable<SocketType>, SocketType>
{
    using Base = RingBufferReadAwaitable<GetStreamMessageAwaitable<SocketType>, SocketType>;
    friend Base;

public:
    GetStreamMessageAwaitable(RingBuffer& ring_buffer, SocketType& socket, StreamMessage& msg)
        : Base(ring_buffer, socket)
        , m_message(msg)
        , m_state(State::ReadHeader)
        , m_body_length(0)
    {}

private:
    enum class State { ReadHeader, ReadBody };

    std::expected<bool, RpcError> parseFromRingBuffer() {
        auto& rb = this->ringBuffer();
        auto read_iovecs = rb.getReadIovecs();
        size_t total_readable = detail::iovecsReadableBytes(read_iovecs);

        if (m_state == State::ReadHeader) {
            if (total_readable < RPC_HEADER_SIZE) {
                return false;
            }

            char header_buf[RPC_HEADER_SIZE];
            detail::copyFromIovecs(read_iovecs, 0, header_buf, RPC_HEADER_SIZE);

            if (!m_header.deserialize(header_buf)) {
                return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid header"));
            }

            rb.consume(RPC_HEADER_SIZE);
            m_body_length = m_header.m_body_length;
            m_message.streamId(m_header.m_request_id);

            auto msg_type = static_cast<RpcMessageType>(m_header.m_type);
            m_message.messageType(msg_type);

            if (msg_type == RpcMessageType::STREAM_END || msg_type == RpcMessageType::STREAM_CANCEL) {
                m_message.setEnd(true);
                m_state = State::ReadHeader;
                return true;
            }

            if (m_body_length == 0) {
                m_state = State::ReadHeader;
                return true;
            }

            m_state = State::ReadBody;
        }

        if (m_state == State::ReadBody) {
            read_iovecs = rb.getReadIovecs();
            if (detail::iovecsReadableBytes(read_iovecs) < m_body_length) {
                return false;
            }

            std::vector<char> body(m_body_length);
            detail::copyFromIovecs(read_iovecs, 0, body.data(), m_body_length);

            m_message.deserializeBody(body.data(), body.size());
            rb.consume(m_body_length);
            m_state = State::ReadHeader;
            return true;
        }

        return false;
    }

    StreamMessage& m_message;
    State m_state;
    RpcHeader m_header;
    size_t m_body_length;
};

/**
 * @brief 流读取器
 */
template<typename SocketType>
class StreamReaderImpl {
public:
    StreamReaderImpl(RingBuffer& ring_buffer, SocketType& socket)
        : m_ring_buffer(&ring_buffer)
        , m_socket(&socket)
    {}

    /**
     * @brief 获取流消息
     * @param msg 输出消息
     * @return 等待体，co_await返回后表示一条完整流消息
     */
    GetStreamMessageAwaitable<SocketType> getMessage(StreamMessage& msg) {
        return GetStreamMessageAwaitable<SocketType>(*m_ring_buffer, *m_socket, msg);
    }

private:
    RingBuffer* m_ring_buffer = nullptr;
    SocketType* m_socket = nullptr;
};

/**
 * @brief 流写入器
 */
template<typename SocketType>
class StreamWriterImpl {
public:
    StreamWriterImpl(SocketType& socket, uint32_t stream_id)
        : m_socket(&socket)
        , m_stream_id(stream_id)
    {}

    /**
     * @brief 发送流数据
     */
    SendStreamDataAwaitable<SocketType> sendData(const char* data, size_t len) {
        StreamMessage msg(m_stream_id, data, len);
        return SendStreamDataAwaitable<SocketType>(*m_socket, msg.serialize(RpcMessageType::STREAM_DATA));
    }

    SendStreamDataAwaitable<SocketType> sendData(const std::string& data) {
        return sendData(data.data(), data.size());
    }

    /**
     * @brief 发送流初始化请求
     */
    SendStreamDataAwaitable<SocketType> sendInit(const std::string& service, const std::string& method) {
        StreamInitRequest init(m_stream_id, service, method);
        return SendStreamDataAwaitable<SocketType>(*m_socket, init.serialize());
    }

    /**
     * @brief 发送流初始化确认
     */
    SendStreamDataAwaitable<SocketType> sendInitAck() {
        StreamMessage msg(m_stream_id, nullptr, 0);
        return SendStreamDataAwaitable<SocketType>(*m_socket, msg.serialize(RpcMessageType::STREAM_INIT_ACK));
    }

    /**
     * @brief 发送流结束
     */
    SendStreamDataAwaitable<SocketType> sendEnd() {
        StreamMessage msg(m_stream_id, nullptr, 0);
        return SendStreamDataAwaitable<SocketType>(*m_socket, msg.serialize(RpcMessageType::STREAM_END));
    }

    /**
     * @brief 发送流取消
     */
    SendStreamDataAwaitable<SocketType> sendCancel() {
        StreamMessage msg(m_stream_id, nullptr, 0);
        return SendStreamDataAwaitable<SocketType>(*m_socket, msg.serialize(RpcMessageType::STREAM_CANCEL));
    }

private:
    SocketType* m_socket = nullptr;
    uint32_t m_stream_id;
};

/**
 * @brief RPC流会话
 */
template<typename SocketType>
class RpcStreamImpl {
public:
    RpcStreamImpl(SocketType& socket,
                  RingBuffer& ring_buffer,
                  uint32_t stream_id,
                  std::string service_name = {},
                  std::string method_name = {})
        : m_socket(&socket)
        , m_ring_buffer(&ring_buffer)
        , m_stream_id(stream_id)
        , m_service_name(std::move(service_name))
        , m_method_name(std::move(method_name))
        , m_reader(ring_buffer, socket)
        , m_writer(socket, stream_id)
    {}

    uint32_t streamId() const { return m_stream_id; }
    const std::string& serviceName() const { return m_service_name; }
    const std::string& methodName() const { return m_method_name; }

    void setRoute(std::string service_name, std::string method_name) {
        m_service_name = std::move(service_name);
        m_method_name = std::move(method_name);
    }

    StreamReaderImpl<SocketType>& getReader() { return m_reader; }
    StreamWriterImpl<SocketType>& getWriter() { return m_writer; }

    GetStreamMessageAwaitable<SocketType> read(StreamMessage& msg) {
        return m_reader.getMessage(msg);
    }

    SendStreamDataAwaitable<SocketType> sendInit() {
        return m_writer.sendInit(m_service_name, m_method_name);
    }

    SendStreamDataAwaitable<SocketType> sendInit(const std::string& service, const std::string& method) {
        m_service_name = service;
        m_method_name = method;
        return m_writer.sendInit(service, method);
    }

    SendStreamDataAwaitable<SocketType> sendInitAck() { return m_writer.sendInitAck(); }
    SendStreamDataAwaitable<SocketType> sendData(const char* data, size_t len) { return m_writer.sendData(data, len); }
    SendStreamDataAwaitable<SocketType> sendData(const std::string& data) { return m_writer.sendData(data); }
    SendStreamDataAwaitable<SocketType> sendEnd() { return m_writer.sendEnd(); }
    SendStreamDataAwaitable<SocketType> sendCancel() { return m_writer.sendCancel(); }

    SocketType& socket() { return *m_socket; }
    RingBuffer& ringBuffer() { return *m_ring_buffer; }

private:
    SocketType* m_socket = nullptr;
    RingBuffer* m_ring_buffer = nullptr;
    uint32_t m_stream_id;
    std::string m_service_name;
    std::string m_method_name;
    StreamReaderImpl<SocketType> m_reader;
    StreamWriterImpl<SocketType> m_writer;
};

// 类型别名
using StreamReader = StreamReaderImpl<TcpSocket>;
using StreamWriter = StreamWriterImpl<TcpSocket>;
using RpcStream = RpcStreamImpl<TcpSocket>;

} // namespace galay::rpc

#endif // GALAY_RPC_STREAM_H
