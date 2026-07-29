/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2018-2026 Baldur Karlsson
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
#include <unordered_map>
#include "api/replay/nbdarray.h"
#include "spirv_common.h"
#include "spirv_processor.h"

namespace nbdspv
{
class Editor;

struct OperationList : public nbdarray<Operation>
{
  // add an operation and return its result id
  Id add(const nbdspv::Operation &op);
};

struct Binding
{
  Binding() = default;
  Binding(uint32_t s, uint32_t b) : set(s), binding(b) {}
  uint32_t set = 0;
  uint32_t binding = ~0U;

  bool operator<(const Binding &o) const
  {
    if(set != o.set)
      return set < o.set;
    return binding < o.binding;
  }

  bool operator!=(const Binding &o) const { return !operator==(o); }
  bool operator==(const Binding &o) const { return set == o.set && binding == o.binding; }
};

struct StructMember
{
  Id type;
  nbdstr name;
  uint32_t offset;
};

template <typename SPIRVType>
using TypeToId = std::pair<SPIRVType, Id>;

template <typename SPIRVType>
using TypeToIds = nbdarray<TypeToId<SPIRVType>>;

class Editor : public Processor
{
public:
  Editor(nbdarray<uint32_t> &spirvWords);
  ~Editor();

  void Prepare();
  void SetBufferStorageMode(BufferStorageMode mode) { m_StorageMode = mode; }
  void CreateEmpty(uint32_t major, uint32_t minor);

  Id MakeId();

  Id AddOperation(Iter iter, const Operation &op);
  Iter AddOperations(Iter iter, const OperationList &ops);

  // callbacks to allow us to update our internal structures over changes

  // called before any modifications are made. Removes the operation from internal structures.
  void PreModify(Iter iter) { UnregisterOp(iter); }
  // called after any modifications, re-adds the operation to internal structures with its new
  // properties
  void PostModify(Iter iter) { RegisterOp(iter); }
  // removed an operation and replaces it with nops
  void Remove(Iter iter)
  {
    UnregisterOp(iter);
    iter.nopRemove();
  }

  void OffsetBindingsToMatchReservation(size_t numReservedBindings);
  StorageClass PrepareAddedBufferAccess();
  nbdpair<Id, Id> AddBufferVariable(nbdarray<Id> &addedGlobals, Id varType, const nbdstr &name,
                                    uint32_t binding, uint32_t specID, uint64_t fixedAddr);
  Id LoadBufferVariable(OperationList &ops, nbdpair<Id, Id> var);

  Id FindEntryID(ShaderEntryPoint entry);
  void AddEntryGlobals(Id entry, const nbdarray<Id> &newGlobals);
  void ChangeEntry(Id from, Id to);

  Id GetBuiltInVariable(BuiltIn builtin) { return builtinInputs[builtin].variable; }
  nbdpair<Id, Id> AddBuiltinInputLoad(OperationList &ops, ShaderStage stage, BuiltIn builtin,
                                      Id type);
  Id AddBuiltinInputLoad(OperationList &ops, nbdarray<Id> &addedGlobals, ShaderStage stage,
                         BuiltIn builtin, Id type)
  {
    nbdpair<Id, Id> ret = AddBuiltinInputLoad(ops, stage, builtin, type);
    if(ret.second != nbdspv::Id())
      addedGlobals.push_back(ret.second);
    return ret.first;
  }

  StorageClass StorageBufferClass() { return m_StorageBufferClass; }
  bool EntryPointAllGlobals() { return m_MajorVersion > 1 || m_MinorVersion >= 4; }
  void DecorateStorageBufferStruct(Id id);
  void SetName(Id id, const nbdstr &name);
  void SetMemberName(Id id, uint32_t member, const nbdstr &name);
  void AddDecoration(const Operation &op);
  void AddCapability(Capability cap);
  bool HasCapability(Capability cap);
  void AddExtension(const nbdstr &extension);
  void AddExecutionMode(const Operation &mode);
  Id HasExtInst(const char *setname);
  Id ImportExtInst(const char *setname);
  Id AddType(const Operation &op);
  Id AddVariable(const Operation &op);
  Id AddConstant(const Operation &op);
  void AddFunction(const OperationList &ops);

  void FlattenSpecConstants(const nbdarray<SpecConstant> &userSpec);

  Iter GetID(Id id);
  // the entry point has 'two' opcodes, the entrypoint declaration and the function.
  // This returns the first, GetID returns the second.
  Iter GetEntry(Id id);
  Iter Begin(Section::Type section) { return Iter(m_SPIRV, m_Sections[section].startOffset); }
  Iter End(Section::Type section) { return Iter(m_SPIRV, m_Sections[section].endOffset); }
  // fetches the id of this type. If it exists already the old ID will be returned, otherwise it
  // will be declared and the new ID returned
  template <typename SPIRVType>
  Id DeclareType(const SPIRVType &t)
  {
    std::map<SPIRVType, Id> &table = GetTable<SPIRVType>();

    auto it = table.lower_bound(t);
    if(it != table.end() && it->first == t)
      return it->second;

    Operation decl = MakeDeclaration(t);
    Id id = MakeId();
    decl[1] = id.value();
    AddType(decl);

    table.insert(it, std::pair<SPIRVType, Id>(t, id));

    return id;
  }

  template <typename SPIRVType>
  Id GetType(const SPIRVType &t)
  {
    std::map<SPIRVType, Id> &table = GetTable<SPIRVType>();

    auto it = table.find(t);
    if(it != table.end())
      return it->second;

    return Id();
  }

  template <typename SPIRVType>
  TypeToIds<SPIRVType> GetTypes()
  {
    std::map<SPIRVType, Id> &table = GetTable<SPIRVType>();

    TypeToIds<SPIRVType> ret;

    for(auto it = table.begin(); it != table.end(); ++it)
      ret.push_back(*it);

    return ret;
  }

  template <typename SPIRVType>
  const std::map<SPIRVType, Id> &GetTypeInfo() const
  {
    return GetTable<SPIRVType>();
  }

  Binding GetBinding(Id id) const
  {
    auto it = bindings.find(id);
    if(it == bindings.end())
      return Binding();
    return it->second;
  }

  Id DeclareStructType(const nbdarray<Id> &members);
  Id DeclareStructType(const nbdstr &name, const nbdarray<StructMember> &members);

  // helper for AddConstant
  template <typename T>
  Id AddConstantImmediate(T t)
  {
    return AddConstantImmediate<T>(t, MakeId());
  }

  template <typename T>
  Id AddConstantImmediate(T t, nbdspv::Id constantId)
  {
    Id typeId = DeclareType(scalar<T>());
    nbdarray<uint32_t> words = {typeId.value(), constantId.value()};

    words.resize(words.size() + (sizeof(T) + 3) / 4);

    memcpy(&words[2], &t, sizeof(T));

    return AddConstant(Operation(Op::Constant, words));
  }

  template <typename T>
  Id AddSpecConstantImmediate(T t, uint32_t specId)
  {
    Id typeId = DeclareType(scalar<T>());
    nbdarray<uint32_t> words = {typeId.value(), MakeId().value()};

    words.resize(words.size() + (sizeof(T) + 3) / 4);

    memcpy(&words[2], &t, sizeof(T));

    nbdspv::Id ret = AddConstant(Operation(Op::SpecConstant, words));

    words.clear();
    words.push_back(ret.value());
    words.push_back((uint32_t)nbdspv::Decoration::SpecId);
    words.push_back(specId);

    AddDecoration(Operation(Op::Decorate, words));

    return ret;
  }

  template <typename T>
  Id AddConstantDeferred(T t)
  {
    Id typeId = DeclareType(scalar<T>());
    Id retId = MakeId();
    nbdarray<uint32_t> words = {typeId.value(), retId.value()};

    words.resize(words.size() + (sizeof(T) + 3) / 4);

    memcpy(&words[2], &t, sizeof(T));

    m_DeferredConstants.add(Operation(Op::Constant, words));

    return retId;
  }

  using Processor::EvaluateConstant;

private:
  using Processor::Parse;
  inline void addWords(size_t offs, size_t num) { addWords(offs, (int32_t)num); }
  void addWords(size_t offs, int32_t num);
  void InsertOperation(const Operation &op, size_t offset);

  Operation MakeDeclaration(const Scalar &s);
  Operation MakeDeclaration(const Vector &v);
  Operation MakeDeclaration(const Matrix &m);
  Operation MakeDeclaration(const Pointer &p);
  Operation MakeDeclaration(const Image &i);
  Operation MakeDeclaration(const Sampler &s);
  Operation MakeDeclaration(const SampledImage &s);
  Operation MakeDeclaration(const FunctionType &f);

  virtual void RegisterOp(Iter iter);
  virtual void UnregisterOp(Iter iter);
  virtual void PostParse();

  void RegisterBuiltinMembers(nbdspv::Id baseId, const nbdarray<uint32_t> chainSoFar,
                              const DataType *type);

  struct BuiltinInputData
  {
    Id variable;
    Id type;
    nbdarray<uint32_t> chain;
  };

  std::map<BuiltIn, BuiltinInputData> builtinInputs;

  // optimise for repeated U32 constants, only register once
  std::unordered_map<uint32_t, nbdspv::Id> m_U32Consts;

  BufferStorageMode m_StorageMode = BufferStorageMode::Unknown;

  std::map<Id, Binding> bindings;
  nbdarray<Id> m_BufferBlockTypes;
  nbdarray<Id> m_BlockTypes;

  std::map<Scalar, Id> scalarTypeToId;
  std::map<Vector, Id> vectorTypeToId;
  std::map<Matrix, Id> matrixTypeToId;
  std::map<Pointer, Id> pointerTypeToId;
  std::map<Image, Id> imageTypeToId;
  std::map<Sampler, Id> samplerTypeToId;
  std::map<SampledImage, Id> sampledImageTypeToId;
  std::map<FunctionType, Id> functionTypeToId;

  StorageClass m_StorageBufferClass = nbdspv::StorageClass::Uniform;

  template <typename SPIRVType>
  std::map<SPIRVType, Id> &GetTable();

  template <typename SPIRVType>
  const std::map<SPIRVType, Id> &GetTable() const;

  OperationList m_DeferredConstants;

  nbdarray<uint32_t> &m_ExternalSPIRV;
};

template <>
inline Id Editor::AddConstantImmediate(bool b)
{
  Id typeId = DeclareType(scalar<bool>());

  nbdarray<uint32_t> words = {typeId.value(), MakeId().value()};

  return AddConstant(Operation(b ? Op::ConstantTrue : Op::ConstantFalse, words));
}

template <>
inline Id Editor::AddConstantImmediate(uint32_t b)
{
  Id typeId = DeclareType(scalar<uint32_t>());

  if(m_U32Consts[b] != nbdspv::Id())
    return m_U32Consts[b];

  nbdspv::Id constantId = MakeId();
  return AddConstant(Operation(Op::Constant, {typeId.value(), constantId.value(), b}));
}

template <>
inline Id Editor::AddSpecConstantImmediate(bool b, uint32_t specId)
{
  Id typeId = DeclareType(scalar<bool>());
  nbdarray<uint32_t> words = {typeId.value(), MakeId().value()};

  nbdspv::Id ret = AddConstant(Operation(b ? Op::SpecConstantTrue : Op::SpecConstantFalse, words));

  words.clear();
  words.push_back(ret.value());
  words.push_back((uint32_t)nbdspv::Decoration::SpecId);
  words.push_back(specId);

  AddDecoration(Operation(Op::Decorate, words));

  return ret;
}

};    // namespace nbdspv
