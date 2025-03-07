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

#include <memory>
#include <vector>

#include "QueryEngine/JoinHashTable/HashTable.h"

class PerfectHashTable : public HashTable {
 public:
  // CPU + GPU constructor
  PerfectHashTable(Data_Namespace::DataMgr* data_mgr,
                   const HashType layout,
                   const ExecutorDeviceType device_type,
                   const size_t entry_count,
                   const size_t emitted_keys_count)
      : data_mgr_(data_mgr)
      , layout_(layout)
      , entry_count_(entry_count)
      , emitted_keys_count_(emitted_keys_count) {
    if (device_type == ExecutorDeviceType::CPU) {
      cpu_hash_table_buff_.resize(layout_ == HashType::OneToOne
                                      ? entry_count_
                                      : 2 * entry_count_ + emitted_keys_count_);
    }
  }

  ~PerfectHashTable() {
    if (gpu_hash_table_buff_) {
      CHECK(data_mgr_);
      data_mgr_->free(gpu_hash_table_buff_);
    }
  }

  size_t gpuReservedSize() const {
    CHECK(gpu_hash_table_buff_);
    return gpu_hash_table_buff_->reservedSize();
  }

  void allocateGpuMemory(const size_t entries, const int device_id) {
    CHECK_GE(device_id, 0);
    CHECK(!gpu_hash_table_buff_);
    gpu_hash_table_buff_ = CudaAllocator::allocGpuAbstractBuffer(
        data_mgr_, entries * sizeof(int32_t), device_id);
  }

  size_t getHashTableBufferSize(const ExecutorDeviceType device_type) const override {
    if (device_type == ExecutorDeviceType::CPU) {
      return cpu_hash_table_buff_.size() *
             sizeof(decltype(cpu_hash_table_buff_)::value_type);
    } else {
      return gpu_hash_table_buff_ ? gpu_hash_table_buff_->reservedSize() : 0;
    }
  }

  HashType getLayout() const override { return layout_; }

  int8_t* getCpuBuffer() override {
    return reinterpret_cast<int8_t*>(cpu_hash_table_buff_.data());
  }

  int8_t* getGpuBuffer() const override {
    return gpu_hash_table_buff_ ? gpu_hash_table_buff_->getMemoryPtr() : nullptr;
  }

  size_t getEntryCount() const override { return entry_count_; }

  size_t getEmittedKeysCount() const override { return emitted_keys_count_; }

 private:
  Data_Namespace::AbstractBuffer* gpu_hash_table_buff_{nullptr};
  Data_Namespace::DataMgr* data_mgr_;
  std::vector<int32_t> cpu_hash_table_buff_;

  HashType layout_;
  size_t entry_count_;         // number of keys in the hash table
  size_t emitted_keys_count_;  // number of keys emitted across all rows
};
