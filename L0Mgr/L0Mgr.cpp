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

#include "L0Mgr/L0Mgr.h"

#include "Logger/Logger.h"

#include <level_zero/ze_api.h>

#include <limits>

namespace L0Mgr_Namespace {

// TODO: move me
static void L0InitGPUContext(ze_driver_handle_t& hDriver,
                             ze_device_handle_t& hDevice,
                             ze_command_queue_handle_t& hCommandQueue,
                             ze_context_handle_t& hContext) {
  L0_SAFE_CALL(zeInit(0));

  // Discover all the driver instances
  uint32_t driverCount = 0;
  L0_SAFE_CALL(zeDriverGet(&driverCount, nullptr));

  ze_driver_handle_t* allDrivers =
      (ze_driver_handle_t*)malloc(driverCount * sizeof(ze_driver_handle_t));
  L0_SAFE_CALL(zeDriverGet(&driverCount, allDrivers));

  // Find a driver instance with a GPU device
  for (uint32_t i = 0; i < driverCount; ++i) {
    uint32_t deviceCount = 0;
    hDriver = allDrivers[i];
    L0_SAFE_CALL(zeDeviceGet(hDriver, &deviceCount, nullptr));
    ze_device_handle_t* allDevices =
        (ze_device_handle_t*)malloc(deviceCount * sizeof(ze_device_handle_t));
    L0_SAFE_CALL(zeDeviceGet(hDriver, &deviceCount, allDevices));
    for (uint32_t d = 0; d < deviceCount; ++d) {
      ze_device_properties_t device_properties;
      L0_SAFE_CALL(zeDeviceGetProperties(allDevices[d], &device_properties));
      if (ZE_DEVICE_TYPE_GPU == device_properties.type) {
        hDevice = allDevices[d];
        break;
      }
    }
    free(allDevices);
    if (nullptr != hDevice) {
      break;
    }
  }
  free(allDrivers);
  assert(hDriver);
  assert(hDevice);

  ze_context_desc_t ctxtDesc = {ZE_STRUCTURE_TYPE_CONTEXT_DESC, nullptr, 0};
  zeContextCreate(hDriver, &ctxtDesc, &hContext);

  ze_command_queue_desc_t commandQueueDesc = {ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC,
                                              nullptr,
                                              0,
                                              0,
                                              0,
                                              ZE_COMMAND_QUEUE_MODE_DEFAULT,
                                              ZE_COMMAND_QUEUE_PRIORITY_NORMAL};
  L0_SAFE_CALL(
      zeCommandQueueCreate(hContext, hDevice, &commandQueueDesc, &hCommandQueue));
}

L0Mgr::L0Mgr() {
  L0InitGPUContext(hDriver, hDevice, hCommandQueue, hContext);
  std::cout << "Initialized context" << std::endl;
}
L0Mgr::~L0Mgr() {}

void L0Mgr::copyHostToDevice(int8_t* device_ptr,
                             const int8_t* host_ptr,
                             const size_t num_bytes,
                             const int device_num) {
  L0_SAFE_CALL(zeCommandListAppendMemoryCopy(
      hCommandList, device_ptr, host_ptr, num_bytes, nullptr, 0, nullptr));
  L0_SAFE_CALL(zeCommandListAppendBarrier(hCommandList, nullptr, 0, nullptr));
}

void L0Mgr::copyDeviceToHost(int8_t* host_ptr,
                             const int8_t* device_ptr,
                             const size_t num_bytes,
                             const int device_num) {
  L0_SAFE_CALL(zeCommandListAppendMemoryCopy(
      hCommandList, host_ptr, device_ptr, num_bytes, nullptr, 0, nullptr));
  L0_SAFE_CALL(zeCommandListAppendBarrier(hCommandList, nullptr, 0, nullptr));
}

void L0Mgr::copyDeviceToDevice(int8_t* dest_ptr,
                               int8_t* src_ptr,
                               const size_t num_bytes,
                               const int dest_device_num,
                               const int src_device_num) {
  CHECK(false);
}


int8_t* L0Mgr::allocateDeviceMem(const size_t num_bytes, const int device_num) {
  ze_device_mem_alloc_desc_t alloc_desc;
  alloc_desc.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
  alloc_desc.pNext = nullptr;
  alloc_desc.flags = 0;
  alloc_desc.ordinal = 0;

  void *mem;
  L0_SAFE_CALL(zeMemAllocDevice(hContext, &alloc_desc, num_bytes, 0/*align*/, hDevice, &mem));
  // FIXME
  L0_SAFE_CALL(zeKernelSetArgumentValue(hKernel, 0, sizeof(void*), &mem));
  return (int8_t*)mem;
}

void L0Mgr::createModule(unsigned char* code, size_t size_bytes) {
  initModule(code, size_bytes);
}

void L0Mgr::initModule(unsigned char* code, size_t size_bytes) {
  moduleDesc.stype = ZE_STRUCTURE_TYPE_MODULE_DESC;
  moduleDesc.pNext = nullptr;
  moduleDesc.pBuildFlags = ""; // ?
  moduleDesc.format = ZE_MODULE_FORMAT_IL_SPIRV;
  moduleDesc.inputSize = size_bytes;
  moduleDesc.pConstants = nullptr;
  moduleDesc.pInputModule = code;
  L0_SAFE_CALL(zeModuleCreate(hContext, hDevice, &moduleDesc, &hModule, nullptr));
}

void L0Mgr::launch(/*todo: params?*/) {
  L0_SAFE_CALL(zeKernelSetGroupSize(hKernel, 1, 1, 1));
  ze_group_count_t dispatchTraits = {1, 1, 1};
  L0_SAFE_CALL(zeCommandListAppendLaunchKernel(
      hCommandList, hKernel, &dispatchTraits, nullptr, 0, nullptr));
  L0_SAFE_CALL(zeCommandListAppendBarrier(hCommandList, nullptr, 0, nullptr));
}

void L0Mgr::initKernel(std::string name) {
  kernelDesc.stype = ZE_STRUCTURE_TYPE_KERNEL_DESC;
  kernelDesc.pNext = nullptr;
  kernelDesc.flags = 0;
  kernelDesc.pKernelName = name.c_str();

  L0_SAFE_CALL(zeKernelCreate(hModule, &kernelDesc, &hKernel));
}

void L0Mgr::commit() {
  L0_SAFE_CALL(zeCommandListClose(hCommandList));
  L0_SAFE_CALL(
      zeCommandQueueExecuteCommandLists(hCommandQueue, 1, &hCommandList, nullptr));
  L0_SAFE_CALL(
      zeCommandQueueSynchronize(hCommandQueue, std::numeric_limits<uint32_t>::max()));
}

}  // namespace L0Mgr_Namespace