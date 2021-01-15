#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

struct LegacyCleanupPass: public llvm::BasicBlockPass
{
  static char ID;
  LegacyCleanupPass() : BasicBlockPass(ID) {}
  bool runOnBasicBlock(llvm::BasicBlock &BB) override;
};

