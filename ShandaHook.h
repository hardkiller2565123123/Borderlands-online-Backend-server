#pragma once

namespace shanda_hook
{
    // Hooks the managed P/Invoke resolution for ShandaLoginWrapper.dll::Run
    // and installs an inline fallback on the original export once the module loads.
    // The original ShandaLoginWrapper.dll file is never modified on disk.
    void Start();
}
