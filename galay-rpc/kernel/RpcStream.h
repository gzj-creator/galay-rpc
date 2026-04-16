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
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <span>
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
        , m_is_end(false)
    {
        if (data != nullptr && len > 0) {
            payload(data, len);
        }
    }

    uint32_t streamId() const { return m_stream_id; }
    void streamId(uint32_t id) { m_stream_id = id; }

    const std::vector<char>& payload() const {
        materializePayloadIfNeeded();
        return m_payload;
    }
    size_t payloadSize() const {
        return m_payload_owned ? m_payload.size() : m_payload_view.size();
    }
    /**
     * @brief 获取当前 payload 视图
     * @return 若消息持有自有缓冲则返回单段视图；若消息借用外部内存则返回对应借用视图
     *
     * @note 借用视图的生命周期由外部内存决定；若需要长期持有，请调用 `payload()` /
     *       `payloadStr()` 触发实体化副本。
     */
    RpcPayloadView payloadView() const {
        if (m_payload_owned) {
            return RpcPayloadView{
                m_payload.data(),
                m_payload.size(),
                nullptr,
                0
            };
        }
        return m_payload_view;
    }
    std::string payloadStr() const {
        materializePayloadIfNeeded();
        return std::string(m_payload.begin(), m_payload.end());
    }
    bool payloadEquals(std::string_view expected) const {
        const auto view = payloadView();
        if (view.size() != expected.size()) {
            return false;
        }
        if (view.segment1_len > 0 &&
            std::memcmp(view.segment1, expected.data(), view.segment1_len) != 0) {
            return false;
        }
        if (view.segment2_len > 0 &&
            std::memcmp(view.segment2,
                        expected.data() + view.segment1_len,
                        view.segment2_len) != 0) {
            return false;
        }
        return true;
    }

    void payload(const char* data, size_t len) {
        m_payload.assign(data, data + len);
        m_payload_owned = true;
        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
    }
    void payload(const std::string& data) {
        m_payload.assign(data.begin(), data.end());
        m_payload_owned = true;
        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
    }
    /**
     * @brief 设置借用型 payload 视图
     * @param view 外部 payload 视图
     *
     * @note 调用方必须保证 `view` 指向的内存在消息被消费完成前保持有效；
     *       如需脱离外部缓冲长期持有，请改用 `payload(...)`。
     */
    void payloadView(const RpcPayloadView& view) {
        m_payload.clear();
        m_payload_view = view;
        m_payload_owned = false;
    }

    bool isEnd() const { return m_is_end; }
    void setEnd(bool end = true) { m_is_end = end; }

    RpcMessageType messageType() const { return m_msg_type; }
    void messageType(RpcMessageType type) { m_msg_type = type; }

    /**
     * @brief 序列化流消息
     */
    std::vector<char> serialize(RpcMessageType type) const {
        const RpcPayloadView payload_view = payloadView();
        size_t body_size = payload_view.size();
        std::vector<char> buffer(RPC_HEADER_SIZE + body_size);

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(type);
        header.m_request_id = m_stream_id;
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(buffer.data());

        size_t offset = RPC_HEADER_SIZE;
        if (payload_view.segment1_len > 0) {
            std::memcpy(buffer.data() + offset, payload_view.segment1, payload_view.segment1_len);
            offset += payload_view.segment1_len;
        }
        if (payload_view.segment2_len > 0) {
            std::memcpy(buffer.data() + offset, payload_view.segment2, payload_view.segment2_len);
        }

        return buffer;
    }

    /**
     * @brief 反序列化流消息体
     */
    bool deserializeBody(const char* body, size_t length) {
        if (length > 0) {
            payload(body, length);
        } else {
            m_payload.clear();
            m_payload_view = RpcPayloadView{};
            m_payload_owned = true;
        }
        return true;
    }

private:
    void materializePayloadIfNeeded() const {
        if (m_payload_owned) {
            return;
        }

        const size_t total = m_payload_view.size();
        m_payload.resize(total);
        size_t offset = 0;
        if (m_payload_view.segment1_len > 0) {
            std::memcpy(m_payload.data(), m_payload_view.segment1, m_payload_view.segment1_len);
            offset += m_payload_view.segment1_len;
        }
        if (m_payload_view.segment2_len > 0) {
            std::memcpy(m_payload.data() + offset, m_payload_view.segment2, m_payload_view.segment2_len);
        }

        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
        m_payload_owned = true;
    }

private:
    uint32_t m_stream_id = 0;
    mutable std::vector<char> m_payload;
    mutable RpcPayloadView m_payload_view{};
    mutable bool m_payload_owned = true;
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

namespace detail {

class StreamMessageReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult>
{
public:
    StreamMessageReadState(RingBuffer& ring_buffer, StreamMessage& message)
        : RpcRingBufferReadStateBase<RpcAwaitableResult>(ring_buffer)
        , m_message(&message)
    {
    }

    bool parseFromRingBuffer()
    {
        auto& rb = ringBuffer();
        std::array<struct iovec, 2> read_iovecs_storage{};
        size_t read_iovecs_count = rb.getReadIovecs(read_iovecs_storage);
        std::span<const struct iovec> read_iovecs(read_iovecs_storage.data(), read_iovecs_count);
        size_t total_readable = iovecsReadableBytes(read_iovecs);

        if (m_state == State::ReadHeader) {
            if (total_readable < RPC_HEADER_SIZE) {
                return false;
            }

            char header_buf[RPC_HEADER_SIZE];
            copyFromIovecs(read_iovecs, 0, header_buf, RPC_HEADER_SIZE);

            if (!m_header.deserialize(header_buf)) {
                setReadError(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid header"));
                return true;
            }

            rb.consume(RPC_HEADER_SIZE);
            m_body_length = m_header.m_body_length;
            m_message->streamId(m_header.m_request_id);

            auto msg_type = static_cast<RpcMessageType>(m_header.m_type);
            m_message->messageType(msg_type);

            if (msg_type == RpcMessageType::STREAM_END || msg_type == RpcMessageType::STREAM_CANCEL) {
                m_message->setEnd(true);
                m_message->payloadView(RpcPayloadView{});
                m_state = State::ReadHeader;
                return true;
            }

            if (m_body_length == 0) {
                m_message->payloadView(RpcPayloadView{});
                m_state = State::ReadHeader;
                return true;
            }

            m_state = State::ReadBody;
        }

        if (m_state == State::ReadBody) {
            read_iovecs_count = rb.getReadIovecs(read_iovecs_storage);
            read_iovecs = std::span<const struct iovec>(read_iovecs_storage.data(), read_iovecs_count);
            if (iovecsReadableBytes(read_iovecs) < m_body_length) {
                return false;
            }

            RpcPayloadView payload_view;
            if (!payloadViewFromIovecs(read_iovecs, 0, m_body_length, payload_view)) {
                setReadError(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid stream payload view"));
                return true;
            }

            m_message->payloadView(payload_view);
            rb.consume(m_body_length);
            m_state = State::ReadHeader;
            return true;
        }

        return false;
    }

private:
    enum class State {
        ReadHeader,
        ReadBody
    };

    StreamMessage* m_message = nullptr;
    State m_state = State::ReadHeader;
    RpcHeader m_header;
    size_t m_body_length = 0;
};

class StreamFrameWriteState : public RpcWriteStateBase<RpcAwaitableResult>
{
public:
    StreamFrameWriteState(uint32_t stream_id, RpcMessageType type)
    {
        RpcHeader header;
        header.m_type = static_cast<uint8_t>(type);
        header.m_request_id = stream_id;
        header.m_body_length = 0;
        header.serialize(m_header.data());

        auto& iovecs = mutableIovecs();
        iovecs.reserve(1);
        iovecs.push_back(iovec{m_header.data(), RPC_HEADER_SIZE});
    }

    StreamFrameWriteState(uint32_t stream_id, const char* data, size_t len)
    {
        if (len > 0) {
            m_owned_payload.assign(data, data + len);
        }
        buildDataFrame(stream_id, payloadView());
    }

    StreamFrameWriteState(uint32_t stream_id, const RpcPayloadView& payload_view)
    {
        buildDataFrame(stream_id, payload_view);
    }

    StreamFrameWriteState(uint32_t stream_id,
                          std::string_view service,
                          std::string_view method)
        : m_service(service)
        , m_method(method)
    {
        const size_t body_size = 2 + m_service.size() + 2 + m_method.size();

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::STREAM_INIT);
        header.m_request_id = stream_id;
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(m_header.data());

        m_service_len = rpcHtons(static_cast<uint16_t>(m_service.size()));
        m_method_len = rpcHtons(static_cast<uint16_t>(m_method.size()));
        auto& iovecs = mutableIovecs();
        iovecs.reserve(5);
        iovecs.push_back(iovec{m_header.data(), RPC_HEADER_SIZE});
        iovecs.push_back(iovec{&m_service_len, sizeof(m_service_len)});
        if (!m_service.empty()) {
            iovecs.push_back(iovec{const_cast<char*>(m_service.data()), m_service.size()});
        }
        iovecs.push_back(iovec{&m_method_len, sizeof(m_method_len)});
        if (!m_method.empty()) {
            iovecs.push_back(iovec{const_cast<char*>(m_method.data()), m_method.size()});
        }
    }

private:
    RpcPayloadView payloadView() const
    {
        if (m_owned_payload.empty()) {
            return RpcPayloadView{};
        }
        return RpcPayloadView{
            m_owned_payload.data(),
            m_owned_payload.size(),
            nullptr,
            0
        };
    }

    void buildDataFrame(uint32_t stream_id, const RpcPayloadView& payload_view)
    {
        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::STREAM_DATA);
        header.m_request_id = stream_id;
        header.m_body_length = static_cast<uint32_t>(payload_view.size());
        header.serialize(m_header.data());
        auto& iovecs = mutableIovecs();
        iovecs.reserve(3);
        iovecs.push_back(iovec{m_header.data(), RPC_HEADER_SIZE});
        if (payload_view.segment1_len > 0) {
            iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment1),
                payload_view.segment1_len
            });
        }
        if (payload_view.segment2_len > 0) {
            iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment2),
                payload_view.segment2_len
            });
        }
    }

    std::array<char, RPC_HEADER_SIZE> m_header{};
    std::vector<char> m_owned_payload;
    std::string m_service;
    std::string m_method;
    uint16_t m_service_len = 0;
    uint16_t m_method_len = 0;
};

}  // namespace detail

/**
 * @brief 流数据发送等待体
 */
template<typename SocketType>
class SendStreamDataAwaitable : public TimeoutSupport<SendStreamDataAwaitable<SocketType>>
{
public:
    using Result = detail::RpcAwaitableResult;

    SendStreamDataAwaitable(SocketType& socket, std::shared_ptr<detail::StreamFrameWriteState> state)
        : m_state(std::move(state))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                socket.controller(),
                detail::RpcWritevMachine<detail::StreamFrameWriteState>(m_state))
                .build())
    {}

    SendStreamDataAwaitable(SendStreamDataAwaitable&&) noexcept = default;
    SendStreamDataAwaitable& operator=(SendStreamDataAwaitable&&) noexcept = default;
    SendStreamDataAwaitable(const SendStreamDataAwaitable&) = delete;
    SendStreamDataAwaitable& operator=(const SendStreamDataAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcWritevMachine<detail::StreamFrameWriteState>>;

    std::shared_ptr<detail::StreamFrameWriteState> m_state;
    InnerAwaitable m_inner;
};

/**
 * @brief 流消息接收等待体
 */
template<typename SocketType>
class GetStreamMessageAwaitable : public TimeoutSupport<GetStreamMessageAwaitable<SocketType>>
{
public:
    using Result = detail::RpcAwaitableResult;

    GetStreamMessageAwaitable(RingBuffer& ring_buffer, SocketType& socket, StreamMessage& msg)
        : m_state(std::make_shared<detail::StreamMessageReadState>(ring_buffer, msg))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                socket.controller(),
                detail::RpcRingBufferReadMachine<detail::StreamMessageReadState>(m_state))
                .build())
    {}

    GetStreamMessageAwaitable(GetStreamMessageAwaitable&&) noexcept = default;
    GetStreamMessageAwaitable& operator=(GetStreamMessageAwaitable&&) noexcept = default;
    GetStreamMessageAwaitable(const GetStreamMessageAwaitable&) = delete;
    GetStreamMessageAwaitable& operator=(const GetStreamMessageAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<detail::StreamMessageReadState>>;

    std::shared_ptr<detail::StreamMessageReadState> m_state;
    InnerAwaitable m_inner;
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
     * @brief 更新当前写入器绑定的 stream_id
     * @param stream_id 后续发送帧要使用的逻辑流 ID
     */
    void streamId(uint32_t stream_id) { m_stream_id = stream_id; }

    /**
     * @brief 发送流数据
     */
    SendStreamDataAwaitable<SocketType> sendData(const char* data, size_t len) {
        return SendStreamDataAwaitable<SocketType>(
            *m_socket,
            std::make_shared<detail::StreamFrameWriteState>(m_stream_id, data, len));
    }

    SendStreamDataAwaitable<SocketType> sendData(const std::string& data) {
        return sendData(data.data(), data.size());
    }

    /**
     * @brief 发送借用型 payload 视图
     * @param payload_view 调用方拥有的 payload 视图
     *
     * @note 调用方需保证 payload 所指向内存在本次 `co_await` 完成前保持有效。
     */
    SendStreamDataAwaitable<SocketType> sendData(const RpcPayloadView& payload_view) {
        return SendStreamDataAwaitable<SocketType>(
            *m_socket,
            std::make_shared<detail::StreamFrameWriteState>(m_stream_id, payload_view));
    }

    /**
     * @brief 发送流初始化请求
     */
    SendStreamDataAwaitable<SocketType> sendInit(const std::string& service, const std::string& method) {
        return SendStreamDataAwaitable<SocketType>(
            *m_socket,
            std::make_shared<detail::StreamFrameWriteState>(m_stream_id, service, method));
    }

    /**
     * @brief 发送流初始化确认
     */
    SendStreamDataAwaitable<SocketType> sendInitAck() {
        return SendStreamDataAwaitable<SocketType>(
            *m_socket,
            std::make_shared<detail::StreamFrameWriteState>(m_stream_id, RpcMessageType::STREAM_INIT_ACK));
    }

    /**
     * @brief 发送流结束
     */
    SendStreamDataAwaitable<SocketType> sendEnd() {
        return SendStreamDataAwaitable<SocketType>(
            *m_socket,
            std::make_shared<detail::StreamFrameWriteState>(m_stream_id, RpcMessageType::STREAM_END));
    }

    /**
     * @brief 发送流取消
     */
    SendStreamDataAwaitable<SocketType> sendCancel() {
        return SendStreamDataAwaitable<SocketType>(
            *m_socket,
            std::make_shared<detail::StreamFrameWriteState>(m_stream_id, RpcMessageType::STREAM_CANCEL));
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
    /**
     * @brief 更新逻辑流 ID，并同步到底层写入器
     * @param stream_id 新的逻辑流 ID
     */
    void streamId(uint32_t stream_id) {
        m_stream_id = stream_id;
        m_writer.streamId(stream_id);
    }
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
    /**
     * @brief 发送借用型 payload 视图
     * @param payload_view 调用方拥有的 payload 视图
     *
     * @note 调用方需保证 payload 所指向内存在本次 `co_await` 完成前保持有效。
     */
    SendStreamDataAwaitable<SocketType> sendData(const RpcPayloadView& payload_view) { return m_writer.sendData(payload_view); }
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
