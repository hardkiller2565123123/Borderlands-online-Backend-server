#pragma once

namespace mono_load_inspector
{
    // Read-only monitor for the managed LevelLoadingManager singleton. It uses
    // Mono embedding exports already present in the Unity player and reports
    // the stock client's exact 15-step map-loading stage, progress and stall
    // predicate without forcing or modifying managed state.
    void Start();
}
