#pragma once

// Internal: the per-connection request loop run by the server's workers.

#include "http/FileDescriptor.hpp"
#include "http/Server.hpp"

namespace http::detail {

// Serves requests on `connection` until the client closes it, a timeout or
// error ends it, or `stopFd` becomes readable.
void serveConnection(
    FileDescriptor connection, const Handler& handler, const ServerConfig& config, int stopFd);

} // namespace http::detail
