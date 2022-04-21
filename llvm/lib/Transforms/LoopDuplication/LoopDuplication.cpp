//===-- LoopDuplication.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements common infrastructure for libLLVMLoopDuplicationOpts.a,
// which implements several loop duplication transformations over the LLVM
// intermediate representation, including the C bindings for that library.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/LoopDuplication.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

/// initializeLoopDuplicationOpts - Initialize all passes linked into the
/// LoopDuplicationOpts library.
void llvm::initializeLoopDuplicationOpts(PassRegistry &Registry) {
  initializeLegacyLASBCPassPass(Registry);
  initializeLegacyLoopConditionalLICMPassPass(Registry);
}
