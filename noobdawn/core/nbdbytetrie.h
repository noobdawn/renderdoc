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

#pragma once

#include <algorithm>
#include "api/replay/apidefs.h"
#include "api/replay/nbdarray.h"
#include "api/replay/replay_enums.h"
#include "common/result.h"

// this is a container with a key-value interface where the expectation is that keys are large and
// sparse and so are processed as raw bytes with an intention to do lookups in O(n) time for an n-byte long key.
template <typename Value, uint16_t MaxKeySize = 256>
struct nbdbytetrie
{
  ~nbdbytetrie()
  {
    for(byte *alloc : m_Allocator.allocations)
      delete[] alloc;
  }

  // a view of N bytes
  struct Key
  {
    Key(const byte *b, size_t s) : bytes(b), size(s & NodeOrLeaf::PrefixLengthMask) {}
    Key(const bytebuf &b) : bytes(b.data()), size(b.size() & NodeOrLeaf::PrefixLengthMask) {}
    Key(const Key &) = default;
    Key &operator=(const Key &) = delete;

    byte operator[](uint16_t idx) const { return bytes[idx]; }

    Key ExclusivePrefixBefore(uint16_t n) const
    {
      if(n >= size)
        return Key(NULL, 0);

      return Key(bytes, n);
    }

    Key ExclusiveSuffixAfter(uint16_t n) const
    {
      if(n >= size)
        return Key(NULL, 0);

      return Key(bytes + n + 1, size - 1 - n);
    }

    const byte *bytes;
    const uint16_t size;
  };

  bool contains(const Key &key)
  {
    NodeOrLeaf *n = find(key);
    return n != NULL;
  }

  Value lookup(const Key &key) const
  {
    NodeOrLeaf *n = find(key);
    if(n)
      return n->GetValue();
    return Value();
  }

  bool insert(const Key &key, const Value &val)
  {
    NodeOrLeaf *n = create(key);

    // shouldn't happen unless key is invalid
    if(!n)
      return false;

    if(n->HasValue())
    {
      if(n->GetValue() == val)
        return true;

      // used only so the tests can EXPECT_ERROR()
      if(m_StrictErrorChecking)
      {
        RDResult err;
        SET_ERROR_RESULT(err, ResultCode::InternalError,
                         "Duplicate key with differing value located");
        (void)err;
      }
      else
      {
        NBDWARN("Duplicate key with differing value located");
      }

      return false;
    }

    n->SetValue(val);
    return true;
  }

  void SetStrictErrorChecking(bool check) { m_StrictErrorChecking = check; }

private:
  ///////////////////////////////
  // nodes
  ///////////////////////////////
  struct NodeOrLeaf
  {
    static const uint16_t PrefixLengthMask = 0x1FFFU;
    static const uint16_t ValueBit = 0x2000U;
    static const uint16_t NodeTypeShift = 14;    // 0xC000

    enum NodeType
    {
      Fat = 3,
      Small8 = 2,
      Small2 = 1,
      Leaf = 0,
    };

    bool IsLeaf() const { return NodeType(v._trie >> NodeTypeShift) == Leaf; }
    bool IsNode() const { return NodeType(v._trie >> NodeTypeShift) != Leaf; }
    bool IsFatNode() const { return NodeType(v._trie >> NodeTypeShift) == Fat; }
    bool IsSmall8Node() const { return NodeType(v._trie >> NodeTypeShift) == Small8; }
    bool IsSmall2Node() const { return NodeType(v._trie >> NodeTypeShift) == Small2; }

    bool HasValue() const { return (v._trie & ValueBit) != 0; }
    void RemoveValue() { v._trie &= ~ValueBit; }
    const Value &GetValue() const { return v; }
    void SetValue(const Value &newVal)
    {
      uint16_t _trie = v._trie;
      v = newVal;
      v._trie = _trie | ValueBit;
    }
    void SetValue(Value &&newVal)
    {
      uint16_t _trie = v._trie;
      v = newVal;
      v._trie = _trie | ValueBit;
    }
    uint16_t GetPrefixLength() const { return v._trie & NodeOrLeaf::PrefixLengthMask; }
    Key GetPrefix() const
    {
      // prefix stored immediately after this structure for both node and leaf
      return Key((byte *)(this + 1), GetPrefixLength());
    }

    void SetPrefix(const Key &k)
    {
      if(IsLeaf() && k.size > GetPrefixLength())
        NBDERR("Expanding prefix on leaf - invalid");

      // prefix stored immediately after this structure for both node and leaf
      byte *prefix = (byte *)(this + 1);
      // use memmove to account for prefix shrinking in place
      if(prefix != k.bytes)
        memmove(prefix, k.bytes, k.size);
      SetPrefixLength(k.size);
    }

  protected:
    void SetNode(NodeType type) { v._trie = uint16_t(type) << NodeTypeShift; }
    void SetPrefixLength(uint16_t length)
    {
      v._trie = (v._trie & ~NodeOrLeaf::PrefixLengthMask) | (length & NodeOrLeaf::PrefixLengthMask);
    }

  private:
    Value v;
  };

  struct FatNode : public NodeOrLeaf
  {
    FatNode() { NodeOrLeaf::SetNode(NodeOrLeaf::Fat); }

    byte prefix[MaxKeySize];
    NodeOrLeaf *children[256];
  };

  template <uint8_t N>
  struct SmallNode : public NodeOrLeaf
  {
    SmallNode() { NodeOrLeaf::SetNode(N == 8 ? NodeOrLeaf::Small8 : NodeOrLeaf::Small2); }

    byte prefix[MaxKeySize];
    NodeOrLeaf *children[N];
    byte childBytes[N];

    NodeOrLeaf **GetChild(byte b)
    {
      for(uint8_t i = 0; i < N; i++)
      {
        // we set children linearly, so if we're looking for byte 0 and it isn't here and we come
        // across a childBytes that matches because it's zero-initialised, we'll just return NULL
        // and know that subsequent children will not be the 'real' match.
        if(childBytes[i] == b && children[i])
          return &children[i];
      }

      return NULL;
    }

    bool AddChild(byte b, NodeOrLeaf *c)
    {
      for(uint8_t i = 0; i < N; i++)
      {
        if(children[i] == NULL)
        {
          childBytes[i] = b;
          children[i] = c;
          return true;
        }
      }

      return false;
    }
  };

  struct Leaf : public NodeOrLeaf
  {
    Leaf(uint16_t prefixLen) { NodeOrLeaf::SetPrefixLength(prefixLen); }

    // variable length
    byte prefix[1];
  };

  // clang complains on some of these offsetof() calls
#if ENABLED(RDOC_WIN32)
  NBDCOMPILE_ASSERT(sizeof(Leaf) == sizeof(Value) + alignof(Value), "Leaf should not be padded");
  NBDCOMPILE_ASSERT(offsetof(Leaf, prefix) == sizeof(Value),
                    "Leaf prefix should immediately follow value");
  NBDCOMPILE_ASSERT(offsetof(FatNode, prefix) == sizeof(Value),
                    "Node prefix should immediately follow value");
  NBDCOMPILE_ASSERT(offsetof(SmallNode<8>, prefix) == sizeof(Value),
                    "Node prefix should immediately follow value");
  NBDCOMPILE_ASSERT(offsetof(SmallNode<2>, prefix) == sizeof(Value),
                    "Node prefix should immediately follow value");

  NBDCOMPILE_ASSERT(sizeof(Value::_trie) == sizeof(uint16_t),
                    "nbdbytetrie requires a `uint16_t _trie` member");
  NBDCOMPILE_ASSERT(MaxKeySize < NodeOrLeaf::PrefixLengthMask,
                    "nbdbytetrie does not support a large key size");
#endif

  ///////////////////////////////
  // bump allocator
  ///////////////////////////////
  struct BumpAllocator
  {
    nbdarray<byte *> allocations;
    byte *curFree = NULL;
    size_t bytesRemaining = 0;
  } m_Allocator;

  byte *allocate(size_t n)
  {
    n = AlignUp(n, (size_t)8);

    if(n > m_Allocator.bytesRemaining)
    {
      m_Allocator.bytesRemaining = AllocSize;
      m_Allocator.curFree = new byte[AllocSize];
      memset(m_Allocator.curFree, 0, AllocSize);
      m_Allocator.allocations.push_back(m_Allocator.curFree);
    }

    byte *ret = m_Allocator.curFree;
    m_Allocator.curFree += n;
    m_Allocator.bytesRemaining -= n;
    return ret;
  }

  template <typename Node>
  Node *MakeNode()
  {
    byte *ret = allocate(sizeof(Node));

    new(ret) Node();

    Node *n = (Node *)ret;

    return n;
  }

  Leaf *MakeLeaf(const Key &prefix)
  {
    byte *ret = allocate(sizeof(Leaf) - 1 + prefix.size);

    new(ret) Leaf(prefix.size);

    Leaf *n = (Leaf *)ret;
    n->SetPrefix(prefix);

    return n;
  }

  NodeOrLeaf *find(const Key &search) const
  {
    if(search.size > MaxKeySize)
    {
      // used only so the tests can EXPECT_ERROR()
      RDResult err;
      SET_ERROR_RESULT(err, ResultCode::InternalError, "Invalid key larger than max size %u",
                       MaxKeySize);
      (void)err;

      return NULL;
    }

    return find(m_Root, search);
  }

  NodeOrLeaf *create(const Key &search)
  {
    if(search.size > MaxKeySize)
    {
      // used only so the tests can EXPECT_ERROR()
      RDResult err;
      SET_ERROR_RESULT(err, ResultCode::InternalError, "Invalid key larger than max size %u",
                       MaxKeySize);
      (void)err;

      return NULL;
    }

    if(m_Root == NULL)
    {
      // we can make a leaf with the full key because when this is inevitably split, the leaf will
      // just shrink to whatever smaller and remain. It means a bit of wasted prefix memory, but
      // that's not a big deal
      Leaf *l = MakeLeaf(search);
      m_Root = l;
      return l;
    }

    return create(m_Root, search);
  }

  NodeOrLeaf *find(NodeOrLeaf *root, const Key &search) const
  {
    // if we're called with a NULL node, obviously nothing to find.
    if(root == NULL)
      return NULL;

    // start looking through this node's prefix
    Key prefix = root->GetPrefix();

    // if the prefix is longer than the search, we can't match anything
    if(prefix.size > search.size)
      return NULL;

    for(uint16_t i = 0; i < prefix.size; i++)
    {
      // if it's the same, continue
      if(prefix[i] == search[i])
        continue;

      // if it's different we've failed, this node only contains things that include the whole
      // prefix (either a value or children)
      return NULL;
    }

    // the prefix is identical. If the length of key is also the same, we found our node - return it
    // if it has a value (it may be an intermediate node)
    if(prefix.size == search.size)
      return root->HasValue() ? root : NULL;

    // if the length is different, see if we're on a node and try to go to the next child
    if(root->IsFatNode())
    {
      FatNode *node = (FatNode *)root;
      return find(node->children[search[prefix.size]], search.ExclusiveSuffixAfter(prefix.size));
    }
    else if(root->IsSmall8Node())
    {
      SmallNode<8> *node = (SmallNode<8> *)root;
      NodeOrLeaf **child = node->GetChild(search[prefix.size]);
      if(child == NULL)
        return NULL;
      return find(*child, search.ExclusiveSuffixAfter(prefix.size));
    }
    else if(root->IsSmall2Node())
    {
      SmallNode<2> *node = (SmallNode<2> *)root;
      NodeOrLeaf **child = node->GetChild(search[prefix.size]);
      if(child == NULL)
        return NULL;
      return find(*child, search.ExclusiveSuffixAfter(prefix.size));
    }

    return NULL;
  }

  NodeOrLeaf *create(NodeOrLeaf *&root, const Key &search)
  {
    Key prefix = root->GetPrefix();
    for(uint16_t i = 0; i < prefix.size && i < search.size; i++)
    {
      if(prefix[i] == search[i])
        continue;

      // i is different, save these bytes
      byte diffPrefixByte = prefix[i];
      byte diffSearchByte = search[i];

      // make the split existing keys
      Key prefixBefore = prefix.ExclusivePrefixBefore(i);
      Key prefixAfter = prefix.ExclusiveSuffixAfter(i);
      Key searchAfter = search.ExclusiveSuffixAfter(i);

      // after this point prefix may have its contents modified, so we don't use it

      // create a new node for the split, with the common root so far (not including i)
      // this can start as a small2 Node because it's new and we only have two children to add
      SmallNode<2> *n = MakeNode<SmallNode<2>>();
      n->SetPrefix(prefixBefore);

      // the old root is going to be appended as a child after i, so truncate its subset to
      // everything after i (exclusively).
      root->SetPrefix(prefixAfter);

      // attach the old root as the first child
      n->childBytes[0] = diffPrefixByte;
      n->children[0] = root;

      // make a leaf for the key we're creating
      Leaf *leaf = MakeLeaf(searchAfter);

      // attach the new leaf as the second child
      n->childBytes[1] = diffSearchByte;
      n->children[1] = leaf;

      // replace the previous node in the tree with this one
      root = n;

      // return this node, we're done
      return leaf;
    }

    // the common subset of prefix and search string are identical

    // if the search string is shorter than the prefix, this node needs to be split
    if(search.size < prefix.size)
    {
      byte firstExtraKeyByte = prefix[search.size];
      Key prefixAfter = prefix.ExclusiveSuffixAfter(search.size);
      Key prefixBefore = prefix.ExclusivePrefixBefore(search.size);

      // the current node will be appended on as a child, it will truncate its key to prefixAfter
      NodeOrLeaf *oldRoot = root;

      // create a new node with the prefix before
      // this can be a small2 node as we only need one child so far
      SmallNode<2> *newRoot = MakeNode<SmallNode<2>>();
      root = newRoot;

      // set new root prefix first as this copies into the node, both prefixBefore and prefixAfter
      // reference subsets of the old prefix (which is stored in oldRoot)
      newRoot->SetPrefix(prefixBefore);
      oldRoot->SetPrefix(prefixAfter);

      // the old root is appended on after the right child
      newRoot->childBytes[0] = firstExtraKeyByte;
      newRoot->children[0] = oldRoot;

      // this node is the one that matches our search string
      return newRoot;
    }

    // if there's still search string left, then this node uses a key that's a substring of the search key.
    if(search.size > prefix.size)
    {
      byte nextSearchByte = search[prefix.size];
      Key searchAfter = search.ExclusiveSuffixAfter(prefix.size);

      // if we're on a leaf
      if(root->IsLeaf())
      {
        Leaf *leaf = (Leaf *)root;
        // we have to convert this to a node so that it can contain a new child

        SmallNode<2> *promoted = MakeNode<SmallNode<2>>();
        promoted->SetPrefix(leaf->GetPrefix());
        promoted->SetValue(std::move(leaf->GetValue()));

        // re-use the leaf later if it has enough prefix space. Since we just promoted this to a
        // node we know it will have no children, so below we are going to hit the case of the next
        // byte having no child and we can put this leaf there.
        if(leaf->GetPrefixLength() >= searchAfter.size)
        {
          leaf->RemoveValue();
          leaf->SetPrefix(searchAfter);
        }
        else
        {
          leaf = MakeLeaf(searchAfter);
        }

        root = promoted;

        promoted->childBytes[0] = nextSearchByte;
        promoted->children[0] = leaf;

        return leaf;
      }
      // if we're a fat node
      else if(root->IsFatNode())
      {
        FatNode *rootNode = (FatNode *)root;

        // if we have a child at this byte, recurse
        if(rootNode->children[nextSearchByte])
          return create(rootNode->children[nextSearchByte], searchAfter);

        // otherwise make a leaf for the key we're creating
        Leaf *leaf = MakeLeaf(searchAfter);

        rootNode->children[nextSearchByte] = leaf;

        return leaf;
      }
      else if(root->IsSmall2Node())
      {
        SmallNode<2> *rootNode = (SmallNode<2> *)root;

        // if we have a child at this byte, recurse
        NodeOrLeaf **child = rootNode->GetChild(nextSearchByte);
        if(child)
          return create(*child, searchAfter);

        // otherwise make a leaf
        Leaf *leaf = MakeLeaf(searchAfter);

        // if we can successfully add this leaf, we're done
        if(rootNode->AddChild(nextSearchByte, leaf))
          return leaf;

        // otherwise the node is full, we need to promote it to a larger size. Move everything across first
        SmallNode<8> *promoted = MakeNode<SmallNode<8>>();
        promoted->SetPrefix(rootNode->GetPrefix());
        if(rootNode->HasValue())
          promoted->SetValue(std::move(rootNode->GetValue()));
        memcpy(promoted->childBytes, rootNode->childBytes, sizeof(rootNode->childBytes));
        memcpy(promoted->children, rootNode->children, sizeof(rootNode->children));

        // replace the node
        root = promoted;

        // now add the new child
        if(!promoted->AddChild(nextSearchByte, leaf))
          NBDERR("Failed to add node child after promotion");

        return leaf;
      }
      else if(root->IsSmall8Node())
      {
        SmallNode<8> *rootNode = (SmallNode<8> *)root;

        // if we have a child at this byte, recurse
        NodeOrLeaf **child = rootNode->GetChild(nextSearchByte);
        if(child)
          return create(*child, searchAfter);

        // otherwise make a leaf
        Leaf *leaf = MakeLeaf(searchAfter);

        // if we can successfully add this leaf, we're done
        if(rootNode->AddChild(nextSearchByte, leaf))
          return leaf;

        // otherwise the node is full, we need to promote it to a larger size. Move everything across first
        FatNode *promoted = MakeNode<FatNode>();
        promoted->SetPrefix(rootNode->GetPrefix());
        if(rootNode->HasValue())
          promoted->SetValue(std::move(rootNode->GetValue()));
        for(uint8_t i = 0; i < ARRAY_COUNT(rootNode->children); i++)
          promoted->children[rootNode->childBytes[i]] = rootNode->children[i];

        // replace the node
        root = promoted;

        // now add the new child
        promoted->children[nextSearchByte] = leaf;

        return leaf;
      }

      // unrecognised type, should not get here
      NBDERR("Unrecognised node type in trie");

      // otherwise, recurse to the child at that byte

      return NULL;
    }

    // if we got here, the prefix is the same size as the search and matches it! duplicate key!
    return root;
  }

  static const size_t AllocSize = 0x80000;

  ///////////////////////////////
  // actual members
  ///////////////////////////////

  NodeOrLeaf *m_Root = NULL;
  bool m_StrictErrorChecking = false;
};
