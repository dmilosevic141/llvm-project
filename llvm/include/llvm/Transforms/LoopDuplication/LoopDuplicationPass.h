//===- LoopDuplicationPass.h - Loop Duplication Pass ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// LoopDuplicationPass is an abstract class/interface for passes which duplicate
// a loop and branch to the original/duplicated loop based on a certain
// condition. Such passes need to implement these abstract methods:
//  - isCurrentLoopACandidate: Should the current loop be considered for the
//  transformation?
//  - runPreEmitPreheaderBranchAnalyses (optional): Run additional analyses
//  before emitting the preheader branch.
//  - emitPreheaderBnrach: Emit a branch in the preheader which chooses one out
//  of the original/duplicated loop.
//  - optimizeDuplicatedLoop: Transform the duplicated loop accordingly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LOOPDUPLICATION_LOOPDUPLICATIONPASS_H
#define LLVM_TRANSFORMS_LOOPDUPLICATION_LOOPDUPLICATIONPASS_H

#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/IR/PassManager.h"
#include <unordered_set>

namespace llvm {

class LPMUpdater;
class LPPassManager;

class LoopDuplicationPass {
public:
  LoopDuplicationPass() = default;

  virtual ~LoopDuplicationPass(){};

protected:
  // Analyses.
  DominatorTree *DT = nullptr;
  LoopInfo *LI = nullptr;
  MemorySSAUpdater *MSSAU = nullptr;
  TargetLibraryInfo *TLI = nullptr;

  // Current loop properties.
  Loop *CurrentLoop = nullptr;
  BasicBlock *Preheader = nullptr;
  BasicBlock *Header = nullptr;

  // Duplicated loop properties.
  Loop *DuplicatedLoop = nullptr;

  const DataLayout *DL = nullptr;
  LPMUpdater *LPMU = nullptr;
  LPPassManager *LPPM = nullptr;

  std::unordered_set<Loop *> ProcessedLoops;

  void transformCurrentLoop();

private:
  virtual bool isCurrentLoopACandidate() = 0;

  virtual void emitPreheaderBranch(BasicBlock *TrueDest, BasicBlock *FalseDest,
                                   BranchInst *OldBranch) = 0;

  virtual void optimizeDuplicatedLoop() = 0;

  void splitExitEdges(const SmallVectorImpl<BasicBlock *> &ExitBlocks);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_LOOPDUPLICATION_LOOPDUPLICATIONPASS_H
