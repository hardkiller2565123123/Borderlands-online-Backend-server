<div align="center">

# Borderlands Online Server
![Status](https://img.shields.io/badge/status-early%20development-orange)
![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Source](https://img.shields.io/badge/source-open%20source-brightgreen)

</div>

---

Borderlands Online Server is an open-source project dedicated to restoring the discontinued Borderlands Online PC client by recreating the backend services it originally depended on.

## Status

- Local account login
- Photon authentication
- Lobby connection
- Character service
- Character loading
- Online/game server communication
- Early profile, inventory, presence and quest services

Work is continuing on completing the remaining services and restoring full functionality.

## How to Use

1. Place `BOLEmulator.exe` and `version.dll` in the same folder as the Borderlands Online game executable.
2. Launch the game normally.

`BOLEmulator.exe` will start automatically when the game is launched.

You can also start `BOLEmulator.exe` manually before launching the game.

## Building

The project is open source and can be built from source.

1. Clone or download the repository.
2. Open the included Visual Studio solution.
3. Build the project.
4. Place the resulting `BOLEmulator.exe` and `version.dll` in the Borderlands Online game folder.

## Disclaimer

This is an unofficial preservation project and is not affiliated with **Gearbox Software**, **2K**, or the original Borderlands Online developers or publishers.

Game files are **not provided**. You must supply your own copy of the client.
