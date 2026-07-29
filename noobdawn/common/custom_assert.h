/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

#ifdef NBDASSERTMSG
#error NBDASSERTMSG already defined when including custom_assert.h
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////
// excellent set of macros to wrap individual parameters in a varargs macro expansion.
// See: http://stackoverflow.com/a/1872506/4070143
//      http://groups.google.com/group/comp.std.c/browse_thread/thread/77ee8c8f92e4a3fb/346fc464319b1ee5
//
// Some modification needed on VC++.
// See: http://compgroups.net/comp.lang.c++/visual-c++-too-few-many-args-warnings-for-apply/2075805
//
// A few more twiddles by hand to get everything playing nicely

#define NBDASSERT_FAILMSG_1(value) failmsg += (STRINGIZE(value) "=") + ToStr(value) + ", ";
#define NBDASSERT_FAILMSG_2(value, _1)                     \
  failmsg += (STRINGIZE(value) "=") + ToStr(value) + ", "; \
  NBDASSERT_FAILMSG_1(_1)
#define NBDASSERT_FAILMSG_3(value, _1, _2)                 \
  failmsg += (STRINGIZE(value) "=") + ToStr(value) + ", "; \
  NBDASSERT_FAILMSG_2(_1, _2)
#define NBDASSERT_FAILMSG_4(value, _1, _2, _3)             \
  failmsg += (STRINGIZE(value) "=") + ToStr(value) + ", "; \
  NBDASSERT_FAILMSG_3(_1, _2, _3)
#define NBDASSERT_FAILMSG_5(value, _1, _2, _3, _4)         \
  failmsg += (STRINGIZE(value) "=") + ToStr(value) + ", "; \
  NBDASSERT_FAILMSG_4(_1, _2, _3, _4)
#define NBDASSERT_FAILMSG_6(value, _1, _2, _3, _4, _5)     \
  failmsg += (STRINGIZE(value) "=") + ToStr(value) + ", "; \
  NBDASSERT_FAILMSG_5(_1, _2, _3, _4, _5)
#define NBDASSERT_FAILMSG_7(value, _1, _2, _3, _4, _5, _6) \
  failmsg += (STRINGIZE(value) "=") + ToStr(value) + ", "; \
  NBDASSERT_FAILMSG_6(_1, _2, _3, _4, _5, _6)
#define NBDASSERT_FAILMSG_8(value, _1, _2, _3, _4, _5, _6, _7) \
  failmsg += (STRINGIZE(value) "=") + ToStr(value) + ", ";     \
  NBDASSERT_FAILMSG_7(_1, _2, _3, _4, _5, _6, _7)

// this is the terminating clause
#define NBDASSERT_FAILMSG_DISCARD_1(cond)
#define NBDASSERT_FAILMSG_DISCARD_2(cond, _1) NBDASSERT_FAILMSG_1(_1)
#define NBDASSERT_FAILMSG_DISCARD_3(cond, _1, _2) NBDASSERT_FAILMSG_2(_1, _2)
#define NBDASSERT_FAILMSG_DISCARD_4(cond, _1, _2, _3) NBDASSERT_FAILMSG_3(_1, _2, _3)
#define NBDASSERT_FAILMSG_DISCARD_5(cond, _1, _2, _3, _4) NBDASSERT_FAILMSG_4(_1, _2, _3, _4)
#define NBDASSERT_FAILMSG_DISCARD_6(cond, _1, _2, _3, _4, _5) \
  NBDASSERT_FAILMSG_5(_1, _2, _3, _4, _5)
#define NBDASSERT_FAILMSG_DISCARD_7(cond, _1, _2, _3, _4, _5, _6) \
  NBDASSERT_FAILMSG_6(_1, _2, _3, _4, _5, _6)
#define NBDASSERT_FAILMSG_DISCARD_8(cond, _1, _2, _3, _4, _5, _6, _7) \
  NBDASSERT_FAILMSG_7(_1, _2, _3, _4, _5, _6, _7)

#define NBDASSERT_FAILMSG_NARG(...) NBDASSERT_FAILMSG_NARG_(__VA_ARGS__, NBDASSERT_FAILMSG_RSEQ_N())
#define NBDASSERT_FAILMSG_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, N, ...) N
#define NBDASSERT_FAILMSG_RSEQ_N() 8, 7, 6, 5, 4, 3, 2, 1, 0

#define NBDASSERT_FAILMSG(...) NBDASSERT_FAILMSG_(NBDASSERT_FAILMSG_NARG(__VA_ARGS__), __VA_ARGS__)

#define NBDASSERT_GETCOND(cond, ...) cond

#if ENABLED(RDOC_MSVS)

// only needed on VC++, but unfortunately breaks on g++/clang++
#define NBDASSERT_FAILMSG_INVOKE(macro, args) macro args

#define NBDASSERT_FAILMSG_NARG_(...) \
  NBDASSERT_FAILMSG_INVOKE(NBDASSERT_FAILMSG_ARG_N, (__VA_ARGS__))
#define NBDASSERT_FAILMSG_(N, ...) \
  NBDASSERT_FAILMSG_INVOKE(CONCAT(NBDASSERT_FAILMSG_DISCARD_, N), (__VA_ARGS__))

#define NBDASSERT_IFCOND(cond, ...) NBDASSERT_FAILMSG_INVOKE(NBDASSERT_GETCOND, (cond))

#else

#define NBDASSERT_FAILMSG_NARG_(...) NBDASSERT_FAILMSG_ARG_N(__VA_ARGS__)
#define NBDASSERT_FAILMSG_(N, ...) CONCAT(NBDASSERT_FAILMSG_DISCARD_, N)(__VA_ARGS__)

#define NBDASSERT_IFCOND(cond, ...) (cond)

#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define NBDASSERTMSG(msg, ...)                                                           \
  do                                                                                     \
  {                                                                                      \
    if(!(NBDASSERT_IFCOND(__VA_ARGS__)))                                                 \
    {                                                                                    \
      const char custommsg[] = msg;                                                      \
      (void)custommsg;                                                                   \
      nbdstr assertmsg = "'" STRINGIZE(NBDASSERT_GETCOND(__VA_ARGS__)) "' ";             \
      assertmsg += (sizeof(custommsg) > 1) ? msg " " : "";                               \
      nbdstr failmsg;                                                                    \
      NBDASSERT_FAILMSG(__VA_ARGS__);                                                    \
      if(!failmsg.empty())                                                               \
      {                                                                                  \
        failmsg.pop_back();                                                              \
        failmsg.pop_back();                                                              \
      }                                                                                  \
      nbdstr combinedmsg = assertmsg + (failmsg.empty() ? "" : "(" + failmsg + ")");     \
      nbdassert(combinedmsg.c_str(), __FILE__, __LINE__, __PRETTY_FUNCTION_SIGNATURE__); \
      nbdlog_flush();                                                                    \
      NBDBREAK();                                                                        \
    }                                                                                    \
  } while((void)0, 0)
