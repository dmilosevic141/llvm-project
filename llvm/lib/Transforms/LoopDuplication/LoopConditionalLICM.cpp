//===-- LoopConditionalLICM.cpp - Loop Conditional LICM Pass --------------===//
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
// where ptr_a would be the promotion candidate.
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

#include "llvm/Transforms/LoopDuplication/LoopConditionalLICM.h"
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

using MemLoc = std::pair<Value *, uint64_t>;

PreservedAnalyses LoopConditionalLICMPass::run(Loop &L, LoopAnalysisManager &AM,
                                               LoopStandardAnalysisResults &AR,
                                               LPMUpdater &U) {
  if (!AR.MSSA)
    report_fatal_error("LoopConditionalLICM requires MemorySSA analysis!");

  std::unique_ptr<MemorySSAUpdater> MSSAU =
      std::make_unique<MemorySSAUpdater>(AR.MSSA);

  std::function<const LoopAccessInfo &(Loop &)> GetLAA =
      [&](Loop &L) -> const LoopAccessInfo & {
    return AM.getResult<LoopAccessAnalysis>(L, AR);
  };

  if (!runOnLoop(&L, &AR.AA, &AR.DT, &AR.LI, &U, nullptr, MSSAU.get(), &AR.SE,
                 &AR.TLI, GetLAA))
    return PreservedAnalyses::all();

  auto PA = getLoopPassPreservedAnalyses();

  PA.preserve<DominatorTreeAnalysis>();
  PA.preserve<LoopAnalysis>();
  PA.preserve<MemorySSAAnalysis>();

  return PA;
}

static void foreachMemoryAccess(MemorySSA *MSSA, Loop *L,
                                function_ref<void(Instruction *)> Fn) {
  for (const BasicBlock *BB : L->blocks())
    if (const auto *Accesses = MSSA->getBlockAccesses(BB))
      for (const auto &Access : *Accesses)
        if (const auto *MUD = dyn_cast<MemoryUseOrDef>(&Access))
          Fn(MUD->getMemoryInst());
}

/// Similar to the LICM's collectPromotionCandidates, except that it does not
/// discard sets for which there is an aliasing non-promotable access.
static SmallVector<SmallSetVector<Value *, 8>, 0>
collectPromotionCandidates(MemorySSA *MSSA, AliasAnalysis *AA, Loop *L) {
  AliasSetTracker AST(*AA);

  auto IsPotentiallyPromotable = [L](const Instruction *I) {
    if (const auto *SI = dyn_cast<StoreInst>(I))
      return L->isLoopInvariant(SI->getPointerOperand());
    if (const auto *LI = dyn_cast<LoadInst>(I))
      return L->isLoopInvariant(LI->getPointerOperand());
    return false;
  };

  // Populate AST with potentially promotable accesses and remove them from
  // MaybePromotable, so they will not be checked again on the next iteration.
  SmallPtrSet<Value *, 16> AttemptingPromotion;
  foreachMemoryAccess(MSSA, L, [&](Instruction *I) {
    if (IsPotentiallyPromotable(I)) {
      AttemptingPromotion.insert(I);
      AST.add(I);
    }
  });

  // We're only interested in must-alias sets that contain a mod.
  SmallVector<const AliasSet *, 8> Sets;
  for (AliasSet &AS : AST)
    if (!AS.isForwardingAliasSet() && AS.isMod() && AS.isMustAlias())
      Sets.push_back(&AS);

  if (Sets.empty())
    return {}; // Nothing to promote...

  SmallVector<SmallSetVector<Value *, 8>, 0> Result;
  for (const AliasSet *Set : Sets) {
    SmallSetVector<Value *, 8> PointerMustAliases;
    for (const auto &ASI : *Set)
      PointerMustAliases.insert(ASI.getValue());
    Result.push_back(std::move(PointerMustAliases));
  }

  return Result;
}

bool LoopConditionalLICMPass::isCurrentLoopACandidate() {
  // Require loops with preheaders and dedicated exits.
  if (!CurrentLoop->isLoopSimplifyForm())
    return false;

  // Require loops with an induction variable.
  if (!CurrentLoop->getInductionVariable(*SE))
    return false;

  // Since cloning is used to split the loop, it has to be safe to clone.
  if (!CurrentLoop->isSafeToClone())
    return false;

  // If the loop has multiple exiting blocks, do not split.
  if (!CurrentLoop->getExitingBlock())
    return false;

  // If the loop has multiple exit blocks, do not split.
  if (!CurrentLoop->getExitBlock())
    return false;

  // Only split innermost loops. Thus, if the loop has any children, it cannot
  // be split.
  if (!CurrentLoop->getSubLoops().empty())
    return false;

  return true;
}

/// Given a value, check if it is accessible in the preheader.
/// A value is accessible in the preheader if:
///   - It is a constant, an argument to the function, or a global value.
///   - It is an instruction, such that the parent BB dominates the preheader.
bool LoopConditionalLICMPass::isAccessibleInPreheader(const Value *V) {
  if (isa<Constant>(V) || isa<Argument>(V) || isa<GlobalValue>(V))
    return true;
  // Otherwise, it has to be an instruction...
  /// TODO: Does it?
  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return false;
  return DT->dominates(I->getParent(), Preheader);
}

/// TODO: So far, only the ptr_a = ptr_a <BinaryOperator> ptr_b cases are
/// handled.
/// Walk through each memory access, searching for a store to a
/// promotion candidate. If such store is found, make sure that the value
/// operand is a BinaryOperator with both operands being loads of
/// GetElementPtrInst pointers (and one of them being the promotion candidate to
/// which the store is happening). Such operands are registered within the
/// PromotionPtrDeps map, as a pair: {Starting Address, Offset}.
bool LoopConditionalLICMPass::populatePromotionPtrDeps() {
  (void)&GetLAA(*CurrentLoop);

  for (const BasicBlock *BB : CurrentLoop->blocks())
    if (const auto *Accesses = MSSAU->getMemorySSA()->getBlockAccesses(BB))
      for (const auto &Access : *Accesses) {
        const auto *MUD = dyn_cast<MemoryUseOrDef>(&Access);
        if (!MUD)
          continue;

        const auto *SI = dyn_cast<StoreInst>(MUD->getMemoryInst());
        if (!SI)
          continue;
        // Only intereted in stores *to* a promotion candidate.
        Value *PointerOperand = const_cast<Value *>(SI->getPointerOperand());
        if (!PromotionPtrDeps.count(PointerOperand))
          continue;
        // Value being stored to a promotion candidate should be a
        // BinaryOperator.
        auto *BinOp = dyn_cast<BinaryOperator>(SI->getValueOperand());
        if (!BinOp)
          continue;
        // Check BinaryOperator's operands - both of them should be loads of
        // GetElementPtr instructions.
        bool SawPointerOperand = false;
        for (Value *Op : BinOp->operands()) {
          if (const auto *LI = dyn_cast<LoadInst>(Op)) {
            auto *LIPointerOperand = LI->getPointerOperand();
            if (PointerOperand == LIPointerOperand)
              SawPointerOperand = true;
            if (auto GEP = dyn_cast<GEPOperator>(LIPointerOperand)) {
              // Multidimensional arrays with unknown dimensions are accessed
              // through a chain of GEP instructions (one for each dimension).
              // Walk that (potential) chain, computing the actual offset along
              // the way.
              Value *StartAddr = nullptr;
              APInt OffsetFromStartAddr;
              while (LI && GEP) {
                // Compute GEP's offset.
                APInt Offset;
                if (GEP->getNumIndices() == 1) {
                  Value *Index = GEP->getOperand(1);
                  auto SEExpr =
                      SE->getSCEVAtScope(Index, CurrentLoop->getParentLoop());
                  Offset = SE->getUnsignedRangeMax(SEExpr);
                } else
                  // Complex GEP. Just get the type of the src elem type
                  // since the size will indicate the offset off the end
                  // of the data structure.
                  Offset = APInt(
                      64,
                      DL->getTypeSizeInBits(GEP->getSourceElementType()) / 8);

                // Update the actual offset and the starting address.
                if (StartAddr)
                  OffsetFromStartAddr *= Offset;
                else
                  OffsetFromStartAddr = Offset;

                StartAddr = const_cast<Value *>(GEP->getPointerOperand());

                // Move up the chain, if possible.
                LI = dyn_cast<LoadInst>(StartAddr);
                GEP = LI ? dyn_cast<GEPOperator>(LI->getPointerOperand())
                         : nullptr;
              }
              // StartAddr has to be accessible in the preheader, so the
              // appropriate checks can be inserted.
              if (!isAccessibleInPreheader(StartAddr))
                return false;

              PromotionPtrDeps[PointerOperand].push_back(
                  {StartAddr, *OffsetFromStartAddr.getRawData()});
            } else
              return false;
          } else
            return false;
        }
        // One of the operands has to be the promotion candidate to satisfy the
        // format: ptr_a = ptr_a <BinaryOperator> ptr_b.
        if (!SawPointerOperand)
          return false;
      }
  // Has anything been collected?
  for (auto &Pair : PromotionPtrDeps)
    if (Pair.second.size() > 0)
      return true;
  return false;
}

/// Insert appropriate checks (given the PromotionPtrDeps map) in the original
/// preheader, as well as an appropriate branch to the original/cloned loop.
void LoopConditionalLICMPass::emitPreheaderBranch(BasicBlock *TrueDest,
                                                  BasicBlock *FalseDest,
                                                  BranchInst *OldBranch) {
  assert(OldBranch->isUnconditional() && "Preheader is not split correctly!");
  assert(TrueDest != FalseDest && "Branch targets should be different!");

  BasicBlock *OldBranchSucc = OldBranch->getSuccessor(0);
  BasicBlock *OldBranchParent = OldBranch->getParent();

  auto &Ctx = Preheader->getTerminator()->getContext();

  struct OverlapConditionPair {
    Value *StartA, *StartB, *EndA, *EndB;
    OverlapConditionPair(Value *_StartA, Value *_StartB, Value *_EndA,
                         Value *_EndB)
        : StartA(_StartA), StartB(_StartB), EndA(_EndA), EndB(_EndB) {}
  };

  // Get the pointers to the 'Starting Address' and 'Starting Address' +
  // 'Offset'.
  auto GetStartAndEnd = [&Ctx, OldBranch](std::pair<Value *, uint64_t> &Dep) {
    Type *Ty = nullptr;
    if (Dep.first->getType()->isOpaquePointerTy())
      Ty = Dep.first->getType();
    else
      Ty = Dep.first->getType()->getNonOpaquePointerElementType();

    Value *End = nullptr;
    auto EndPtr = GetElementPtrInst::Create(
        Ty, Dep.first, ConstantInt::get(Type::getInt64Ty(Ctx), Dep.second),
        "lclicm.depptrend", OldBranch);
    if (EndPtr->getType()->isIntegerTy(64))
      End = EndPtr;
    else
      End = new PtrToIntInst(EndPtr, Type::getInt64Ty(Ctx),
                             "lclicm.ptrtointend", OldBranch);

    Value *Start = nullptr;
    auto StartPtr = GetElementPtrInst::Create(
        Ty, Dep.first, ConstantInt::get(Type::getInt64Ty(Ctx), 0),
        "lclicm.depptrstart", OldBranch);
    if (StartPtr->getType()->isIntegerTy(64))
      Start = StartPtr;
    else
      Start = new PtrToIntInst(StartPtr, Type::getInt64Ty(Ctx),
                               "lclicm.ptrtointstart", OldBranch);

    return std::make_pair<>(Start, End);
  };

  SmallVector<OverlapConditionPair> OverlapConditionPairs;
  for (auto &Pair : PromotionPtrDeps) {
    for (int i = 0, Size = Pair.second.size(); i < Size - 1; ++i) {
      auto A = GetStartAndEnd(Pair.second[i]);
      for (int j = i + 1; j < Size; ++j) {
        auto B = GetStartAndEnd(Pair.second[j]);
        OverlapConditionPairs.push_back(
            OverlapConditionPair(A.first, B.first, A.second, B.second));
      }
    }
  }

  // Given two starting addresses: SAA and SAB, and two offsets: OA
  // and OB, accesses are sure not to overlap iff one of the following two is
  // satisfied:
  //  - SAA > (SAB + OB), or
  //  - (SAA + OA) < SAB.
  SmallVector<BinaryOperator *> Ors;
  for (auto &OCP : OverlapConditionPairs) {
    ICmpInst *Cmp1 = new ICmpInst(OldBranch, CmpInst::Predicate::ICMP_SGT,
                                  OCP.StartA, OCP.EndB);
    ICmpInst *Cmp2 = new ICmpInst(OldBranch, CmpInst::Predicate::ICMP_SLT,
                                  OCP.EndB, OCP.StartA);
    BinaryOperator *Or = BinaryOperator::Create(Instruction::Or, Cmp1, Cmp2,
                                                "lclicm.or", OldBranch);
    Ors.push_back(Or);
  }

  // Make sure that the overlap conditions for each promotion candidate are
  // satisfied.
  Value *FinalCond = nullptr;
  if (Ors.size() == 1)
    FinalCond = *Ors.begin();
  else {
    FinalCond = BinaryOperator::Create(Instruction::And, Ors[0], Ors[1],
                                       "lclicm.and", OldBranch);
    for (int i = 2, sz = Ors.size(); i < sz; ++i)
      FinalCond = BinaryOperator::Create(Instruction::And, FinalCond, Ors[i],
                                         "lclicm.and", OldBranch);
  }

  BranchInst *NewTerminator =
      BranchInst::Create(TrueDest, FalseDest, FinalCond);

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
  MSSAU->applyUpdates(Updates, *DT, /* UpdateDT */ true);
}

/// Promote the candidates in the cloned loop.
void LoopConditionalLICMPass::optimizeDuplicatedLoop() {
  for (auto &PromotionPtrMustAliasSet : PromotionPtrMustAliasSets) {
    SmallVector<BasicBlock *, 8> ExitBlocks;
    DuplicatedLoop->getUniqueExitBlocks(ExitBlocks);

    PredIteratorCache PIC;
    SmallVector<Instruction *, 8> InsertPts;
    SmallVector<MemoryAccess *, 8> MSSAInsertPts;
    InsertPts.reserve(ExitBlocks.size());
    MSSAInsertPts.reserve(ExitBlocks.size());
    for (BasicBlock *ExitBlock : ExitBlocks) {
      InsertPts.push_back(&*ExitBlock->getFirstInsertionPt());
      MSSAInsertPts.push_back(nullptr);
    }

    ICFLoopSafetyInfo SafetyInfo;
    SafetyInfo.computeLoopSafetyInfo(DuplicatedLoop);

    OptimizationRemarkEmitter ORE(DuplicatedLoop->getHeader()->getParent());

    promoteLoopAccessesToScalars(PromotionPtrMustAliasSet, ExitBlocks,
                                 InsertPts, MSSAInsertPts, PIC, LI, DT, TLI,
                                 DuplicatedLoop, *MSSAU, &SafetyInfo, &ORE,
                                 /* AllowSpeculation */ false);
  }
}

bool LoopConditionalLICMPass::runOnLoop(
    Loop *L, AAResults *_AA, DominatorTree *_DT, LoopInfo *_LI,
    LPMUpdater *_LPMU, LPPassManager *_LPPM, MemorySSAUpdater *_MSSAU,
    ScalarEvolution *_SE, TargetLibraryInfo *_TLI,
    std::function<const LoopAccessInfo &(Loop &)> &_GetLAA) {
  AA = _AA;
  DT = _DT;
  LI = _LI;
  MSSAU = _MSSAU;
  SE = _SE;
  TLI = _TLI;

  CurrentLoop = L;
  Preheader = CurrentLoop->getLoopPreheader();
  Header = CurrentLoop->getHeader();

  DuplicatedLoop = nullptr;

  DL = &Preheader->getParent()->getParent()->getDataLayout();
  LPMU = _LPMU;
  LPPM = _LPPM;

  // Check if the loop has already been processed.
  if (ProcessedLoops.count(CurrentLoop))
    return false;

  if (!isCurrentLoopACandidate())
    return false;

  // Collect the promotion candidates, if any.
  PromotionPtrMustAliasSets.clear();
  PromotionPtrMustAliasSets =
      collectPromotionCandidates(MSSAU->getMemorySSA(), AA, L);
  if (PromotionPtrMustAliasSets.empty())
    return false;

  PromotionPtrDeps.clear();
  for (auto &PromotionPtrAliasSet : PromotionPtrMustAliasSets)
    PromotionPtrDeps.insert({PromotionPtrAliasSet[0], {}});
  GetLAA = _GetLAA;
  // Make sure that each promotion candidate satisfies the ptr_a = ptr_a
  // <BinaryOperator> ptr_b format.
  if (!populatePromotionPtrDeps())
    return false;

  transformCurrentLoop();

  return true;
}

namespace {
struct LegacyLoopConditionalLICMPass : public LoopPass {
  static char ID; // Pass identification, replacement for typeid
  LegacyLoopConditionalLICMPass() : LoopPass(ID), LoopConditionalLICM() {
    initializeLegacyLoopConditionalLICMPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override {
    MemorySSA *MSSA = &getAnalysis<MemorySSAWrapperPass>().getMSSA();
    std::unique_ptr<MemorySSAUpdater> MSSAU =
        std::make_unique<MemorySSAUpdater>(MSSA);
    auto *SE = getAnalysisIfAvailable<ScalarEvolutionWrapperPass>();
    auto *LAA = &getAnalysis<LoopAccessLegacyAnalysis>();
    std::function<const LoopAccessInfo &(Loop &)> GetLAA =
        [&](Loop &L) -> const LoopAccessInfo & { return LAA->getInfo(&L); };

    return LoopConditionalLICM.runOnLoop(
        L, &getAnalysis<AAResultsWrapperPass>().getAAResults(),
        &getAnalysis<DominatorTreeWrapperPass>().getDomTree(),
        &getAnalysis<LoopInfoWrapperPass>().getLoopInfo(), nullptr, &LPM,
        MSSAU.get(), SE ? &SE->getSE() : nullptr,
        &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(
            *L->getHeader()->getParent()),
        GetLAA);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addRequired<LoopAccessLegacyAnalysis>();
    AU.addRequired<MemorySSAWrapperPass>();
    AU.addPreserved<MemorySSAWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    getLoopAnalysisUsage(AU);
  }

private:
  LoopConditionalLICMPass LoopConditionalLICM;
};
} // namespace

char LegacyLoopConditionalLICMPass::ID = 0;
INITIALIZE_PASS_BEGIN(LegacyLoopConditionalLICMPass, "loop-conditional-licm",
                      "Loop Conditional LICM", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopAccessLegacyAnalysis)
INITIALIZE_PASS_DEPENDENCY(LoopPass)
INITIALIZE_PASS_DEPENDENCY(MemorySSAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(LegacyLoopConditionalLICMPass, "loop-conditional-licm",
                    "Loop Conditional LICM", false, false)

Pass *llvm::createLoopConditionalLICMPass() {
  return new LegacyLoopConditionalLICMPass();
}
