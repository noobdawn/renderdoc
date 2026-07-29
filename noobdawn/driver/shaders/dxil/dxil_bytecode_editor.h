/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2021-2026 Baldur Karlsson
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

#include "dxil_bytecode.h"
#include "dxil_common.h"
#include "dxil_metadata.h"

namespace DXBC
{
class DXBCContainer;
enum class GlobalShaderFlags : int64_t;
};

namespace LLVMBC
{
class BitcodeWriter;
};

namespace DXIL
{
enum class HandleKind
{
  SRV = 0,
  UAV = 1,
  CBuffer = 2,
  Sampler = 3,
};

class ProgramEditor : public Program
{
public:
  ProgramEditor(const DXBC::DXBCContainer *container, bytebuf &outBlob);
  ~ProgramEditor();

  // publish these interfaces
  using Program::GetVoidType;
  using Program::GetBoolType;
  using Program::GetInt32Type;
  using Program::GetInt8Type;
  using Program::GetPointerType;

  int32_t GetKind(const nbdstr &kind) { return m_Kinds.indexOf(kind); }

  const nbdarray<Type *> &GetTypes() const { return m_Types; }
  const AttributeSet *GetAttributeSet(Attribute desiredAttrs);
  Type *CreateScalarType(Type::ScalarKind scalarType, uint32_t bitWidth);
  Type *CreateNamedStructType(const nbdstr &name, nbdarray<const Type *> members);
  Type *CreateFunctionType(const Type *retType, nbdarray<const Type *> params);
  Type *CreatePointerType(const Type *inner, Type::PointerAddrSpace addrSpace);
  Function *GetFunctionByName(const nbdstr &name);
  Function *GetFunctionByPrefix(const nbdstr &name);
  Metadata *GetMetadataByName(const nbdstr &name);

  Function *DeclareFunction(const nbdstr &name, const Type *retType, nbdarray<const Type *> params,
                            Attribute desiredAttrs);
  Function *DeclareFunction(const Function &f);
  Block *CreateBlock();
  Metadata *CreateMetadata();
  Metadata *CreateConstantMetadata(Constant *val);
  Metadata *CreateConstantMetadata(uint32_t val);
  Metadata *CreateConstantMetadata(uint8_t val);
  Metadata *CreateConstantMetadata(bool val);
  Metadata *CreateConstantMetadata(const char *str) { return CreateConstantMetadata(nbdstr(str)); }
  Metadata *CreateConstantMetadata(const nbdstr &str);
  NamedMetadata *CreateNamedMetadata(const nbdstr &name);

  Literal *CreateLiteral(uint64_t val);

  // I think constants have to be unique, so this will return an existing constant (for simple cases
  // like integers or NULL) if it exists
  Constant *CreateConstant(const Constant &c);
  Constant *CreateConstant(const Type *t, const nbdarray<Value *> &members);
  Constant *CreateConstantGEP(const Type *resultType, const nbdarray<Value *> &pointerAndIdxs);
  Constant *CreateUndef(const Type *t);
  Constant *CreateNULL(const Type *t);

  Constant *CreateConstant(uint32_t u) { return CreateConstant(Constant(m_Int32Type, u)); }
  Constant *CreateConstant(uint8_t u) { return CreateConstant(Constant(m_Int8Type, u)); }
  Constant *CreateConstant(bool b) { return CreateConstant(Constant(m_BoolType, b)); }
  Instruction *CreateInstruction(Operation op);
  Instruction *CreateInstruction(Operation op, const Type *retType, const nbdarray<Value *> &args);
  Instruction *CreateInstruction(const Function *f);
  Instruction *CreateInstruction(const Function *f, DXOp op, const nbdarray<Value *> &args);
  Instruction::ExtraInstructionInfo &GetInstructionExtras(Instruction *inst)
  {
    return inst->extra(alloc);
  }

  Instruction *AddInstruction(Function *f, Instruction *i)
  {
    f->instructions.push_back(i);
    return i;
  }

  Instruction *InsertInstruction(Function *f, size_t idx, Instruction *i)
  {
    f->instructions.insert(idx, i);
    return i;
  }

  void RegisterUAV(DXILResourceType type, uint32_t space, uint32_t regBase, uint32_t regEnd,
                   ResourceKind kind);
  void SetNumThreads(uint32_t dim[3]);
  void SetASPayloadSize(uint32_t payloadSize);
  void SetMSPayloadSize(uint32_t payloadSize);
  void PatchGlobalShaderFlags(std::function<void(DXBC::GlobalShaderFlags &)> patcher);
private:
  bytebuf &m_OutBlob;

  DXIL::RDATData m_RDAT;
  DXIL::PSVData m_PSV;
  bool m_RDATPresent = false;
  bool m_PSVPresent = false;

  nbdarray<Constant *> m_Constants;

  Type *CreateNewType();

  bytebuf EncodeProgram();

  void EncodeConstants(LLVMBC::BitcodeWriter &writer, const nbdarray<const Value *> &values,
                       size_t firstIdx, size_t count) const;
  void EncodeMetadata(LLVMBC::BitcodeWriter &writer, const nbdarray<const Metadata *> &meta) const;
};

};    // namespace DXIL
