/*
 * Copyright 2020 OmniSci, Inc.
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

// TODO: remove
#include <level_zero/ze_api.h>
#include <stdexcept>
#include <string>

#include <iostream>

namespace L0Mgr_Namespace {

using L0result = int;

std::string errorMessage(L0result const);

// Enable me
// class L0Exception : public std::runtime_error {
//  public:
//   L0Exception(L0result status);
//   L0result getStatus() const { return status_; }

//  private:
//   L0result const status_;
// };

// TODO: move to L0 helpers
#define L0_SAFE_CALL(call)                                                           \
  {                                                                                  \
    auto status = (call);                                                            \
    if (!status) {                                                                   \
      std::cerr << "L0 error: " << (int)status << " " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                        \
      exit(status);                                                                  \
    }                                                                                \
  }

class L0Mgr {
 public:
  L0Mgr();
  ~L0Mgr();

  void copyHostToDevice(int8_t* device_ptr,
                        const int8_t* host_ptr,
                        const size_t num_bytes,
                        const int device_num);
  void copyDeviceToHost(int8_t* host_ptr,
                        const int8_t* device_ptr,
                        const size_t num_bytes,
                        const int device_num);
  void copyDeviceToDevice(int8_t* dest_ptr,
                          int8_t* src_ptr,
                          const size_t num_bytes,
                          const int dest_device_num,
                          const int src_device_num);

  int8_t* allocateDeviceMem(const size_t num_bytes, const int device_num);


// in-mem spv, should return module ref? ignore for now
  void createModule(unsigned char* code, size_t size_bytes);
  void launch(); // FIXME: rename
  void commit(); // for now run only on commit

 private:
  void initModule(unsigned char* code, size_t size_bytes);
  void initKernel(std::string name = "SomeKernelName");
 // TODO: multiple devices
 // omnisci::DeviceGroup ?
  ze_device_handle_t hDevice = nullptr;
  ze_module_handle_t hModule = nullptr;
  ze_driver_handle_t hDriver = nullptr;
  ze_command_queue_handle_t hCommandQueue = nullptr;
  ze_context_handle_t hContext = nullptr;

  ze_command_list_handle_t hCommandList;
  ze_kernel_handle_t hKernel;
  ze_module_desc_t moduleDesc;
  ze_kernel_desc_t kernelDesc;
};

class Kernel {};
// TODO
namespace detail {
class Device {};
class Event {};
class Context {};
class CommandQueue {};
}  // namespace detail

}  // namespace L0Mgr_Namespace
