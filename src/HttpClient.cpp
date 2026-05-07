#include "HttpClient.h"
#include "NPCBotsConfig.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "Log.h"

#include <sstream>
#include <string>
#include <atomic>
#include <vector>

/// 🔥 Escape JSON properly (THIS FIXES YOUR ERROR)
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

    // Remove "http://"
    if (endpoint.find("http://") == 0)
        endpoint = endpoint.substr(7);

    // Split host:port and path
    std::string hostPort;
    std::string path = "/";

    size_t slashPos = endpoint.find('/');
    if (slashPos != std::string::npos)
    {
        hostPort = endpoint.substr(0, slashPos);
        path = endpoint.substr(slashPos);
    }
    else
    {
        hostPort = endpoint;
    }

    // Split host and port
    std::string host = hostPort;
    uint16 port = 80;

    size_t colonPos = hostPort.find(':');
    if (colonPos != std::string::npos)
    {
        host = hostPort.substr(0, colonPos);
        port = static_cast<uint16>(std::stoi(hostPort.substr(colonPos + 1)));
    }

    // Setup socket
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(host.c_str());

    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0)
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
    send(sock, reqStr.c_str(), reqStr.size(), 0);

    char buffer[4096];
    std::string response;

    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer), 0)) > 0)
    {
        response.append(buffer, bytes);
    }

    close(sock);

    // Remove HTTP headers
    size_t pos = response.find("\r\n\r\n");
    if (pos != std::string::npos)
        response = response.substr(pos + 4);

    // Release leastactive slot
    if (NPCBotsConfig::LoadBalancingMode == "leastactive" && usedLeastActive)
    {
        activeRequests[selectedIndex]->fetch_sub(1, std::memory_order_relaxed);
    }

    return response;
}
