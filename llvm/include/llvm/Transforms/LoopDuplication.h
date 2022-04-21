//===-- LoopDuplication.h - Loop Duplication Transformations ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header file defines prototypes for accessor functions that expose passes
// in the LoopDuplication transformations library.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LOOPDUPLICATION_H
#define LLVM_TRANSFORMS_LOOPDUPLICATION_H

#include "llvm/Transforms/Utils/SimplifyCFGOptions.h"
#include <functional>

namespace llvm {

class Pass;

//===----------------------------------------------------------------------===//
//
// LASBC - This pass adds array subscript bound checking to allow additional
// loop optimizations.
//
Pass *createLASBCPass();

//===----------------------------------------------------------------------===//
//
// Loop Conditional LICM - ...
//
Pass *createLoopConditionalLICMPass();
} // End llvm namespace

#endif
