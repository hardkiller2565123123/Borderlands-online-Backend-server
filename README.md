<div align="center">

# Borderlands Online Server

![Status](https://img.shields.io/badge/status-backend%20reconstruction-orange)
![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Source](https://img.shields.io/badge/source-open%20source-brightgreen)

**An open-source preservation project for restoring the discontinued Borderlands Online PC client.**

</div>

---

## About

**Borderlands Online Server** is an open-source preservation project focused on restoring functionality to the discontinued **Borderlands Online** PC client.

The project recreates the backend services the original client depended on, allowing the game to authenticate, communicate with replacement services, load player data, and progress through portions of the original online flow without relying on the discontinued official infrastructure.

> **The game is not currently playable. The client does not successfully enter gameplay yet.**

Development is still ongoing, with the current focus on reconstructing the remaining services and completing the transition from frontend/backend communication into an actual playable game session.

## Current Status

The replacement backend currently supports:

* ✅ Local account login
* ✅ Photon authentication
* ✅ Lobby connection
* ✅ Character service
* ✅ Character loading
* ✅ Online/game-server communication
* 🟡 Profile services
* 🟡 Inventory services
* 🟡 Presence services
* 🟡 Quest services
* 🟡 Additional backend service reconstruction
* ❌ Entering gameplay
* ❌ Fully playable game sessions

The client can currently progress through the reconstructed authentication, lobby, character, and game-server communication paths.

**It does not currently transition into a playable in-game session.**

The remaining backend/game-session systems are still being researched and implemented before the game can be considered playable.

## How It Works

The project consists of two main components:

* `version.dll` — loaded by the Borderlands Online client and handles client-side integration/bootstrap.
* `BOLEmulator.exe` — provides the replacement local backend services required by the game.

When the game starts, `version.dll` automatically starts `BOLEmulator.exe` if it is not already running.

## How to Use

1. Place `BOLEmulator.exe` and `version.dll` in the same folder as the **Borderlands Online** game executable.
2. Launch Borderlands Online normally.
3. `BOLEmulator.exe` should start automatically.
4. The client will connect to the reconstructed local backend.

`BOLEmulator.exe` can also be started manually before launching the game.

**Current builds are intended for preservation, testing, and development. They do not yet allow you to enter and play the game normally.**

## Building

The project is fully open source and can be built using Visual Studio.

1. Clone or download the repository.
2. Open the included Visual Studio solution.
3. Build the required projects.
4. Copy the resulting `BOLEmulator.exe` and `version.dll` into the Borderlands Online game directory.
5. Launch the game normally.

## Project Goal

The goal of the project is to recreate enough of the original Borderlands Online infrastructure to preserve the PC client and restore its original functionality as accurately as possible.

Development currently focuses on completing the remaining backend services, reconstructing the game-session flow, and reaching the point where the client can successfully transition from the frontend into actual gameplay.

## Disclaimer

This is an **unofficial preservation project** and is not affiliated with, endorsed by, or associated with **Gearbox Software**, **2K**, or the original Borderlands Online developers or publishers.

No copyrighted game files are distributed with this project.

You must provide your own copy of the **Borderlands Online** client.
