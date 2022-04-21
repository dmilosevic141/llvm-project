//===- LASBC.h - Loop Array Subscript Bound Checking Pass -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Loop Array Subscript Bound Checking (LASBC) pass adds checks for
// out-of-bounds references on array subscripts, to allow additional loop
// optimizations to occur. For example, the following C code:
//
//    extern long a[], b[], c, n;
//    for (int32_t i = 0; i < n; i++)
//    {
//      a[i] = b[i] * c;
//    }
//
// Requires several sign extensions to be generated when calculating array
// subscripts:
//
//    int64_t i1 = (int64_t) ((int32_t) (((int64_t) i) * 8));
//    char a1 = ((char ) a) + i1;
//    *((int64_t *) &a1) = ...
//
// If it can be determined that the 'i * 8' will not overflow INT32_MAX, then
// the loop can be duplicated and transformed into:
//
//    if (n > 0 && (n * sizeof(uint64_t)) <= 32767)
//    {
//      for (uint64_t  i = 0; i < n; i++)
//      {
//        a[i] = b[i] * c;
//      }
//    }
//    else
//    {
//      for (int32_t i = 0; i < n; i++)
//      {
//        a[i] = b[i] * c;
//      }
//    }
//
// 'int32' induction variables inhibit further loop transformations since
// the array subscript calculation can, hypothetically, overflow INT32_MAX,
// which is undefined. The addition of the loop with the 'int64' (unsigned long)
// induction variable allows further loop strength reductions and loop unrolling
// to occur.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LOOPDUPLICATION_LASBC_H
#define LLVM_TRANSFORMS_LOOPDUPLICATION_LASBC_H

#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/LoopDuplication/LoopDuplicationPass.h"
#include <unordered_set>

namespace llvm {

class BinaryOperator;
class ConstantInt;
class LPMUpdater;
class LPPassManager;
class ZExtInst;

class LASBCPass : public PassInfoMixin<LASBCPass>, public LoopDuplicationPass {
  PHINode *IV = nullptr;
  BinaryOperator *IVLoopIterationValue = nullptr;
  ConstantInt *IVLoopIterationValueConstOperand = nullptr;
  ZExtInst *IVExtension = nullptr;
  Value *N = nullptr;

public:
  PreservedAnalyses run(Loop &L, LoopAnalysisManager &AM,
                        LoopStandardAnalysisResults &AR, LPMUpdater &U);

  bool runOnLoop(Loop *L, DominatorTree *_DT, LoopInfo *_LI, LPMUpdater *_LPMU,
                 LPPassManager *_LPPM, MemorySSAUpdater *_MSSAU,
                 TargetLibraryInfo *_TLI);

  virtual ~LASBCPass() = default;

private:
  bool isPHIAnAppropriateIV(PHINode *PN);

  bool isCurrentLoopACandidate() override;

  void emitPreheaderBranch(BasicBlock *TrueDest, BasicBlock *FalseDest,
                           BranchInst *OldBranch) override;

  void optimizeDuplicatedLoop() override;
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_LOOPDUPLICATION_LASBC_H
