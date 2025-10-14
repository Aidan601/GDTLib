#pragma once

#ifdef GDTLIB_EXPORTS
#define GDTLIB_API __declspec(dllexport)
#else
#define GDTLIB_API __declspec(dllimport)
#endif

extern "C" GDTLIB_API int GdtAdd(int a, int b);
extern "C" GDTLIB_API const char *GdtVersion();

