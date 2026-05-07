# mod_ai_npcbots
Bring NPCBots to life with AI-generated banter, combat dialogue, memory, and dynamic conversations.

An AI-powered dialogue system for AzerothCore NPCBots that adds immersive banter, combat chatter, contextual memory, and dynamic conversations between bots. Originally created as a small experiment to make bots "feel alive", the project evolved into a lightweight AI ecosystem inside WoW with: - Context-aware NPC conversations - AI-generated combat chatter - Per-player conversation memory - Multi-endpoint AI support - Worker thread processing - Queue safety & anti-spam systems - Dynamic cooldown scaling - Performance-focused throttling The module is designed to remain lightweight and configurable while avoiding excessive AI spam or server overload.

# Requirements

This module requires a small external AI gateway written in Node.js which handles communication between AzerothCore and the AI backend.

The included `server.js` is configured by default to use:

- Ollama
- `mistral:7b`

The AI model can easily be changed inside `server.js` to any Ollama-supported model or compatible backend.

Example default setup:

```bash
ollama run mistral:7b
node server.js
```

---

# AI Backend Support

The module supports both single and multi-endpoint AI setups.

Supported load balancing modes:

- `single`
- `roundrobin`
- `leastactive`

Example:

```ini
AI.Endpoints = http://127.0.0.1:3000/chat,http://127.0.0.1:3001/chat
AI.LoadBalancingMode = roundrobin
```

This allows multiple Ollama instances to be used simultaneously for improved performance and reduced response latency.

The system was designed with lightweight local AI hosting in mind and works well even on modest hardware when configured correctly.

---

# Chat Visibility & Immersion

Bot dialogue is intentionally only visible to the bot owner through `MonsterSay`.

This design choice was made because NPCBots are technically server-side NPCs, and broadcasting AI-generated chatter globally would quickly become overwhelming on populated servers.

The module focuses on:
- immersion
- companion-style interaction
- controlled pacing
- minimal server intrusion

---

# Core Compatibility

The module was intentionally designed to avoid requiring AzerothCore core modifications.

No core edits are required.

This keeps installation simple, improves compatibility with future AzerothCore updates, and makes the module easier to maintain and distribute.






