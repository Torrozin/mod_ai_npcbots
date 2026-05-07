console.log("🟢 server.js starting...");

const express = require('express');
console.log("fetch exists:", typeof fetch);

const app = express();
app.use(express.json());

// ===============================
// 🔧 SETTINGS
// ===============================

// 🌐 Server settings
const SERVER_HOST = '127.0.0.1';
const SERVER_PORT = 3000;

// 🤖 Ollama endpoint
const OLLAMA_URL = 'http://localhost:11434/api/generate';
const OLLAMA_MODEL = 'mistral:7b';

// ⚙️ Performance
const MAX_ACTIVE = 1;              // 🔥 CPU SAFE (change to 2–4 with GPU)
const MAX_QUEUE = 50;             // max queued requests
const REQUEST_TIMEOUT_MS = 70000; // must be > worst-case inference time

// ⏱️ Queue pacing (VERY IMPORTANT)
const MIN_INTERVAL_MS = 150;      // delay between starting jobs (CPU: 100–300)

// ===============================
// 📊 STATE
// ===============================

let activeRequests = 0;
const queue = [];
let lastStartTime = 0;

// ===============================
// 📊 DEBUG STATS
// ===============================
function printStats() {
    console.log(`📊 Queue: ${queue.length}/${MAX_QUEUE} Active: ${activeRequests}`);
}

// ===============================
// 🧠 OLLAMA CALL
// ===============================
async function handleOllama(job) {
    const { message } = job;

    console.log("🤖 Sending to Ollama:", message);

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);

    try {
        const response = await fetch(OLLAMA_URL, {
            method: 'POST',
            signal: controller.signal,
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                model: OLLAMA_MODEL,
                prompt: message,
                stream: false,
                options: {
                    num_predict: 80,
                    temperature: 0.8
                }
            })
        });

        clearTimeout(timeout);

        const data = await response.json();

        const text =
            (data && typeof data.response === 'string' && data.response) ||
            (data && data.message && typeof data.message.content === 'string' && data.message.content) ||
            "";

        if (!text) {
            console.warn("⚠️ Empty or invalid AI response:", JSON.stringify(data));
        }

        console.log("🧠 Raw AI response:", text || "EMPTY");

        return { response: text };

    } catch (err) {
        if (err.name === 'AbortError') {
            console.error("⏱️ Ollama timeout");
        } else {
            console.error("❌ Ollama error:", err.message);
        }

        return { response: "..." };
    }
}

// ===============================
// ⚙️ QUEUE PROCESSOR (SAFE)
// ===============================
function processQueue() {
    // Already at max concurrency
    if (activeRequests >= MAX_ACTIVE)
        return;

    // Nothing to process
    if (queue.length === 0)
        return;

    const now = Date.now();

    // Enforce pacing between jobs
    if (now - lastStartTime < MIN_INTERVAL_MS) {
        setTimeout(processQueue, 25);
        return;
    }

    const job = queue.shift();
    if (!job)
        return;

    lastStartTime = now;
    activeRequests++;
    printStats();

    handleOllama(job)
        .then(result => job.resolve(result))
        .catch(err => job.reject(err))
        .finally(() => {
            activeRequests--;
            printStats();

            // Slight delay prevents burst refilling
            setTimeout(processQueue, 10);
        });
}

// ===============================
// 🌐 API ENDPOINT
// ===============================
app.post('/chat', async (req, res) => {
    const { message } = req.body;

    console.log("📩 /chat hit:", req.body);

    // Queue full protection
    if (queue.length >= MAX_QUEUE) {
        console.warn("⚠️ Dropping request (queue full)");
        return res.status(429).send("Server busy");
    }

    await new Promise((resolve, reject) => {
        queue.push({
            message,
            resolve: (result) => {
                res.send(result.response);
                resolve();
            },
            reject: (err) => {
                res.status(500).send(err.toString());
                reject();
            }
        });

        processQueue();
        printStats();
    });
});

// ===============================
// 🚀 START SERVER
// ===============================
app.listen(SERVER_PORT, SERVER_HOST, () => {
    console.log(`🚀 AI server running on http://${SERVER_HOST}:${SERVER_PORT}`);
    console.log(`🤖 Using Ollama at: ${OLLAMA_URL}`);
});

// 🔥 keep node alive
process.stdin.resume();
