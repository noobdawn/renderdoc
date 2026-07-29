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

#include "controlflow.h"
#include "api/replay/nbdstr.h"
#include "api/replay/stringise.h"
#include "common/common.h"
#include "core/settings.h"

#include <unordered_set>

RDOC_CONFIG(bool, Shader_Debug_ControlFlow_Logging, false,
            "Debug logging for shader debugger controlflow");

/*

- Implements control flow based on SPV_KHR_maximal_reconvergence specification
- Generates a collection of Tangles, each tangle represents a group of threads which are converged
(program counter is at the same instruction)
- Threads diverge when they execute: conditional branches, switch statements, kill, demote to helper
- Threads reconverge when they reach a merge point (OpSelectionMerge, OpLoopMerge)
- Threads reconverge when executing the next instruction after OpFunctionCall
- Threads **MAY** reconverge (implementation defined) when exiting a switch statement
- For this implementation OpLoopMerge, OpSelectionMerge are treated as points of reconvergence

Struct/Classes:

ThreadReference:
- ThreadReferences are defined by a unique identifier (ThreadIndex)
- ThreadReferences state:
* ExecutionPoint: the current execution point (instruction index) being executed
* Alive: whether the thread is alive (still executing instructions, might be a helper thread)

Tangle:
- A collection of ThreadReferences
* Stack of merge points (ExecutionPoint)
* Stack of function return points (ExecutionPoint) (which are also added to the merge point stack)
* Diverged: whether the threads have diverged (executing different instructions)
* A unique identifier
* Alive: true if the tangle contains threads which are alive
* Active: the tangle is active (its threads can execute instructions)
* Diverged: the tangle has diverged and needs to be divided into new tangles (each new tangle starts converged)
* Converged: the tangle has converged which means its execution point has reached the merge point head
* StateChanged: the state of the tangle has been modified and the tangle needs to be updated

TangleGroup;
- A collection of Tangles

ControlFlow: the public API for managing control flow
- Owns the collection of Tangles (TangleGroup)
- Updates the state of the tangles based on their state (creates/removes tangles)

ThreadExecutionStates: container of thread states
- ThreadIndex paired with a list of ExecutionPoint's that were executed by that thread

Usage:

ControlFlow:
1. Construct(): initialises the control flow instance with a list of thread identifiers (ThreadIndex)

Perform a single simulation step:
  1. GetTangles(): the currently active tangles to execute (returned tangle data is writable)
  2. For each active tangle execute the threads in the tangle
       Update the state for each tangle that is executed
         * SetDiverged(): mark the tangle as having executed an instruction which could trigger divergence
         * SetThreadDead(): marks a specific thread in the tangle as dead i.e. kill
         * AddMergePoint(): pushes a merge point onto the top of the merge point stack for this tangle
         * AddFunctionReturnPoint(): pushes a function return point onto the function point and merge point stacks
  3. UpdateState() : informs the control flow of the current thread states (ThreadExecutionStates)

Notes:
* Control flow state is not only owned by this class.
* External code also modifies the control flow state and those updates have to be replayed to ensure
the control flow state is in sync

Algorithm:

UpdateState():
* Replays each thread state and updates the control flow state after each step
* The thread states might include execution of multiple convergence points per thread
* For each thread state update the ThreadReference execution point to the new execution point
* If a Tangle executes its merge point, mark the tangle as active and pop the merge point stack
* If any state changes are detected then update the control flow TangleGroup state:
* Deactivate tangles when all the threads are dead
* Detect which Tangles have diverged and create new Tangles from the divergence
* Detect which Tangles have reached a merge point and mark them as converged
* Detect which Tangles converged to a function return point, prune the merge point stack
* Merge converged Tangles which have the same merge stack
* Decide which inactive tangles to activate
* Deactivate tangles when all the threads are dead
* Prune deactivated tangles from the TangleGroup
*/

namespace nbdshaders
{
int32_t ControlFlow::s_NextTangleId = 0;

void Tangle::SetThreadExecutionPoint(ThreadIndex threadId, ExecutionPoint execPoint)
{
  for(ThreadReference &threadRef : m_ThreadRefs)
  {
    if(threadRef.id == threadId)
    {
      if(threadRef.execPoint != execPoint)
      {
        threadRef.execPoint = execPoint;
        m_StateChanged = true;
      }
      return;
    }
  }
  NBDASSERTMSG("Thread not found", threadId);
}

void Tangle::SetThreadMergePoint(ThreadIndex threadId, ExecutionPoint execPoint)
{
  for(ThreadReference &threadRef : m_ThreadRefs)
  {
    if(threadRef.id == threadId)
    {
      threadRef.mergePoint = execPoint;
      return;
    }
  }
}

void Tangle::SetThreadAlive(ThreadIndex threadId, bool value)
{
  for(ThreadReference &threadRef : m_ThreadRefs)
  {
    if(threadRef.id == threadId)
    {
      if(threadRef.m_Alive != value)
      {
        threadRef.m_Alive = value;
        m_StateChanged = true;
      }
      return;
    }
  }
  NBDASSERTMSG("Thread not found", threadId);
}

bool Tangle::ContainsThread(ThreadIndex threadId) const
{
  for(const ThreadReference &threadRef : m_ThreadRefs)
  {
    if(threadRef.id == threadId)
      return true;
  }
  return false;
}

void Tangle::CheckForDivergence()
{
  if(m_ThreadRefs.empty())
    return;

  ExecutionPoint commonExecPoint = m_ThreadRefs[0].execPoint;
  for(const ThreadReference &threadRef : m_ThreadRefs)
  {
    ExecutionPoint execPoint = threadRef.execPoint;
    if(execPoint != commonExecPoint)
    {
      m_Diverged = true;
      return;
    }
  }
  m_Diverged = false;
}

// Remove all merge points above the execPoint which must exist in the merge point stack
void Tangle::PruneMergePoints(ExecutionPoint execPoint)
{
  NBDASSERT(m_MergePoints.contains(execPoint));

  size_t countPoints = m_MergePoints.size();
  size_t index = countPoints;
  for(size_t i = 0; i < countPoints; i++)
  {
    size_t idx = countPoints - 1 - i;
    if(m_MergePoints[idx] == execPoint)
    {
      index = idx;
      break;
    }
  }
  m_MergePoints.erase(index + 1, m_MergePoints.size() - index);
}

// Get the merge point from the threads in the tangle and add it to the merge point stack
void Tangle::AddMergePointFromThreads()
{
  ExecutionPoint threadMergePoint = INVALID_EXECUTION_POINT;
  size_t countNewMergePoints = 0;
  for(ThreadReference &threadRef : m_ThreadRefs)
  {
    if(threadRef.mergePoint != INVALID_EXECUTION_POINT)
    {
      ++countNewMergePoints;
      if(threadMergePoint == INVALID_EXECUTION_POINT)
        threadMergePoint = threadRef.mergePoint;

      NBDASSERTEQUAL(threadMergePoint, threadRef.mergePoint);
      threadRef.mergePoint = INVALID_EXECUTION_POINT;
    }
  }
  if(threadMergePoint == INVALID_EXECUTION_POINT)
    return;

  NBDASSERTEQUAL(countNewMergePoints, m_ThreadRefs.size());
  AddMergePoint(threadMergePoint);
}

// Define tangles to be entangled if the merge point stack of one tangle is contained within the other
bool Tangle::Entangled(const Tangle &other) const
{
  size_t countPoints = m_MergePoints.size();
  if(other.m_MergePoints.size() < countPoints)
    return false;
  for(size_t i = 0; i < countPoints; ++i)
  {
    if(m_MergePoints[i] != other.m_MergePoints[i])
      return false;
  }
  return true;
}

// Creates a TangleGroup from the input tangle, each new tangle contains threads at the same
// execution point The TangleGroup replaces the input tangle The input tangle is modified to mark it
// as inactive and its ThreadReferences are moved to the new Tangles
TangleGroup ControlFlow::DivergeTangle(Tangle &tangle)
{
  tangle.SetActive(false);
  TangleGroup newTangles;
  for(const ThreadReference &threadRef : tangle.GetThreadRefs())
  {
    ExecutionPoint execPoint = threadRef.execPoint;
    bool needNewTangle = true;
    for(Tangle &newTangle : newTangles)
    {
      if(execPoint == newTangle.GetExecutionPoint())
      {
        newTangle.AddThreadReference(threadRef);
        needNewTangle = false;
        break;
      }
    }

    if(needNewTangle)
    {
      Tangle newTangle;
      newTangle.SetId(GetNewTangleId());
      newTangle.SetMergePoints(tangle.GetMergePoints());
      newTangle.SetFunctionReturnPoints(tangle.GetFunctionReturnPoints());
      newTangle.SetDiverged(false);
      newTangle.SetConverged(tangle.IsConverged());
      newTangle.SetActive(!tangle.IsConverged());
      newTangle.SetAlive(true);
      newTangle.SetStateChanged(true);
      newTangle.AddThreadReference(threadRef);
      newTangles.push_back(newTangle);
    }
  }

  for(Tangle &newTangle : newTangles)
  {
    newTangle.AddMergePointFromThreads();
    for(const ThreadReference &threadRef : newTangle.GetThreadRefs())
      tangle.RemoveThreadReference(threadRef.id);
  }

  tangle.SetActive(false);
  tangle.SetAlive(false);
  NBDASSERTEQUAL(tangle.GetThreadCount(), 0U);

  if(Shader_Debug_ControlFlow_Logging())
  {
    NBDLOG("Tangle:%u ThreadCount:%u diverged", tangle.GetId(), tangle.GetThreadCount());
    for(Tangle &newTangle : newTangles)
      NBDLOG("Tangle:%u ThreadCount:%u ExecPoint:%u", newTangle.GetId(), newTangle.GetThreadCount(),
             newTangle.GetExecutionPoint());
  }

  return newTangles;
}

void ControlFlow::ProcessTangleDivergence()
{
  TangleGroup newTangles;
  for(Tangle &tangle : m_Tangles)
  {
    if(!tangle.IsAliveActive())
      continue;
    // Do divergence before convergence (a branch target could be a convergence point)
    tangle.CheckForDivergence();
    if(tangle.IsDiverged())
      newTangles.append(DivergeTangle(tangle));
    else
      tangle.AddMergePointFromThreads();
  }

  m_Tangles.append(newTangles);
}

void ControlFlow::ProcessTangleDeactivation()
{
  // If all threads in a tangle are dead then the tangle is dead
  for(Tangle &tangle : m_Tangles)
  {
    if(!tangle.IsAlive())
      continue;
    bool allDead = true;
    for(const ThreadReference &threadRef : tangle.GetThreadRefs())
    {
      if(threadRef.m_Alive)
      {
        allDead = false;
        break;
      }
    }
    if(allDead)
      tangle.SetAlive(false);
  }
}

void ControlFlow::ActivateIndependentTangles()
{
  // Decide which inactive tangles to activate (the tangle should be converged)
  // Can activate the tangle if its merge point stack is not within the stack of another tangle
  // Pop the merge point stack when activating the tangle
  for(Tangle &tangle : m_Tangles)
  {
    // Want Alive but not Active tangles
    if(!tangle.IsAlive())
      continue;
    if(tangle.IsAliveActive())
      continue;

    NBDASSERT(tangle.IsConverged());
    bool activate = true;
    for(Tangle &otherTangle : m_Tangles)
    {
      if(!otherTangle.IsAlive())
        continue;
      if(otherTangle.GetId() == tangle.GetId())
        continue;
      if(tangle.Entangled(otherTangle))
      {
        activate = false;
        break;
      }
    }
    if(activate)
    {
      tangle.SetActive(true);
      tangle.SetConverged(false);
      tangle.SetDiverged(false);
      NBDASSERTNOTEQUAL(tangle.GetMergePoint(), INVALID_EXECUTION_POINT);
      tangle.PopMergePoint();
      tangle.SetStateChanged(true);
      if(Shader_Debug_ControlFlow_Logging())
      {
        NBDLOG("Tangle:%u ThreadCount:%u at ExecPoint:%u activated new MergePoint:%u", tangle.GetId(),
               tangle.GetThreadCount(), tangle.GetExecutionPoint(), tangle.GetMergePoint());
      }
    }
  }
}

// Detect which Tangles have reached a merge point and mark them as converged
// Detect which Tangles converged to a function return point, prune the merge point stack
void ControlFlow::ProcessTangleConvergence()
{
  for(Tangle &tangle : m_Tangles)
  {
    if(!tangle.IsAlive())
      continue;
    ExecutionPoint mergePoint = tangle.GetMergePoint();
    if(mergePoint != INVALID_EXECUTION_POINT)
    {
      bool converged = true;
      for(const ThreadReference &threadRef : tangle.GetThreadRefs())
      {
        if(threadRef.execPoint != mergePoint)
        {
          converged = false;
          break;
        }
      }
      if(converged)
      {
        tangle.SetConverged(true);
        // if the tangle converged to a function return point
        if(tangle.GetExecutionPoint() == tangle.GetFunctionReturnPoint())
        {
          if(Shader_Debug_ControlFlow_Logging())
          {
            NBDLOG(
                "Tangle:%u ThreadCount:%u is converged at ExecPoint:%u FunctionReturnPoint:%u "
                "Next "
                "MergePoint:%u",
                tangle.GetId(), tangle.GetThreadCount(), tangle.GetExecutionPoint(),
                tangle.GetFunctionReturnPoint());
          }
          tangle.PruneMergePoints(tangle.GetFunctionReturnPoint());
          tangle.PopFunctionReturnPoint();
        }
      }
    }
  }
}

// Merge converged Tangles which have the same merge stack
void ControlFlow::MergeConvergedTangles()
{
  for(Tangle &tangle : m_Tangles)
  {
    if(!tangle.IsAlive())
      continue;
    if(!tangle.IsConverged())
      continue;

    if(Shader_Debug_ControlFlow_Logging())
    {
      NBDLOG("Tangle:%u ThreadCount:%u is converged at ExecPoint:%u Next MergePoint:%u",
             tangle.GetId(), tangle.GetThreadCount(), tangle.GetExecutionPoint(),
             tangle.GetMergePoint());
    }
    tangle.SetActive(false);

    // loop over all tangles which are converged
    for(Tangle &convTangle : m_Tangles)
    {
      if(!convTangle.IsAlive())
        continue;
      if(convTangle.GetId() == tangle.GetId())
        continue;
      if(!convTangle.IsConverged())
        continue;
      // This can happen if the outside simulation does multiple simulation steps in a single control flow update
      if(convTangle.GetExecutionPoint() != tangle.GetExecutionPoint())
        continue;

      // merge tangles if they have the same merge stack
      if(convTangle.GetMergePoints() == tangle.GetMergePoints())
      {
        if(Shader_Debug_ControlFlow_Logging())
        {
          NBDLOG(
              "Tangle:%u ThreadCount:%u converged with Tangle:%u ThreadCount:%u ExecPoint:%u,%u at "
              "MergePoint:%u,%u",
              tangle.GetId(), tangle.GetThreadCount(), convTangle.GetId(),
              convTangle.GetThreadCount(), tangle.GetExecutionPoint(),
              convTangle.GetExecutionPoint(), tangle.GetMergePoint(), convTangle.GetMergePoint());
        }
        tangle.AppendThreadReferences(convTangle.GetThreadRefs());
        convTangle.ClearMergePoints();
        convTangle.ClearFunctionReturnPoints();
        convTangle.ClearThreadReferences();
        convTangle.SetActive(false);
        convTangle.SetConverged(false);
        convTangle.SetDiverged(false);
        convTangle.SetAlive(false);
      }
    }
  }
}

// Replay each thread state and update the control flow state after each step
// The thread states might include execution of multiple convergence points per thread
// For each thread state update the ThreadReference execution point to the new execution point
// Process if at tangle is at a merge point or a function return point
// If any state changes are detected then update the control flow TangleGroup state
void ControlFlow::UpdateState(const ThreadExecutionStates &threadExecutionStates)
{
  nbdflatmap<ThreadIndex, uint32_t> threadExecutionIndexes;
  for(const auto &it : threadExecutionStates)
    threadExecutionIndexes[it.first] = 0;

  bool stateChanged = false;
  do
  {
    nbdarray<ThreadIndex> activeThreads;
    stateChanged = false;
    // Update the execution point for each thread in the alive tangles
    for(Tangle &tangle : m_Tangles)
    {
      if(!tangle.IsAlive())
        continue;

      if(tangle.IsStateChanged())
      {
        stateChanged = true;
        tangle.SetStateChanged(false);
      }

      for(const ThreadReference &threadRef : tangle.GetThreadRefs())
      {
        ThreadIndex threadId = threadRef.id;
        const auto it = threadExecutionStates.find(threadId);
        if(it != threadExecutionStates.end())
        {
          const EnteredExecutionPoints &enteredPoints = it->second;
          uint32_t threadExecutionIndex = threadExecutionIndexes[threadId];
          if(threadExecutionIndex < enteredPoints.size())
          {
            ExecutionPoint execPoint = enteredPoints[threadExecutionIndex];
            tangle.SetThreadExecutionPoint(threadId, execPoint);
            stateChanged = true;
            activeThreads.push_back(threadId);
          }
        }
      }
    }
    if(!stateChanged)
      continue;

    // Deactivate tangles when all the threads are dead
    ProcessTangleDeactivation();
    // Update tangle divergence after all threads have executed a step
    ProcessTangleDivergence();

    // Process if at merge point or a function return point
    for(Tangle &tangle : m_Tangles)
    {
      if(!tangle.IsAlive())
        continue;

      const ExecutionPoint functionReturnPoint = tangle.GetFunctionReturnPoint();
      bool atMergePoint = tangle.m_MergePoints.contains(tangle.GetExecutionPoint());
      bool atFunctionReturnPoint = (tangle.GetExecutionPoint() == functionReturnPoint) &&
                                   (functionReturnPoint != INVALID_EXECUTION_POINT);
      bool threadExecuted = false;
      const ExecutionPoint mergePoint =
          atMergePoint ? tangle.GetExecutionPoint() : tangle.GetMergePoint();

      for(const ThreadReference &threadRef : tangle.GetThreadRefs())
      {
        ThreadIndex threadId = threadRef.id;
        if(activeThreads.contains(threadId))
        {
          ExecutionPoint execPoint = threadRef.execPoint;
          threadExecuted = true;
          // when detecting external execution of merge point : ALL threads should be at the same
          // execution point
          if(atMergePoint)
            NBDASSERTEQUAL(mergePoint, execPoint);
          // when detecting external execution of function return point : ALL threads should be at
          // the same execution point
          if(atFunctionReturnPoint)
            NBDASSERTEQUAL(functionReturnPoint, execPoint);
        }
      }

      if(threadExecuted)
      {
        if(atFunctionReturnPoint)
        {
          NBDASSERTEQUAL(tangle.GetExecutionPoint(), tangle.GetFunctionReturnPoint());
          NBDASSERTNOTEQUAL(tangle.GetFunctionReturnPoint(), INVALID_EXECUTION_POINT);
          tangle.PruneMergePoints(tangle.GetFunctionReturnPoint());
          if(Shader_Debug_ControlFlow_Logging())
          {
            NBDLOG(
                "Tangle:%u ThreadCount:%u at ExecPoint:%u auto-activated FunctionReturnPoint:%u "
                "Next MergePoint:%u",
                tangle.GetId(), tangle.GetThreadCount(), tangle.GetExecutionPoint(),
                tangle.GetFunctionReturnPoint(), tangle.GetMergePoint());
          }
          tangle.PopFunctionReturnPoint();
          tangle.SetStateChanged(true);
        }
        else if(atMergePoint)
        {
          tangle.PruneMergePoints(tangle.GetExecutionPoint());
          tangle.SetActive(false);
          tangle.SetConverged(true);
          tangle.SetDiverged(false);
          tangle.SetStateChanged(true);
          if(Shader_Debug_ControlFlow_Logging())
          {
            NBDLOG("Tangle:%u ThreadCount:%u at ExecPoint:%u auto-activated new MergePoint:%u",
                   tangle.GetId(), tangle.GetThreadCount(), tangle.GetExecutionPoint(),
                   tangle.GetMergePoint());
          }
        }
      }
    }

    // Update the execution indexes
    for(const ThreadIndex &threadId : activeThreads)
    {
      const auto it = threadExecutionStates.find(threadId);
      NBDASSERT(it != threadExecutionStates.end());
      if(it != threadExecutionStates.end())
      {
        const EnteredExecutionPoints &enteredPoints = it->second;
        const auto itIndex = threadExecutionIndexes.find(threadId);
        NBDASSERT(itIndex != threadExecutionIndexes.end());
        uint32_t &threadExecutionIndex = itIndex->second;
        NBDASSERT(threadExecutionIndex < enteredPoints.size());
        ++threadExecutionIndex;
      }
    }
    ProcessTangleDeactivation();
    ProcessTangleDivergence();
    ProcessTangleConvergence();
    MergeConvergedTangles();
    ActivateIndependentTangles();
    ProcessTangleDeactivation();

    // Prune dead tangles
    m_Tangles.removeIf([](const Tangle &tangle) { return !tangle.IsAlive(); });

  } while(stateChanged);

  // Check all thread execution states were processed
  for(const auto &it : threadExecutionStates)
  {
    NBDASSERTEQUAL(threadExecutionIndexes[it.first], it.second.size());
  }
}

void ControlFlow::Construct(const nbdarray<ThreadIndex> &threadIds)
{
  s_NextTangleId = 0;
  const nbdarray<ExecutionPoint> sentinelPoints = {INVALID_EXECUTION_POINT};
  Tangle rootTangle;
  rootTangle.SetId(GetNewTangleId());
  rootTangle.SetActive(true);
  rootTangle.SetAlive(true);
  rootTangle.SetMergePoints(sentinelPoints);
  rootTangle.SetFunctionReturnPoints(sentinelPoints);
  rootTangle.SetDiverged(false);
  rootTangle.SetConverged(false);
  ThreadReference threadRef;
  for(uint32_t i = 0; i < threadIds.size(); ++i)
  {
    threadRef.id = threadIds[i];
    threadRef.m_Alive = true;
    rootTangle.AddThreadReference(threadRef);
  }

  m_Tangles.clear();
  m_Tangles.push_back(rootTangle);
}
};    // namespace nbdshaders

#if ENABLED(ENABLE_UNIT_TESTS)

#include <limits>
#include "catch/catch.hpp"

using namespace nbdshaders;

const ExecutionPoint EXEC_POINT_1 = 1;
const ExecutionPoint EXEC_POINT_2 = 2;
const ExecutionPoint EXEC_POINT_3 = 3;
const ExecutionPoint EXEC_POINT_4 = 4;
const ExecutionPoint EXEC_POINT_5 = 5;
const ExecutionPoint EXEC_POINT_EXIT = 1000;

const uint32_t TID_0 = 0;
const uint32_t TID_1 = 1;

const uint32_t TANGLE_0 = 0;
const uint32_t TANGLE_1 = 1;

const uint32_t STATE_0 = 0;
const uint32_t STATE_1 = 1;
const uint32_t STATE_2 = 2;
const uint32_t STATE_3 = 3;
const uint32_t STATE_4 = 4;
const uint32_t STATE_5 = 5;

const uint32_t NO_DATA = (uint32_t)-1;

enum class Op
{
  EXECUTE,
  FUNCTIONRETURN,
  MERGE,
  DIVERGE,
  EXIT,
  UPDATESTATE,
  INVALID,
};

struct TestInstruction
{
  uint32_t tangleIndex = NO_DATA;
  ThreadIndex threadId = INVALID_THREAD_INDEX;
  ExecutionPoint execPoint = INVALID_EXECUTION_POINT;
  Op op = Op::INVALID;
  uint32_t opData = NO_DATA;
};

struct TestTangleData
{
  ExecutionPoint execPoint;
  nbdarray<ThreadIndex> threadIds;
};

typedef nbdarray<TestTangleData> TestTangles;
struct Program
{
  void Execute(uint32_t threadId, uint32_t execPoint)
  {
    TestInstruction instr;
    instr.op = Op::EXECUTE;
    instr.threadId = threadId;
    instr.execPoint = execPoint;
    instructions.push_back(instr);
  }
  void Exit(uint32_t tangleIndex, uint32_t threadId)
  {
    TestInstruction instr;
    instr.op = Op::EXIT;
    instr.tangleIndex = tangleIndex;
    instr.threadId = threadId;
    instructions.push_back(instr);
  }
  void Merge(uint32_t tangleIndex, uint32_t mergePoint)
  {
    TestInstruction instr;
    instr.op = Op::MERGE;
    instr.tangleIndex = tangleIndex;
    instr.opData = mergePoint;
    instructions.push_back(instr);
  }
  void Diverge(uint32_t tangleIndex, uint32_t threadId, uint32_t execPoint)
  {
    TestInstruction instr;
    instr.op = Op::DIVERGE;
    instr.tangleIndex = tangleIndex;
    instr.threadId = threadId;
    instr.execPoint = execPoint;
    instructions.push_back(instr);
  }
  void FunctionReturn(uint32_t tangleIndex, uint32_t functionReturnPoint)
  {
    TestInstruction instr;
    instr.op = Op::FUNCTIONRETURN;
    instr.tangleIndex = tangleIndex;
    instr.opData = functionReturnPoint;
    instructions.push_back(instr);
  }
  void UpdateState(uint32_t state)
  {
    TestInstruction instr;
    instr.op = Op::UPDATESTATE;
    instr.opData = state;
    instructions.push_back(instr);
  }
  void AddInstruction(const TestInstruction &instr) { instructions.push_back(instr); }
  nbdarray<TestInstruction> instructions;
};

void RunTest(const Program &program, const nbdarray<TestTangles> &expected)
{
  ControlFlow controlFlow;
  controlFlow.Construct({TID_0, TID_1});
  TangleGroup &tangles = controlFlow.GetTangles();
  REQUIRE(1 == tangles.count());
  ThreadExecutionStates threadExecutionStates;
  for(const TestInstruction &instr : program.instructions)
  {
    bool setExecPoint = false;
    bool tangleMustBeAlive = true;
    uint32_t tangleIndex = instr.tangleIndex;

    Op op = instr.op;
    if(op == Op::MERGE)
    {
      REQUIRE(tangleIndex < tangles.size());
      Tangle &tangle = tangles[tangleIndex];
      tangle.AddMergePoint(instr.opData);
    }
    else if(op == Op::DIVERGE)
    {
      NBDASSERTEQUAL(NO_DATA, instr.opData);
      setExecPoint = true;
      REQUIRE(tangleIndex < tangles.size());
      Tangle &tangle = tangles[tangleIndex];
      tangle.SetDiverged(true);
    }
    else if(op == Op::EXECUTE)
    {
      NBDASSERTEQUAL(NO_DATA, instr.tangleIndex);
      NBDASSERTEQUAL(NO_DATA, instr.opData);
      setExecPoint = true;
    }
    else if(op == Op::FUNCTIONRETURN)
    {
      REQUIRE(tangleIndex < tangles.size());
      Tangle &tangle = tangles[tangleIndex];
      tangle.AddFunctionReturnPoint(instr.opData);
    }
    else if(op == Op::EXIT)
    {
      NBDASSERTEQUAL(NO_DATA, instr.opData);
      NBDASSERTEQUAL(NO_DATA, instr.execPoint);
      REQUIRE(tangleIndex < tangles.size());
      Tangle &tangle = tangles[tangleIndex];
      tangle.SetThreadDead(instr.threadId);
    }
    else if(op == Op::UPDATESTATE)
    {
      NBDASSERTEQUAL(NO_DATA, instr.tangleIndex);
      NBDASSERTEQUAL(NO_DATA, instr.execPoint);
      setExecPoint = false;
      tangleMustBeAlive = false;
      controlFlow.UpdateState(threadExecutionStates);
      tangles = controlFlow.GetTangles();
      uint32_t expectedIndex = instr.opData;
      REQUIRE(expectedIndex < expected.size());
      const TestTangles &expectedTangles = expected[expectedIndex];
      REQUIRE(expectedTangles.size() == tangles.size());
      for(uint32_t i = 0; i < expectedTangles.size(); ++i)
      {
        const TestTangleData &testTangle = expectedTangles[i];
        const Tangle &tangle = tangles[i];
        REQUIRE(tangle.IsAlive());
        REQUIRE(testTangle.execPoint == tangle.GetExecutionPoint());
        REQUIRE(testTangle.threadIds.size() == tangle.GetThreadCount());
        for(uint32_t threadIndex = 0; threadIndex < testTangle.threadIds.size(); ++threadIndex)
        {
          ThreadIndex threadId = testTangle.threadIds[threadIndex];
          REQUIRE(tangle.ContainsThread(threadId));
        }
      }
      threadExecutionStates.clear();
    }
    else
    {
      REQUIRE(false);
    }
    if(setExecPoint)
    {
      threadExecutionStates[instr.threadId].push_back(instr.execPoint);
    }
    if(tangleMustBeAlive)
    {
      if(tangleIndex == NO_DATA)
      {
        // Find the tangle from the threadId
        for(uint32_t i = 0; i < tangles.size(); ++i)
        {
          if(tangles[i].ContainsThread(instr.threadId))
          {
            tangleIndex = i;
            break;
          }
        }
      }
      REQUIRE(tangleIndex != NO_DATA);
      REQUIRE(tangleIndex < tangles.size());
      const Tangle &tangle = tangles[tangleIndex];
      REQUIRE(tangle.IsAliveActive());
    }
  }
}

TEST_CASE("Shader Control Flow", "[shader][controlflow]")
{
  SECTION("Maximal Reconvergence")
  {
    TestTangles tanglesExit = {};

    SECTION("No Branch")
    {
      // no branch
      // TID_0: EXEC_POINT_1
      // TID_1: EXEC_POINT_1
      Program program;
      program.Execute(TID_0, EXEC_POINT_1);
      program.Execute(TID_1, EXEC_POINT_1);
      program.UpdateState(STATE_0);
      program.Execute(TID_0, EXEC_POINT_1);
      program.Execute(TID_1, EXEC_POINT_1);
      program.UpdateState(STATE_0);
      program.Exit(TANGLE_0, TID_0);
      program.Exit(TANGLE_0, TID_1);
      program.UpdateState(STATE_1);

      nbdarray<TestTangles> expected;
      TestTangles tangles0 = {
          {EXEC_POINT_1, {TID_0, TID_1}},
      };
      expected.push_back(tangles0);
      expected.push_back(tanglesExit);

      RunTest(program, expected);
    }
    SECTION("Uniform Branch")
    {
      // uniform branch
      // TID_0: EXEC_POINT_1 -> EXEC_POINT_EXIT
      // TID_1: EXEC_POINT_1 -> EXEC_POINT_EXIT
      Program program;
      program.Merge(TANGLE_0, EXEC_POINT_EXIT);
      program.Execute(TID_0, EXEC_POINT_1);
      program.Execute(TID_1, EXEC_POINT_1);
      program.UpdateState(STATE_0);
      program.Execute(TID_0, EXEC_POINT_1);
      program.Execute(TID_1, EXEC_POINT_1);
      program.UpdateState(STATE_0);
      program.Diverge(TANGLE_0, TID_0, EXEC_POINT_EXIT);
      program.Diverge(TANGLE_0, TID_1, EXEC_POINT_EXIT);
      program.UpdateState(STATE_1);
      program.Execute(TID_0, EXEC_POINT_EXIT);
      program.Execute(TID_1, EXEC_POINT_EXIT);
      program.UpdateState(STATE_1);
      program.Exit(TANGLE_0, TID_0);
      program.Exit(TANGLE_0, TID_1);
      program.UpdateState(STATE_2);

      TestTangles tangles0 = {
          {EXEC_POINT_1, {TID_0, TID_1}},
      };
      TestTangles tangles1 = {
          {EXEC_POINT_EXIT, {TID_0, TID_1}},
      };

      nbdarray<TestTangles> expected;
      expected.push_back(tangles0);
      expected.push_back(tangles1);
      expected.push_back(tanglesExit);

      RunTest(program, expected);
    }
    SECTION("50/50 Branch")
    {
      // 50/50 branch
      // TID_0: EXEC_POINT_1 -> EXEC_POINT_2 -> EXEC_POINT_EXIT
      // TID_1: EXEC_POINT_1 -> EXEC_POINT_3 -> EXEC_POINT_EXIT
      Program program;
      program.Merge(TANGLE_0, EXEC_POINT_EXIT);
      program.Execute(TID_0, EXEC_POINT_1);
      program.Execute(TID_1, EXEC_POINT_1);
      program.UpdateState(STATE_0);
      program.Diverge(TANGLE_0, TID_0, EXEC_POINT_2);
      program.Diverge(TANGLE_0, TID_1, EXEC_POINT_3);
      program.UpdateState(STATE_1);
      program.Execute(TID_0, EXEC_POINT_2);
      program.Execute(TID_1, EXEC_POINT_3);
      program.UpdateState(STATE_1);
      program.Diverge(TANGLE_0, TID_0, EXEC_POINT_EXIT);
      program.Diverge(TANGLE_1, TID_1, EXEC_POINT_EXIT);
      program.UpdateState(STATE_2);
      program.Exit(TANGLE_0, TID_0);
      program.Exit(TANGLE_0, TID_1);
      program.UpdateState(STATE_3);

      TestTangles tangles0 = {
          {EXEC_POINT_1, {TID_0, TID_1}},
      };
      TestTangles tangles1 = {
          {EXEC_POINT_2, {TID_0}},
          {EXEC_POINT_3, {TID_1}},
      };
      TestTangles tangles2 = {
          {EXEC_POINT_EXIT, {TID_0, TID_1}},
      };

      nbdarray<TestTangles> expected;
      expected.push_back(tangles0);
      expected.push_back(tangles1);
      expected.push_back(tangles2);
      expected.push_back(tanglesExit);

      RunTest(program, expected);
    }
    SECTION("Uniform Branch with a function call")
    {
      // uniform branch
      // EXEC_POINT_2 : is a function call with the return point EXEC_POINT_3
      // TID_0: EXEC_POINT_1 -> EXEC_POINT_2 -> EXEC_POINT_3 -> EXEC_POINT_EXIT
      // TID_1: EXEC_POINT_1 -> EXEC_POINT_2 -> EXEC_POINT_3 -> EXEC_POINT_EXIT
      Program program;
      program.Merge(TANGLE_0, EXEC_POINT_EXIT);
      program.Execute(TID_0, EXEC_POINT_1);
      program.Execute(TID_1, EXEC_POINT_1);
      program.UpdateState(STATE_0);
      program.Diverge(TANGLE_0, TID_0, EXEC_POINT_2);
      program.Diverge(TANGLE_0, TID_1, EXEC_POINT_2);
      program.UpdateState(STATE_1);
      program.FunctionReturn(TANGLE_0, EXEC_POINT_3);
      program.UpdateState(STATE_1);
      program.Execute(TID_0, EXEC_POINT_3);
      program.Execute(TID_1, EXEC_POINT_3);
      program.UpdateState(STATE_2);
      program.Execute(TID_0, EXEC_POINT_EXIT);
      program.Execute(TID_1, EXEC_POINT_EXIT);
      program.UpdateState(STATE_3);
      program.Exit(TANGLE_0, TID_0);
      program.Exit(TANGLE_0, TID_1);
      program.UpdateState(STATE_4);

      TestTangles tangles0 = {
          {EXEC_POINT_1, {TID_0, TID_1}},
      };
      TestTangles tangles1 = {
          {EXEC_POINT_2, {TID_0, TID_1}},
      };
      TestTangles tangles2 = {
          {EXEC_POINT_3, {TID_0, TID_1}},
      };
      TestTangles tangles3 = {
          {EXEC_POINT_EXIT, {TID_0, TID_1}},
      };

      nbdarray<TestTangles> expected;
      expected.push_back(tangles0);
      expected.push_back(tangles1);
      expected.push_back(tangles2);
      expected.push_back(tangles3);
      expected.push_back(tanglesExit);

      RunTest(program, expected);
    }
    SECTION("Uniform Branch with a function call which diverges")
    {
      // uniform branch
      // EXEC_POINT_2 : is a function call with the return point EXEC_POINT_5
      // TID_0: EXEC_POINT_1 -> EXEC_POINT_2 -> EXEC_POINT_3 -> EXEC_POINT_5 -> EXEC_POINT_EXIT
      // TID_1: EXEC_POINT_1 -> EXEC_POINT_2 -> EXEC_POINT_4 -> EXEC_POINT_5 -> EXEC_POINT_EXIT
      Program program;
      program.Merge(TANGLE_0, EXEC_POINT_EXIT);
      program.Execute(TID_0, EXEC_POINT_1);
      program.Execute(TID_1, EXEC_POINT_1);
      program.UpdateState(STATE_0);
      program.Diverge(TANGLE_0, TID_0, EXEC_POINT_2);
      program.Diverge(TANGLE_0, TID_1, EXEC_POINT_2);
      program.UpdateState(STATE_1);
      program.FunctionReturn(TANGLE_0, EXEC_POINT_5);
      program.UpdateState(STATE_1);
      program.Execute(TID_0, EXEC_POINT_3);
      program.Execute(TID_1, EXEC_POINT_4);
      program.UpdateState(STATE_2);
      program.Execute(TID_0, EXEC_POINT_5);
      program.Execute(TID_1, EXEC_POINT_5);
      program.UpdateState(STATE_3);
      program.Execute(TID_0, EXEC_POINT_EXIT);
      program.Execute(TID_1, EXEC_POINT_EXIT);
      program.UpdateState(STATE_4);
      program.Exit(TANGLE_0, TID_0);
      program.Exit(TANGLE_0, TID_1);
      program.UpdateState(STATE_5);

      TestTangles tangles0 = {
          {EXEC_POINT_1, {TID_0, TID_1}},
      };
      TestTangles tangles1 = {
          {EXEC_POINT_2, {TID_0, TID_1}},
      };
      TestTangles tangles2 = {
          {EXEC_POINT_3, {TID_0}},
          {EXEC_POINT_4, {TID_1}},
      };
      TestTangles tangles3 = {
          {EXEC_POINT_5, {TID_0, TID_1}},
      };
      TestTangles tangles4 = {
          {EXEC_POINT_EXIT, {TID_0, TID_1}},
      };

      nbdarray<TestTangles> expected;
      expected.push_back(tangles0);
      expected.push_back(tangles1);
      expected.push_back(tangles2);
      expected.push_back(tangles3);
      expected.push_back(tangles4);
      expected.push_back(tanglesExit);

      RunTest(program, expected);
    }
    SECTION("50/50 Branch - one branch exits early")
    {
      // 50/50 branch
      // TID_0: EXEC_POINT_1 -> EXEC_POINT_2 -> EXEC_POINT_EXIT
      // TID_1: EXEC_POINT_1 -> EXEC_POINT_3 -> EXIT
      Program program;
      program.Merge(TANGLE_0, EXEC_POINT_EXIT);
      program.Execute(TID_0, EXEC_POINT_1);
      program.Execute(TID_1, EXEC_POINT_1);
      program.UpdateState(STATE_0);
      program.Diverge(TANGLE_0, TID_0, EXEC_POINT_2);
      program.Diverge(TANGLE_0, TID_1, EXEC_POINT_3);
      program.UpdateState(STATE_1);
      program.Execute(TID_0, EXEC_POINT_2);
      program.Execute(TID_1, EXEC_POINT_3);
      program.UpdateState(STATE_1);
      program.Execute(TID_0, EXEC_POINT_EXIT);
      program.UpdateState(STATE_2);
      program.Exit(TANGLE_1, TID_1);
      program.UpdateState(STATE_3);
      program.Exit(TANGLE_0, TID_0);
      program.UpdateState(STATE_4);

      TestTangles tangles0 = {
          {EXEC_POINT_1, {TID_0, TID_1}},
      };
      TestTangles tangles1 = {
          {EXEC_POINT_2, {TID_0}},
          {EXEC_POINT_3, {TID_1}},
      };
      TestTangles tangles2 = {
          {EXEC_POINT_EXIT, {TID_0}},
          {EXEC_POINT_3, {TID_1}},
      };
      TestTangles tangles3 = {
          {EXEC_POINT_EXIT, {TID_0}},
      };

      nbdarray<TestTangles> expected;
      expected.push_back(tangles0);
      expected.push_back(tangles1);
      expected.push_back(tangles2);
      expected.push_back(tangles3);
      expected.push_back(tanglesExit);

      RunTest(program, expected);
    }
  }
}
#endif    // ENABLED(ENABLE_UNIT_TESTS)
