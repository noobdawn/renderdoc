/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
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

#include "api/replay/nbdarray.h"
#include "api/replay/nbdstr.h"

nbdstr strlower(const nbdstr &str);
nbdstr strupper(const nbdstr &str);

uint32_t strhash(const char *str, uint32_t existingHash);
uint32_t strhash(const char *str);

nbdstr get_basename(const nbdstr &path);
nbdstr get_dirname(const nbdstr &path);
nbdstr strip_extension(const nbdstr &path);

// Replace all directory separators combinations with '/'
// i.e. '\' -> '/' and '//' -> '/'
nbdstr standardise_directory_separator(const nbdstr &path);

// remove everything but alphanumeric ' ' and '.'
// It replaces everything else with _
// for logging strings where they might contain garbage characters
void strip_nonbasic(nbdstr &str);

void split(const nbdstr &in, nbdarray<nbdstr> &out, const char sep);
void merge(const nbdarray<nbdstr> &in, nbdstr &out, const char sep);
