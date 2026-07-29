#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <stdio.h>
#include <time.h>
#include "miniz/miniz.h"

// local variable is initialized but not referenced
#pragma warning(disable : 4189)
// signed/unsigned mismatch
#pragma warning(disable : 4018)
#pragma warning(disable : 4245)
#pragma warning(disable : 4389)
// unreachable code
#pragma warning(disable : 4702)
// forcing value to true or false
#pragma warning(disable : 4800)
// assignment within conditional expression
#pragma warning(disable : 4706)

// Tell tinyexr to not explicitly include <miniz.h>, we have included
// our own version just above. This will make tinyexr expect zlib
// compatible functions to be available, which miniz does.
#define TINYEXR_USE_MINIZ 0

#define TINYEXR_USE_THREAD 1

#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
