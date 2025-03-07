/*
 * Copyright 2018 MapD Technologies, Inc.
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

#include "QueryExecutionContext.h"
#include "AggregateUtils.h"
#include "Descriptors/QueryMemoryDescriptor.h"
#include "Execute.h"
#include "GpuInitGroups.h"
#include "InPlaceSort.h"
#include "QueryMemoryInitializer.h"
#include "RelAlgExecutionUnit.h"
#include "ResultSet.h"
#include "Shared/likely.h"
#include "SpeculativeTopN.h"
#include "StreamingTopN.h"

QueryExecutionContext::QueryExecutionContext(
    const RelAlgExecutionUnit& ra_exe_unit,
    const QueryMemoryDescriptor& query_mem_desc,
    const Executor* executor,
    const ExecutorDeviceType device_type,
    const ExecutorDispatchMode dispatch_mode,
    const int device_id,
    const int64_t num_rows,
    const std::vector<std::vector<const int8_t*>>& col_buffers,
    const std::vector<std::vector<uint64_t>>& frag_offsets,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
    const bool output_columnar,
    const bool sort_on_gpu,
    const size_t thread_idx,
    RenderInfo* render_info)
    : query_mem_desc_(query_mem_desc)
    , executor_(executor)
    , device_type_(device_type)
    , dispatch_mode_(dispatch_mode)
    , row_set_mem_owner_(row_set_mem_owner)
    , output_columnar_(output_columnar) {
  CHECK(executor);
  auto data_mgr = executor->getDataMgr();
  if (device_type == ExecutorDeviceType::GPU) {
    gpu_allocator_ = std::make_unique<CudaAllocator>(data_mgr, device_id);
  }

  auto render_allocator_map = render_info && render_info->isPotentialInSituRender()
                                  ? render_info->render_allocator_map_ptr.get()
                                  : nullptr;
  query_buffers_ = std::make_unique<QueryMemoryInitializer>(ra_exe_unit,
                                                            query_mem_desc,
                                                            device_id,
                                                            device_type,
                                                            dispatch_mode,
                                                            output_columnar,
                                                            sort_on_gpu,
                                                            num_rows,
                                                            col_buffers,
                                                            frag_offsets,
                                                            render_allocator_map,
                                                            render_info,
                                                            row_set_mem_owner,
                                                            gpu_allocator_.get(),
                                                            thread_idx,
                                                            executor);
}

ResultSetPtr QueryExecutionContext::groupBufferToDeinterleavedResults(
    const size_t i) const {
  CHECK(!output_columnar_);
  const auto& result_set = query_buffers_->getResultSet(i);
  auto deinterleaved_query_mem_desc =
      ResultSet::fixupQueryMemoryDescriptor(query_mem_desc_);
  deinterleaved_query_mem_desc.setHasInterleavedBinsOnGpu(false);
  deinterleaved_query_mem_desc.useConsistentSlotWidthSize(8);

  auto deinterleaved_result_set =
      std::make_shared<ResultSet>(result_set->getTargetInfos(),
                                  std::vector<ColumnLazyFetchInfo>{},
                                  std::vector<std::vector<const int8_t*>>{},
                                  std::vector<std::vector<int64_t>>{},
                                  std::vector<int64_t>{},
                                  ExecutorDeviceType::CPU,
                                  -1,
                                  deinterleaved_query_mem_desc,
                                  row_set_mem_owner_,
                                  executor_->getCatalog(),
                                  executor_->blockSize(),
                                  executor_->gridSize());
  auto deinterleaved_storage =
      deinterleaved_result_set->allocateStorage(executor_->plan_state_->init_agg_vals_);
  auto deinterleaved_buffer =
      reinterpret_cast<int64_t*>(deinterleaved_storage->getUnderlyingBuffer());
  const auto rows_ptr = result_set->getStorage()->getUnderlyingBuffer();
  size_t deinterleaved_buffer_idx = 0;
  const size_t agg_col_count{query_mem_desc_.getSlotCount()};
  auto do_work = [&](const size_t bin_base_off) {
    std::vector<int64_t> agg_vals(agg_col_count, 0);
    memcpy(&agg_vals[0],
           &executor_->plan_state_->init_agg_vals_[0],
           agg_col_count * sizeof(agg_vals[0]));
    ResultSetStorage::reduceSingleRow(rows_ptr + bin_base_off,
                                      executor_->warpSize(),
                                      false,
                                      true,
                                      agg_vals,
                                      query_mem_desc_,
                                      result_set->getTargetInfos(),
                                      executor_->plan_state_->init_agg_vals_);
    for (size_t agg_idx = 0; agg_idx < agg_col_count;
         ++agg_idx, ++deinterleaved_buffer_idx) {
      deinterleaved_buffer[deinterleaved_buffer_idx] = agg_vals[agg_idx];
    }
  };
  if (g_enable_non_kernel_time_query_interrupt) {
    for (size_t bin_base_off = query_mem_desc_.getColOffInBytes(0), bin_idx = 0;
         bin_idx < result_set->entryCount();
         ++bin_idx, bin_base_off += query_mem_desc_.getColOffInBytesInNextBin(0)) {
      if (UNLIKELY((bin_idx & 0xFFFF) == 0 &&
                   executor_->checkNonKernelTimeInterrupted())) {
        throw std::runtime_error(
            "Query execution has interrupted during result set reduction");
      }
      do_work(bin_base_off);
    }
  } else {
    for (size_t bin_base_off = query_mem_desc_.getColOffInBytes(0), bin_idx = 0;
         bin_idx < result_set->entryCount();
         ++bin_idx, bin_base_off += query_mem_desc_.getColOffInBytesInNextBin(0)) {
      do_work(bin_base_off);
    }
  }
  query_buffers_->resetResultSet(i);
  return deinterleaved_result_set;
}

int64_t QueryExecutionContext::getAggInitValForIndex(const size_t index) const {
  CHECK(query_buffers_);
  return query_buffers_->getAggInitValForIndex(index);
}

ResultSetPtr QueryExecutionContext::getRowSet(
    const RelAlgExecutionUnit& ra_exe_unit,
    const QueryMemoryDescriptor& query_mem_desc) const {
  auto timer = DEBUG_TIMER(__func__);
  std::vector<std::pair<ResultSetPtr, std::vector<size_t>>> results_per_sm;
  CHECK(query_buffers_);
  const auto group_by_buffers_size = query_buffers_->getNumBuffers();
  if (device_type_ == ExecutorDeviceType::CPU) {
    const size_t expected_num_buffers = query_mem_desc.hasVarlenOutput() ? 2 : 1;
    CHECK_EQ(expected_num_buffers, group_by_buffers_size);
    return groupBufferToResults(0);
  }
  const size_t step{query_mem_desc_.threadsShareMemory() ? executor_->blockSize() : 1};
  const size_t group_by_output_buffers_size =
      group_by_buffers_size - (query_mem_desc.hasVarlenOutput() ? 1 : 0);
  for (size_t i = 0; i < group_by_output_buffers_size; i += step) {
    results_per_sm.emplace_back(groupBufferToResults(i), std::vector<size_t>{});
  }
  CHECK(device_type_ == ExecutorDeviceType::GPU);
  return executor_->reduceMultiDeviceResults(
      ra_exe_unit, results_per_sm, row_set_mem_owner_, query_mem_desc);
}

ResultSetPtr QueryExecutionContext::groupBufferToResults(const size_t i) const {
  if (query_mem_desc_.interleavedBins(device_type_)) {
    return groupBufferToDeinterleavedResults(i);
  }
  return query_buffers_->getResultSetOwned(i);
}

#ifdef HAVE_CUDA
namespace {

int32_t aggregate_error_codes(const std::vector<int32_t>& error_codes) {
  // Check overflow / division by zero / interrupt first
  for (const auto err : error_codes) {
    if (err > 0) {
      return err;
    }
  }
  for (const auto err : error_codes) {
    if (err) {
      return err;
    }
  }
  return 0;
}

}  // namespace
#endif

std::vector<int64_t*> QueryExecutionContext::launchGpuCode(
    const RelAlgExecutionUnit& ra_exe_unit,
    const GpuCompilationContext* cu_functions,
    const bool hoist_literals,
    const std::vector<int8_t>& literal_buff,
    std::vector<std::vector<const int8_t*>> col_buffers,
    const std::vector<std::vector<int64_t>>& num_rows,
    const std::vector<std::vector<uint64_t>>& frag_offsets,
    const int32_t scan_limit,
    Data_Namespace::DataMgr* data_mgr,
    const unsigned block_size_x,
    const unsigned grid_size_x,
    const int device_id,
    const size_t shared_memory_size,
    int32_t* error_code,
    const uint32_t num_tables,
    const bool allow_runtime_interrupt,
    const std::vector<int64_t>& join_hash_tables,
    RenderAllocatorMap* render_allocator_map) {
  auto timer = DEBUG_TIMER(__func__);
  INJECT_TIMER(lauchGpuCode);
#ifdef HAVE_CUDA
  CHECK(gpu_allocator_);
  CHECK(query_buffers_);
  const auto& init_agg_vals = query_buffers_->init_agg_vals_;

  bool is_group_by{query_mem_desc_.isGroupBy()};

  RenderAllocator* render_allocator = nullptr;
  if (render_allocator_map) {
    render_allocator = render_allocator_map->getRenderAllocator(device_id);
  }

  CHECK(cu_functions);
  const auto native_code = cu_functions->getNativeCode(device_id);
  auto cu_func = static_cast<CUfunction>(native_code.first);
  std::vector<int64_t*> out_vec;
  uint32_t num_fragments = col_buffers.size();
  std::vector<int32_t> error_codes(grid_size_x * block_size_x);

  CUevent start0, stop0;  // preparation
  cuEventCreate(&start0, 0);
  cuEventCreate(&stop0, 0);
  CUevent start1, stop1;  // cuLaunchKernel
  cuEventCreate(&start1, 0);
  cuEventCreate(&stop1, 0);
  CUevent start2, stop2;  // finish
  cuEventCreate(&start2, 0);
  cuEventCreate(&stop2, 0);

  if (g_enable_dynamic_watchdog || (allow_runtime_interrupt && !render_allocator)) {
    cuEventRecord(start0, 0);
  }

  if (g_enable_dynamic_watchdog) {
    initializeDynamicWatchdog(native_code.second, device_id);
  }

  if (allow_runtime_interrupt && !render_allocator) {
    initializeRuntimeInterrupter(native_code.second, device_id);
  }

  auto kernel_params = prepareKernelParams(col_buffers,
                                           literal_buff,
                                           num_rows,
                                           frag_offsets,
                                           scan_limit,
                                           init_agg_vals,
                                           error_codes,
                                           num_tables,
                                           join_hash_tables,
                                           data_mgr,
                                           device_id,
                                           hoist_literals,
                                           is_group_by);

  CHECK_EQ(static_cast<size_t>(KERN_PARAM_COUNT), kernel_params.size());
  CHECK_EQ(CUdeviceptr(0), kernel_params[GROUPBY_BUF]);

  const unsigned block_size_y = 1;
  const unsigned block_size_z = 1;
  const unsigned grid_size_y = 1;
  const unsigned grid_size_z = 1;
  const auto total_thread_count = block_size_x * grid_size_x;
  const auto err_desc = kernel_params[ERROR_CODE];

  if (is_group_by) {
    CHECK(!(query_buffers_->getGroupByBuffersSize() == 0) || render_allocator);
    bool can_sort_on_gpu = query_mem_desc_.sortOnGpu();
    auto gpu_group_by_buffers =
        query_buffers_->createAndInitializeGroupByBufferGpu(ra_exe_unit,
                                                            query_mem_desc_,
                                                            kernel_params[INIT_AGG_VALS],
                                                            device_id,
                                                            dispatch_mode_,
                                                            block_size_x,
                                                            grid_size_x,
                                                            executor_->warpSize(),
                                                            can_sort_on_gpu,
                                                            output_columnar_,
                                                            render_allocator);
    const auto max_matched = static_cast<int32_t>(gpu_group_by_buffers.entry_count);
    copy_to_gpu(data_mgr,
                kernel_params[MAX_MATCHED],
                &max_matched,
                sizeof(max_matched),
                device_id);

    kernel_params[GROUPBY_BUF] = gpu_group_by_buffers.ptrs;
    std::vector<void*> param_ptrs;
    for (auto& param : kernel_params) {
      param_ptrs.push_back(&param);
    }

    if (g_enable_dynamic_watchdog || (allow_runtime_interrupt && !render_allocator)) {
      cuEventRecord(stop0, 0);
      cuEventSynchronize(stop0);
      float milliseconds0 = 0;
      cuEventElapsedTime(&milliseconds0, start0, stop0);
      VLOG(1) << "Device " << std::to_string(device_id)
              << ": launchGpuCode: group-by prepare: " << std::to_string(milliseconds0)
              << " ms";
      cuEventRecord(start1, 0);
    }

    if (hoist_literals) {
      checkCudaErrors(cuLaunchKernel(cu_func,
                                     grid_size_x,
                                     grid_size_y,
                                     grid_size_z,
                                     block_size_x,
                                     block_size_y,
                                     block_size_z,
                                     shared_memory_size,
                                     nullptr,
                                     &param_ptrs[0],
                                     nullptr));
    } else {
      param_ptrs.erase(param_ptrs.begin() + LITERALS);  // TODO(alex): remove
      checkCudaErrors(cuLaunchKernel(cu_func,
                                     grid_size_x,
                                     grid_size_y,
                                     grid_size_z,
                                     block_size_x,
                                     block_size_y,
                                     block_size_z,
                                     shared_memory_size,
                                     nullptr,
                                     &param_ptrs[0],
                                     nullptr));
    }
    if (g_enable_dynamic_watchdog || (allow_runtime_interrupt && !render_allocator)) {
      executor_->registerActiveModule(native_code.second, device_id);
      cuEventRecord(stop1, 0);
      cuEventSynchronize(stop1);
      executor_->unregisterActiveModule(native_code.second, device_id);
      float milliseconds1 = 0;
      cuEventElapsedTime(&milliseconds1, start1, stop1);
      VLOG(1) << "Device " << std::to_string(device_id)
              << ": launchGpuCode: group-by cuLaunchKernel: "
              << std::to_string(milliseconds1) << " ms";
      cuEventRecord(start2, 0);
    }

    gpu_allocator_->copyFromDevice(reinterpret_cast<int8_t*>(error_codes.data()),
                                   reinterpret_cast<int8_t*>(err_desc),
                                   error_codes.size() * sizeof(error_codes[0]));
    *error_code = aggregate_error_codes(error_codes);
    if (*error_code > 0) {
      return {};
    }

    if (!render_allocator) {
      if (query_mem_desc_.useStreamingTopN()) {
        query_buffers_->applyStreamingTopNOffsetGpu(data_mgr,
                                                    query_mem_desc_,
                                                    gpu_group_by_buffers,
                                                    ra_exe_unit,
                                                    total_thread_count,
                                                    device_id);
      } else {
        if (use_speculative_top_n(ra_exe_unit, query_mem_desc_)) {
          try {
            inplace_sort_gpu(ra_exe_unit.sort_info.order_entries,
                             query_mem_desc_,
                             gpu_group_by_buffers,
                             data_mgr,
                             device_id);
          } catch (const std::bad_alloc&) {
            throw SpeculativeTopNFailed("Failed during in-place GPU sort.");
          }
        }
        if (query_mem_desc_.getQueryDescriptionType() ==
            QueryDescriptionType::Projection) {
          if (query_mem_desc_.didOutputColumnar()) {
            query_buffers_->compactProjectionBuffersGpu(
                query_mem_desc_,
                data_mgr,
                gpu_group_by_buffers,
                get_num_allocated_rows_from_gpu(
                    data_mgr, kernel_params[TOTAL_MATCHED], device_id),
                device_id);
          } else {
            size_t num_allocated_rows{0};
            if (ra_exe_unit.use_bump_allocator) {
              num_allocated_rows = get_num_allocated_rows_from_gpu(
                  data_mgr, kernel_params[TOTAL_MATCHED], device_id);
              // First, check the error code. If we ran out of slots, don't copy data back
              // into the ResultSet or update ResultSet entry count
              if (*error_code < 0) {
                return {};
              }
            }
            query_buffers_->copyGroupByBuffersFromGpu(
                data_mgr,
                query_mem_desc_,
                ra_exe_unit.use_bump_allocator ? num_allocated_rows
                                               : query_mem_desc_.getEntryCount(),
                gpu_group_by_buffers,
                &ra_exe_unit,
                block_size_x,
                grid_size_x,
                device_id,
                can_sort_on_gpu && query_mem_desc_.hasKeylessHash());
            if (num_allocated_rows) {
              CHECK(ra_exe_unit.use_bump_allocator);
              CHECK(!query_buffers_->result_sets_.empty());
              query_buffers_->result_sets_.front()->updateStorageEntryCount(
                  num_allocated_rows);
            }
          }
        } else {
          query_buffers_->copyGroupByBuffersFromGpu(
              data_mgr,
              query_mem_desc_,
              query_mem_desc_.getEntryCount(),
              gpu_group_by_buffers,
              &ra_exe_unit,
              block_size_x,
              grid_size_x,
              device_id,
              can_sort_on_gpu && query_mem_desc_.hasKeylessHash());
        }
      }
    }
  } else {
    std::vector<CUdeviceptr> out_vec_dev_buffers;
    const size_t agg_col_count{ra_exe_unit.estimator ? size_t(1) : init_agg_vals.size()};
    // by default, non-grouped aggregate queries generate one result per available thread
    // in the lifetime of (potentially multi-fragment) kernel execution.
    // We can reduce these intermediate results internally in the device and hence have
    // only one result per device, if GPU shared memory optimizations are enabled.
    const auto num_results_per_agg_col =
        shared_memory_size ? 1 : block_size_x * grid_size_x * num_fragments;
    const auto output_buffer_size_per_agg = num_results_per_agg_col * sizeof(int64_t);
    if (ra_exe_unit.estimator) {
      estimator_result_set_.reset(new ResultSet(
          ra_exe_unit.estimator, ExecutorDeviceType::GPU, device_id, data_mgr));
      out_vec_dev_buffers.push_back(reinterpret_cast<CUdeviceptr>(
          estimator_result_set_->getDeviceEstimatorBuffer()));
    } else {
      for (size_t i = 0; i < agg_col_count; ++i) {
        CUdeviceptr out_vec_dev_buffer =
            num_fragments ? reinterpret_cast<CUdeviceptr>(
                                gpu_allocator_->alloc(output_buffer_size_per_agg))
                          : 0;
        out_vec_dev_buffers.push_back(out_vec_dev_buffer);
        if (shared_memory_size) {
          CHECK_EQ(output_buffer_size_per_agg, size_t(8));
          gpu_allocator_->copyToDevice(reinterpret_cast<int8_t*>(out_vec_dev_buffer),
                                       reinterpret_cast<const int8_t*>(&init_agg_vals[i]),
                                       output_buffer_size_per_agg);
        }
      }
    }
    auto out_vec_dev_ptr = gpu_allocator_->alloc(agg_col_count * sizeof(CUdeviceptr));
    gpu_allocator_->copyToDevice(out_vec_dev_ptr,
                                 reinterpret_cast<int8_t*>(out_vec_dev_buffers.data()),
                                 agg_col_count * sizeof(CUdeviceptr));
    kernel_params[GROUPBY_BUF] = reinterpret_cast<CUdeviceptr>(out_vec_dev_ptr);
    std::vector<void*> param_ptrs;
    for (auto& param : kernel_params) {
      param_ptrs.push_back(&param);
    }

    if (g_enable_dynamic_watchdog || (allow_runtime_interrupt && !render_allocator)) {
      cuEventRecord(stop0, 0);
      cuEventSynchronize(stop0);
      float milliseconds0 = 0;
      cuEventElapsedTime(&milliseconds0, start0, stop0);
      VLOG(1) << "Device " << std::to_string(device_id)
              << ": launchGpuCode: prepare: " << std::to_string(milliseconds0) << " ms";
      cuEventRecord(start1, 0);
    }

    if (hoist_literals) {
      checkCudaErrors(cuLaunchKernel(cu_func,
                                     grid_size_x,
                                     grid_size_y,
                                     grid_size_z,
                                     block_size_x,
                                     block_size_y,
                                     block_size_z,
                                     shared_memory_size,
                                     nullptr,
                                     &param_ptrs[0],
                                     nullptr));
    } else {
      param_ptrs.erase(param_ptrs.begin() + LITERALS);  // TODO(alex): remove
      checkCudaErrors(cuLaunchKernel(cu_func,
                                     grid_size_x,
                                     grid_size_y,
                                     grid_size_z,
                                     block_size_x,
                                     block_size_y,
                                     block_size_z,
                                     shared_memory_size,
                                     nullptr,
                                     &param_ptrs[0],
                                     nullptr));
    }

    if (g_enable_dynamic_watchdog || (allow_runtime_interrupt && !render_allocator)) {
      executor_->registerActiveModule(native_code.second, device_id);
      cuEventRecord(stop1, 0);
      cuEventSynchronize(stop1);
      executor_->unregisterActiveModule(native_code.second, device_id);
      float milliseconds1 = 0;
      cuEventElapsedTime(&milliseconds1, start1, stop1);
      VLOG(1) << "Device " << std::to_string(device_id)
              << ": launchGpuCode: cuLaunchKernel: " << std::to_string(milliseconds1)
              << " ms";
      cuEventRecord(start2, 0);
    }

    copy_from_gpu(data_mgr,
                  &error_codes[0],
                  err_desc,
                  error_codes.size() * sizeof(error_codes[0]),
                  device_id);
    *error_code = aggregate_error_codes(error_codes);
    if (*error_code > 0) {
      return {};
    }
    if (ra_exe_unit.estimator) {
      CHECK(estimator_result_set_);
      estimator_result_set_->syncEstimatorBuffer();
      return {};
    }
    for (size_t i = 0; i < agg_col_count; ++i) {
      int64_t* host_out_vec = new int64_t[output_buffer_size_per_agg];
      copy_from_gpu(data_mgr,
                    host_out_vec,
                    out_vec_dev_buffers[i],
                    output_buffer_size_per_agg,
                    device_id);
      out_vec.push_back(host_out_vec);
    }
  }
  const auto count_distinct_bitmap_mem = query_buffers_->getCountDistinctBitmapPtr();
  if (count_distinct_bitmap_mem) {
    copy_from_gpu(data_mgr,
                  query_buffers_->getCountDistinctHostPtr(),
                  count_distinct_bitmap_mem,
                  query_buffers_->getCountDistinctBitmapBytes(),
                  device_id);
  }

  const auto varlen_output_gpu_buf = query_buffers_->getVarlenOutputPtr();
  if (varlen_output_gpu_buf) {
    CHECK(query_mem_desc_.varlenOutputBufferElemSize());
    const size_t varlen_output_buf_bytes =
        query_mem_desc_.getEntryCount() *
        query_mem_desc_.varlenOutputBufferElemSize().value();
    CHECK(query_buffers_->getVarlenOutputHostPtr());
    copy_from_gpu(data_mgr,
                  query_buffers_->getVarlenOutputHostPtr(),
                  varlen_output_gpu_buf,
                  varlen_output_buf_bytes,
                  device_id);
  }

  if (g_enable_dynamic_watchdog || (allow_runtime_interrupt && !render_allocator)) {
    cuEventRecord(stop2, 0);
    cuEventSynchronize(stop2);
    float milliseconds2 = 0;
    cuEventElapsedTime(&milliseconds2, start2, stop2);
    VLOG(1) << "Device " << std::to_string(device_id)
            << ": launchGpuCode: finish: " << std::to_string(milliseconds2) << " ms";
  }

  return out_vec;
#else
  return {};
#endif
}

std::vector<int64_t*> QueryExecutionContext::launchCpuCode(
    const RelAlgExecutionUnit& ra_exe_unit,
    const CpuCompilationContext* native_code,
    const bool hoist_literals,
    const std::vector<int8_t>& literal_buff,
    std::vector<std::vector<const int8_t*>> col_buffers,
    const std::vector<std::vector<int64_t>>& num_rows,
    const std::vector<std::vector<uint64_t>>& frag_offsets,
    const int32_t scan_limit,
    int32_t* error_code,
    const uint32_t num_tables,
    const std::vector<int64_t>& join_hash_tables) {
  auto timer = DEBUG_TIMER(__func__);
  INJECT_TIMER(lauchCpuCode);

  CHECK(query_buffers_);
  const auto& init_agg_vals = query_buffers_->init_agg_vals_;

  std::vector<const int8_t**> multifrag_col_buffers;
  for (auto& col_buffer : col_buffers) {
    multifrag_col_buffers.push_back(col_buffer.empty() ? nullptr : col_buffer.data());
  }
  const int8_t*** multifrag_cols_ptr{
      multifrag_col_buffers.empty() ? nullptr : &multifrag_col_buffers[0]};
  const uint64_t num_fragments =
      multifrag_cols_ptr ? static_cast<uint64_t>(col_buffers.size()) : uint64_t(0);
  const auto num_out_frags = multifrag_cols_ptr ? num_fragments : uint64_t(0);

  const bool is_group_by{query_mem_desc_.isGroupBy()};
  std::vector<int64_t*> out_vec;
  if (ra_exe_unit.estimator) {
    estimator_result_set_.reset(
        new ResultSet(ra_exe_unit.estimator, ExecutorDeviceType::CPU, 0, nullptr));
    out_vec.push_back(
        reinterpret_cast<int64_t*>(estimator_result_set_->getHostEstimatorBuffer()));
  } else {
    if (!is_group_by) {
      for (size_t i = 0; i < init_agg_vals.size(); ++i) {
        auto buff = new int64_t[num_out_frags];
        out_vec.push_back(static_cast<int64_t*>(buff));
      }
    }
  }

  CHECK_EQ(num_rows.size(), col_buffers.size());
  std::vector<int64_t> flatened_num_rows;
  for (auto& nums : num_rows) {
    flatened_num_rows.insert(flatened_num_rows.end(), nums.begin(), nums.end());
  }
  std::vector<uint64_t> flatened_frag_offsets;
  for (auto& offsets : frag_offsets) {
    flatened_frag_offsets.insert(
        flatened_frag_offsets.end(), offsets.begin(), offsets.end());
  }
  int64_t rowid_lookup_num_rows{*error_code ? *error_code + 1 : 0};
  auto num_rows_ptr =
      rowid_lookup_num_rows ? &rowid_lookup_num_rows : &flatened_num_rows[0];
  int32_t total_matched_init{0};

  std::vector<int64_t> cmpt_val_buff;
  if (is_group_by) {
    cmpt_val_buff =
        compact_init_vals(align_to_int64(query_mem_desc_.getColsSize()) / sizeof(int64_t),
                          init_agg_vals,
                          query_mem_desc_);
  }

  CHECK(native_code);
  const int64_t* join_hash_tables_ptr =
      join_hash_tables.size() == 1
          ? reinterpret_cast<int64_t*>(join_hash_tables[0])
          : (join_hash_tables.size() > 1 ? &join_hash_tables[0] : nullptr);
  if (hoist_literals) {
    using agg_query = void (*)(const int8_t***,  // col_buffers
                               const uint64_t*,  // num_fragments
                               const int8_t*,    // literals
                               const int64_t*,   // num_rows
                               const uint64_t*,  // frag_row_offsets
                               const int32_t*,   // max_matched
                               int32_t*,         // total_matched
                               const int64_t*,   // init_agg_value
                               int64_t**,        // out
                               int32_t*,         // error_code
                               const uint32_t*,  // num_tables
                               const int64_t*);  // join_hash_tables_ptr
    if (is_group_by) {
      reinterpret_cast<agg_query>(native_code->func())(
          multifrag_cols_ptr,
          &num_fragments,
          literal_buff.data(),
          num_rows_ptr,
          flatened_frag_offsets.data(),
          &scan_limit,
          &total_matched_init,
          cmpt_val_buff.data(),
          query_buffers_->getGroupByBuffersPtr(),
          error_code,
          &num_tables,
          join_hash_tables_ptr);
    } else {
      reinterpret_cast<agg_query>(native_code->func())(multifrag_cols_ptr,
                                                       &num_fragments,
                                                       literal_buff.data(),
                                                       num_rows_ptr,
                                                       flatened_frag_offsets.data(),
                                                       &scan_limit,
                                                       &total_matched_init,
                                                       init_agg_vals.data(),
                                                       out_vec.data(),
                                                       error_code,
                                                       &num_tables,
                                                       join_hash_tables_ptr);
    }
  } else {
    using agg_query = void (*)(const int8_t***,  // col_buffers
                               const uint64_t*,  // num_fragments
                               const int64_t*,   // num_rows
                               const uint64_t*,  // frag_row_offsets
                               const int32_t*,   // max_matched
                               int32_t*,         // total_matched
                               const int64_t*,   // init_agg_value
                               int64_t**,        // out
                               int32_t*,         // error_code
                               const uint32_t*,  // num_tables
                               const int64_t*);  // join_hash_tables_ptr
    if (is_group_by) {
      reinterpret_cast<agg_query>(native_code->func())(
          multifrag_cols_ptr,
          &num_fragments,
          num_rows_ptr,
          flatened_frag_offsets.data(),
          &scan_limit,
          &total_matched_init,
          cmpt_val_buff.data(),
          query_buffers_->getGroupByBuffersPtr(),
          error_code,
          &num_tables,
          join_hash_tables_ptr);
    } else {
      reinterpret_cast<agg_query>(native_code->func())(multifrag_cols_ptr,
                                                       &num_fragments,
                                                       num_rows_ptr,
                                                       flatened_frag_offsets.data(),
                                                       &scan_limit,
                                                       &total_matched_init,
                                                       init_agg_vals.data(),
                                                       out_vec.data(),
                                                       error_code,
                                                       &num_tables,
                                                       join_hash_tables_ptr);
    }
  }

  if (ra_exe_unit.estimator) {
    return {};
  }

  if (rowid_lookup_num_rows && *error_code < 0) {
    *error_code = 0;
  }

  if (query_mem_desc_.useStreamingTopN()) {
    query_buffers_->applyStreamingTopNOffsetCpu(query_mem_desc_, ra_exe_unit);
  }

  if (query_mem_desc_.didOutputColumnar() &&
      query_mem_desc_.getQueryDescriptionType() == QueryDescriptionType::Projection) {
    query_buffers_->compactProjectionBuffersCpu(query_mem_desc_, total_matched_init);
  }

  return out_vec;
}

#ifdef HAVE_CUDA
void QueryExecutionContext::initializeDynamicWatchdog(void* native_module,
                                                      const int device_id) const {
  auto cu_module = static_cast<CUmodule>(native_module);
  CHECK(cu_module);
  CUevent start, stop;
  cuEventCreate(&start, 0);
  cuEventCreate(&stop, 0);
  cuEventRecord(start, 0);

  CUdeviceptr dw_cycle_budget;
  size_t dw_cycle_budget_size;
  // Translate milliseconds to device cycles
  uint64_t cycle_budget = executor_->deviceCycles(g_dynamic_watchdog_time_limit);
  if (device_id == 0) {
    LOG(INFO) << "Dynamic Watchdog budget: GPU: "
              << std::to_string(g_dynamic_watchdog_time_limit) << "ms, "
              << std::to_string(cycle_budget) << " cycles";
  }
  checkCudaErrors(cuModuleGetGlobal(
      &dw_cycle_budget, &dw_cycle_budget_size, cu_module, "dw_cycle_budget"));
  CHECK_EQ(dw_cycle_budget_size, sizeof(uint64_t));
  checkCudaErrors(cuMemcpyHtoD(
      dw_cycle_budget, reinterpret_cast<void*>(&cycle_budget), sizeof(uint64_t)));

  CUdeviceptr dw_sm_cycle_start;
  size_t dw_sm_cycle_start_size;
  checkCudaErrors(cuModuleGetGlobal(
      &dw_sm_cycle_start, &dw_sm_cycle_start_size, cu_module, "dw_sm_cycle_start"));
  CHECK_EQ(dw_sm_cycle_start_size, 128 * sizeof(uint64_t));
  checkCudaErrors(cuMemsetD32(dw_sm_cycle_start, 0, 128 * 2));

  if (!executor_->interrupted_.load()) {
    // Executor is not marked as interrupted, make sure dynamic watchdog doesn't block
    // execution
    CUdeviceptr dw_abort;
    size_t dw_abort_size;
    checkCudaErrors(cuModuleGetGlobal(&dw_abort, &dw_abort_size, cu_module, "dw_abort"));
    CHECK_EQ(dw_abort_size, sizeof(uint32_t));
    checkCudaErrors(cuMemsetD32(dw_abort, 0, 1));
  }

  cuEventRecord(stop, 0);
  cuEventSynchronize(stop);
  float milliseconds = 0;
  cuEventElapsedTime(&milliseconds, start, stop);
  VLOG(1) << "Device " << std::to_string(device_id)
          << ": launchGpuCode: dynamic watchdog init: " << std::to_string(milliseconds)
          << " ms\n";
}

void QueryExecutionContext::initializeRuntimeInterrupter(void* native_module,
                                                         const int device_id) const {
  if (!executor_->interrupted_.load()) {
    // Executor is not marked as interrupted, make sure interrupt flag doesn't block
    // execution
    auto cu_module = static_cast<CUmodule>(native_module);
    CHECK(cu_module);
    CUevent start, stop;
    cuEventCreate(&start, 0);
    cuEventCreate(&stop, 0);
    cuEventRecord(start, 0);

    CUdeviceptr runtime_interrupt_flag;
    size_t runtime_interrupt_flag_size;
    checkCudaErrors(cuModuleGetGlobal(&runtime_interrupt_flag,
                                      &runtime_interrupt_flag_size,
                                      cu_module,
                                      "runtime_interrupt_flag"));
    CHECK_EQ(runtime_interrupt_flag_size, sizeof(uint32_t));
    checkCudaErrors(cuMemsetD32(runtime_interrupt_flag, 0, 1));

    cuEventRecord(stop, 0);
    cuEventSynchronize(stop);
    float milliseconds = 0;
    cuEventElapsedTime(&milliseconds, start, stop);
    VLOG(1) << "Device " << std::to_string(device_id)
            << ": launchGpuCode: runtime query interrupter init: "
            << std::to_string(milliseconds) << " ms";
  }
}

std::vector<CUdeviceptr> QueryExecutionContext::prepareKernelParams(
    const std::vector<std::vector<const int8_t*>>& col_buffers,
    const std::vector<int8_t>& literal_buff,
    const std::vector<std::vector<int64_t>>& num_rows,
    const std::vector<std::vector<uint64_t>>& frag_offsets,
    const int32_t scan_limit,
    const std::vector<int64_t>& init_agg_vals,
    const std::vector<int32_t>& error_codes,
    const uint32_t num_tables,
    const std::vector<int64_t>& join_hash_tables,
    Data_Namespace::DataMgr* data_mgr,
    const int device_id,
    const bool hoist_literals,
    const bool is_group_by) const {
  CHECK(gpu_allocator_);
  std::vector<CUdeviceptr> params(KERN_PARAM_COUNT, 0);
  const uint64_t num_fragments = static_cast<uint64_t>(col_buffers.size());
  const size_t col_count{num_fragments > 0 ? col_buffers.front().size() : 0};
  if (col_count) {
    std::vector<CUdeviceptr> multifrag_col_dev_buffers;
    for (auto frag_col_buffers : col_buffers) {
      std::vector<CUdeviceptr> col_dev_buffers;
      for (auto col_buffer : frag_col_buffers) {
        col_dev_buffers.push_back(reinterpret_cast<CUdeviceptr>(col_buffer));
      }
      auto col_buffers_dev_ptr = reinterpret_cast<CUdeviceptr>(
          gpu_allocator_->alloc(col_count * sizeof(CUdeviceptr)));
      copy_to_gpu(data_mgr,
                  col_buffers_dev_ptr,
                  &col_dev_buffers[0],
                  col_count * sizeof(CUdeviceptr),
                  device_id);
      multifrag_col_dev_buffers.push_back(col_buffers_dev_ptr);
    }
    params[COL_BUFFERS] = reinterpret_cast<CUdeviceptr>(
        gpu_allocator_->alloc(num_fragments * sizeof(CUdeviceptr)));
    copy_to_gpu(data_mgr,
                params[COL_BUFFERS],
                &multifrag_col_dev_buffers[0],
                num_fragments * sizeof(CUdeviceptr),
                device_id);
  }
  params[NUM_FRAGMENTS] =
      reinterpret_cast<CUdeviceptr>(gpu_allocator_->alloc(sizeof(uint64_t)));
  copy_to_gpu(
      data_mgr, params[NUM_FRAGMENTS], &num_fragments, sizeof(uint64_t), device_id);
  CUdeviceptr literals_and_addr_mapping = reinterpret_cast<CUdeviceptr>(
      gpu_allocator_->alloc(literal_buff.size() + 2 * sizeof(int64_t)));
  CHECK_EQ(CUdeviceptr{0}, literals_and_addr_mapping % 8);
  std::vector<int64_t> additional_literal_bytes;
  const auto count_distinct_bitmap_mem = query_buffers_->getCountDistinctBitmapPtr();
  if (count_distinct_bitmap_mem) {
    // Store host and device addresses
    const auto count_distinct_bitmap_host_mem = query_buffers_->getCountDistinctHostPtr();
    CHECK(count_distinct_bitmap_host_mem);
    additional_literal_bytes.push_back(
        reinterpret_cast<int64_t>(count_distinct_bitmap_host_mem));
    additional_literal_bytes.push_back(static_cast<int64_t>(count_distinct_bitmap_mem));
    copy_to_gpu(data_mgr,
                literals_and_addr_mapping,
                &additional_literal_bytes[0],
                additional_literal_bytes.size() * sizeof(additional_literal_bytes[0]),
                device_id);
  }
  params[LITERALS] = literals_and_addr_mapping + additional_literal_bytes.size() *
                                                     sizeof(additional_literal_bytes[0]);
  if (!literal_buff.empty()) {
    CHECK(hoist_literals);
    copy_to_gpu(
        data_mgr, params[LITERALS], &literal_buff[0], literal_buff.size(), device_id);
  }
  CHECK_EQ(num_rows.size(), col_buffers.size());
  std::vector<int64_t> flatened_num_rows;
  for (auto& nums : num_rows) {
    CHECK_EQ(nums.size(), num_tables);
    flatened_num_rows.insert(flatened_num_rows.end(), nums.begin(), nums.end());
  }
  params[NUM_ROWS] = reinterpret_cast<CUdeviceptr>(
      gpu_allocator_->alloc(sizeof(int64_t) * flatened_num_rows.size()));
  copy_to_gpu(data_mgr,
              params[NUM_ROWS],
              &flatened_num_rows[0],
              sizeof(int64_t) * flatened_num_rows.size(),
              device_id);

  CHECK_EQ(frag_offsets.size(), col_buffers.size());
  std::vector<int64_t> flatened_frag_offsets;
  for (auto& offsets : frag_offsets) {
    CHECK_EQ(offsets.size(), num_tables);
    flatened_frag_offsets.insert(
        flatened_frag_offsets.end(), offsets.begin(), offsets.end());
  }
  params[FRAG_ROW_OFFSETS] = reinterpret_cast<CUdeviceptr>(
      gpu_allocator_->alloc(sizeof(int64_t) * flatened_frag_offsets.size()));
  copy_to_gpu(data_mgr,
              params[FRAG_ROW_OFFSETS],
              &flatened_frag_offsets[0],
              sizeof(int64_t) * flatened_frag_offsets.size(),
              device_id);

  // Note that this will be overwritten if we are setting the entry count during group by
  // buffer allocation and initialization
  const int32_t max_matched{scan_limit};
  params[MAX_MATCHED] =
      reinterpret_cast<CUdeviceptr>(gpu_allocator_->alloc(sizeof(max_matched)));
  copy_to_gpu(
      data_mgr, params[MAX_MATCHED], &max_matched, sizeof(max_matched), device_id);

  int32_t total_matched{0};
  params[TOTAL_MATCHED] =
      reinterpret_cast<CUdeviceptr>(gpu_allocator_->alloc(sizeof(total_matched)));
  copy_to_gpu(
      data_mgr, params[TOTAL_MATCHED], &total_matched, sizeof(total_matched), device_id);

  if (is_group_by && !output_columnar_) {
    auto cmpt_sz = align_to_int64(query_mem_desc_.getColsSize()) / sizeof(int64_t);
    auto cmpt_val_buff = compact_init_vals(cmpt_sz, init_agg_vals, query_mem_desc_);
    params[INIT_AGG_VALS] =
        reinterpret_cast<CUdeviceptr>(gpu_allocator_->alloc(cmpt_sz * sizeof(int64_t)));
    copy_to_gpu(data_mgr,
                params[INIT_AGG_VALS],
                &cmpt_val_buff[0],
                cmpt_sz * sizeof(int64_t),
                device_id);
  } else {
    params[INIT_AGG_VALS] = reinterpret_cast<CUdeviceptr>(
        gpu_allocator_->alloc(init_agg_vals.size() * sizeof(int64_t)));
    copy_to_gpu(data_mgr,
                params[INIT_AGG_VALS],
                &init_agg_vals[0],
                init_agg_vals.size() * sizeof(int64_t),
                device_id);
  }

  params[ERROR_CODE] = reinterpret_cast<CUdeviceptr>(
      gpu_allocator_->alloc(error_codes.size() * sizeof(error_codes[0])));
  copy_to_gpu(data_mgr,
              params[ERROR_CODE],
              &error_codes[0],
              error_codes.size() * sizeof(error_codes[0]),
              device_id);

  params[NUM_TABLES] =
      reinterpret_cast<CUdeviceptr>(gpu_allocator_->alloc(sizeof(uint32_t)));
  copy_to_gpu(data_mgr, params[NUM_TABLES], &num_tables, sizeof(uint32_t), device_id);

  const auto hash_table_count = join_hash_tables.size();
  switch (hash_table_count) {
    case 0: {
      params[JOIN_HASH_TABLES] = CUdeviceptr(0);
      break;
    }
    case 1:
      params[JOIN_HASH_TABLES] = static_cast<CUdeviceptr>(join_hash_tables[0]);
      break;
    default: {
      params[JOIN_HASH_TABLES] = reinterpret_cast<CUdeviceptr>(
          gpu_allocator_->alloc(hash_table_count * sizeof(int64_t)));
      copy_to_gpu(data_mgr,
                  params[JOIN_HASH_TABLES],
                  &join_hash_tables[0],
                  hash_table_count * sizeof(int64_t),
                  device_id);
      break;
    }
  }

  return params;
}
#endif
