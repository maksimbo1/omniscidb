#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

struct LegacyCleanupIntrinsicsPass: public llvm::BasicBlockPass
{
  static char ID;
  LegacyCleanupIntrinsicsPass() : BasicBlockPass(ID) {}
  bool runOnBasicBlock(llvm::BasicBlock &BB) override;
};

namespace llvm {
  BasicBlockPass* createLegacyCleanupIntrinsicsPass();
}
