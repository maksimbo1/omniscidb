#pragma once
#include <iostream>
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

struct RemoveAddrSpacesPass : llvm::ModulePass {
  static char ID;
  RemoveAddrSpacesPass() : llvm::ModulePass(ID) {}

  static bool isPointerWithNonZeroAddrSpace(llvm::Type* type) {
    llvm::PointerType* ptr_type = llvm::dyn_cast<llvm::PointerType>(type);
    if (!ptr_type) {
      return false;
    }
    return ptr_type->getAddressSpace() != 0;
  }

  static bool requireConversion(llvm::FunctionType* sig) {
    if (isPointerWithNonZeroAddrSpace(sig->getReturnType())) {
      return true;
    }
    if (llvm::any_of(sig->params(), isPointerWithNonZeroAddrSpace)) {
      return true;
    }
    return false;
  }

  static llvm::Type* removeAddrSpace(llvm::Type* type) {
    llvm::PointerType* ptr_type = llvm::dyn_cast<llvm::PointerType>(type);
    if (!ptr_type) {
      return type;
    }
    return llvm::PointerType::get(removeAddrSpace(ptr_type->getElementType()), 0);
  }

  static llvm::FunctionType* convertFunctionSignature(llvm::FunctionType* old_sig) {
    llvm::SmallVector<llvm::Type*, 16> params(
        llvm::map_range(old_sig->params(), removeAddrSpace));
    llvm::FunctionType* res =
        llvm::FunctionType::get(removeAddrSpace(old_sig->getReturnType()), params, false);
    return res;
  }

  void convertInstruction(llvm::Instruction* inst,
                          std::vector<llvm::Instruction*>& erase_list) {
    if (auto asc = llvm::dyn_cast<llvm::AddrSpaceCastInst>(inst)) {
      asc->replaceAllUsesWith(asc->getPointerOperand());
      erase_list.push_back(asc);
      return;
    }
    if (auto alloc = llvm::dyn_cast<llvm::AllocaInst>(inst)) {
      alloc->setAllocatedType(removeAddrSpace(alloc->getAllocatedType()));
      return;
    }
    if (auto gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
      gep->setSourceElementType(removeAddrSpace(gep->getSourceElementType()));
      gep->setResultElementType(removeAddrSpace(gep->getResultElementType()));
      return;
    }
    if (auto call = llvm::dyn_cast<llvm::CallInst>(inst)) {
      call->mutateFunctionType(convertFunctionSignature(call->getFunctionType()));
    }

    inst->mutateType(removeAddrSpace(inst->getType()));
    for (int i = 0; i < inst->getNumOperands(); i++) {
      auto op = inst->getOperand(i);
      op->mutateType(removeAddrSpace(op->getType()));
    }
  }

  bool runOnModule(llvm::Module& module) override {
    bool converted = false;
    std::vector<llvm::Function*> dead_funcs;
    for (auto& f : module) {
      if (!requireConversion(f.getFunctionType())) {
        continue;
      }
      converted = true;
      dead_funcs.emplace_back(&f);
    }

    for (auto& f : dead_funcs) {
      f->removeFromParent();  // TODO(L0): Maybe erase

      auto nf = llvm::Function::Create(convertFunctionSignature(f->getFunctionType()),
                                       f->getLinkage(),
                                       f->getAddressSpace(),
                                       f->getName(),
                                       &module);

      for (int i = 0; i < f->arg_size(); i++) {
        f->getArg(i)->replaceAllUsesWith(nf->getArg(i));
      }

      nf->getBasicBlockList().splice(nf->begin(), f->getBasicBlockList());
      f->replaceAllUsesWith(nf);
    }

    std::vector<llvm::Instruction*> dead_instructions;
    for (auto& f : module) {
      for (llvm::BasicBlock& b : f.getBasicBlockList()) {
        for (llvm::Instruction& i : b) {
          convertInstruction(&i, dead_instructions);
        }
      }
    }

    for (auto i : dead_instructions) {
      i->eraseFromParent();
    }

    return converted;
  }
};

char RemoveAddrSpacesPass::ID = 0;

struct CallInstVisitor : llvm::InstVisitor<CallInstVisitor> {
  void visitCallInst(llvm::CallInst& inst) { visitCallBase(inst); }
  void visitCallBase(llvm::CallBase& call) {
    llvm::PointerType* fpty =
        llvm::cast<llvm::PointerType>(call.getCalledOperand()->getType());

    if (fpty->getElementType() != call.getFunctionType()) {
      llvm::outs() << "call type: \n" << call.getFunctionType() << "\n";
      call.getFunctionType()->print(llvm::outs());
      llvm::outs() << "\n";

      llvm::outs() << "func type: " << fpty->getElementType() << "\n";
      fpty->getElementType()->print(llvm::outs());
      llvm::outs() << "\n";
    }
  }
};

struct CallVerifierPass : llvm::FunctionPass {
  static char ID;
  CallVerifierPass() : llvm::FunctionPass(ID) {}

  bool runOnFunction(llvm::Function& f) override {
    CallInstVisitor v;
    v.visit(f);
    return false;
  }
};

char CallVerifierPass::ID = 1;

struct SortedPrintModulePass : llvm::ModulePass {
  static char ID;
  SortedPrintModulePass() : llvm::ModulePass(ID) {}

  bool runOnModule(llvm::Module& m) override {
    std::vector<llvm::Function*> functions;

    for (auto& f : m) {
      functions.push_back(&f);
    }

    std::sort(functions.begin(), functions.end(), [](auto lhs, auto rhs) {
      return lhs->getName() < rhs->getName();
    });

    for (auto f : functions) {
      f->print(llvm::outs());
    }
    return false;
  }
};

char SortedPrintModulePass::ID = 2;

llvm::Pass* createRemoveAddrSpacesPass() {
  return new RemoveAddrSpacesPass();
};

llvm::Pass* createCallVerifierPass() {
  return new CallVerifierPass();
}

llvm::Pass* createSortedPrintModulePass() {
  return new SortedPrintModulePass();
}