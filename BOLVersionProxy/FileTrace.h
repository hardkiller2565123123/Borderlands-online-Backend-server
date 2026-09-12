#pragma once

namespace file_trace
{
    // Installs lightweight IAT hooks for file opens in game-owned modules.
    // Used only to identify which Unity loading stage is reached after the
    // SessionInfo map-change trigger.
    void Start();
}
