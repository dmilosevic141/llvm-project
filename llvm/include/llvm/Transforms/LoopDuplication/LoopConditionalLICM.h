//===- LoopConditionalLICM.h - Loop Conditional LICM Pass -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Loop Conditional LICM (LCLICM) pass is based on the LICM pass. Promotion
// candidates are in the form of:
//      ptr_a = ptr_a <BinaryOperator> ptr_b
// where the ptr_a would be the promotion candidate.
// Such candidates should be promoted iff it is determined that the accesses on
// the RHS do not alias. Most of the time, however, that cannot be proven at
// compile-time. The LCLICM pass aims to duplicate loops containing such
// candidates, inserting a branch which explicitly checks if the RHS accesses
// alias and branches to one of the loops accordingly. If the condition is
// satisfied, candidates are promoted. Consider the following example:
//
//      extern long *a, b[100][8];
//      for (long i = 0; i < 100; i++)
//      {
//        a[i] = 0;
//        for (int j = 0; j < 8; j++)
//        {
//          a[i] += b[i][j];
//        }
//      }
//
// A transformation on the store to a[i] can be done if it is determined that a
// does not overlap b:
//
//      for (long i = 0; i < 100; i++)
//      {
//        a[i] = 0;
//        if (&a[100] < b || a > &b[100][8])
//        {
//          long a1 = a[i];
//          for (int j = 0; j < 8; j++)
//          {
//            a1 += b[i][j];
//          }
//          a[i] = a1;
//        }
//        else
//        {
//          for (int j = 0; j < 8; j++)
//          {
//            a[i] += b[i][j];
//          }
//        }
//      }
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LOOPDUPLICATION_LOOPCONDITIONALLICM_H
#define LLVM_TRANSFORMS_LOOPDUPLICATION_LOOPCONDITIONALLICM_H

#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/LoopDuplication/LoopDuplicationPass.h"
#include <unordered_map>

namespace llvm {

class LPMUpdater;
class LPPassManager;

class LoopConditionalLICMPass : public PassInfoMixin<LoopConditionalLICMPass>,
                                public LoopDuplicationPass {
  // Additional analyses.
  AliasAnalysis *AA;
  ScalarEvolution *SE;

  /// FIXME: Do we need the map, if we only handle ptr_a = ptr_a
  /// <BinaryOperator> ptr_b formats?
  SmallDenseMap<Value *, SmallVector<std::pair<Value *, uint64_t>>>
      PromotionPtrDeps;

  SmallVector<SmallSetVector<Value *, 8U>> PromotionPtrMustAliasSets;

  std::function<const LoopAccessInfo &(Loop &)> GetLAA;

public:
  PreservedAnalyses run(Loop &L, LoopAnalysisManager &AM,
                        LoopStandardAnalysisResults &AR, LPMUpdater &U);

  bool runOnLoop(Loop *L, AAResults *_AA, DominatorTree *_DT, LoopInfo *_LI,
                 LPMUpdater *_LPMU, LPPassManager *_LPPM,
                 MemorySSAUpdater *_MSSAU, ScalarEvolution *_SE,
                 TargetLibraryInfo *_TLI,
                 std::function<const LoopAccessInfo &(Loop &)> &_GetLAA);

  virtual ~LoopConditionalLICMPass() = default;

private:
  bool isCurrentLoopACandidate() override;

  void emitPreheaderBranch(BasicBlock *TrueDest, BasicBlock *FalseDest,
                           BranchInst *OldBranch) override;

  void optimizeDuplicatedLoop() override;

  bool isAccessibleInPreheader(const Value *V);

  bool populatePromotionPtrDeps();
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_LOOPDUPLICATION_LOOPCONDITIONALLICM_H
