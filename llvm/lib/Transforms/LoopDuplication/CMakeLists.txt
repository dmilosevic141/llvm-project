add_llvm_component_library(LLVMLoopDuplicationOpts
  LoopDuplicationPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/LoopDuplication

  DEPENDS
  intrinsics_gen

  COMPONENT_NAME
  LoopDuplication

  LINK_COMPONENTS
  AggressiveInstCombine
  Analysis
  Core
  InstCombine
  Support
  TransformUtils
  )
