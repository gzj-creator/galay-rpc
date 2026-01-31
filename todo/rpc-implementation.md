# Galay-RPC 实现待办列表

## 核心模块

- [true] 1. 项目结构搭建 - CMakeLists.txt、目录结构
- [true] 2. RPC协议设计 - 消息格式、序列化/反序列化
- [true] 3. RPC服务端实现 - RpcServer、服务注册、请求分发
- [true] 4. RPC客户端实现 - RpcClient、请求发送、响应接收
- [true] 5. 服务定义接口 - Service基类、方法注册宏
- [true] 6. 连接管理 - RpcConn、RingBuffer配合readv/writev
- [true] 7. 错误处理 - RpcError、错误码定义

## 测试模块

- [true] 8. 单元测试 - 协议解析、序列化测试
- [true] 9. 集成测试 - 客户端服务端通信测试
- [true] 10. 压力测试 - QPS、延迟测试

## 文档

- [true] 11. API文档 - README.md
- [true] 12. 使用示例 - E1-EchoServer, E2-EchoClient
