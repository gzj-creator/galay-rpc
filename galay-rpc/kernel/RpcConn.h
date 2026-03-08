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

#include "RpcAwaitableBase.h"
#include "galay-rpc/protoc/RpcMessage.h"
#include "galay-rpc/protoc/RpcError.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/kernel/Awaitable.h"
#include "galay-kernel/common/Buffer.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <span>

namespace galay::rpc
{

using namespace galay::kernel;
using namespace galay::async;

// 前向声明
template<typename SocketType> class RpcConnImpl;
template<typename SocketType> class RpcReaderImpl;
template<typename SocketType> class RpcWriterImpl;

// RPC 连接默认环形缓冲区大小（8KB）
inline constexpr size_t kDefaultRpcRingBufferSize = 8 * 1024;

namespace detail {

inline size_t iovecsReadableBytes(std::span<const iovec> iovecs) {
    size_t total = 0;
    for (const auto& iov : iovecs) {
        total += iov.iov_len;
    }
    return total;
}

inline bool copyFromIovecs(std::span<const iovec> iovecs,
                           size_t src_offset,
                           char* out,
                           size_t bytes) {
    size_t copied = 0;
    size_t offset = src_offset;

    for (const auto& iov : iovecs) {
        if (copied >= bytes) {
            break;
        }

        if (offset >= iov.iov_len) {
            offset -= iov.iov_len;
            continue;
        }

        const auto* base = static_cast<const char*>(iov.iov_base);
        const size_t available = iov.iov_len - offset;
        const size_t to_copy = std::min(available, bytes - copied);
        std::memcpy(out + copied, base + offset, to_copy);
        copied += to_copy;
        offset = 0;
    }

    return copied == bytes;
}

inline bool payloadViewFromIovecs(std::span<const iovec> iovecs,
                                  size_t src_offset,
                                  size_t bytes,
                                  RpcPayloadView& view) {
    view = RpcPayloadView{};
    if (bytes == 0) {
        return true;
    }

    size_t remaining = bytes;
    size_t offset = src_offset;

    for (const auto& iov : iovecs) {
        if (remaining == 0) {
            break;
        }

        if (offset >= iov.iov_len) {
            offset -= iov.iov_len;
            continue;
        }

        const auto* base = static_cast<const char*>(iov.iov_base);
        const size_t available = iov.iov_len - offset;
        const size_t take = std::min(available, remaining);
        const char* segment_ptr = base + offset;

        if (view.segment1_len == 0) {
            view.segment1 = segment_ptr;
            view.segment1_len = take;
        } else if (view.segment2_len == 0) {
            view.segment2 = segment_ptr;
            view.segment2_len = take;
        } else {
            return false;
        }

        remaining -= take;
        offset = 0;
    }

    return remaining == 0;
}

inline std::expected<size_t, RpcError> tryParseRequestMessage(std::span<const iovec> iovecs,
                                                              size_t total_readable,
                                                              size_t max_message_size,
                                                              RpcRequest& request) {
    if (total_readable < RPC_HEADER_SIZE) {
        return 0;
    }

    char header_buf[RPC_HEADER_SIZE];
    if (!copyFromIovecs(iovecs, 0, header_buf, RPC_HEADER_SIZE)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Failed to read request header"));
    }

    RpcHeader header;
    if (!header.deserialize(header_buf)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid message header"));
    }

    if (header.m_type != static_cast<uint8_t>(RpcMessageType::REQUEST)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Not a request message"));
    }

    const size_t msg_len = RPC_HEADER_SIZE + header.m_body_length;
    if (msg_len > max_message_size + RPC_HEADER_SIZE) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Message too large"));
    }

    if (total_readable < msg_len) {
        return 0;
    }

    size_t cursor = RPC_HEADER_SIZE;
    uint16_t service_len_net = 0;
    if (!copyFromIovecs(iovecs, cursor, reinterpret_cast<char*>(&service_len_net), sizeof(service_len_net))) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid service length"));
    }
    const size_t service_len = rpcNtohs(service_len_net);
    cursor += sizeof(service_len_net);

    if (cursor + service_len > msg_len) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Service field out of range"));
    }
    std::string service_name(service_len, '\0');
    if (service_len > 0 && !copyFromIovecs(iovecs, cursor, service_name.data(), service_len)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid service field"));
    }
    cursor += service_len;

    uint16_t method_len_net = 0;
    if (!copyFromIovecs(iovecs, cursor, reinterpret_cast<char*>(&method_len_net), sizeof(method_len_net))) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid method length"));
    }
    const size_t method_len = rpcNtohs(method_len_net);
    cursor += sizeof(method_len_net);

    if (cursor + method_len > msg_len) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Method field out of range"));
    }
    std::string method_name(method_len, '\0');
    if (method_len > 0 && !copyFromIovecs(iovecs, cursor, method_name.data(), method_len)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid method field"));
    }
    cursor += method_len;

    const size_t payload_len = msg_len - cursor;
    RpcPayloadView payload_view;
    if (!payloadViewFromIovecs(iovecs, cursor, payload_len, payload_view)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid request payload view"));
    }

    request.requestId(header.m_request_id);
    request.callMode(rpcDecodeCallMode(header.m_flags));
    request.endOfStream(rpcIsEndStream(header.m_flags));
    // 直接move进request，避免再做一次字符串拷贝。
    request.serviceName(std::move(service_name));
    request.methodName(std::move(method_name));
    // 借用RingBuffer中的payload内存，避免额外拷贝。
    // 该视图在后续读取覆盖对应缓冲区前有效。
    request.payloadView(payload_view);
    return msg_len;
}

inline std::expected<size_t, RpcError> tryParseResponseMessage(std::span<const iovec> iovecs,
                                                               size_t total_readable,
                                                               size_t max_message_size,
                                                               RpcResponse& response) {
    if (total_readable < RPC_HEADER_SIZE) {
        return 0;
    }

    char header_buf[RPC_HEADER_SIZE];
    if (!copyFromIovecs(iovecs, 0, header_buf, RPC_HEADER_SIZE)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Failed to read response header"));
    }

    RpcHeader header;
    if (!header.deserialize(header_buf)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Invalid message header"));
    }

    if (header.m_type != static_cast<uint8_t>(RpcMessageType::RESPONSE)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Not a response message"));
    }

    const size_t msg_len = RPC_HEADER_SIZE + header.m_body_length;
    if (msg_len > max_message_size + RPC_HEADER_SIZE) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Message too large"));
    }

    if (total_readable < msg_len) {
        return 0;
    }

    if (header.m_body_length < sizeof(uint16_t)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Response body too short"));
    }

    size_t cursor = RPC_HEADER_SIZE;
    uint16_t error_code_net = 0;
    if (!copyFromIovecs(iovecs, cursor, reinterpret_cast<char*>(&error_code_net), sizeof(error_code_net))) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid response error code"));
    }
    cursor += sizeof(error_code_net);

    const size_t payload_len = msg_len - cursor;
    RpcPayloadView payload_view;
    if (!payloadViewFromIovecs(iovecs, cursor, payload_len, payload_view)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid response payload view"));
    }

    response.requestId(header.m_request_id);
    response.callMode(rpcDecodeCallMode(header.m_flags));
    response.endOfStream(rpcIsEndStream(header.m_flags));
    response.errorCode(static_cast<RpcErrorCode>(rpcNtohs(error_code_net)));
    // 借用RingBuffer中的响应payload内存，按需再materialize。
    response.payloadView(payload_view);
    return msg_len;
}

}  // namespace detail

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
class GetRpcRequestAwaitable
    : public RingBufferReadAwaitable<GetRpcRequestAwaitable<SocketType>, SocketType>
{
    using Base = RingBufferReadAwaitable<GetRpcRequestAwaitable<SocketType>, SocketType>;
    friend Base;
public:
    GetRpcRequestAwaitable(RingBuffer& ring_buffer,
                           const RpcReaderSetting& setting,
                           RpcRequest& request,
                           SocketType& socket)
        : Base(ring_buffer, socket)
        , m_setting(setting)
        , m_request(request)
    {}

private:
    std::expected<bool, RpcError> parseFromRingBuffer() {
        if (this->ringBuffer().readable() == 0) {
            return false;
        }

        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = this->ringBuffer().getReadIovecs(read_iovecs);
        if (read_iovecs_count == 0) {
            return false;
        }

        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        const size_t total_readable = detail::iovecsReadableBytes(read_span);
        auto parse_result = detail::tryParseRequestMessage(read_span,
                                                           total_readable,
                                                           m_setting.max_message_size,
                                                           m_request);
        if (!parse_result.has_value()) {
            return std::unexpected(parse_result.error());
        }

        if (parse_result.value() == 0) {
            return false;
        }

        this->ringBuffer().consume(parse_result.value());
        return true;
    }

private:
    const RpcReaderSetting& m_setting;
    RpcRequest& m_request;
};

/**
 * @brief RPC响应读取等待体（使用readv）
 */
template<typename SocketType>
class GetRpcResponseAwaitable
    : public RingBufferReadAwaitable<GetRpcResponseAwaitable<SocketType>, SocketType>
{
    using Base = RingBufferReadAwaitable<GetRpcResponseAwaitable<SocketType>, SocketType>;
    friend Base;
public:
    GetRpcResponseAwaitable(RingBuffer& ring_buffer,
                            const RpcReaderSetting& setting,
                            RpcResponse& response,
                            SocketType& socket)
        : Base(ring_buffer, socket)
        , m_setting(setting)
        , m_response(response)
    {}

private:
    std::expected<bool, RpcError> parseFromRingBuffer() {
        if (this->ringBuffer().readable() == 0) {
            return false;
        }

        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = this->ringBuffer().getReadIovecs(read_iovecs);
        if (read_iovecs_count == 0) {
            return false;
        }

        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        const size_t total_readable = detail::iovecsReadableBytes(read_span);
        auto parse_result = detail::tryParseResponseMessage(read_span,
                                                            total_readable,
                                                            m_setting.max_message_size,
                                                            m_response);
        if (!parse_result.has_value()) {
            return std::unexpected(parse_result.error());
        }

        if (parse_result.value() == 0) {
            return false;
        }

        this->ringBuffer().consume(parse_result.value());
        return true;
    }

private:
    const RpcReaderSetting& m_setting;
    RpcResponse& m_response;
};

/**
 * @brief RPC发送请求等待体（使用writev）
 */
template<typename SocketType>
class SendRpcRequestAwaitable
    : public ResumableWriteAwaitable<SendRpcRequestAwaitable<SocketType>, SocketType>
{
    using Base = ResumableWriteAwaitable<SendRpcRequestAwaitable<SocketType>, SocketType>;
public:
    SendRpcRequestAwaitable(const RpcRequest& request, SocketType& socket)
        : Base(socket)
        , m_request(request)
    {
        rebuildIovecs();
    }

private:
    void rebuildIovecs() {
        RpcPayloadView payload_view = m_request.payloadView();
        const size_t body_size =
            sizeof(uint16_t) + m_request.serviceName().size() +
            sizeof(uint16_t) + m_request.methodName().size() +
            payload_view.size();

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::REQUEST);
        header.m_flags = rpcEncodeFlags(m_request.callMode(), m_request.endOfStream());
        header.m_request_id = m_request.requestId();
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(m_header.data());

        m_service_len = rpcHtons(static_cast<uint16_t>(m_request.serviceName().size()));
        m_method_len = rpcHtons(static_cast<uint16_t>(m_request.methodName().size()));
        this->m_total_bytes = RPC_HEADER_SIZE + body_size;

        this->m_iovecs.clear();
        this->m_iovecs.reserve(6);
        this->m_iovecs.push_back(iovec{m_header.data(), RPC_HEADER_SIZE});
        this->m_iovecs.push_back(iovec{&m_service_len, sizeof(m_service_len)});

        if (!m_request.serviceName().empty()) {
            this->m_iovecs.push_back(iovec{
                const_cast<char*>(m_request.serviceName().data()),
                m_request.serviceName().size()
            });
        }

        this->m_iovecs.push_back(iovec{&m_method_len, sizeof(m_method_len)});
        if (!m_request.methodName().empty()) {
            this->m_iovecs.push_back(iovec{
                const_cast<char*>(m_request.methodName().data()),
                m_request.methodName().size()
            });
        }

        if (payload_view.segment1_len > 0) {
            this->m_iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment1),
                payload_view.segment1_len
            });
        }

        if (payload_view.segment2_len > 0) {
            this->m_iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment2),
                payload_view.segment2_len
            });
        }
    }

private:
    const RpcRequest& m_request;
    std::array<char, RPC_HEADER_SIZE> m_header{};
    uint16_t m_service_len = 0;
    uint16_t m_method_len = 0;
};

/**
 * @brief RPC发送响应等待体（使用writev）
 */
template<typename SocketType>
class SendRpcResponseAwaitable
    : public ResumableWriteAwaitable<SendRpcResponseAwaitable<SocketType>, SocketType>
{
    using Base = ResumableWriteAwaitable<SendRpcResponseAwaitable<SocketType>, SocketType>;
public:
    SendRpcResponseAwaitable(const RpcResponse& response, SocketType& socket)
        : Base(socket)
        , m_response(response)
    {
        rebuildIovecs();
    }

private:
    void rebuildIovecs() {
        RpcPayloadView payload_view = m_response.payloadView();
        const size_t body_size = sizeof(uint16_t) + payload_view.size();

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::RESPONSE);
        header.m_flags = rpcEncodeFlags(m_response.callMode(), m_response.endOfStream());
        header.m_request_id = m_response.requestId();
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(m_header.data());

        m_error_code = rpcHtons(static_cast<uint16_t>(m_response.errorCode()));
        this->m_total_bytes = RPC_HEADER_SIZE + body_size;

        this->m_iovecs.clear();
        this->m_iovecs.reserve(3);
        this->m_iovecs.push_back(iovec{m_header.data(), RPC_HEADER_SIZE});
        this->m_iovecs.push_back(iovec{&m_error_code, sizeof(m_error_code)});

        if (payload_view.segment1_len > 0) {
            this->m_iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment1),
                payload_view.segment1_len
            });
        }

        if (payload_view.segment2_len > 0) {
            this->m_iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment2),
                payload_view.segment2_len
            });
        }
    }

private:
    const RpcResponse& m_response;
    std::array<char, RPC_HEADER_SIZE> m_header{};
    uint16_t m_error_code = 0;
};

/**
 * @brief 发送原始数据等待体（用于流式传输）
 */
template<typename SocketType>
class SendRawDataAwaitable
    : public ResumableWriteAwaitable<SendRawDataAwaitable<SocketType>, SocketType>
{
    using Base = ResumableWriteAwaitable<SendRawDataAwaitable<SocketType>, SocketType>;
public:
    SendRawDataAwaitable(std::vector<char>&& data, SocketType& socket)
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
 * @brief 读取消息头等待体（用于流式传输）
 */
template<typename SocketType>
class GetRpcHeaderAwaitable
    : public RingBufferReadAwaitable<GetRpcHeaderAwaitable<SocketType>, SocketType>
{
    using Base = RingBufferReadAwaitable<GetRpcHeaderAwaitable<SocketType>, SocketType>;
    friend Base;
public:
    GetRpcHeaderAwaitable(RingBuffer& ring_buffer, RpcHeader& header, SocketType& socket)
        : Base(ring_buffer, socket)
        , m_header(header)
    {}

private:
    std::expected<bool, RpcError> parseFromRingBuffer() {
        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = this->ringBuffer().getReadIovecs(read_iovecs);
        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        if (detail::iovecsReadableBytes(read_span) < RPC_HEADER_SIZE) {
            return false;
        }

        char header_buf[RPC_HEADER_SIZE];
        detail::copyFromIovecs(read_span, 0, header_buf, RPC_HEADER_SIZE);

        if (!m_header.deserialize(header_buf)) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid header"));
        }

        this->ringBuffer().consume(RPC_HEADER_SIZE);
        return true;
    }

private:
    RpcHeader& m_header;
};

/**
 * @brief 读取消息体等待体（用于流式传输）
 */
template<typename SocketType>
class GetRpcBodyAwaitable
    : public RingBufferReadAwaitable<GetRpcBodyAwaitable<SocketType>, SocketType>
{
    using Base = RingBufferReadAwaitable<GetRpcBodyAwaitable<SocketType>, SocketType>;
    friend Base;
public:
    GetRpcBodyAwaitable(RingBuffer& ring_buffer, char* body, size_t body_len, SocketType& socket)
        : Base(ring_buffer, socket)
        , m_body(body)
        , m_body_len(body_len)
    {}

private:
    std::expected<bool, RpcError> parseFromRingBuffer() {
        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = this->ringBuffer().getReadIovecs(read_iovecs);
        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        if (detail::iovecsReadableBytes(read_span) < m_body_len) {
            return false;
        }

        detail::copyFromIovecs(read_span, 0, m_body, m_body_len);
        this->ringBuffer().consume(m_body_len);
        return true;
    }

private:
    char* m_body;
    size_t m_body_len;
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
     * @return 等待体，co_await返回后表示请求已完整解析
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
    static constexpr size_t kDefaultRingBufferSize = kDefaultRpcRingBufferSize;

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

    SocketType m_socket;
    RingBuffer m_ring_buffer;
    RpcReaderSetting m_reader_setting;
    RpcWriterSetting m_writer_setting;
};

// 类型别名
using RpcConn = RpcConnImpl<TcpSocket>;

} // namespace galay::rpc

#endif // GALAY_RPC_CONN_H
