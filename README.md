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

The project recreates the backend services the original client depended on, allowing the game to authenticate, communicate with replacement services, load player data, and progress further through the original online flow without relying on the discontinued official infrastructure.

Development is still ongoing and the project is **not yet considered fully playable**.

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

The client can currently progress through the reconstructed authentication, lobby, character, and game-server communication paths.

Remaining backend systems are still being researched and implemented before the game can be considered fully restored.

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

## Building

The project is fully open source and can be built using Visual Studio.

1. Clone or download the repository.
2. Open the included Visual Studio solution.
3. Build the required projects.
4. Copy the resulting `BOLEmulator.exe` and `version.dll` into the Borderlands Online game directory.
5. Launch the game normally.

## Project Goal

The goal of the project is to recreate enough of the original Borderlands Online infrastructure to preserve the PC client and restore
