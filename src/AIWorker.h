#pragma once

#include "Define.h"
#include <queue>
#include <mutex>
#include <thread>
#include <string>
#include <condition_variable>
#include <atomic>
#include <memory>

struct AIRequest {
    uint64 playerGUID;
    uint64 botGUID;
    uint64 botGUID2;
    std::string prompt;
};

struct AIResponse {
    uint64 playerGUID;
    uint64 botGUID;
    uint64 botGUID2;
    std::string text;
};

class AIWorker
{
public:
    static void Start();
    static void Stop();
    static void EnqueueRequest(const AIRequest& req);
    static bool PopResponse(AIResponse& res);
    static bool CanPlayerSpeak(uint64 playerGUID, uint32 delayMs);
    static void CleanupPlayerTalkTime(uint64 playerGUID);
    static void FlushQueues();

private:
    static std::queue<AIRequest> requestQueue;
    static std::queue<AIResponse> responseQueue;
    static std::mutex requestMutex;
    static std::mutex responseMutex;
    static std::condition_variable cv;
    static std::atomic<bool> running;
    static std::atomic<bool> stopped;
    static std::vector<std::thread> workerThreads;

    static void WorkerThread();
};
