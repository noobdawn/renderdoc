/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
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

#include <stdint.h>
#include <map>
#include "api/replay/nbdarray.h"
#include "api/replay/stringise.h"
#include "common/common.h"
#include "spirv_gen.h"

namespace nbdspv
{
// length of 1 word in the top 16-bits, OpNop = 0 in the lower 16-bits
static constexpr uint32_t OpNopWord = 0x00010000U;

class Operation;

template <typename ConstOrNotVector>
class IterBase
{
public:
  // increment to the next op
  IterBase<ConstOrNotVector> operator++(int)
  {
    IterBase<ConstOrNotVector> ret = *this;
    operator++();
    return ret;
  }
  IterBase<ConstOrNotVector> operator++()
  {
    do
    {
      offset += cur() >> nbdspv::WordCountShift;
      // silently skip nops
    } while(*this && opcode() == nbdspv::Op::Nop);

    return *this;
  }
  bool operator==(const IterBase<ConstOrNotVector> &it) const = delete;
  bool operator!=(const IterBase<ConstOrNotVector> &it) const = delete;
  bool operator<(const IterBase<ConstOrNotVector> &it) const
  {
    return wordsVector == it.wordsVector && offset < it.offset;
  }
  // utility functions
  explicit operator bool() const { return wordsVector != NULL && offset < wordsVector->size(); }
  const uint32_t &operator*() const { return cur(); }
  nbdspv::Op opcode() const { return nbdspv::Op(cur() & nbdspv::OpCodeMask); }
  const uint32_t &word(size_t idx) const { return wordsVector->at(offset + idx); }
  const uint32_t *words() const { return wordsVector->data() + offset; }
  size_t offs() const { return offset; }
  size_t size() const { return cur() >> nbdspv::WordCountShift; }
protected:
  IterBase() = default;
  IterBase(ConstOrNotVector &w, size_t o) : wordsVector(&w), offset(o) {}
  inline const uint32_t &cur() const { return wordsVector->at(offset); }
  size_t offset = 0;
  ConstOrNotVector *wordsVector = NULL;
};

class ConstIter : public IterBase<const nbdarray<uint32_t>>
{
public:
  // constructors
  ConstIter() = default;
  ConstIter(const nbdarray<uint32_t> &w, size_t o) : IterBase<const nbdarray<uint32_t>>(w, o) {}
};

class Iter : public ConstIter
{
public:
  // constructors
  Iter() = default;
  Iter(nbdarray<uint32_t> &w, size_t o) : ConstIter(w, o) {}
  // mutable utility functions
  using ConstIter::operator*;
  uint32_t &operator*() { return cur(); }
  using ConstIter::word;
  uint32_t &word(size_t idx) { return mutable_words()->at(offset + idx); }
  // replace part of this operation with NOPs and update the length. Cannot completely erase the
  // operation, or expand it
  void nopRemove(size_t idx, size_t count = 0);
  // completely remove the operation and replace with NOPs
  void nopRemove();
  Iter &operator=(const Operation &op);

private:
  friend class Operation;
  inline uint32_t &cur() { return mutable_words()->at(offset); }
  nbdarray<uint32_t> *mutable_words() { return (nbdarray<uint32_t> *)wordsVector; }
};

class Operation
{
public:
  // constructor of a synthetic operation, from an operation & subsequent words, calculates the
  // length then constructs the first word with opcode + length.
  Operation(nbdspv::Op op, const nbdarray<uint32_t> &data)
  {
    words.push_back(MakeHeader(op, data.size() + 1));
    words.insert(1, data);

    iter = Iter(words, 0);
  }

  Operation(const Operation &op)
  {
    words = op.words;

    iter = Iter(words, 0);
  }
  Operation &operator=(const Operation &op)
  {
    words = op.words;

    iter = Iter(words, 0);
    return *this;
  }

  static Operation copy(Iter it)
  {
    Operation ret(it);

    ret.words.insert(0, &it.word(0), it.size());
    ret.iter = Iter(ret.words, 0);

    return ret;
  }

  // helper for fixed size ops that don't want to generate a temporary vector to use the above
  // constructor
  template <typename FixedOpHelper, size_t WordCopyCount = FixedOpHelper::FixedWordSize>
  Operation(const FixedOpHelper &helper)
  {
    words.resize(WordCopyCount);
    memcpy(words.data(), &helper, WordCopyCount * sizeof(uint32_t));

    iter = Iter(words, 0);
  }

  // constructor that takes existing words from elsewhere and just references it.
  // Since this is iterator based, normal iteration invalidation rules apply, if you modify earlier
  // in the SPIR-V this operation will become invalid.
  Operation(Iter it) : iter(it) {}
  uint32_t &operator[](size_t idx) { return iter.word(idx); }
  const uint32_t &operator[](size_t idx) const { return iter.word(idx); }
  size_t size() const { return iter.size(); }
  // insert the words for this op into the destination vector
  void appendTo(nbdarray<uint32_t> &dest) const { dest.append(&iter.word(0), size()); }
  void insertInto(nbdarray<uint32_t> &dest, size_t offset) const
  {
    dest.insert(offset, &iter.word(0), size());
  }
  inline static uint32_t MakeHeader(nbdspv::Op op, size_t WordCount)
  {
    return (uint32_t(op) & nbdspv::OpCodeMask) | (uint16_t(WordCount) << nbdspv::WordCountShift);
  }

  ConstIter AsIter() const { return iter; }
private:
  // everything is based around this iterator, which may point into our local storage or to external
  // storage.
  Iter iter;

  // may not be used, if we refer to an external iterator
  nbdarray<uint32_t> words;
};

template <typename T>
class SparseIdMap : public std::map<Id, T>
{
public:
  // this is helpful when we have const maps that we expect to contain ids for valid SPIR-V
  using std::map<Id, T>::operator[];
  const T &operator[](Id id) const
  {
    auto it = std::map<Id, T>::find(id);
    if(it != std::map<Id, T>::end())
      return it->second;

    NBDERR("Lookup of invalid Id %u expected in SparseIdMap", id.value());
    return dummy;
  }

private:
  T dummy;
};

// This must be a thread safe container
template <typename T>
class DenseIdMap : public nbdarray<T>
{
public:
  using nbdarray<T>::operator[];
  T &operator[](Id id) { return (*this)[id.value()]; }
  const T &operator[](Id id) const { return (*this)[id.value()]; }
};

struct IdOrWord
{
  constexpr inline IdOrWord() : value(0) {}
  constexpr inline IdOrWord(uint32_t val) : value(val) {}
  inline IdOrWord(Id id) : value(id.value()) {}
  inline operator uint32_t() const { return value; }
  constexpr inline bool operator==(const IdOrWord o) const { return value == o.value; }
  constexpr inline bool operator!=(const IdOrWord o) const { return value != o.value; }
  constexpr inline bool operator<(const IdOrWord o) const { return value < o.value; }
private:
  uint32_t value;
};

// need to do this in a separate struct because you can't specialise a member function in a
// templated class. Blech
struct OpExtInstHelper
{
  nbdarray<uint32_t> params;

  template <typename T>
  T arg(uint32_t idx) const
  {
    return T(params[idx]);
  }
};

template <>
inline Id OpExtInstHelper::arg<Id>(uint32_t idx) const
{
  return Id::fromWord(params[idx]);
}

// helper in the style of the auto-generated one for ext insts
template <typename InstType>
struct OpExtInstGeneric : public OpExtInstHelper
{
  OpExtInstGeneric(IdResultType resultType, IdResult result, Id set, InstType inst,
                   const nbdarray<IdOrWord> &params)
      : op(OpCode), wordCount(MinWordSize + (uint16_t)params.size())
  {
    this->resultType = resultType;
    this->result = result;
    this->set = set;
    this->inst = inst;
    this->params.resize(params.size());
    for(size_t i = 0; i < params.size(); i++)
      this->params[i] = params[i];
  }
  OpExtInstGeneric(const ConstIter &it)
  {
    this->op = OpCode;
    this->wordCount = (uint16_t)it.size();
    this->resultType = Id::fromWord(it.word(1));
    this->result = Id::fromWord(it.word(2));
    this->set = Id::fromWord(it.word(3));
    this->inst = InstType(it.word(4));
    this->params.reserve(it.size() - 5);
    for(size_t word = 5; word < it.size(); word++)
      this->params.push_back(it.word(word));
  }

  operator Operation() const
  {
    nbdarray<uint32_t> words;
    words.push_back(resultType.value());
    words.push_back(result.value());
    words.push_back(set.value());
    words.push_back((uint32_t)inst);
    words.append(params);
    return Operation(OpCode, words);
  }

  static constexpr Op OpCode = Op::ExtInst;
  static constexpr uint16_t MinWordSize = 4U;
  Op op;
  uint16_t wordCount;
  IdResultType resultType;
  IdResult result;
  Id set;
  InstType inst;
};

struct OpExtInst : public OpExtInstGeneric<uint32_t>
{
  OpExtInst(IdResultType resultType, IdResult result, Id set, uint32_t inst,
            const nbdarray<IdOrWord> &params)
      : OpExtInstGeneric(resultType, result, set, inst, params)
  {
  }
  OpExtInst(const ConstIter &it) : OpExtInstGeneric(it) {}
};
struct OpGLSL450 : public OpExtInstGeneric<nbdspv::GLSLstd450>
{
  OpGLSL450(IdResultType resultType, IdResult result, Id set, nbdspv::GLSLstd450 inst,
            const nbdarray<IdOrWord> &params)
      : OpExtInstGeneric(resultType, result, set, inst, params)
  {
  }
  OpGLSL450(const ConstIter &it) : OpExtInstGeneric(it) {}
};
struct OpShaderDbg : public OpExtInstGeneric<nbdspv::ShaderDbg>
{
  OpShaderDbg(IdResultType resultType, IdResult result, Id set, nbdspv::ShaderDbg inst,
              const nbdarray<IdOrWord> &params)
      : OpExtInstGeneric(resultType, result, set, inst, params)
  {
  }
  OpShaderDbg(const ConstIter &it) : OpExtInstGeneric(it) {}
};

template <typename T>
struct SwitchPairLiteralId
{
  T literal;
  Id target;
};

typedef SwitchPairLiteralId<uint32_t> SwitchPairU32LiteralId;
typedef SwitchPairLiteralId<uint64_t> SwitchPairU64LiteralId;

// helpers for OpSwitch 32-bit and 64-bit versions in the style of the auto-generated helpers
struct OpSwitch32
{
  OpSwitch32(const ConstIter &it)
  {
    this->op = OpCode;
    this->wordCount = (uint16_t)it.size();
    this->selector = Id::fromWord(it.word(1));
    this->def = Id::fromWord(it.word(2));
    uint32_t word = 3;
    while(word < it.size())
    {
      uint32_t literal(it.word(word));
      word += 1;
      nbdspv::Id target(Id::fromWord(it.word(word)));
      word += 1;
      this->targets.push_back({literal, target});
    }
  }
  OpSwitch32(Id selector, Id def, const nbdarray<SwitchPairLiteralId<uint32_t>> &targets)
      : op(Op::Switch)
  {
    this->wordCount = MinWordSize + 2 * (uint16_t)targets.count();
    this->selector = selector;
    this->def = def;
    this->targets = targets;
  }
  operator Operation() const
  {
    nbdarray<uint32_t> words;
    words.push_back(selector.value());
    words.push_back(def.value());
    for(size_t i = 0; i < targets.size(); i++)
    {
      words.push_back(targets[i].literal);
      words.push_back(targets[i].target.value());
    }
    return nbdspv::Operation(Op::Switch, words);
  }

  static constexpr Op OpCode = Op::Switch;
  static constexpr uint16_t MinWordSize = 3U;
  Op op;
  uint16_t wordCount;
  Id selector;
  Id def;
  nbdarray<SwitchPairU32LiteralId> targets;
};

struct OpSwitch64
{
  OpSwitch64(const ConstIter &it)
  {
    this->op = OpCode;
    this->wordCount = (uint16_t)it.size();
    this->selector = Id::fromWord(it.word(1));
    this->def = Id::fromWord(it.word(2));
    uint32_t word = 3;
    while(word < it.size())
    {
      uint64_t literal(*(uint64_t *)(it.words() + word));
      word += 2;
      nbdspv::Id target(Id::fromWord(it.word(word)));
      word += 1;
      this->targets.push_back({literal, target});
    }
  }
  OpSwitch64(Id selector, Id def, const nbdarray<SwitchPairU64LiteralId> &targets) : op(Op::Switch)
  {
    this->wordCount = MinWordSize + 3 * (uint16_t)targets.count();
    this->selector = selector;
    this->def = def;
    this->targets = targets;
  }
  operator Operation() const
  {
    nbdarray<uint32_t> words;
    words.push_back(selector.value());
    words.push_back(def.value());
    for(size_t i = 0; i < targets.size(); i++)
    {
      uint32_t *literal = (uint32_t *)&(targets[i].literal);
      words.push_back(literal[0]);
      words.push_back(literal[1]);
      words.push_back(targets[i].target.value());
    }
    return nbdspv::Operation(Op::Switch, words);
  }

  static constexpr Op OpCode = Op::Switch;
  static constexpr uint16_t MinWordSize = 3U;

  Op op;
  uint16_t wordCount;
  Id selector;
  Id def;
  nbdarray<SwitchPairU64LiteralId> targets;
};

};    // namespace nbdspv

struct SpecConstant
{
  SpecConstant() = default;
  SpecConstant(uint32_t id, uint64_t val, size_t size) : specID(id), value(val), dataSize(size) {}
  uint32_t specID = 0;
  uint64_t value = 0;
  size_t dataSize = 0;
};

DECLARE_STRINGISE_TYPE(nbdspv::Id);

enum class ShaderStage : uint8_t;
enum class ShaderBuiltin : uint32_t;

ShaderStage MakeShaderStage(nbdspv::ExecutionModel model);
ShaderBuiltin MakeShaderBuiltin(ShaderStage stage, const nbdspv::BuiltIn el);

enum class BufferStorageMode : uint32_t
{
  Unknown,
  // patched in plain descriptor
  Descriptor,
  // legacy path - EXT_buffer_device_address only
  EXT_bda,
  // KHR_buffer_device_address, but with uint2 addresses to not require int64 types/maths
  KHR_bda32,
  // like above, but requiring int64
  KHR_bda64,
};

constexpr bool IsBDA(BufferStorageMode mode)
{
  return mode == BufferStorageMode::EXT_bda || mode == BufferStorageMode::KHR_bda32 ||
         mode == BufferStorageMode::KHR_bda64;
}

constexpr bool IsKHRBDA(BufferStorageMode mode)
{
  return mode == BufferStorageMode::KHR_bda32 || mode == BufferStorageMode::KHR_bda64;
}

constexpr bool IsBinding(BufferStorageMode mode)
{
  return mode == BufferStorageMode::Descriptor;
}
