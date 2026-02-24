module;

#include "galay-rpc/module/ModulePrelude.hpp"

export module galay.rpc;

export {
#include "galay-rpc/protoc/RpcBase.h"
#include "galay-rpc/protoc/RpcError.h"
#include "galay-rpc/protoc/RpcMessage.h"
#include "galay-rpc/protoc/RpcCodec.h"

#include "galay-rpc/kernel/RpcConn.h"
#include "galay-rpc/kernel/RpcService.h"
#include "galay-rpc/kernel/RpcServer.h"
#include "galay-rpc/kernel/RpcStreamServer.h"
#include "galay-rpc/kernel/RpcClient.h"
#include "galay-rpc/kernel/RpcStream.h"
#include "galay-rpc/kernel/ServiceDiscovery.h"
}
