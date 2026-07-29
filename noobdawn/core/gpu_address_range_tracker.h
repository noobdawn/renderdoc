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

#pragma once

#include <functional>

#include "api/replay/resourceid.h"
#include "common/threading.h"

struct GPUAddressRange
{
  using Address = uint64_t;

  Address start, realEnd, oobEnd;
  ResourceId id;

  uint64_t RealSize() const { return realEnd - start; }

  bool operator<(const Address &o) const { return (start < o); }
};

struct GPUAddressRangeTracker
{
  GPUAddressRangeTracker() {}
  ~GPUAddressRangeTracker() { Clear(); }
  // no copying
  GPUAddressRangeTracker(const GPUAddressRangeTracker &) = delete;
  GPUAddressRangeTracker &operator=(const GPUAddressRangeTracker &) = delete;

  void AddTo(const GPUAddressRange &range);
  void RemoveFrom(GPUAddressRange::Address addr, ResourceId id);
  void Clear();
  bool IsEmpty();
  nbdarray<GPUAddressRange> GetAddresses();
  nbdarray<ResourceId> GetIDs();

  void GetResIDFromAddr(GPUAddressRange::Address addr, ResourceId &id, uint64_t &offs)
  {
    return GetResIDFromAddr<false>(addr, id, offs);
  }
  void GetResIDFromAddrAllowOutOfBounds(GPUAddressRange::Address addr, ResourceId &id, uint64_t &offs)
  {
    return GetResIDFromAddr<true>(addr, id, offs);
  }

  nbdpair<ResourceId, uint64_t> GetResIDFromAddr(GPUAddressRange::Address addr)
  {
    nbdpair<ResourceId, uint64_t> ret;
    GetResIDFromAddr(addr, ret.first, ret.second);
    return ret;
  }
  nbdpair<ResourceId, uint64_t> GetResIDFromAddrAllowOutOfBounds(GPUAddressRange::Address addr)
  {
    nbdpair<ResourceId, uint64_t> ret;
    GetResIDFromAddrAllowOutOfBounds(addr, ret.first, ret.second);
    return ret;
  }
  void GetResIDBoundForAddr(GPUAddressRange::Address addr, ResourceId &lower,
                            GPUAddressRange::Address &lowerVA, ResourceId &upper,
                            GPUAddressRange::Address &upperVA);

  // primarily for unit tests to check there's no leak but can also be used for stats
  size_t GetNumLiveNodes() const
  {
    return NumNodesInBatchAlloc * batchNodeAllocs.size() - freeNodes.size();
  }

private:
  struct OverextendNode : public GPUAddressRange
  {
    OverextendNode() = default;
    OverextendNode(const GPUAddressRange &range) : GPUAddressRange(range) {}

    // singly linked list of nodes which start earlier than this one and extend past its start,
    // which we call an 'overextension'. The list is ordered from last endpoint to earliest
    // endpoint. We ignore OOB since we assume anything which overlaps is all part of the same
    // underlying resource and has the same OOB end.
    //
    // the root node owns all the children (this is not shared with any other list or the nodes in
    // the parent list)
    //
    // usually only the head of this list is used, because we don't aim to find the best fit when
    // using this list - we just fall back to the one with the latest endpoint which is the first in
    // the list. the list is primarily needed to manage things with insertions/deletions if there
    // are multiple levels of overextension
    //
    // This list is only used to find the real resource for an address which is past a given node
    // when our naive search for an address finds a too-small resource immediately preceeding that
    // address. For each resource we only add to lists of others that start > our start (because if
    // they start equal to us, the sorting-by-size will naturally find the right one) and only to
    // ranges where our start is past their end. This is specifically to counteract the fact that we
    // may extend past them and not otherwise be found with a normal search.
    //
    // Note: for lookups we would only need the ranges that end past our end (as we will never use
    // nodes in the list where they end before us, as either the address is found in our range or it
    // will be out of their reach too. However we include them in our list because if a node is
    // added after us then that node may need to know about them and we want to limit how much
    // iteration is needed when adding a new node to collect its overextensions.
    //
    // As an example:
    //
    // A 0x100 -> 0x10000
    // B 0x100 -> 0x150
    // C 0x200 -> 0x300
    // D 0x300 -> 0x400
    // E 0x350 -> 0x360
    //
    // Regardless of the order these are added in, they will have these lists:
    //
    // A - NULL      if we find node A there is nothing else that could be found after it. In
    //               practice with this setup we would only find A when searching for an address
    //               0x100-0x1ff and A will be after B in the list since it has a larger endpoint.
    // B - A         if we find node B anything after it would be in A. In practice, this will not
    //               happen A will be sorted after B and will never be found for any address.
    // C - A         if we find node C anything after it would be in A. In practice, this will never
    //               happen as any address after C will be found in D as they are adjacent
    // D - A         similar to the above, but in this case an address could exist after it such as
    //               0x500
    // E - A->C      the end result is the same as D's list, but if A were deleted we would be
    //               able to now find D for queries after the end of E
    OverextendNode *next = NULL;
  };

  static const int NumNodesInBatchAlloc = 1024;
  nbdarray<OverextendNode *> batchNodeAllocs;
  nbdarray<OverextendNode *> freeNodes;

  OverextendNode *MakeListNode(const GPUAddressRange &range)
  {
    if(freeNodes.empty())
    {
      batchNodeAllocs.push_back(new OverextendNode[NumNodesInBatchAlloc]);
      for(int i = 0; i < NumNodesInBatchAlloc; i++)
        freeNodes.push_back(&batchNodeAllocs.back()[i]);
    }

    OverextendNode *ret = freeNodes.back();
    *ret = range;
    ret->next = NULL;
    freeNodes.pop_back();
    return ret;
  }

  void DeleteNode(OverextendNode *node) { freeNodes.push_back(node); }

  void AddSorted(OverextendNode *cur, const GPUAddressRange &range)
  {
    while(cur->next)
    {
      // if the range we're adding ends later than this node, add it here
      if(range.realEnd > cur->next->realEnd)
      {
        OverextendNode *newNode = MakeListNode(range);
        newNode->next = cur->next;
        cur->next = newNode;
        return;
      }

      cur = cur->next;
    }

    // if we've reached the end of the list, add it here
    cur->next = MakeListNode(range);
  }

  void DeleteWholeList(OverextendNode *head)
  {
    OverextendNode *node = head->next;
    while(node)
    {
      OverextendNode *del = node;
      node = node->next;
      DeleteNode(del);
    }
  }

  nbdarray<OverextendNode> addresses;
  Threading::RWLock addressLock;

  template <bool allowOOB>
  void GetResIDFromAddr(GPUAddressRange::Address addr, ResourceId &id, uint64_t &offs);

  size_t FindLastRangeBeforeOrAtAddress(GPUAddressRange::Address addr);
  void AddRangeAtIndex(size_t idx, const GPUAddressRange &range);
  void RemoveRangeAtIndex(size_t idx);
};
