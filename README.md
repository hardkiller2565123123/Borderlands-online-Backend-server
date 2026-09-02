# Borderlands Online Server

**Borderlands Online Server** is a community preservation project that recreates the backend services required by the discontinued **Borderlands Online** PC client.


## How It Works

The game normally connects to services that are no longer available.

This project replaces those services with a custom backend.

```text
Borderlands Online Client
        ↓
Client Redirect
        ↓
Custom Backend
        ↓
Authentication / Profiles / Lobbies / Game Services
```

A small client-side `version.dll` is used to redirect the game toward the replacement backend while keeping as much of the original client behavior intact as possible.

## Status

**Early development.**

Current work is focused on restoring the login process and reaching the original game frontend through backend emulation.

More services will be implemented as development continues.

## Disclaimer

This is an unofficial preservation project and is not affiliated with Gearbox Software, 2K, or the original Borderlands Online developers or publishers.

Game files are not provided. You must supply your own copy of the client.
