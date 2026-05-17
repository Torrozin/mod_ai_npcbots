#include "AIWorker.h"
#include "NPCBotsConfig.h"
#include "HttpClient.h"

#include "Timer.h"
#include "Log.h"
#include <inttypes.h> // for PRIu64

#include <future>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>

// ---------------------
// Static member definitions
// ---------------------
std::queue<AIRequest> AIWorker::requestQueue;
std::queue<AIResponse> AIWorker::responseQueue;

std::mutex AIWorker::requestMutex;
std::mutex AIWorker::responseMutex;
std::condition_variable AIWorker::cv;

std::atomic<bool> AIWorker::running(false);
std::atomic<bool> AIWorker::stopped(false);
std::vector<std::thread> AIWorker::workerThreads;

// Per-player rate limiter
// static std::unordered_map<uint64, uint32> playerLastTalkTime;
static std::unordered_map<uint64, uint32> playerLastTalkTime;
static std::mutex playerLastTalkTimeMutex;
// NEW: active request tracking per player
static std::unordered_map<uint64, uint32> activeRequests;

static std::mutex activeRequestsMutex;

// ---------------------
// Worker thread
// ---------------------
void AIWorker::WorkerThread()
{
    try
    {
        while (true)
        {
            AIRequest req;
            bool hasRequest = false;

            {
                std::unique_lock<std::mutex> lock(requestMutex);

                cv.wait(lock, [] {
                    return !AIWorker::requestQueue.empty() || !AIWorker::running;
                });

                // SAFE EXIT CONDITION
                if (!running && requestQueue.empty())
                    break;

                if (!requestQueue.empty())
                {
                    req = requestQueue.front();
                    requestQueue.pop();
                    hasRequest = true;
                }
            }

            if (!hasRequest)
                continue;

            // ---------------------------
            // Safe HTTP call
            // ---------------------------
            std::string reply;

            try
            {
                reply = HttpClient::Post(req.prompt);
            }
            catch (...)
            {
                reply = "...";
            }

            if (reply.empty())
                reply = "...";

            AIResponse res;
            res.playerGUID = req.playerGUID;
            res.botGUID  = req.botGUID;
            res.botGUID2 = req.botGUID2;
            res.text     = reply;

            {
                std::lock_guard<std::mutex> lock(responseMutex);
                responseQueue.push(res);
            }

            // Decrease active request count
            {
                std::lock_guard<std::mutex> lock(activeRequestsMutex);

                auto itr = activeRequests.find(res.playerGUID);

                if (itr != activeRequests.end())
                {
                    if (itr->second > 0)
                        --itr->second;

                    if (itr->second == 0)
                        activeRequests.erase(itr);
                }
            }
        }
    }
    catch (...)
    {
        std::cerr << "AIWorker: WorkerThread terminated due to unexpected exception!\n";
    }
}

// ---------------------
// Start / Stop
// ---------------------
void AIWorker::Start()
{
    if (running)
        return;

    stopped = false; // reset shutdown state

    running = true;

    uint32 count = std::max<uint32>(1, NPCBotsConfig::WorkerThreads);

    for (uint32 i = 0; i < count; ++i)
    {
        workerThreads.emplace_back(&AIWorker::WorkerThread);
    }
}

void AIWorker::Stop()
{
    if (stopped.exchange(true))
        return;
        
    printf("\033[1;33m");
    printf("\n========================================\n");
    printf("   AI Module Shutdown\n");
    printf("   Finishing active AI requests...\n");
    printf("   Please Wait...\n");
    printf("========================================\n\n");
    printf("\033[0m");
    
    running = false;

    // Wake ALL worker threads immediately after state change
    cv.notify_all();

    //  Wait for all threads to finish
    for (auto& t : workerThreads)
    {
        if (t.joinable())
            t.join();
    }

    workerThreads.clear();

    // OPTIONAL but SAFE: clear queues
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        std::queue<AIRequest> empty;
        std::swap(requestQueue, empty);
    }

    {
        std::lock_guard<std::mutex> lock(responseMutex);
        std::queue<AIResponse> empty;
        std::swap(responseQueue, empty);
    }
    
    {
    std::lock_guard<std::mutex> lock(playerLastTalkTimeMutex);
    playerLastTalkTime.clear();
    }
    
    {
        std::lock_guard<std::mutex> lock(activeRequestsMutex);

        // Preserve safe shutdown behavior:
        // worker threads are already joined above,
        // so active requests should already be drained naturally.
        // Avoid force-clearing state during shutdown.
        if (!activeRequests.empty())
        {
            printf("   [AI] Waiting requests drained: %zu\n",
                activeRequests.size());
        }
    }
    
    // FINAL MESSAGE
    
    printf("   ✔ Queue handled\n");
    printf("   ✔ Worker threads stopped\n");
    printf("   ✔ AI Module shutdown complete\n\n");
    
    fflush(stdout); // optional
}
// -------------------
// FlushQueues
// -------------------
void AIWorker::FlushQueues()
{
    printf("\033[1;33m[AI] Flushing AI queues...\033[0m\n");

    std::unordered_map<uint64, uint32> discardedRequests;

    {
        std::lock_guard<std::mutex> lock(requestMutex);

        while (!requestQueue.empty())
        {
            AIRequest const& req = requestQueue.front();
            discardedRequests[req.playerGUID]++;
            requestQueue.pop();
        }
    }

    {
        std::lock_guard<std::mutex> lock(responseMutex);
        std::queue<AIResponse> empty;
        std::swap(responseQueue, empty);
    }

    {
        std::lock_guard<std::mutex> lock(activeRequestsMutex);

        for (auto const& pair : discardedRequests)
        {
            auto itr = activeRequests.find(pair.first);

            if (itr == activeRequests.end())
                continue;

            if (itr->second <= pair.second)
                activeRequests.erase(itr);
            else
                itr->second -= pair.second;
        }
    }
}

// ---------------------
// Request / Response
// ---------------------
void AIWorker::EnqueueRequest(const AIRequest& req)
{

    if (!running)
    {
        return;
    }

    // HARD DISABLE CHECK
    if (!NPCBotsConfig::Enabled)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> requestLock(requestMutex);
        std::lock_guard<std::mutex> activeLock(activeRequestsMutex);

        // Queue protection
        if (requestQueue.size() >= NPCBotsConfig::MaxQueueSize)
            return;

        auto itr = activeRequests.find(req.playerGUID);

        uint32 current = (itr != activeRequests.end())
            ? itr->second
            : 0;

        // Per-player limit
        if (current >= NPCBotsConfig::MaxActiveRequestsPerPlayer)
            return;

        requestQueue.push(req);
        activeRequests[req.playerGUID]++;
    }

    cv.notify_one();
}

void AIWorker::CleanupPlayerTalkTime(uint64 playerGUID)
{
    std::lock_guard<std::mutex> lock(playerLastTalkTimeMutex);
    playerLastTalkTime.erase(playerGUID);
}

bool AIWorker::CanPlayerSpeak(uint64 playerGUID, uint32 delayMs)
{
    uint32 now = getMSTime();

    std::lock_guard<std::mutex> lock(playerLastTalkTimeMutex);

    uint32& lastTime = playerLastTalkTime[playerGUID];

    if (now - lastTime < delayMs)
        return false;

    lastTime = now;
    return true;
}

bool AIWorker::PopResponse(AIResponse& res)
{
    if (stopped)
        return false;

    std::lock_guard<std::mutex> lock(responseMutex);
    if (responseQueue.empty()) return false;
    res = responseQueue.front();
    responseQueue.pop();
    return true;
}

struct AIWorkerShutdown
{
    ~AIWorkerShutdown()
    {
        AIWorker::Stop();
    }
};

// Static instance → runs before static destruction chaos
static AIWorkerShutdown _aiWorkerShutdown;
