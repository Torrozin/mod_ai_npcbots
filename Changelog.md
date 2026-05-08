# Changelog

## Phase 1 — Initial AI Integration

- Added AI-generated dialogue support for NPCBots
- Integrated external AI backend support (Ollama/OpenAI-compatible endpoints)
- First implementation of dynamic NPC conversations

---

## Phase 2 — Worker Thread System

- Moved AI processing to background worker threads
- Prevented gameplay blocking and server lag during AI generation
- Added support for multiple simultaneous AI requests

---

## Phase 3 — Idle Banter System

- Added contextual idle conversations between bots
- Implemented conversation chains and bot-to-bot replies
- Added cooldowns and randomization for natural pacing

---

## Phase 4 — Combat Chatter

- Added AI-generated combat dialogue
- Bots now react to:
  - entering combat
  - taking damage
  - ongoing combat events
  - combat victories
- Added combat pacing and suppression systems

---

## Phase 5 — Configurable AI System

- Moved AI behavior settings to `.conf`
- Added configurable:
  - cooldowns
  - chatter frequency
  - combat chances
  - AI safety limits
- No recompilation required for tuning behavior

---

## Phase 6 — AI Safety & Anti-Spam Systems

- Added global anti-spam protection
- Added per-player AI rate limiting
- Added queue safety systems
- Prevented excessive AI generation and message flooding

---

## Phase 7 — Stability & Cleanup

- Removed remaining hardcoded values
- Unified config handling across systems
- Improved validation and config safety checks
- Fixed multiple edge-case crashes and timing issues

---

## Phase 8 — Multi-Player Conversation System

- Added fully independent per-player conversation handling
- Fixed shared memory and cross-player dialogue issues
- Improved AI routing and message delivery reliability
- Added isolated conversation pacing per player

---

## Phase 9 — Multi-Endpoint AI Support

- Added support for multiple AI endpoints
- Added configurable load balancing modes:
  - `single`
  - `roundrobin`
  - `leastactive`
- Added dynamic endpoint selection and failover behavior

---

## Phase 10 — Performance Scaling Improvements

- Added dynamic chatter scaling based on active bot count
- Reduced CPU/GPU spikes during large bot activity
- Improved request distribution across worker threads
- Optimized concurrency handling and thread safety

---

## Phase 11 — Banter Timing & Realism Improvements

- Added per-player update scheduling
- Added timing jitter and desynchronization systems
- Reduced synchronized bot chatter
- Improved speaker rotation fairness
- Added configurable:
  - `GlobalJitter`
  - `RecentSpeakerPenalty`

Result:
- More natural conversations
- Smoother pacing
- Reduced mechanical behavior
- Better scalability and immersion

---

## Added party chat commands for mage NPCBots:

- /p bot food
- /p bot water

Commands now trigger the native NPCBot gossip logic.

---

## Current Features

- AI-generated NPC dialogue
- Idle banter system
- Combat chatter system
- Contextual memory
- Multi-player conversation support
- Multi-endpoint AI support
- Worker thread processing
- Queue safety & throttling
- Dynamic load balancing
- Configurable AI behavior
- Performance-focused design
- No AzerothCore core modifications required
