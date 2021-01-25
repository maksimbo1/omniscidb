#include "QueryEngine/Helpers/CleanupPass.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/Constants.h"

#include <string>

using namespace llvm;

static bool isUnsupported(llvm::IntrinsicInst* Inst) {
  return Inst->getIntrinsicID() == Intrinsic::ID::ctlz ||
    Inst->getIntrinsicID() == Intrinsic::ID::floor;
}

char LegacyCleanupIntrinsicsPass::ID = 0;

bool LegacyCleanupIntrinsicsPass::runOnBasicBlock(llvm::BasicBlock& BB) {
  bool changed = false;
  for (auto BI = BB.begin(); BI != BB.end(); ++BI) {
    auto* intrin = dyn_cast<IntrinsicInst>(BI);
    if (!intrin)
      continue;

    if (isUnsupported(intrin)) {
        Value* V = ConstantInt::get(Type::getInt32Ty(BB.getContext()), 0);
        Instruction* Nop = BinaryOperator::CreateAdd(V, V, "nop");
        ReplaceInstWithInst(BB.getInstList(), BI, Nop);
    }
    BB.print(errs());
  }

  return changed;
}

namespace llvm {
  llvm::BasicBlockPass* createLegacyCleanupIntrinsicsPass() {
    return new LegacyCleanupIntrinsicsPass();
  }
}