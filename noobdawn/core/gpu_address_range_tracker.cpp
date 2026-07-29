/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2024-2026 Baldur Karlsson
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

#include "core/gpu_address_range_tracker.h"
#include "api/replay/replay_enums.h"
#include "common/formatting.h"
#include "core/settings.h"

void GPUAddressRangeTracker::AddTo(const GPUAddressRange &range)
{
  NBDASSERT(range.oobEnd >= range.realEnd);

  SCOPED_WRITELOCK(addressLock);

  // insert ranges ordered by start first, then by size. Ranges with different sizes starting at the
  // same point will be ordered such that the last one is largest

  // search for the range. This will return the largest range which starts before or at this address
  size_t idx = FindLastRangeBeforeOrAtAddress(range.start);

  // if we search for an address that's past the end of the last range, we'll return that index. The
  // only case where we return no valid index is if the address is before the first range - so
  // insert ours at the start of the list and return
  if(idx == ~0U)
  {
    AddRangeAtIndex(0, range);
    return;
  }

  // if the range found doesn't start at the same point as us, insert immediately so we preserve the
  // sorting by range start
  if(addresses[idx].start != range.start)
  {
    AddRangeAtIndex(idx + 1, range);
    return;
  }

  // we get here if the range starts at the same point as us, so we need to sort by size.
  // if we are smaller than the found range, move backwards to insert before it. Keep going as long
  // as we're looking at ranges that start at the same address and are larger than us
  while(addresses[idx].start == range.start && addresses[idx].realEnd > range.realEnd)
  {
    // we could be smaller than the very first range in the list. If that's the case, insert at 0 and return now
    if(idx == 0)
    {
      AddRangeAtIndex(0, range);
      return;
    }

    // otherwise move backwards, to insert before the current range
    idx--;
  }

  // insert after the idx we arrived at, which is the first range either starting before, or that is smaller than us
  AddRangeAtIndex(idx + 1, range);
}

void GPUAddressRangeTracker::RemoveFrom(GPUAddressRange::Address addr, ResourceId id)
{
  {
    SCOPED_WRITELOCK(addressLock);

    // search for the range. This will return the largest range which starts before or at this address
    size_t idx = FindLastRangeBeforeOrAtAddress(addr);

    if(idx != ~0U)
    {
      // there might be multiple buffers with the same range start, find the exact range for this
      // buffer. We only have to search backwards because we returned the largest (aka last) range before this address
      while(addresses[idx].start == addr)
      {
        if(addresses[idx].id == id)
        {
          RemoveRangeAtIndex(idx);

          return;
        }

        // this should not happen, it's just for safety/readability. The only time we would reverse
        // all the way back to the first entry and still not find the range is if the desired
        // address were before the first address range in the first place, at which point we'd have
        // failed from FindLastRangeBeforeOrAtAddress() above
        if(idx == 0)
          break;    // @NoCoverage

        --idx;
      }
    }
  }

  // used only so the tests can EXPECT_ERROR()
  RDResult err;
  SET_ERROR_RESULT(err, ResultCode::InternalError, "Couldn't find matching range to remove for %s",
                   ToStr(id).c_str());
  (void)err;
}

void GPUAddressRangeTracker::AddRangeAtIndex(size_t idx, const GPUAddressRange &range)
{
  // the caller must lock.

  OverextendNode newNode(range);

  // we only want to inherit overextensions that would naturally have been added. This means we only
  // need to look at the two neighbouring entries [idx] and [idx-1]:
  //
  // [idx].start will be >= range.start due to the ordering of ranges.
  //
  // If [idx].start > range.start then we will ignore it because it's starting afterwards and
  // anything that overextends it which we want will also overextend (or exist at) [idx-1]. It is
  // not possible for something to overextend starting at [idx] and not appear at all at [idx-1]
  // unless it also starts after us (at which point we don't need it in our overextend list)
  //
  // If [idx].start == range.start then we have already picked a natural order and the sorting of
  // the overall list by size will ensure we are found at the right point. We can copy its list of
  // overextends as they will all apply to us.
  //
  // Whether [idx-1].start < range.start or [idx-1].start == range.start doens't matter. In either
  // case all of [idx-1]'s overextends could potentially overextend us so we filter the list and
  // apply all the ones which are relevant.
  //
  // We don't have to search any further back because [idx-1]'s we define overextension to be
  // conservative - anything which ends past a range's start. This means there are things in
  // [idx-1]'s list which can never be used because tehy are smaller than it, but this is for the
  // benefit of the check here so we can inherit its list into our potentially smaller range.
  //
  // In both cases we also need to apply any overextend directly from [idx-1] and [idx] because
  // nodes don't appear in their own lists.

  // keep track of which ranges we've already added, include ourselves implicitly, and only add ones we haven't seen
  nbdarray<ResourceId> already = {range.id};

  // if we have a next neighbour which starts at the same point as us
  if(idx < addresses.size() && addresses[idx].start == range.start)
  {
    OverextendNode *src = addresses[idx].next;
    OverextendNode *dst = &newNode;

    // copy all entries. These all need to be included since our overextension list includes all ranges
    // that overlap our start point (and since the start point is identical, the list is identical).
    while(src)
    {
      dst->next = MakeListNode(*src);
      dst = dst->next;
      already.push_back(src->id);

      src = src->next;
    }

    // idx does over-extend us then and is not in its own list - add it here
    AddSorted(&newNode, addresses[idx]);
    already.push_back(addresses[idx].id);
  }

  // if we have a previous neighbour
  if(idx > 0)
  {
    OverextendNode *src = addresses[idx - 1].next;

    while(src)
    {
      // this should always be true, because overextends of a previous node should always start before our range
      NBDASSERT(src->start <= range.start);

      // this node does overextend then insert in sorted order
      if(src->realEnd > range.start && !already.contains(src->id))
      {
        AddSorted(&newNode, *src);
        already.push_back(src->id);
      }

      src = src->next;
    }

    // idx-1 is not in its own list, if it overextends us add it here
    if(addresses[idx - 1].realEnd > range.start && !already.contains(addresses[idx - 1].id))
      AddSorted(&newNode, addresses[idx - 1]);
  }

  addresses.insert(idx, newNode);

  // reverse to the first range with the same start, ignoring size
  while(idx > 0 && addresses[idx - 1].start == range.start)
    idx--;

  // loop over every range we really overextend
  for(; idx < addresses.size(); idx++)
  {
    // stop if we've reached a range that we don't overextend
    if(range.realEnd <= addresses[idx].start)
      break;

    // add ourselves to this range's overextend list if we overextend it
    if(range.realEnd > addresses[idx].start && range.id != addresses[idx].id)
      AddSorted(&addresses[idx], range);
  }
}

void GPUAddressRangeTracker::RemoveRangeAtIndex(size_t idx)
{
  GPUAddressRange range = addresses[idx];
  // the caller must lock.

  // delete our own largest list, if there is one
  DeleteWholeList(&addresses[idx]);
  addresses.erase(idx);

  // reverse to the first range with the same start
  while(idx > 0 && addresses[idx - 1].start == range.start)
    idx--;

  // loop over every range we could overextend
  for(; idx < addresses.size(); idx++)
  {
    // stop if we've reached a range that we don't overextend
    if(range.realEnd <= addresses[idx].start)
      break;

    // remove ourselves from this range's list, if present
    OverextendNode *prev = NULL;
    OverextendNode *cur = addresses[idx].next;
    while(cur)
    {
      // if we found the id we're looking for
      if(cur->id == range.id)
      {
        // if prev is NULL this is the head node, update the head pointer. Otherwise take the prev
        // node and point it to the next node
        if(prev == NULL)
        {
          addresses[idx].next = cur->next;
        }
        else
        {
          prev->next = cur->next;
        }

        // delete the node
        DeleteNode(cur);
        break;
      }

      prev = cur;
      cur = cur->next;
    }
  }
}

void GPUAddressRangeTracker::Clear()
{
  SCOPED_WRITELOCK(addressLock);

  // clear addresses list. Linked lists will be deleted in batch below
  addresses.clear();

  for(size_t i = 0; i < batchNodeAllocs.size(); i++)
    delete[] batchNodeAllocs[i];
  batchNodeAllocs.clear();
  freeNodes.clear();
}

bool GPUAddressRangeTracker::IsEmpty()
{
  SCOPED_READLOCK(addressLock);
  return addresses.empty();
}

nbdarray<GPUAddressRange> GPUAddressRangeTracker::GetAddresses()
{
  nbdarray<GPUAddressRange> ret;
  ret.reserve(addresses.size());

  {
    SCOPED_READLOCK(addressLock);
    for(size_t i = 0; i < addresses.size(); i++)
      ret.push_back(addresses[i]);
  }

  return ret;
}

nbdarray<ResourceId> GPUAddressRangeTracker::GetIDs()
{
  nbdarray<ResourceId> ret;
  ret.reserve(addresses.size());

  {
    SCOPED_READLOCK(addressLock);
    for(size_t i = 0; i < addresses.size(); i++)
      ret.push_back(addresses[i].id);
  }

  return ret;
}

size_t GPUAddressRangeTracker::FindLastRangeBeforeOrAtAddress(GPUAddressRange::Address addr)
{
  // the caller must lock.

  if(addresses.empty())
    return ~0U;

  // start looking at the whole range
  size_t first = 0;
  size_t count = addresses.size();

  while(count > 1)
  {
    // look at the midpoint
    size_t halfrange = count / 2;
    size_t mid = first + halfrange;

    // if the midpoint is after our address, bisect down to the lower half and exclude the midpoint
    if(addr < addresses[mid].start)
    {
      count = halfrange;
    }
    else
    {
      // midpoint is before or at our address, use upper half
      first = mid;
      count -= halfrange;
    }
  }

  // if first is 0 and the address range doesn't match, indicate that by returning ~0U
  if(first == 0 && addr < addresses[first].start)
    return ~0U;

  return first;
}

template <bool allowOOB>
void GPUAddressRangeTracker::GetResIDFromAddr(GPUAddressRange::Address addr, ResourceId &id,
                                              uint64_t &offs)
{
  id = ResourceId();
  offs = 0;

  if(addr == 0)
    return;

  GPUAddressRange range;

  {
    SCOPED_READLOCK(addressLock);

    // search for the address. This will return the largest range which starts before or at this address
    size_t idx = FindLastRangeBeforeOrAtAddress(addr);

    // ~0U is returned if the address is before the first range in our list. That means no match
    if(idx == ~0U)
      return;

    // this range is already the largest before or at the address by virtue of our sorting and search
    range = addresses[idx];

    // if this is out of the range and we have a next list of overextensions, go to the first one in
    // the list immediately and try with that. It may still fail but it has the best chance to succeed
    if(addr >= range.realEnd && addresses[idx].next)
      range = *addresses[idx].next;
  }

  // this should not happen, it's just for safety/readability. The only time the found range would
  // be after the address is if the address is before all ranges which would return above after
  // FindLastRangeBeforeOrAtAddress() fails
  if(addr < range.start)
    return;    // @NoCoverage

  // if OOB isn't allowed, check against real end
  if(!allowOOB)
  {
    if(addr >= range.realEnd)
      return;
  }

  // always check against OOB end
  if(addr >= range.oobEnd)
    return;

  id = range.id;
  offs = addr - range.start;
}

template void GPUAddressRangeTracker::GetResIDFromAddr<false>(GPUAddressRange::Address addr,
                                                              ResourceId &id, uint64_t &offs);
template void GPUAddressRangeTracker::GetResIDFromAddr<true>(GPUAddressRange::Address addr,
                                                             ResourceId &id, uint64_t &offs);

void GPUAddressRangeTracker::GetResIDBoundForAddr(GPUAddressRange::Address addr, ResourceId &lower,
                                                  GPUAddressRange::Address &lowerVA,
                                                  ResourceId &upper,
                                                  GPUAddressRange::Address &upperVA)
{
  lower = upper = ResourceId();
  lowerVA = upperVA = 0;

  if(addr == 0)
    return;

  {
    SCOPED_READLOCK(addressLock);

    if(addresses.empty())
      return;

    size_t idx = FindLastRangeBeforeOrAtAddress(addr);

    // if the addr is before first known range, it's bounded on upper only
    if(idx == ~0U)
    {
      upper = addresses[0].id;
      upperVA = addresses[0].start;
      return;
    }

    lower = addresses[idx].id;
    lowerVA = addresses[idx].start;

    // if this range contains the address exactly, return it as a tight bound
    if(addresses[idx].realEnd > addr)
    {
      upper = addresses[idx].id;
      upperVA = addresses[idx].realEnd;
      return;
    }

    // otherwise the address is past its end but before the next. Move one allocation along - we
    // already know that we picked the largest allocation that covers this address
    idx++;

    // if this wasn't the end, return the upper bound
    if(idx < addresses.size())
    {
      upper = addresses[idx].id;
      upperVA = addresses[idx].start;
    }
  }
}

#if ENABLED(ENABLE_UNIT_TESTS)

#undef None
#undef Always

#include "catch/catch.hpp"

namespace TestIDs
{
ResourceId a = ResourceIDGen::GetNewUniqueID();
ResourceId b = ResourceIDGen::GetNewUniqueID();
ResourceId c = ResourceIDGen::GetNewUniqueID();
ResourceId d = ResourceIDGen::GetNewUniqueID();
ResourceId e = ResourceIDGen::GetNewUniqueID();
ResourceId f = ResourceIDGen::GetNewUniqueID();
ResourceId g = ResourceIDGen::GetNewUniqueID();

nbdarray<ResourceId> extraIDs;
};

template <>
nbdstr DoStringise(const nbdpair<ResourceId, uint64_t> &el)
{
  using namespace TestIDs;

  nbdarray<ResourceId> ids = {a, b, c, d, e, f, g};
  nbdstr idname = "a";
  int idx = ids.indexOf(el.first);
  if(idx >= 0)
    idname[0] += (char)idx;
  else if(el.first == ResourceId())
    idname = "-";
  else
    idname = StringFormat::Fmt("extra[%d]", extraIDs.indexOf(el.first));

  return "{ " + idname + ", " + StringFormat::Fmt("%#x", el.second) + " }";
}

bool operator==(const GPUAddressRange &a, const GPUAddressRange &b)
{
  return a.start == b.start && a.id == b.id && a.realEnd == b.realEnd && a.oobEnd == b.oobEnd;
}

bool operator<(const GPUAddressRange &a, const GPUAddressRange &b)
{
  if(a.start != b.start)
    return a.start < b.start;

  if(a.realEnd != b.realEnd)
    return !(a.realEnd < b.realEnd);

  return false;
}

static GPUAddressRange MakeRange(ResourceId id, GPUAddressRange::Address addr, uint64_t size,
                                 uint64_t oobPadding = 0)
{
  return {
      addr,
      addr + size,
      addr + size + oobPadding,
      id,
  };
}

nbdpair<ResourceId, uint64_t> make_idoffs(ResourceId a, uint64_t b)
{
  return {a, b};
}

// for the randomly-generated blitz, we don't want to check specific contractual behaviour.
//
// * if the address is contained within only one range, then that is the one that is returned.
// * if the address is contained in multiple ranges, then either:
//   - the returned range has (one of) the closest start point(s) to the address. For an ambiguous
//     mapping this is arguably as good as it can get. This corresponds to the simple matching
//     case
//   - the only ranges that contain the address are smaller and start at the same point. i.e. this
//     was the largest range of a series of equal-start aliases and there is nothing closer.
//   - there is at least one range between the start of first range that contains the address and
//     the address which does *not* contain the address. i.e. the specific carveout we have for our
//     current imperfect results where if we find a bad match at first we allow the return of any of
//     the larger overlaps. This is only allowed if there is such a 'problem' overlaps with small ranges.
//
// These conditions are rather awkward, but it is the only way to require something slightly
// better than just "is the result valid" while allowing for the current imperfect returns that
// don't try to find the tightest bound range as it's not needed. Allowing for a non-containing
// range effectively leaves this leeway that the current system uses by immediately jumping to the
// largest possible result when an exact simple match isn't found by startpoint.
void CheckValidResult(GPUAddressRangeTracker &tracker, const nbdarray<GPUAddressRange> &ranges,
                      GPUAddressRange::Address addr)
{
  nbdpair<ResourceId, uint64_t> result = tracker.GetResIDFromAddr(addr);

  // make the list of ranges that include this address, and track the smallest one and which one the result is from
  uint64_t closestStart = 0;
  size_t resultRangeIndex = ~0U;
  nbdarray<GPUAddressRange> containRanges;
  for(const GPUAddressRange &range : ranges)
  {
    if(range.start <= addr && addr < range.realEnd)
    {
      if(range.start > closestStart)
        closestStart = range.start;

      if(range.id == result.first)
      {
        // verify the offset was calculated properly
        CHECK(addr - range.start == result.second);

        resultRangeIndex = containRanges.size();
      }

      containRanges.push_back(range);
    }
  }

  // if no ranges contain this, we should have returned as such
  if(containRanges.empty())
  {
    REQUIRE(result.first == ResourceId());
    REQUIRE(result.second == 0);
    return;
  }

  // if only one range contains the address, that must be our result if it's to be valid
  if(containRanges.size() == 1)
  {
    REQUIRE(resultRangeIndex == 0);
    return;
  }

  // if the range was the closest start, that's valid
  if(containRanges[resultRangeIndex].start == closestStart)
  {
    SUCCEED("Closest starting range returned");
    return;
  }

  // otherwise, iterate the whole list of ranges - when we encounter our returned range start
  // looking for a small non-matching range in between its start and the address
  bool validImperfectResult = false;
  bool searching = false;
  for(size_t i = 0; i < ranges.size(); i++)
  {
    if(!searching && ranges[i].start <= addr && addr < ranges[i].realEnd)
    {
      GPUAddressRange::Address start = ranges[i].start;

      // skip past all equal-starting ranges that are smaller than the returned range
      while(i < ranges.size() && ranges[i].start == start &&
            ranges[i].RealSize() <= containRanges[resultRangeIndex].RealSize())
        i++;

      // if we're now at a range that's past the address, we already had the largest
      if(i < ranges.size() && ranges[i].start > addr)
      {
        validImperfectResult = true;
        break;
      }

      searching = true;
      continue;
    }

    if(searching && ranges[i].realEnd <= addr)
    {
      validImperfectResult = true;
      break;
    }
  }

  CHECK(validImperfectResult);
}

TEST_CASE("Check GPUAddressRangeTracker", "[gpuaddr]")
{
  GPUAddressRangeTracker tracker;

  nbdpair<ResourceId, uint64_t> none = make_idoffs(ResourceId(), 0ULL);

  using namespace TestIDs;

  SECTION("Basics")
  {
    ResourceId lower, upper;
    GPUAddressRange::Address lowerVA, upperVA;

    CHECK(tracker.GetResIDFromAddr(0) == none);
    CHECK(tracker.GetResIDFromAddr(0x1230000) == none);
    CHECK(tracker.GetResIDFromAddr(0x9990000) == none);

    tracker.GetResIDBoundForAddr(0, lower, lowerVA, upper, upperVA);

    CHECK(lower == ResourceId());
    CHECK(upper == ResourceId());
    CHECK(lowerVA == 0);
    CHECK(upperVA == 0);

    tracker.GetResIDBoundForAddr(0x1230000, lower, lowerVA, upper, upperVA);

    CHECK(lower == ResourceId());
    CHECK(upper == ResourceId());
    CHECK(lowerVA == 0);
    CHECK(upperVA == 0);

    tracker.AddTo(MakeRange(a, 0x1230000, 128));
    tracker.AddTo(MakeRange(b, 0x1250000, 128));

    CHECK(tracker.GetResIDFromAddr(0) == none);

    CHECK(tracker.GetResIDFromAddr(0x1230000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(a, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230001) == make_idoffs(a, 1ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 127) == make_idoffs(a, 127ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 128) == none);

    CHECK(tracker.GetResIDFromAddr(0x1250000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000) == make_idoffs(b, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x1250001) == make_idoffs(b, 1ULL));
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 127) == make_idoffs(b, 127ULL));
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 128) == none);

    tracker.RemoveFrom(0x1250000, b);

    CHECK(tracker.GetResIDFromAddr(0x1230000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(a, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230001) == make_idoffs(a, 1ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 127) == make_idoffs(a, 127ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 128) == none);

    CHECK(tracker.GetResIDFromAddr(0x1250000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250001) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 127) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 128) == none);

    tracker.AddTo(MakeRange(c, 0x1270000, 128));

    CHECK(tracker.GetResIDFromAddr(0x1230000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(a, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230001) == make_idoffs(a, 1ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 127) == make_idoffs(a, 127ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 128) == none);

    CHECK(tracker.GetResIDFromAddr(0x1250000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250001) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 127) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 128) == none);

    CHECK(tracker.GetResIDFromAddr(0x1270000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1270000) == make_idoffs(c, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x1270001) == make_idoffs(c, 1ULL));
    CHECK(tracker.GetResIDFromAddr(0x1270000 + 127) == make_idoffs(c, 127ULL));
    CHECK(tracker.GetResIDFromAddr(0x1270000 + 128) == none);

    EXPECT_ERROR();

    // wrong ID, don't remove
    tracker.RemoveFrom(0x1270000, g);

    CHECK(DID_ERROR_HAPPEN());

    CHECK(tracker.GetResIDFromAddr(0x1230000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(a, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230001) == make_idoffs(a, 1ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 127) == make_idoffs(a, 127ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 128) == none);

    CHECK(tracker.GetResIDFromAddr(0x1250000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250001) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 127) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 128) == none);

    CHECK(tracker.GetResIDFromAddr(0x1270000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1270000) == make_idoffs(c, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x1270001) == make_idoffs(c, 1ULL));
    CHECK(tracker.GetResIDFromAddr(0x1270000 + 127) == make_idoffs(c, 127ULL));
    CHECK(tracker.GetResIDFromAddr(0x1270000 + 128) == none);

    EXPECT_ERROR();

    // wrong address, don't remove
    tracker.RemoveFrom(0x1000, a);

    CHECK(DID_ERROR_HAPPEN());

    CHECK(tracker.GetResIDFromAddr(0x1230000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(a, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230001) == make_idoffs(a, 1ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 127) == make_idoffs(a, 127ULL));
    CHECK(tracker.GetResIDFromAddr(0x1230000 + 128) == none);

    CHECK(tracker.GetResIDFromAddr(0x1250000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250001) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 127) == none);
    CHECK(tracker.GetResIDFromAddr(0x1250000 + 128) == none);

    CHECK(tracker.GetResIDFromAddr(0x1270000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x1270000) == make_idoffs(c, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x1270001) == make_idoffs(c, 1ULL));
    CHECK(tracker.GetResIDFromAddr(0x1270000 + 127) == make_idoffs(c, 127ULL));
    CHECK(tracker.GetResIDFromAddr(0x1270000 + 128) == none);

    nbdarray<ResourceId> ids_ref = {a, c};
    nbdarray<GPUAddressRange> ranges_ref;
    ranges_ref.push_back(MakeRange(a, 0x1230000, 128));
    ranges_ref.push_back(MakeRange(c, 0x1270000, 128));

    nbdarray<ResourceId> ids = tracker.GetIDs();
    nbdarray<GPUAddressRange> ranges = tracker.GetAddresses();

    std::sort(ids.begin(), ids.end());
    std::sort(ranges.begin(), ranges.end());

    CHECK((ids == ids_ref));
    CHECK((ranges == ranges_ref));

    tracker.GetResIDBoundForAddr(0, lower, lowerVA, upper, upperVA);

    CHECK(lower == ResourceId());
    CHECK(upper == ResourceId());
    CHECK(lowerVA == 0);
    CHECK(upperVA == 0);

    tracker.GetResIDBoundForAddr(0x1000, lower, lowerVA, upper, upperVA);

    CHECK(lower == ResourceId());
    CHECK(upper == a);
    CHECK(lowerVA == 0);
    CHECK(upperVA == 0x1230000);

    tracker.GetResIDBoundForAddr(0x1230000, lower, lowerVA, upper, upperVA);

    CHECK(lower == a);
    CHECK(upper == a);
    CHECK(lowerVA == 0x1230000);
    CHECK(upperVA == 0x1230080);

    tracker.GetResIDBoundForAddr(0x1230010, lower, lowerVA, upper, upperVA);

    CHECK(lower == a);
    CHECK(upper == a);
    CHECK(lowerVA == 0x1230000);
    CHECK(upperVA == 0x1230080);

    tracker.GetResIDBoundForAddr(0x1230100, lower, lowerVA, upper, upperVA);

    CHECK(lower == a);
    CHECK(upper == c);
    CHECK(lowerVA == 0x1230000);
    CHECK(upperVA == 0x1270000);

    tracker.GetResIDBoundForAddr(0x1280000, lower, lowerVA, upper, upperVA);

    CHECK(lower == c);
    CHECK(upper == ResourceId());
    CHECK(lowerVA == 0x1270000);
    CHECK(upperVA == 0);
  }

  SECTION("Insertion order doesn't affect return value")
  {
    // smallest-to-largest
    tracker.AddTo(MakeRange(a, 0x1230000, 128));
    tracker.AddTo(MakeRange(b, 0x1230000, 256));
    tracker.AddTo(MakeRange(c, 0x1230000, 512));

    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(c, 0ULL));

    tracker.Clear();

    // largest-to-smallest
    tracker.AddTo(MakeRange(c, 0x1230000, 512));
    tracker.AddTo(MakeRange(b, 0x1230000, 256));
    tracker.AddTo(MakeRange(a, 0x1230000, 128));

    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(c, 0ULL));

    tracker.Clear();

    // out-of-order, largest last
    tracker.AddTo(MakeRange(b, 0x1230000, 256));
    tracker.AddTo(MakeRange(a, 0x1230000, 128));
    tracker.AddTo(MakeRange(c, 0x1230000, 512));

    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(c, 0ULL));

    tracker.Clear();

    // out-of-order, smallest last
    tracker.AddTo(MakeRange(b, 0x1230000, 256));
    tracker.AddTo(MakeRange(c, 0x1230000, 512));
    tracker.AddTo(MakeRange(a, 0x1230000, 128));

    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(c, 0ULL));

    tracker.Clear();

    // with a pre-existing address before the ranges
    tracker.AddTo(MakeRange(d, 0x1200000, 512));
    tracker.AddTo(MakeRange(c, 0x1230000, 512));
    tracker.AddTo(MakeRange(b, 0x1230000, 256));
    tracker.AddTo(MakeRange(a, 0x1230000, 128));

    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(c, 0ULL));

    tracker.Clear();

    // with a pre-existing address after the ranges
    tracker.AddTo(MakeRange(d, 0x1250000, 512));
    tracker.AddTo(MakeRange(c, 0x1230000, 512));
    tracker.AddTo(MakeRange(b, 0x1230000, 256));
    tracker.AddTo(MakeRange(a, 0x1230000, 128));

    CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(c, 0ULL));

    tracker.Clear();
  }

  SECTION("OOB")
  {
    tracker.AddTo(MakeRange(a, 0x1230000, 128, 128));

    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230001) == make_idoffs(a, 1ULL));
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230000 + 127) == make_idoffs(a, 127ULL));
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230000 + 128) == make_idoffs(a, 128ULL));
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230000 + 255) == make_idoffs(a, 255ULL));
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230000 + 256) == none);

    tracker.RemoveFrom(0x1230000, a);

    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230001) == none);
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230000 + 127) == none);
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230000 + 128) == none);
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230000 + 255) == none);
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230000 + 256) == none);

    tracker.AddTo(MakeRange(a, 0x1230000, 0x10000, 0x10000));
    tracker.AddTo(MakeRange(b, 0x1250000, 0x10000, 0x10000));

    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1230001) == make_idoffs(a, 0x1ULL));
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1240000) == make_idoffs(a, 0x10000ULL));
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1240001) == make_idoffs(a, 0x10001ULL));
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x124ffff) == make_idoffs(a, 0x1ffffULL));
    CHECK(tracker.GetResIDFromAddrAllowOutOfBounds(0x1250000) == make_idoffs(b, 0ULL));
  }

  SECTION("co-sited overlap returning largest")
  {
    auto checker = [&tracker, none](ResourceId id) {
      CHECK(tracker.GetResIDFromAddr(0x1230000 - 1) == none);
      CHECK(tracker.GetResIDFromAddr(0x1230000) == make_idoffs(id, 0ULL));
      CHECK(tracker.GetResIDFromAddr(0x1230001) == make_idoffs(id, 1ULL));
      CHECK(tracker.GetResIDFromAddr(0x1230010) == make_idoffs(id, 0x10ULL));
      CHECK(tracker.GetResIDFromAddr(0x1230000 + 127) == make_idoffs(id, 127ULL));

      // check the range of a we expect
      if(id == a)
      {
        CHECK(tracker.GetResIDFromAddr(0x1230000 + 128) == make_idoffs(id, 128ULL));
        CHECK(tracker.GetResIDFromAddr(0x1230000 + 255) == make_idoffs(id, 255ULL));
      }
      else
      {
        CHECK(tracker.GetResIDFromAddr(0x1230000 + 128) == none);
        CHECK(tracker.GetResIDFromAddr(0x1230000 + 255) == none);
      }
    };

    SECTION("big before small")
    {
      tracker.AddTo(MakeRange(a, 0x1230000, 256));
      tracker.AddTo(MakeRange(b, 0x1230000, 128));

      // should find a regardless of added order
      checker(a);

      SECTION("remove a")
      {
        // if a is removed, we now find b
        tracker.RemoveFrom(0x1230000, a);

        checker(b);
      }

      SECTION("remove b")
      {
        // if b is removed, we still find a
        tracker.RemoveFrom(0x1230000, b);

        checker(a);
      }
    }

    SECTION("small before big")
    {
      tracker.AddTo(MakeRange(b, 0x1230000, 128));
      tracker.AddTo(MakeRange(a, 0x1230000, 256));

      // should find a regardless of added order
      checker(a);

      SECTION("remove a")
      {
        // if a is removed, we now find b
        tracker.RemoveFrom(0x1230000, a);

        checker(b);
      }

      SECTION("remove b")
      {
        // if b is removed, we still find a
        tracker.RemoveFrom(0x1230000, b);

        checker(a);
      }
    }
  }

  SECTION("Partially overlapping ranges that aren't super/subset")
  {
    tracker.AddTo(MakeRange(c, 0x12000, 0x0800));
    tracker.AddTo(MakeRange(d, 0x12600, 0x0800));
    tracker.AddTo(MakeRange(e, 0x12800, 0x0200));

    CHECK(tracker.GetResIDFromAddr(0x12000) == make_idoffs(c, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12100) == make_idoffs(c, 0x100ULL));
    CHECK(tracker.GetResIDFromAddr(0x125ff) == make_idoffs(c, 0x5ffULL));
    CHECK(tracker.GetResIDFromAddr(0x12600) == make_idoffs(d, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12700) == make_idoffs(d, 0x100ULL));
    CHECK(tracker.GetResIDFromAddr(0x127ff) == make_idoffs(d, 0x1ffULL));
    CHECK(tracker.GetResIDFromAddr(0x12800) == make_idoffs(e, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12900) == make_idoffs(e, 0x100ULL));
    CHECK(tracker.GetResIDFromAddr(0x129ff) == make_idoffs(e, 0x1ffULL));
  }

  SECTION("lots of overlap and removals")
  {
    tracker.AddTo(MakeRange(a, 0x12300000, 100));
    tracker.AddTo(MakeRange(b, 0x12300000, 200));
    tracker.AddTo(MakeRange(c, 0x12300000, 300));
    tracker.AddTo(MakeRange(d, 0x12300000, 400));
    tracker.AddTo(MakeRange(e, 0x12300000, 500));
    tracker.AddTo(MakeRange(f, 0x12300000, 600));

    CHECK(tracker.GetResIDFromAddr(0x12300000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x12300000) == make_idoffs(f, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12300f00) == none);

    tracker.RemoveFrom(0x12300000, c);

    CHECK(tracker.GetResIDFromAddr(0x12300000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x12300000) == make_idoffs(f, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12300f00) == none);

    tracker.RemoveFrom(0x12300000, f);

    CHECK(tracker.GetResIDFromAddr(0x12300000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x12300000) == make_idoffs(e, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12300f00) == none);

    tracker.RemoveFrom(0x12300000, a);

    CHECK(tracker.GetResIDFromAddr(0x12300000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x12300000) == make_idoffs(e, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12300f00) == none);

    tracker.RemoveFrom(0x12300000, d);

    CHECK(tracker.GetResIDFromAddr(0x12300000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x12300000) == make_idoffs(e, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12300f00) == none);

    tracker.RemoveFrom(0x12300000, e);

    CHECK(tracker.GetResIDFromAddr(0x12300000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x12300000) == make_idoffs(b, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12300f00) == none);

    tracker.RemoveFrom(0x12300000, b);

    CHECK(tracker.GetResIDFromAddr(0x12300000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x12300000) == none);
    CHECK(tracker.GetResIDFromAddr(0x12300f00) == none);
  }

  /////////////////////////////////////////////////////////////////////////////////////////////////
  // tests below here are the 'hard' ones - they test finding a larger overlapping range when
  // searching for an address that is after a smaller range mid-way through (not at the start)

  SECTION("Finding addresses in overlapping ranges, largest added first")
  {
    // we should find a in between any gaps the others define
    tracker.AddTo(MakeRange(a, 0x12000000, 0x1000000));
    // cosited to start with
    tracker.AddTo(MakeRange(b, 0x12000000, 0x1000));

    CHECK(tracker.GetResIDFromAddr(0x12000000 - 1) == none);
    CHECK(tracker.GetResIDFromAddr(0x12000000) == make_idoffs(a, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12000100) == make_idoffs(a, 0x100ULL));
    CHECK(tracker.GetResIDFromAddr(0x12f00000) == make_idoffs(a, 0xf00000ULL));

    // then a few later ranges
    tracker.AddTo(MakeRange(c, 0x12100000, 0x1000));
    tracker.AddTo(MakeRange(d, 0x12200000, 0x1000));
    tracker.AddTo(MakeRange(e, 0x12300000, 0x1000));

    // we can find in those ranges
    CHECK(tracker.GetResIDFromAddr(0x12100000) == make_idoffs(c, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12100100) == make_idoffs(c, 0x100ULL));

    CHECK(tracker.GetResIDFromAddr(0x12200000) == make_idoffs(d, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12200100) == make_idoffs(d, 0x100ULL));

    CHECK(tracker.GetResIDFromAddr(0x12300000) == make_idoffs(e, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12300100) == make_idoffs(e, 0x100ULL));

    // in between those ranges we should find a again, even though the closest match before is one
    // of the smaller ranges
    CHECK(tracker.GetResIDFromAddr(0x120f0000) == make_idoffs(a, 0x0f0000ULL));
    CHECK(tracker.GetResIDFromAddr(0x121f0000) == make_idoffs(a, 0x1f0000ULL));
    CHECK(tracker.GetResIDFromAddr(0x122f0000) == make_idoffs(a, 0x2f0000ULL));
    CHECK(tracker.GetResIDFromAddr(0x123f0000) == make_idoffs(a, 0x3f0000ULL));
    CHECK(tracker.GetResIDFromAddr(0x12f00000) == make_idoffs(a, 0xf00000ULL));

    // remove the nodes now starting with the largest, and ensure we have completely tidied up and not leaked
    tracker.RemoveFrom(0x12000000, a);
    tracker.RemoveFrom(0x12000000, b);
    tracker.RemoveFrom(0x12100000, c);
    tracker.RemoveFrom(0x12200000, d);
    tracker.RemoveFrom(0x12300000, e);

    CHECK(tracker.IsEmpty());
    CHECK(tracker.GetNumLiveNodes() == 0);
  }

  SECTION("Finding addresses in overlapping ranges, largest added last")
  {
    // add the small ranges first
    tracker.AddTo(MakeRange(c, 0x12100000, 0x1000));
    tracker.AddTo(MakeRange(d, 0x12200000, 0x1000));
    tracker.AddTo(MakeRange(e, 0x12300000, 0x1000));

    // we can find in those ranges
    CHECK(tracker.GetResIDFromAddr(0x12100000) == make_idoffs(c, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12100100) == make_idoffs(c, 0x100ULL));

    CHECK(tracker.GetResIDFromAddr(0x12200000) == make_idoffs(d, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12200100) == make_idoffs(d, 0x100ULL));

    CHECK(tracker.GetResIDFromAddr(0x12300000) == make_idoffs(e, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12300100) == make_idoffs(e, 0x100ULL));

    // in between there's nothing
    CHECK(tracker.GetResIDFromAddr(0x120f0000) == none);
    CHECK(tracker.GetResIDFromAddr(0x121f0000) == none);
    CHECK(tracker.GetResIDFromAddr(0x122f0000) == none);
    CHECK(tracker.GetResIDFromAddr(0x123f0000) == none);
    CHECK(tracker.GetResIDFromAddr(0x12f00000) == none);

    // now we should find a in between any gaps the others define
    tracker.AddTo(MakeRange(a, 0x12000000, 0x1000000));
    // cosited small range to ensure that doesn't break anything
    tracker.AddTo(MakeRange(b, 0x12000000, 0x1000));

    // in between those ranges we should find a now, even though the closest match before is one
    // of the smaller ranges
    CHECK(tracker.GetResIDFromAddr(0x120f0000) == make_idoffs(a, 0x0f0000ULL));
    CHECK(tracker.GetResIDFromAddr(0x121f0000) == make_idoffs(a, 0x1f0000ULL));
    CHECK(tracker.GetResIDFromAddr(0x122f0000) == make_idoffs(a, 0x2f0000ULL));
    CHECK(tracker.GetResIDFromAddr(0x123f0000) == make_idoffs(a, 0x3f0000ULL));
    CHECK(tracker.GetResIDFromAddr(0x12f00000) == make_idoffs(a, 0xf00000ULL));

    // remove the nodes now ending with the largest, and ensure we have completely tidied up and not leaked
    tracker.RemoveFrom(0x12000000, b);
    tracker.RemoveFrom(0x12100000, c);
    tracker.RemoveFrom(0x12200000, d);
    tracker.RemoveFrom(0x12300000, e);
    tracker.RemoveFrom(0x12000000, a);

    CHECK(tracker.IsEmpty());
    CHECK(tracker.GetNumLiveNodes() == 0);
  }

  SECTION("Finding addresses in overlapping ranges, nested levels of overlap")
  {
    // large range which is the backstop
    tracker.AddTo(MakeRange(a, 0x12000000, 0x1000000));
    // cosited small range
    tracker.AddTo(MakeRange(b, 0x12000000, 0x1000));

    // then a later ranges, which overlap
    tracker.AddTo(MakeRange(c, 0x12100000, 0x10000));
    tracker.AddTo(MakeRange(d, 0x12101000, 0x1000));
    tracker.AddTo(MakeRange(e, 0x12200000, 0x1000));

    // first addresses are just in c
    CHECK(tracker.GetResIDFromAddr(0x12100000) == make_idoffs(c, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12100100) == make_idoffs(c, 0x100ULL));

    // these addresses are more tightly in d
    CHECK(tracker.GetResIDFromAddr(0x12101000) == make_idoffs(d, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12101fff) == make_idoffs(d, 0xfffULL));

    // this address is past d, back in c. Our behaviour does not guarantee that we return c though,
    // it returns the largest valid address when the 'simple' lookup fails which is in a
    // CHECK(tracker.GetResIDFromAddr(0x12102000) == make_idoffs(c, 0x2000ULL));
    CHECK(tracker.GetResIDFromAddr(0x12102000) == make_idoffs(a, 0x102000ULL));

    // and this address is past c and back in a, since it's before e
    CHECK(tracker.GetResIDFromAddr(0x12120000) == make_idoffs(a, 0x120000ULL));

    // remove the nodes now and ensure we have completely tidied up and not leaked
    tracker.RemoveFrom(0x12000000, a);
    tracker.RemoveFrom(0x12000000, b);
    tracker.RemoveFrom(0x12100000, c);
    tracker.RemoveFrom(0x12101000, d);
    tracker.RemoveFrom(0x12200000, e);

    CHECK(tracker.IsEmpty());
    CHECK(tracker.GetNumLiveNodes() == 0);
  }

  SECTION("Finding addresses in overlapping ranges, partial overlaps")
  {
    // large range which is the backstop
    tracker.AddTo(MakeRange(a, 0x12000000, 0x1000000));
    // cosited small range
    tracker.AddTo(MakeRange(b, 0x12000000, 0x1000));

    // then a later ranges, which overlap with c covering only some of d
    tracker.AddTo(MakeRange(c, 0x12100000, 0x10000));
    tracker.AddTo(MakeRange(d, 0x12101000, 0x10000));
    tracker.AddTo(MakeRange(e, 0x12200000, 0x10000));

    // first addresses are just in c
    CHECK(tracker.GetResIDFromAddr(0x12100000) == make_idoffs(c, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12100100) == make_idoffs(c, 0x100ULL));

    // these addresses are more tightly in d
    CHECK(tracker.GetResIDFromAddr(0x12101000) == make_idoffs(d, 0ULL));
    CHECK(tracker.GetResIDFromAddr(0x12101fff) == make_idoffs(d, 0xfffULL));
    CHECK(tracker.GetResIDFromAddr(0x12102000) == make_idoffs(d, 0x1000ULL));

    // this address is in d but not in c
    CHECK(tracker.GetResIDFromAddr(0x12110fff) == make_idoffs(d, 0xffffULL));

    // and this address is past d but before e
    CHECK(tracker.GetResIDFromAddr(0x12120000) == make_idoffs(a, 0x120000ULL));

    // remove the nodes now and ensure we have completely tidied up and not leaked
    tracker.RemoveFrom(0x12000000, a);
    tracker.RemoveFrom(0x12000000, b);
    tracker.RemoveFrom(0x12100000, c);
    tracker.RemoveFrom(0x12101000, d);
    tracker.RemoveFrom(0x12200000, e);

    CHECK(tracker.IsEmpty());
    CHECK(tracker.GetNumLiveNodes() == 0);
  }

  SECTION(
      "Finding addresses in overlapping ranges, multiple overlaps with different removal orders")
  {
    // large range which is the backstop
    tracker.AddTo(MakeRange(a, 0x12000000, 0x1000000));

    tracker.AddTo(MakeRange(b, 0x12010000, 0x10000));
    tracker.AddTo(MakeRange(c, 0x12015000, 0x10000));
    tracker.AddTo(MakeRange(d, 0x12018000, 0x10000));
    tracker.AddTo(MakeRange(e, 0x12022000, 0x10000));

    // this range is overextended many times
    tracker.AddTo(MakeRange(f, 0x1201a000, 0x1000));

    CHECK(tracker.GetResIDFromAddr(0x1201a100) == make_idoffs(f, 0x100ULL));
    // once we're past f we should return the largest, which is a right now
    CHECK(tracker.GetResIDFromAddr(0x1201b000) == make_idoffs(a, 0x1b000ULL));
    CHECK(tracker.GetResIDFromAddr(0x12023000) == make_idoffs(e, 0x1000ULL));

    SECTION("remove a then d")
    {
      tracker.RemoveFrom(0x12000000, a);

      // d now is the last overextend
      CHECK(tracker.GetResIDFromAddr(0x1201b000) == make_idoffs(d, 0x3000ULL));
      CHECK(tracker.GetResIDFromAddr(0x12023000) == make_idoffs(e, 0x1000ULL));

      tracker.RemoveFrom(0x12018000, d);

      // c is the last overextend
      CHECK(tracker.GetResIDFromAddr(0x1201b000) == make_idoffs(c, 0x6000ULL));
      CHECK(tracker.GetResIDFromAddr(0x12023000) == make_idoffs(e, 0x1000ULL));
    }

    SECTION("remove d then a")
    {
      tracker.RemoveFrom(0x12018000, d);

      CHECK(tracker.GetResIDFromAddr(0x1201b000) == make_idoffs(a, 0x1b000ULL));
      CHECK(tracker.GetResIDFromAddr(0x12023000) == make_idoffs(e, 0x1000ULL));

      tracker.RemoveFrom(0x12000000, a);

      // c is the last overextend as d is already removed
      CHECK(tracker.GetResIDFromAddr(0x1201b000) == make_idoffs(c, 0x6000ULL));
      CHECK(tracker.GetResIDFromAddr(0x12023000) == make_idoffs(e, 0x1000ULL));
    }
  }

  SECTION("Ensure overextensions are carried properly")
  {
    tracker.AddTo(MakeRange(a, 0x12010000, 0x10000));
    tracker.AddTo(MakeRange(b, 0x12015000, 0x30000));
    tracker.AddTo(MakeRange(c, 0x12018000, 0x1000));

    // b ends after a, so any results after c should return b not a
    CHECK(tracker.GetResIDFromAddr(0x12018800) == make_idoffs(c, 0x800ULL));
    CHECK(tracker.GetResIDFromAddr(0x12019000) == make_idoffs(b, 0x4000ULL));

    // however as soon as b is removed, we need that information to now return a. Ensure it was preserved
    tracker.RemoveFrom(0x12015000, b);
    CHECK(tracker.GetResIDFromAddr(0x12019000) == make_idoffs(a, 0x9000ULL));
  }

  SECTION("Large-scale overlap blitz")
  {
    Catch::SimplePcg32 rng;
    // consistent seed
    rng.seed(0x1a2b3c4d);

    // ensure the rng hasn't changed
    REQUIRE(rng() == 0xe95de192);

    // we use the random number generator but we don't just generate random ranges as that would be
    // too hard to trigger specific edge cases we care about. Instead we use it mostly to make
    // randomly ordered decisions

    nbdarray<GPUAddressRange> baseRanges;

    // Some of these will be split into multiple ranges, subdivided, or duplicated to
    // create more actual ranges
    for(size_t iter = 0; iter < 5000; iter++)
    {
      // generate new address range a low amount of the time (or until we have enough ranges)
      if(baseRanges.size() < 4 || (rng() % 5) == 0)
      {
        ResourceId id = ResourceIDGen::GetNewUniqueID();
        extraIDs.push_back(id);

        // base for all addresses
        GPUAddressRange::Address addr = 0x10000000ULL;

        // don't overlap base ranges, this will be handled with the suballocations
        if(!baseRanges.empty())
          addr = AlignUp(baseRanges.back().realEnd, (uint64_t)0x100000ULL);

        addr += uint64_t((rng() % 0x10000U) + 0x10000U) << 16;
        // size is at least 64k up to 8GB
        uint64_t size = uint64_t((rng() % 0x10000U) + 0x10000U) << 16;

        baseRanges.push_back(MakeRange(id, addr, size));
        tracker.AddTo(baseRanges.back());
      }
      else
      {
        GPUAddressRange &range = baseRanges[rng() % baseRanges.size()];

        uint64_t suballocSize = NBDMAX((uint64_t)256ULL, range.RealSize() / 16);

        uint32_t mode = rng() % 100;
        if(mode < 20)
        {
          // pick a random subrange and allocate it
          ResourceId id = ResourceIDGen::GetNewUniqueID();
          uint64_t size = NBDMAX((uint64_t)256ULL, AlignUp(rng() % suballocSize, (uint64_t)256ULL));
          uint64_t offset = rng() % NBDMIN((uint64_t)1ULL, range.RealSize() - size);

          tracker.AddTo(MakeRange(id, range.start + offset, size));
        }
        else if(mode < 40)
        {
          // generate N ranges that are contiguous
          uint64_t size = NBDMAX((uint64_t)256ULL, AlignUp(rng() % suballocSize, (uint64_t)256ULL));
          uint64_t offset = rng() % NBDMIN((uint64_t)1ULL, range.RealSize() - size);
          uint64_t numRanges = NBDMAX(1ULL, NBDMIN(size / 256ULL, rng() % 6ULL));

          size /= numRanges;

          REQUIRE(size >= 256ULL);

          for(uint64_t i = 0; i < numRanges; i++)
          {
            ResourceId id = ResourceIDGen::GetNewUniqueID();
            tracker.AddTo(MakeRange(id, range.start + offset, size));
            offset += size;
          }
        }
        else if(mode < 98)
        {
          // generate some deliberately overlapping ranges
          uint64_t size = NBDMAX((uint64_t)256ULL, AlignUp(rng() % suballocSize, (uint64_t)256ULL));
          uint64_t step = size >> 4;
          uint64_t offset = rng() % NBDMIN((uint64_t)1ULL, range.RealSize() - size);
          uint64_t numRanges = NBDMAX(1ULL, NBDMIN(size / 256ULL, rng() % 6ULL));

          size /= numRanges;

          REQUIRE(size >= 256ULL);

          for(uint64_t i = 0; i < numRanges; i++)
          {
            ResourceId id = ResourceIDGen::GetNewUniqueID();
            tracker.AddTo(MakeRange(id, range.start + offset, size));
            offset += step;
          }
        }
        else
        {
          // add a random range cosited with the start of the base range
          ResourceId id = ResourceIDGen::GetNewUniqueID();
          uint64_t size = NBDMAX((uint64_t)256ULL, AlignUp(rng() % suballocSize, (uint64_t)256ULL));

          tracker.AddTo(MakeRange(id, range.start, size));
        }
      }
    }

    nbdarray<GPUAddressRange> ranges = tracker.GetAddresses();

    // for every range, check a series of addresses around it and ensure that the resulting query is valid
    for(const GPUAddressRange &range : ranges)
    {
      CheckValidResult(tracker, ranges, NBDMAX(range.start, (uint64_t)0x100ULL) - 0x100);
      CheckValidResult(tracker, ranges, NBDMAX(range.start, (uint64_t)0x80ULL) - 0x80);
      CheckValidResult(tracker, ranges, NBDMAX(range.start, (uint64_t)1ULL) - 1);
      CheckValidResult(tracker, ranges, range.start);
      CheckValidResult(tracker, ranges, range.start + 1);
      CheckValidResult(tracker, ranges, range.start + 2);
      CheckValidResult(tracker, ranges, range.start + 0x80);
      CheckValidResult(tracker, ranges, range.start + 123);
      CheckValidResult(tracker, ranges, range.start + 0x100);
      CheckValidResult(tracker, ranges, range.realEnd - 10);
      CheckValidResult(tracker, ranges, range.realEnd - 2);
      CheckValidResult(tracker, ranges, range.realEnd - 1);
      CheckValidResult(tracker, ranges, range.realEnd);
      CheckValidResult(tracker, ranges, range.realEnd + 1);
      CheckValidResult(tracker, ranges, range.realEnd + 2);
      CheckValidResult(tracker, ranges, range.realEnd + 0x80);
      CheckValidResult(tracker, ranges, range.realEnd + 123);
      CheckValidResult(tracker, ranges, range.realEnd + 0x100);
    }
  }

  // don't clear (which is fast and doesn't care to tidy up properly). Remove each range, to ensure
  // lists are cleaned up with no leaks
  {
    for(const GPUAddressRange &range : tracker.GetAddresses())
      tracker.RemoveFrom(range.start, range.id);
  }

  // ensure no leaks
  CHECK(tracker.GetNumLiveNodes() == 0);
}

#endif
