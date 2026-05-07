### --- CHANGELOG ---

# 🧾 NPCBots AI Module — Changelog (User-Friendly with Phases)

---

## 🚀 Phase 1 — Basic AI System

* Bots can now **talk using AI**
* Connected to external AI (like Ollama)
* First version of AI-generated dialogue

---

## 🧵 Phase 2 — Performance Upgrade

* AI system moved to **background threads**
* No gameplay lag or blocking
* Handles multiple AI requests smoothly

---

## 💬 Phase 3 — Idle Banter System

* Bots now **chat while idle**
* Random conversations between bots
* Cooldowns and chance system added
* Bots can respond to each other naturally

---

## ⚔️ Phase 4 — Combat Chatter

* Bots now **react during combat**
* Includes:

  * Entering combat
  * Taking damage
  * Mid-fight chatter
  * Victory lines

👉 Makes combat feel more alive

---

## ⚙️ Phase 5 — Config System

* Most behavior moved to `.conf`
* You can control:

  * Chat frequency
  * Combat chatter chances
  * Cooldowns

👉 No need to recompile

---

## 🛑 Phase 6 — Anti-Spam System

* Prevents bots from talking too much
* Each player has independent limits
* AI responses are paced naturally

---

## 🎛️ Phase 7 — Global Talk Delay

* Added:

  ```
  AI.GlobalTalkDelay
  ```
* Controls how often bots can talk
* Works for both banter and combat

---

## 🧼 Phase 8 — Cleanup & Stability

* Removed hardcoded values
* Unified config usage
* Fixed compile issues and inconsistencies

---

## 🛡️ Phase 9 — Config Safety

* Added protection against bad configs:

  * Invalid values
  * Broken cooldowns
  * Too many threads

👉 Prevents crashes and weird behavior

---

## ⚔️ Phase 10 — Combat vs Banter

* Bots now **focus on combat when fighting**
* Banter pauses during combat
* More natural behavior overall

---

## 🧠 Phase 11 — Thread & Performance Tuning

* Improved AI worker performance
* Reduced CPU contention
* Fixed potential instability and deadlocks

---

## 🧠 Phase 12 — AI Improvements & Stability

### AI Behavior Improvements

* Better handling of **location awareness**
* Conversations shift naturally to new topics
* Less repetition and more natural flow

### Dialogue Fixes

* Removed cases where bots added their own name incorrectly
* Cleaner and more natural sentences

### Stability Fixes

* Fixed shutdown issues and crashes
* AI now safely stops before server shutdown
* No more database conflicts

### Shutdown Improvements

* Added safe queue flushing
* Ensures clean shutdown with no leftover AI tasks

### Thread Safety

* Fixed rare crashes during shutdown
* Improved thread lifecycle handling

---

## ⚔️ Phase 13 — Combat AI Improvements

* Fixed missing enemy names
* Improved enemy recognition (race/type)
* Removed incorrect values like:

  * "None"
  * "Unknown"

### Better Combat Behavior

* Enter combat → reacts naturally
* Mid-fight → continues smoothly
* Less repetitive responses

### Performance Scaling

* AI adapts based on number of bots
* Large groups → less spam
* Small groups → more active chatter

---

## 🧠 Phase 14 — Core System Fixes (Major Update)

### Multi-player Support (Critical Fix)

* All players now handled correctly
* No more shared or broken conversations

### AI Routing Fix

* AI responses now always go to the correct player
* Fixed issue where responses were lost

### Chat Fixes

* Messages always sent to correct player
* No more invisible or missing chat

### Memory System

* Each player now has:

  * Independent conversation history
  * Independent topics
  * Independent pacing

👉 Conversations feel unique per player

---

## ⚔️ Phase 15 — Enemy Recognition System

* Rebuilt enemy detection system
* Bots now understand what they are fighting

### Improvements

* Uses name-based detection (e.g. Murloc, Naga, etc.)
* Fallback to creature type if needed
* Easy to expand and customize

---

## 🎯 Final Result

### Before:

* Only one player worked properly
* Missing AI responses
* Broken or shared conversations

### Now:

* All players supported
* Stable and reliable AI
* Natural and independent conversations

---

## 🌍 New Behavior

* Different players get different conversations
* More immersive and dynamic AI world

---

## 💡 Summary

* ✔ Stable AI system
* ✔ Full multi-player support
* ✔ Configurable behavior
* ✔ Better performance
* ✔ Natural conversations
* ✔ Clean and maintainable system

---


## Phase 16

### 🔧 AI NPCBots Update

* Added config toggles:

  * `AI.Enabled` (master switch)
  * `Banter.ChatterEnabled`
  * `Combat.ChatterEnabled`
* Fixed bug where AI would stop for all players due to incorrect `return` in loop
* Improved multi-player AI behavior (no cross-player blocking)
* Fixed combat AI limiter inconsistency
* Cleaned up redundant checks and improved stability

🛡️ Safe update – no changes to core AIWorker threading
⚙️ Fully configurable via `.conf` without rebuild


---

## Phase 17 - (Multi-Ollama Support) load balancing roundrobin

### Added

* Support for multiple AI endpoints (`AI.Endpoints`)
* Configurable load balancing mode (`AI.LoadBalancingMode`)
* Round-robin request distribution across endpoints

### Changed

* Renamed config namespace from `NPCBots.*` → `AI.*`
* `HttpClient` now selects endpoint dynamically instead of using a single static endpoint
* Config parsing updated to handle comma-separated endpoint list

### Improved

* Fallback logic: uses `AI.Endpoint` if no endpoints are configured
* Added optional logging for selected endpoint (debugging load balancing)

### Notes

* Backwards compatible with single endpoint setup
* No existing logic removed

---

## Phase 18 – Load Balancing leastactive

* Added thread-safe `roundrobin` using atomic index
* Implemented `leastactive` load balancing:
* Tracks active requests per endpoint
* Dynamically routes new requests to least busy instance
* Replaced std::vector<std::atomic<int>> with std::vector<int> + atomic builtins for GCC compatibility
* Added safe request tracking:
* Increment on dispatch
* Decrement on completion / failure
* Ensured thread-safe initialization using std::call_once
* Added safeguards for:
    empty endpoint list
    size mismatches
    connection failures

---

## Phase 19 – Banter Desynchronization & Load Smoothing

* Replaced global banter update timer with per-player update scheduling
* Added deterministic player-based offset to prevent synchronized execution
* Removed global UpdateInterval gate that caused all players to trigger at the same time
* Converted system from burst-driven → flow-driven execution

Improvements

* Eliminated synchronized bot chatter across players
* AI requests are now evenly distributed over time
* Significantly reduced CPU spikes and overall load
* Improved scalability with multiple players online
* leastactive load balancing now operates under real concurrency instead of burst spikes

Behavior Notes

* Banters.UpdateInterval is now applied per player instead of globally
* All existing config values remain respected and unchanged in function
* No changes to cooldowns, limits, or AI safety systems

Result

* Smoother gameplay experience
* Natural, unsynchronized bot conversations
* More efficient CPU usage
* Better foundation for scaling (CPU → GPU)

---

## Phase 20 — Banter Timing & Realism Improvements

### ✨ Added

* **GlobalJitter**

  * Adds random delay to banter update timing
  * Breaks predictable “metronome” behavior

* **RecentSpeakerPenalty**

  * Prevents the same bot from speaking again too soon
  * Improves rotation between bots

---

### ⚙️ Improved

* **Per-player update timing**

  * Added jitter to existing per-player scheduling
  * Removes subtle synchronization patterns

* **Bot selection fairness**

  * Reduced chance of the same bot being picked repeatedly

* **Conversation pacing**

  * More natural gaps and uneven timing
  * Less mechanical feel

---

### 🧹 Cleanup

* Removed unused `botLastSpeakTime` map
* Unified usage of `botLastSpeak`

---

### 🛡️ Config & Safety

* Added validation for:

  * `GlobalJitter` (upper clamp)
  * `RecentSpeakerPenalty` (bounded by BotCooldownMax)

* Improved config documentation:

  * Clear relationships between timing values
  * Recommended ranges for tuning

---

* System now behaves as a **tunable conversation simulator**
* No additional hard constraints added — maintains AI freedom

---

😄 *Big step forward — from experimental just make bots chat to solid system*

---



