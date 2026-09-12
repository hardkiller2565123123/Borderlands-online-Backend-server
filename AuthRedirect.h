#pragma once

namespace auth_redirect
{
    // Starts the local emulator (when BOLEmulator.exe is beside BOL.exe)
    // and watches sdologin.exe so its old CAS endpoints are redirected in memory.
    void Start();
}
