/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2026 Baldur Karlsson
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

#include "api/replay/data_types.h"

// Texture data for file reading/writing. Used in more complex cases
// than just a single image, e.g. DDS (can have arrays, mips) or EXR
// (can have mips).
struct tex_data
{
  uint32_t width;
  uint32_t height;
  uint32_t depth;

  uint32_t mips;
  uint32_t slices;

  bool cubemap;

  ResourceFormat format;
};

struct read_tex_data : public tex_data
{
  bytebuf buffer;

  // pairs of {offset, size} into above data buffer
  nbdarray<nbdpair<size_t, size_t>> subresources;
};

struct write_tex_data : public tex_data
{
  nbdarray<byte *> subresources;
};
