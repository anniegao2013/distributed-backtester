#pragma once

#include "comm/tcp_channel.h"

// Backwards-compatible worker-facing name for the TCP client channel.
//
// The implementation lives in the shared communication layer so the
// coordinator and worker use the same framing and connection semantics.
using TcpTransport = comm::TcpClientChannel;
