#include "HttpClient.h"
#include "NPCBotsConfig.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include "Log.h"

#include <sstream>
#include <string>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cctype>

/// Escape JSON properly (THIS FIXES YOUR ERROR)
static std::string EscapeJson(const std::string& input)
{
    std::string output;

    for (char c : input)
    {
        switch (c)
        {
            case '\"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += c; break;
        }
    }

    return output;
}

static timeval MakeSocketTimeout(uint32 timeoutMs)
{
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return tv;
}

static bool SetSocketTimeouts(int sock, uint32 sendTimeoutMs, uint32 recvTimeoutMs)
{
    timeval recvTv = MakeSocketTimeout(recvTimeoutMs);
    timeval sendTv = MakeSocketTimeout(sendTimeoutMs);

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recvTv, sizeof(recvTv)) < 0)
        return false;

    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &sendTv, sizeof(sendTv)) < 0)
        return false;

    return true;
}

static bool ConnectWithTimeout(int sock, sockaddr_in const& server, uint32 timeoutMs)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
        return false;

    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
        return false;

    int result = connect(sock, reinterpret_cast<sockaddr const*>(&server), sizeof(server));
    if (result == 0)
    {
        fcntl(sock, F_SETFL, flags);
        return true;
    }

    if (errno != EINPROGRESS)
    {
        fcntl(sock, F_SETFL, flags);
        return false;
    }

    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);

    timeval tv = MakeSocketTimeout(timeoutMs);
    result = select(sock + 1, nullptr, &writeSet, nullptr, &tv);

    if (result <= 0 || !FD_ISSET(sock, &writeSet))
    {
        fcntl(sock, F_SETFL, flags);
        return false;
    }

    int socketError = 0;
    socklen_t len = sizeof(socketError);

    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socketError, &len) < 0)
    {
        fcntl(sock, F_SETFL, flags);
        return false;
    }

    fcntl(sock, F_SETFL, flags);
    return socketError == 0;
}

static bool SendAllWithTimeout(int sock, std::string const& data)
{
    size_t sentTotal = 0;

    while (sentTotal < data.size())
    {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif

        ssize_t sent = send(sock, data.c_str() + sentTotal, data.size() - sentTotal, flags);

        if (sent < 0 && errno == EINTR)
            continue;

        if (sent <= 0)
            return false;

        sentTotal += static_cast<size_t>(sent);
    }

    return true;
}

static bool IsValidHost(std::string const& host)
{
    if (host.empty() || host.size() > 255)
        return false;

    for (char c : host)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) ||
              c == '.' || c == '-' ))
        {
            return false;
        }
    }

    return true;
}

static bool ParseEndpoint(
    std::string endpoint,
    std::string& host,
    uint16& port,
    std::string& path)
{
    // Reject CRLF/header injection
    if (endpoint.find('\r') != std::string::npos ||
        endpoint.find('\n') != std::string::npos)
    {
        return false;
    }

    // Only support http://
    if (endpoint.find("http://") == 0)
    {
        endpoint.erase(0, 7);
    }
    else if (endpoint.find("https://") == 0)
    {
        // HTTPS unsupported by current socket implementation
        return false;
    }

    // Split path
    std::string hostPort;
    path = "/";

    size_t slashPos = endpoint.find('/');

    if (slashPos != std::string::npos)
    {
        hostPort = endpoint.substr(0, slashPos);
        path = endpoint.substr(slashPos);

        if (path.empty())
            path = "/";
    }
    else
    {
        hostPort = endpoint;
    }

    // Path must start with /
    if (path.empty() || path[0] != '/')
        return false;

    // Prevent absolute URI confusion / proxy poisoning style issues
    if (path.find("http://") != std::string::npos ||
        path.find("https://") != std::string::npos)
    {
        return false;
    }

    host = hostPort;
    port = 80;

    size_t colonPos = hostPort.find(':');

    if (colonPos != std::string::npos)
    {
        host = hostPort.substr(0, colonPos);

        std::string portText = hostPort.substr(colonPos + 1);

        if (portText.empty())
            return false;

        for (char c : portText)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;
        }

        unsigned long parsedPort = std::strtoul(portText.c_str(), nullptr, 10);

        if (parsedPort == 0 || parsedPort > 65535)
            return false;

        port = static_cast<uint16>(parsedPort);
    }

    if (!IsValidHost(host))
        return false;

    return true;
}

static std::string DecodeChunkedBody(std::string const& body)
{
    std::string decoded;
    size_t pos = 0;

    while (pos < body.size())
    {
        size_t lineEnd = body.find("\r\n", pos);
        if (lineEnd == std::string::npos)
            return body;

        std::string sizeText = body.substr(pos, lineEnd - pos);
        size_t semi = sizeText.find(';');
        if (semi != std::string::npos)
            sizeText = sizeText.substr(0, semi);

        char* end = nullptr;
        unsigned long chunkSize = std::strtoul(sizeText.c_str(), &end, 16);

        if (end == sizeText.c_str())
            return body;

        pos = lineEnd + 2;

        if (chunkSize == 0)
            break;

        if (pos + chunkSize > body.size())
            return body;

        decoded.append(body, pos, chunkSize);
        pos += chunkSize;

        if (body.compare(pos, 2, "\r\n") == 0)
            pos += 2;
    }

    return decoded;
}

std::string HttpClient::Post(const std::string& message)
{
    std::string endpoint;
    size_t selectedIndex = 0;
    bool usedLeastActive = false;

    // =====================================================
    // Thread-safe tracking for least-active load balancing
    // =====================================================
    static std::vector<std::unique_ptr<std::atomic<int>>> activeRequests;
    static std::mutex activeRequestsMutex;

    auto EnsureActiveRequestsSize = [&]()
    {
        std::lock_guard<std::mutex> lock(activeRequestsMutex);

        size_t requiredSize = NPCBotsConfig::Endpoints.size();

        if (activeRequests.size() != requiredSize)
        {
            activeRequests.clear();
            activeRequests.reserve(requiredSize);

            for (size_t i = 0; i < requiredSize; ++i)
            {
                activeRequests.emplace_back(std::make_unique<std::atomic<int>>(0));
            }
        }
    };

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        return "SOCKET ERROR";
    }

    if (!SetSocketTimeouts(sock, NPCBotsConfig::HttpTimeoutMs, NPCBotsConfig::HttpResponseTimeoutMs))
    {
        close(sock);
        return "SOCKET TIMEOUT ERROR";
    }

    // 🔹 Load balancing
    if (NPCBotsConfig::LoadBalancingMode == "roundrobin" && !NPCBotsConfig::Endpoints.empty())
    {
        static std::atomic<size_t> rrIndex{0};

        size_t index = rrIndex.fetch_add(1, std::memory_order_relaxed);
        selectedIndex = index % NPCBotsConfig::Endpoints.size();
        endpoint = NPCBotsConfig::Endpoints[selectedIndex];
    }
    else if (NPCBotsConfig::LoadBalancingMode == "leastactive" && !NPCBotsConfig::Endpoints.empty())
    {
        EnsureActiveRequestsSize();

        selectedIndex = 0;
        int lowest = INT_MAX;

        for (size_t i = 0; i < activeRequests.size(); ++i)
        {
            int current = activeRequests[i]->load(std::memory_order_relaxed);

            if (current < lowest)
            {
                lowest = current;
                selectedIndex = i;
            }
        }

        activeRequests[selectedIndex]->fetch_add(1, std::memory_order_relaxed);
        usedLeastActive = true;

        endpoint = NPCBotsConfig::Endpoints[selectedIndex];
    }
    else if (!NPCBotsConfig::Endpoints.empty())
    {
        endpoint = NPCBotsConfig::Endpoints[0];
    }
    else
    {
        endpoint = NPCBotsConfig::Endpoint;
    }

    std::string host;
    std::string path;
    uint16 port = 80;

    if (!ParseEndpoint(endpoint, host, port, path))
    {
        close(sock);

        if (NPCBotsConfig::LoadBalancingMode == "leastactive" && usedLeastActive)
        {
        activeRequests[selectedIndex]->fetch_sub(1, std::memory_order_relaxed);
        }

        return "INVALID ENDPOINT";
    }

    // Setup socket
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &server.sin_addr) != 1)
    {
        close(sock);

        if (NPCBotsConfig::LoadBalancingMode == "leastactive" && usedLeastActive)
        {
            activeRequests[selectedIndex]->fetch_sub(1, std::memory_order_relaxed);
        }

        return "INVALID HOST";
    }

    if (!ConnectWithTimeout(sock, server, NPCBotsConfig::HttpTimeoutMs))
    {
        close(sock);

        if (NPCBotsConfig::LoadBalancingMode == "leastactive" && usedLeastActive)
        {
            activeRequests[selectedIndex]->fetch_sub(1, std::memory_order_relaxed);
        }

        return "CONNECT ERROR";
    }

    // JSON payload
    std::string safe = EscapeJson(message);
    std::string json = "{\"message\":\"" + safe + "\"}";

    std::stringstream request;
    request << "POST " << path << " HTTP/1.1\r\n";
    request << "Host: " << host << "\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Content-Length: " << json.length() << "\r\n";
    request << "Connection: close\r\n\r\n";
    request << json;

    std::string reqStr = request.str();
    if (!SendAllWithTimeout(sock, reqStr))
    {
        close(sock);

        if (NPCBotsConfig::LoadBalancingMode == "leastactive" && usedLeastActive)
        {
            activeRequests[selectedIndex]->fetch_sub(1, std::memory_order_relaxed);
        }

        return "SEND ERROR";
    }

    char buffer[4096];
    std::string response;

    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer), 0)) > 0)
    {
        response.append(buffer, bytes);
    }

    close(sock);

    bool chunked = response.find("Transfer-Encoding: chunked") != std::string::npos ||
        response.find("transfer-encoding: chunked") != std::string::npos;

    // Remove HTTP headers
    size_t pos = response.find("\r\n\r\n");
    if (pos != std::string::npos)
        response = response.substr(pos + 4);

    if (chunked)
        response = DecodeChunkedBody(response);

    // Release leastactive slot
    if (NPCBotsConfig::LoadBalancingMode == "leastactive" && usedLeastActive)
    {
        activeRequests[selectedIndex]->fetch_sub(1, std::memory_order_relaxed);
    }

    return response;
}
