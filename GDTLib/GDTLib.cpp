#include "pch.h"
#include "GDTLib.h"
#include <string>

extern "C"
{
    GDTLIB_API int GdtAdd(int a, int b) { return a + b; }

    static std::string version = "1.0.0";
    GDTLIB_API const char *GdtVersion() { return version.c_str(); }
}