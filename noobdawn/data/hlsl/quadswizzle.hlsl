/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2025-2026 Baldur Karlsson
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

float4 quadSwizzleHelper(float4 c0, uint quadLaneIndex, uint readIndex)
{
  bool quadX = (quadLaneIndex & 1u) != 0u;
  bool quadY = (quadLaneIndex & 2u) != 0u;

  bool readX = ((readIndex & 1u) != 0u);
  bool readY = ((readIndex & 2u) != 0u);

  float4 sign_x = float4(0, 0, 0, 0);
  sign_x.x = quadX ? -1.0f : 1.0f;
  sign_x.y = quadX ? -1.0f : 1.0f;
  sign_x.z = quadX ? -1.0f : 1.0f;
  sign_x.w = quadX ? -1.0f : 1.0f;

  float4 sign_y = float4(0, 0, 0, 0);
  sign_y.x = quadY ? -1.0f : 1.0f;
  sign_y.y = quadY ? -1.0f : 1.0f;
  sign_y.z = quadY ? -1.0f : 1.0f;
  sign_y.w = quadY ? -1.0f : 1.0f;

  float4 c1 = c0 + (sign_x * ddx_fine(c0));
  float4 c2 = c0 + (sign_y * ddy_fine(c0));
  float4 c3 = c2 + (sign_x * ddx_fine(c2));

  if(quadLaneIndex == readIndex)
    return c0;
  else if(readY == quadY)
    return c1;
  else if(readX == quadX)
    return c2;
  else
    return c3;
}

// helpers for smaller vector sizes
float3 quadSwizzleHelper(float3 c0, uint quadLaneIndex, uint readIndex)
{
  return quadSwizzleHelper(float4(c0, 0.0f), quadLaneIndex, readIndex).xyz;
}

float2 quadSwizzleHelper(float2 c0, uint quadLaneIndex, uint readIndex)
{
  return quadSwizzleHelper(float4(c0, 0.0f, 0.0f), quadLaneIndex, readIndex).xy;
}

float quadSwizzleHelper(float c0, uint quadLaneIndex, uint readIndex)
{
  return quadSwizzleHelper(float4(c0, 0.0f, 0.0f, 0.0f), quadLaneIndex, readIndex).x;
}

// integer helpers. We expect to only use this for scalar uint and only for certain builtin inputs
// that have small values and so can be cast to/from float accurately
uint4 quadSwizzleHelper(uint4 c0, uint quadLaneIndex, uint readIndex)
{
  return uint4(quadSwizzleHelper(float4(c0), quadLaneIndex, readIndex));
}

uint3 quadSwizzleHelper(uint3 c0, uint quadLaneIndex, uint readIndex)
{
  return uint3(quadSwizzleHelper(float3(c0), quadLaneIndex, readIndex));
}

uint2 quadSwizzleHelper(uint2 c0, uint quadLaneIndex, uint readIndex)
{
  return uint2(quadSwizzleHelper(float2(c0), quadLaneIndex, readIndex));
}

uint quadSwizzleHelper(uint c0, uint quadLaneIndex, uint readIndex)
{
  return uint(quadSwizzleHelper(float(c0), quadLaneIndex, readIndex));
}

int4 quadSwizzleHelper(int4 c0, uint quadLaneIndex, uint readIndex)
{
  return int4(quadSwizzleHelper(float4(c0), quadLaneIndex, readIndex));
}

int3 quadSwizzleHelper(int3 c0, uint quadLaneIndex, uint readIndex)
{
  return int3(quadSwizzleHelper(float3(c0), quadLaneIndex, readIndex));
}

int2 quadSwizzleHelper(int2 c0, uint quadLaneIndex, uint readIndex)
{
  return int2(quadSwizzleHelper(float2(c0), quadLaneIndex, readIndex));
}

int quadSwizzleHelper(int c0, uint quadLaneIndex, uint readIndex)
{
  return int(quadSwizzleHelper(float(c0), quadLaneIndex, readIndex));
}
