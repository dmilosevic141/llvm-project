//===-- LoopDuplicationPass.cpp - Loop Duplication Pass -------------------===//
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

#include "llvm/Transforms/LoopDuplication/LoopDuplicationPass.h"
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

/// Split all of the edges from inside the loop to their exit blocks.
/// Update the appropriate PHINodes.
void LoopDuplicationPass::splitExitEdges(
    const SmallVectorImpl<BasicBlock *> &ExitBlocks) {
  for (unsigned I = 0, E = ExitBlocks.size(); I != E; ++I) {
    BasicBlock *ExitBlock = ExitBlocks[I];
    SmallVector<BasicBlock *, 4> Preds(predecessors(ExitBlock));

    // Although SplitBlockPredecessors doesn't preserve loop-simplify in
    // general, calling it on all predecessors of all exits does.
    SplitBlockPredecessors(ExitBlock, Preds, ".ld-lcssa", DT, LI, MSSAU,
                           /* PreserveLCSSA */ true);
  }
}

/// Duplicate the current loop, and 'prepare' the original preheader for the
/// conditional branch to be inserted.
void LoopDuplicationPass::transformCurrentLoop() {
  // LoopBBs will contain all basic blocks of the "new" loop -
  // the new preheader, split exit edges, and potentially updated exit
  // blocks.
  std::vector<BasicBlock *> LoopBBs;

  BasicBlock *NewPreheader = SplitEdge(Preheader, Header, DT, LI, MSSAU);
  // The "new" loop will start with a new basic block, which comes after the
  // original preheader.
  LoopBBs.push_back(NewPreheader);

  // Add all loop basic blocks (excluding the preheader and the exit
  // blocks) to LoopBBs.
  llvm::append_range(LoopBBs, CurrentLoop->blocks());

  SmallVector<BasicBlock *, 8> ExitBlocks;
  CurrentLoop->getUniqueExitBlocks(ExitBlocks);

  splitExitEdges(ExitBlocks);

  // The exit blocks may have been changed due to edge splitting, recompute.
  ExitBlocks.clear();
  CurrentLoop->getUniqueExitBlocks(ExitBlocks);
  // Add exit blocks to LoopBBs.
  llvm::append_range(LoopBBs, ExitBlocks);

  // NewBBs will contain all basic blocks of the duplicated loop.
  std::vector<BasicBlock *> NewBBs;
  NewBBs.reserve(LoopBBs.size());
  ValueToValueMapTy VMap;
  Function *F = Preheader->getParent();
  for (unsigned I = 0, E = LoopBBs.size(); I != E; ++I) {
    BasicBlock *NewBB = CloneBasicBlock(LoopBBs[I], VMap, ".ld.clone", F);
    NewBBs.push_back(NewBB);
    VMap[LoopBBs[I]] = NewBB; // Keep the BB mapping.
  }

  // Splice the newly inserted blocks into the function right before the
  // original preheader.
  F->getBasicBlockList().splice(NewPreheader->getIterator(),
                                F->getBasicBlockList(),
                                NewBBs[0]->getIterator(), F->end());

  // Create the new Loop object for the duplicated loop.
  Loop *ParentLoop = CurrentLoop->getParentLoop();
  DuplicatedLoop = cloneLoop(CurrentLoop, ParentLoop, VMap, LI, LPPM);
  // Since cloneLoop requires LPPassManager which may not be provided (if the
  // new pass manager is being used), add the duplicated loop to the loop pass
  // manager manually, using LPMUpdater.
  if (LPMU)
    LPMU->addSiblingLoops({DuplicatedLoop});

  // Add the duplicated preheader to the parent loop
  // as well.
  if (ParentLoop)
    ParentLoop->addBasicBlockToLoop(NewBBs[0], *LI);

  for (unsigned EBI = 0, EBE = ExitBlocks.size(); EBI != EBE; ++EBI) {
    BasicBlock *NewExit = cast<BasicBlock>(VMap[ExitBlocks[EBI]]);
    // The new exit block should be in the same loop as the old one.
    if (Loop *ExitBBLoop = LI->getLoopFor(ExitBlocks[EBI]))
      ExitBBLoop->addBasicBlockToLoop(NewExit, *LI);

    assert(NewExit->getTerminator()->getNumSuccessors() == 1 &&
           "Exit block should have been split to have one successor!");
    BasicBlock *ExitSucc = NewExit->getTerminator()->getSuccessor(0);

    // If the successor of the exit block had PHI nodes, add an entry for
    // NewExit.
    for (PHINode &PN : ExitSucc->phis()) {
      Value *V = PN.getIncomingValueForBlock(ExitBlocks[EBI]);
      ValueToValueMapTy::iterator It = VMap.find(V);
      if (It != VMap.end())
        V = It->second;
      PN.addIncoming(V, NewExit);
    }

    if (LandingPadInst *LPad = NewExit->getLandingPadInst()) {
      PHINode *PN = PHINode::Create(LPad->getType(), 0, "",
                                    &*ExitSucc->getFirstInsertionPt());

      for (BasicBlock *BB : predecessors(ExitSucc)) {
        LandingPadInst *LPI = BB->getLandingPadInst();
        LPI->replaceAllUsesWith(PN);
        PN->addIncoming(LPI, BB);
      }
    }
  }

  // Rewrite the code to refer to itself.
  for (unsigned NBI = 0, NBE = NewBBs.size(); NBI != NBE; ++NBI)
    for (Instruction &I : *NewBBs[NBI])
      RemapInstruction(&I, VMap,
                       RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);

  // Rewrite the original preheader to select between versions of the loop.
  BranchInst *OldBr = cast<BranchInst>(Preheader->getTerminator());
  assert(OldBr->isUnconditional() && OldBr->getSuccessor(0) == LoopBBs[0] &&
         "Preheader splitting did not work correctly!");

  // Update MemorySSA after cloning, and before splitting to unreachables,
  // since that invalidates the 1:1 mapping of clones in VMap.
  if (MSSAU) {
    LoopBlocksRPO LBRPO(CurrentLoop);
    LBRPO.perform(LI);
    MSSAU->updateForClonedLoop(LBRPO, ExitBlocks, VMap);
  }

  // Emit the new branch that selects between the two versions of this loop.
  emitPreheaderBranch(LoopBBs[0], NewBBs[0], OldBr);

  // Clients need to override this method, in order to update the duplicated
  // loop accordingly.
  optimizeDuplicatedLoop();

  // Update MemoryPhis in Exit blocks.
  if (MSSAU)
    MSSAU->updateExitBlocksForClonedLoop(ExitBlocks, VMap, *DT);

  // Mark the two loops, and all of their inner loops as processed.
  SmallVector<Loop *, 4> Loops = CurrentLoop->getLoopsInPreorder();
  for (Loop *Lp : Loops)
    ProcessedLoops.insert(Lp);
  Loops = DuplicatedLoop->getLoopsInPreorder();
  for (Loop *Lp : Loops)
    ProcessedLoops.insert(Lp);

  if (VerifyMemorySSA && MSSAU)
    MSSAU->getMemorySSA()->verifyMemorySSA();
}
