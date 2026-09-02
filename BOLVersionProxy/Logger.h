#pragma once

#include <windows.h>
#include <cstdarg>

namespace bol_log
{
    void Initialize();
    void Shutdown();
    void Write(const char* format, ...);
    void WriteV(const char* format, va_list args);
}
