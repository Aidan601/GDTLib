#pragma once

#ifdef GDTLIB_EXPORTS
#define GDTLIB_API __declspec(dllexport)
#else
#define GDTLIB_API __declspec(dllimport)
#endif