# Borderlands Online Server

**Borderlands Online Server** is an open-source preservation and backend emulation project focused on restoring functionality to the discontinued **Borderlands Online** PC client.

> **Early development.** The project is currently focused on reverse engineering the original login flow, recreating required backend services, and reaching the game's frontend/menu without the original online infrastructure.

---

## Project Goal

Borderlands Online depended heavily on online services that are no longer available.

The goal of this project is to recreate enough of the original backend infrastructure for the game client to operate again using locally hosted or community-hosted services.

The long-term goal is to restore as much functionality as possible, including:

* Client startup
* Authentication
* Account/session handling
* Character data
* Player profiles
* Inventory
* Game configuration
* Matchmaking
* Lobby services
* Game sessions
* Friends/social systems
* Persistent progression

This is a preservation project and is still heavily experimental.

---

# Current Status

```text
Borderlands Online
│
├── Client Bootstrap        — In Progress
├── Login Stack Research    — In Progress
├── Authentication Backend  — In Progress
├── Account Service         — Planned
├── Profile Service         — Planned
├── Character Service       — Planned
├── Lobby Service           — Planned
├── Matchmaking             — Planned
├── Game Server Support     — Planned
└── Persistence             — Planned
```

The current target client is:

```text
Architecture: x86 / Win32
Engine:       Unity / Mono
```

---

# Client Bootstrap

The project currently uses a `version.dll` proxy/bootstrapper loaded by the Borderlands Online client.

Rather than immediately forcing the game past authentication checks, the current work focuses on observing the original login stack and understanding what the client expects from the retired services.

The bootstrap is being used for:

* Runtime logging
* Network observation
* Login flow research
* Backend redirection
* Service emulation testing
* Client/backend protocol research

---

# Login Stack

Several important components used by the original client have already been identified.

```text
Assembly-CSharp.dll
Photon3Unity3D.dll
ShandaLoginWrapper.dll
GPKitClt.dll
```

These appear to cover different portions of the original game, authentication, and networking stack.

### Assembly-CSharp

Contains much of the managed Unity game logic and is one of the primary targets for understanding how the frontend communicates with the login and backend systems.

### Photon

`Photon3Unity3D.dll` indicates that Photon networking is used by at least part of the game's online infrastructure.

Research will determine which parts of the original Photon protocol need to be reproduced or replaced.

### Shanda Login

`ShandaLoginWrapper.dll` is part of the original authentication/login integration.

Recreating the expected responses from this layer is one of the current priorities.

### GPKit

`GPKitClt.dll` is another component involved in the original online platform stack.

Its requests and expected responses are currently being mapped so equivalent behavior can eventually be provided by the replacement backend.

---

# Planned Architecture

The project is intended to separate client compatibility code from the actual server implementation.

```text
Borderlands Online Client
        │
        ▼
   version.dll
 Client Bootstrap
        │
        ▼
 Backend Redirect
        │
        ▼
┌──────────────────────────┐
│ Borderlands Online Server│
├──────────────────────────┤
│ Authentication           │
│ Accounts                 │
│ Sessions                 │
│ Profiles                 │
│ Characters               │
│ Inventory                │
│ Social / Friends         │
│ Matchmaking              │
│ Lobbies                  │
│ Game Services            │
└──────────────────────────┘
        │
        ▼
    Persistence
```

The intention is for the client modification to remain as small as practical.

Where possible, the original client behavior will be preserved and the missing server infrastructure will be emulated instead of replacing large portions of the game.

---

# Development Priorities

Current development is primarily focused on:

1. Mapping the original startup and login sequence.
2. Identifying calls between the game and `ShandaLoginWrapper`.
3. Understanding the responsibilities of `GPKitClt`.
4. Recording Photon initialization and connection behavior.
5. Identifying original service endpoints.
6. Redirecting retired endpoints to the replacement backend.
7. Reproducing the minimum authentication responses required by the client.
8. Reaching the original frontend/menu using emulated backend responses.
9. Expanding the emulator into account, profile, lobby, and gameplay services.

---

# Repository Structure

The repository is expected to evolve as more of the original infrastructure is understood.

```text
BorderlandsOnlineServer/
│
├── client/
│   ├── bootstrap/
│   ├── hooks/
│   ├── network/
│   └── logging/
│
├── server/
│   ├── auth/
│   ├── account/
│   ├── profile/
│   ├── character/
│   ├── inventory/
│   ├── social/
│   ├── lobby/
│   ├── matchmaking/
│   └── game/
│
├── protocol/
│   ├── packets/
│   └── serialization/
│
└── tools/
```

This structure is not final.

---

# Preservation

Borderlands Online was discontinued and its original infrastructure is no longer available to normal players.

Without replacement services, the original client is effectively unusable regardless of whether someone still possesses the game files.

This project exists to research and preserve that software by recreating the services it depended on.

The project does **not** distribute Borderlands Online game files.

You must provide your own legitimately obtained client files.

---

# Contributing

The project is in a research-heavy stage, so contributions related to reverse engineering and protocol documentation are especially useful.

Areas of interest include:

* Unity / Mono research
* Network protocol analysis
* Photon
* Shanda networking
* GPKit
* Authentication protocols
* Backend development
* Packet serialization
* Database design
* Game server research

Documented findings are valuable even when they do not immediately result in working code.

---

# Disclaimer

This is an unofficial community preservation project.

It is not affiliated with, endorsed by, or associated with **Gearbox Software**, **2K**, **Shanda Games**, or any other company involved with Borderlands Online.

All trademarks and copyrighted material belong to their respective owners.

This project is intended for preservation, interoperability research, and use with legitimately obtained game files.
