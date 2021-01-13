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
#pragma once

#include "QueryEngine/CompilationContext.h"
#include <cstring>

// namespace xpu?

class XpuCompilationContext : CompilationContext {
 public:
  XpuCompilationContext(const void* module, const size_t sz): _module(nullptr), _sz(0u) {
    _module = static_cast<void*>(new char[sz]);
    _sz = sz;
    std::memcpy(_module, module, _sz);
  }
  ~XpuCompilationContext() {
    delete [] static_cast<char*>(_module);
  }

  void* getModule() const { return _module; }

 private:
  void* _module;
  size_t _sz;
};