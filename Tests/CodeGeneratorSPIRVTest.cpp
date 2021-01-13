/*
 * Copyright 2021 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>

#include "Analyzer/Analyzer.h"
#include "QueryEngine/CodeGenerator.h"
#include "QueryEngine/Execute.h"
#include "QueryEngine/IRCodegenUtils.h"
#include "QueryEngine/LLVMGlobalContext.h"
#include "TestHelpers.h"


TEST(CodeGeneratorSPVTest, IntegerConstantCopy) {
  auto& ctx = getGlobalLLVMContext();
  // FIXME: requires llvm.ctlz.i64 intrinsic supported for llvm9
  // see https://github.com/KhronosGroup/SPIRV-LLVM-Translator/pull/872
  // std::unique_ptr<llvm::Module> module(read_template_module(ctx));
  std::unique_ptr<llvm::Module> module = std::make_unique<llvm::Module>("TestSPVModule", ctx);
  ScalarCodeGenerator code_generator(std::move(module));
  CompilationOptions co = CompilationOptions::defaults(ExecutorDeviceType::XPU);
  co.hoist_literals = false;

  Datum d;
  d.intval = 42;
  auto constant = makeExpr<Analyzer::Constant>(kINT, false, d);
  const auto compiled_expr = code_generator.compile(constant.get(), true, co);

  auto binary = code_generator.generateSPV();

  // for (size_t id = 0; id < func_ptrs.size(); ++id) {
    TestHelpers::AlignedArray<int, 32> arr;
    TestHelpers::AlignedArray<int, 32> out;

    arr.data[0] = d.intval;

    auto in = code_generator.getL0Mgr()->allocateDeviceMem(32, 0/*unused*/);
    code_generator.getL0Mgr()->copyHostToDevice((int8_t*)in, (int8_t*)arr.data, 32, 0/*unused*/);
    code_generator.getL0Mgr()->createModule((unsigned char*)binary.data(), 0/*FIXME*/);
    code_generator.getL0Mgr()->launch();
    code_generator.getL0Mgr()->copyDeviceToHost((int8_t*)out.data, (int8_t*)in, 32, 0/*unused*/);
    code_generator.getL0Mgr()->commit();

    ASSERT_EQ(out[0], d.intval);
  // }
}

int main(int argc, char** argv) {
  TestHelpers::init_logger_stderr_only(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  int err = RUN_ALL_TESTS();
  return err;
}