//===- LASBC.h - Loop Array Subscript Bound Checking Pass ----------------===//
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

#include "llvm/Transforms/LoopDuplication/LASBC.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/MustExecute.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PredIteratorCache.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/LoopDuplication.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <vector>
using namespace llvm;

PreservedAnalyses LASBCPass::run(Loop &L, LoopAnalysisManager &AM,
                                 LoopStandardAnalysisResults &AR,
                                 LPMUpdater &U) {
  Optional<MemorySSAUpdater> OptionalMSSAU;
  if (AR.MSSA)
    OptionalMSSAU = MemorySSAUpdater(AR.MSSA);

  if (!runOnLoop(&L, &AR.DT, &AR.LI, &U, nullptr,
                 OptionalMSSAU.hasValue() ? OptionalMSSAU.getPointer()
                                          : nullptr,
                 &AR.TLI))
    return PreservedAnalyses::all();

  auto PA = getLoopPassPreservedAnalyses();

  PA.preserve<DominatorTreeAnalysis>();
  PA.preserve<LoopAnalysis>();
  if (AR.MSSA)
    PA.preserve<MemorySSAAnalysis>();

  return PA;
}

bool LASBCPass::isPHIAnAppropriateIV(PHINode *PN) {
  // Check IV's type.
  auto *IVType = PN->getType();
  if (!IVType->isIntegerTy() || DL->getTypeSizeInBits(IVType) != 32)
    return false;

  unsigned IncomingEdge = CurrentLoop->contains(PN->getIncomingBlock(0));
  unsigned BackEdge = IncomingEdge ^ 1;

  // IterValue should be a BinaryOperator with only one user - PN.
  auto *IterValue = dyn_cast<BinaryOperator>(PN->getIncomingValue(BackEdge));
  if (!IterValue || !IterValue->hasOneUser())
    return false;

  // First operand of IterValue should be PN, while the second operand should be
  // a ConstantInt.
  ConstantInt *ConstOperandValue =
      dyn_cast<ConstantInt>(IterValue->getOperand(1));
  if (!ConstOperandValue || IterValue->getOperand(0) != PN)
    return false;
  // PN should have only two users - a ZEXT instruction, and an instruction to
  // update the IV.
  Value::user_iterator ValueUser = PN->user_begin();
  Instruction *U1 = cast<Instruction>(*ValueUser++);
  if (ValueUser == PN->user_end())
    return false;
  Instruction *U2 = cast<Instruction>(*ValueUser++);
  if (ValueUser != PN->user_end())
    return false;

  // ZExt should have one user for the exit condition, which should be an
  // ICmpInst, and one or more users for GetElementPtrInsts. The exit condition
  // should only be used by a single branch instruction.
  ZExtInst *ZExt = dyn_cast<ZExtInst>(U1);
  if (!ZExt)
    ZExt = dyn_cast<ZExtInst>(U2);
  if (!ZExt)
    return false;
  ICmpInst *Compare = nullptr;
  bool SawGEPInstruction = false;
  for (ValueUser = ZExt->user_begin(); ValueUser != ZExt->user_end();
       ++ValueUser) {
    Instruction *I = cast<Instruction>(*ValueUser);
    if (ICmpInst *ICmp = dyn_cast<ICmpInst>(I))
      if (!Compare && ICmp->hasOneUser() && isa<BranchInst>(ICmp->user_back()))
        Compare = ICmp;
      else
        return false;
    else if (isa<GetElementPtrInst>(I))
      SawGEPInstruction = true;
    else
      return false;
  }
  if (!Compare || !SawGEPInstruction)
    return false;

  // Capture N.
  N = Compare->getOperand(0);
  if (N == ZExt->getOperand(0))
    N = Compare->getOperand(1);
  // N has to be a loop-invariant value, located in the preheader.
  Instruction *NInst = dyn_cast<Instruction>(N);
  if (!NInst || !CurrentLoop->isLoopInvariant(NInst) ||
      NInst->getParent() != CurrentLoop->getLoopPreheader())
    return false;

  BranchInst *TheBranch =
      dyn_cast<BranchInst>(cast<Instruction>(Compare->user_back()));
  // Verify that the branch actually controls the iteration count
  // of the loop. The branch block must be in the loop and one of the successors
  // must be out of the loop.
  assert(TheBranch->isConditional() && "Can't use ICMP if not conditional!");
  if (!CurrentLoop->contains(TheBranch->getParent()) ||
      (CurrentLoop->contains(TheBranch->getSuccessor(0)) &&
       CurrentLoop->contains(TheBranch->getSuccessor(1))))
    return false;

  // Capture PHINode, back edge incoming value for the IV, its
  // constant operand and ZExtInst.
  IV = PN;
  IVLoopIterationValue = IterValue;
  IVLoopIterationValueConstOperand = ConstOperandValue;
  IVExtension = ZExt;

  return true;
}

/// TODO: Revisit the format for the loop candidate.
/// A loop candidate for the transformation to occur has:
///   - 'int32' IV,
///   - 'int64' exit condition value,
///   - A ConstantInt value which updates the IV,
///   - No other modifiers of the IV within the loop,
///   - Two types of uses for the IV - a ZExtInst (to compare with the exit
///     condition value), and GetElementPtrInsts (to calculate the array
///     subscripts).
bool LASBCPass::isCurrentLoopACandidate() {
  for (PHINode &PN : Header->phis())
    if (isPHIAnAppropriateIV(&PN))
      return true;
  return false;
}

/// Insert the "n > 0 && (n * sizeof(int64_t)) <= INT32_MAX)" check in the
/// original preheader, as well as an appropriate branch to the
/// original/cloned loop.
void LASBCPass::emitPreheaderBranch(BasicBlock *TrueDest, BasicBlock *FalseDest,
                                    BranchInst *OldBranch) {
  assert(OldBranch->isUnconditional() && "Preheader is not split correctly!");
  assert(TrueDest != FalseDest && "Branch targets should be different!");

  BasicBlock *OldBranchSucc = OldBranch->getSuccessor(0);
  BasicBlock *OldBranchParent = OldBranch->getParent();

  ICmpInst *NGreaterThanZero =
      new ICmpInst(Preheader->getTerminator(), CmpInst::Predicate::ICMP_SGT, N,
                   ConstantInt::get(N->getType(), /* Value */ 0));

  Instruction *MulInst = BinaryOperator::CreateMul(
      N,
      ConstantInt::get(N->getType(), DL->getTypeSizeInBits(N->getType()) / 8),
      "mul.lasbc", OldBranch);
  ICmpInst *NLesserThanINT32_MAX =
      new ICmpInst(OldBranch, CmpInst::Predicate::ICMP_SLE, MulInst,
                   ConstantInt::get(N->getType(), /* Value */ INT32_MAX));

  Instruction *AndInst = BinaryOperator::CreateAnd(
      NGreaterThanZero, NLesserThanINT32_MAX, "and.lasbc", OldBranch);
  ICmpInst *BranchCondition =
      new ICmpInst(OldBranch, CmpInst::Predicate::ICMP_EQ, AndInst,
                   ConstantInt::get(AndInst->getType(), /* Value */ 1));

  BranchInst *NewTerminator =
      BranchInst::Create(TrueDest, FalseDest, BranchCondition);
  // OldBranch is the original preheader's terminator - replace it.
  ReplaceInstWithInst(OldBranch, NewTerminator);

  SmallVector<DominatorTree::UpdateType, 3> Updates;
  if (TrueDest != OldBranchSucc)
    Updates.push_back({DominatorTree::Insert, OldBranchParent, TrueDest});
  if (FalseDest != OldBranchSucc)
    Updates.push_back({DominatorTree::Insert, OldBranchParent, FalseDest});
  // If both of the new successors are different from the old one, inform the
  // DT that the edge was deleted.
  if (OldBranchSucc != TrueDest && OldBranchSucc != FalseDest)
    Updates.push_back({DominatorTree::Delete, OldBranchParent, OldBranchSucc});
  if (MSSAU)
    MSSAU->applyUpdates(Updates, *DT, /* UpdateDT */ true);
  else
    DT->applyUpdates(Updates);
}

/// Update IV's type from 'int32' to 'int64', and its uses
/// accordingly.
void LASBCPass::optimizeDuplicatedLoop() {
  Type *NType = N->getType();

  PHINode *NewPHI = PHINode::Create(NType, 2, IV->getName() + ".lasbc.n", IV);
  unsigned IncomingEdge = CurrentLoop->contains(IV->getIncomingBlock(0));
  unsigned BackEdge = IncomingEdge ^ 1;

  NewPHI->addIncoming(ConstantInt::get(NType, /* Value */ 0),
                      IV->getIncomingBlock(IncomingEdge));

  auto *IVIterValueAsBinaryOperator =
      dyn_cast<BinaryOperator>(IVLoopIterationValue);
  Value *NewIterValue = BinaryOperator::Create(
      IVIterValueAsBinaryOperator->getOpcode(), NewPHI,
      ConstantInt::get(NType, IVLoopIterationValueConstOperand->getSExtValue()),
      IVIterValueAsBinaryOperator->getName() + ".lasbc",
      IVIterValueAsBinaryOperator);
  NewPHI->addIncoming(NewIterValue, IV->getIncomingBlock(BackEdge));

  // Remove IVExtension, since an extension of the IV is not needed anymore.
  IVExtension->replaceAllUsesWith(NewPHI);
  RecursivelyDeleteTriviallyDeadInstructions(IVExtension, TLI, MSSAU);

  // Remove IVLoopIterationValue, since NewIterValue was created.
  IVLoopIterationValue->replaceAllUsesWith(
      UndefValue::get(IVLoopIterationValue->getType()));
  RecursivelyDeleteTriviallyDeadInstructions(IVLoopIterationValue, TLI, MSSAU);

  if (VerifyMemorySSA && MSSAU)
    MSSAU->getMemorySSA()->verifyMemorySSA();
}

bool LASBCPass::runOnLoop(Loop *L, DominatorTree *_DT, LoopInfo *_LI,
                          LPMUpdater *_LPMU, LPPassManager *_LPPM,
                          MemorySSAUpdater *_MSSAU, TargetLibraryInfo *_TLI) {

  DT = _DT;
  LI = _LI;
  MSSAU = _MSSAU;
  TLI = _TLI;

  CurrentLoop = L;
  Preheader = CurrentLoop->getLoopPreheader();
  Header = CurrentLoop->getHeader();

  DuplicatedLoop = nullptr;

  DL = &Preheader->getParent()->getParent()->getDataLayout();
  LPMU = _LPMU;
  LPPM = _LPPM;

  N = nullptr;
  IV = nullptr;
  IVLoopIterationValue = nullptr;
  IVLoopIterationValueConstOperand = nullptr;
  IVExtension = nullptr;
  if (ProcessedLoops.count(CurrentLoop))
    return false;

  if (!isCurrentLoopACandidate())
    return false;

  transformCurrentLoop();
  return true;
}

namespace {
struct LegacyLASBCPass : public LoopPass {
  static char ID; // Pass identification, replacement for typeid.
  LegacyLASBCPass() : LoopPass(ID), LASBC() {
    initializeLegacyLASBCPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override {
    auto *MSSAAnalysis = getAnalysisIfAvailable<MemorySSAWrapperPass>();
    Optional<MemorySSAUpdater> MSSAU;
    if (MSSAAnalysis)
      MSSAU = MemorySSAUpdater(&MSSAAnalysis->getMSSA());
    return LASBC.runOnLoop(
        L, &getAnalysis<DominatorTreeWrapperPass>().getDomTree(),
        &getAnalysis<LoopInfoWrapperPass>().getLoopInfo(), nullptr, &LPM,
        MSSAU.hasValue() ? MSSAU.getPointer() : nullptr,
        &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(
            *L->getHeader()->getParent()));
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addPreserved<MemorySSAWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    getLoopAnalysisUsage(AU);
  }

private:
  LASBCPass LASBC;
};
} // namespace

char LegacyLASBCPass::ID = 0;
INITIALIZE_PASS_BEGIN(LegacyLASBCPass, "lasbc",
                      "Loop Array Subscripts Bounds Checking", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(LegacyLASBCPass, "lasbc",
                    "Loop Array Subscripts Bounds Checking", false, false)

Pass *llvm::createLASBCPass() { return new LegacyLASBCPass(); }
