// Copyright 2024 Advanced Micro Devices, Inc.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "shortfin/array/array.h"

#include <sstream>

#include "fmt/core.h"
#include "fmt/ranges.h"
#include "shortfin/array/xtensor_bridge.h"

namespace shortfin::array {

template class InlinedDims<iree_hal_dim_t>;

// -------------------------------------------------------------------------- //
// device_array
// -------------------------------------------------------------------------- //

device_array::device_array(class storage storage,
                           std::span<const Dims::value_type> shape, DType dtype)
    : base_array(shape, dtype), storage_(std::move(storage)) {
  auto needed_size = this->dtype().compute_dense_nd_size(this->shape());
  if (storage_.byte_length() < needed_size) {
    throw std::invalid_argument(
        fmt::format("Array storage requires at least {} bytes but has only {}",
                    needed_size, storage_.byte_length()));
  }
}

const mapping device_array::data() const { return storage_.map_read(); }

mapping device_array::data() { return storage_.map_read(); }

mapping device_array::data_rw() { return storage_.map_read_write(); }

mapping device_array::data_w() { return storage_.map_write_discard(); }

std::optional<mapping> device_array::map_memory_for_xtensor() {
  if (storage_.is_mappable_for_read_write()) {
    return storage_.map_read_write();
  } else if (storage_.is_mappable_for_read()) {
    return storage_.map_read();
  }
  return {};
}

std::string device_array::to_s() const {
  std::string contents;
  const char *contents_prefix = " ";
  if (!storage_.is_mappable_for_read()) {
    contents = "<unmappable for host read>";
  } else {
    auto maybe_contents = contents_to_s();
    if (maybe_contents) {
      contents = std::move(*maybe_contents);
      contents_prefix = "\n";
    } else {
      contents = "<unsupported dtype or unmappable storage>";
    }
  }

  return fmt::format(
      "device_array([{}], dtype='{}', device={}(type={}, usage={}, access={})) "
      "={}{}",
      fmt::join(shape(), ", "), dtype().name(), storage_.device().to_s(),
      storage_.formatted_memory_type(), storage_.formatted_buffer_usage(),
      storage_.formatted_memory_access(), contents_prefix, contents);
}

void device_array::AddAsInvocationArgument(
    local::ProgramInvocation *inv, local::ProgramResourceBarrier barrier) {
  auto dims_span = shape();
  iree_hal_buffer_view_t *buffer_view;
  SHORTFIN_THROW_IF_ERROR(iree_hal_buffer_view_create(
      storage_, dims_span.size(), dims_span.data(), dtype(),
      IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR, storage_.host_allocator(),
      &buffer_view));

  iree::vm_opaque_ref ref;
  *(&ref) = iree_hal_buffer_view_move_ref(buffer_view);
  inv->AddArg(std::move(ref));

  storage().AddInvocationArgBarrier(inv, barrier);
}

iree_vm_ref_type_t device_array::invocation_marshalable_type() {
  return iree_hal_buffer_view_type();
}

device_array device_array::CreateFromInvocationResultRef(
    local::ProgramInvocation *inv, iree::vm_opaque_ref ref) {
  // We don't retain the buffer view in the device array, so just deref it
  // vs stealing the ref.
  iree_hal_buffer_view_t *bv = iree_hal_buffer_view_deref(*ref.get());
  iree::hal_buffer_ptr buffer =
      iree::hal_buffer_ptr::borrow_reference(iree_hal_buffer_view_buffer(bv));

  auto imported_storage =
      storage::ImportInvocationResultStorage(inv, std::move(buffer));
  std::span<const iree_hal_dim_t> shape(iree_hal_buffer_view_shape_dims(bv),
                                        iree_hal_buffer_view_shape_rank(bv));
  return device_array(
      std::move(imported_storage), shape,
      DType::import_element_type(iree_hal_buffer_view_element_type(bv)));
}

device_array device_array::view(Dims &offsets, Dims &sizes) {
  auto rank = shape().size();
  if (offsets.size() != sizes.size() || offsets.empty() ||
      offsets.size() > rank) {
    throw std::invalid_argument(
        "view offsets and sizes must be of equal size and be of a rank "
        "<= the array rank");
  }
  if (rank == 0) {
    throw std::invalid_argument("view cannot operate on rank 0 arrays");
  }
  // Compute row strides.
  Dims row_stride_bytes(shape().size());
  iree_device_size_t accum = dtype().dense_byte_count();
  for (int i = rank - 1; i >= 0; --i) {
    row_stride_bytes[i] = accum;
    accum *= shape()[i];
  }

  Dims new_dims(shape_container());
  bool has_stride = false;
  iree_device_size_t start_offset = 0;
  iree_device_size_t span_size = storage().byte_length();
  for (size_t i = 0; i < offsets.size(); ++i) {
    auto row_stride = row_stride_bytes[i];
    auto dim_size = shape()[i];
    auto slice_offset = offsets[i];
    auto slice_size = sizes[i];
    if (slice_offset >= dim_size || (slice_offset + slice_size) > dim_size) {
      throw std::invalid_argument(
          fmt::format("Cannot index ({}:{}) into dim size {} at position {}",
                      slice_offset, slice_size, dim_size, i));
    }
    if (has_stride && (slice_offset > 0 || slice_size != dim_size)) {
      throw std::invalid_argument(
          fmt::format("Cannot create a view with dimensions following a "
                      "spanning dim (at position {})",
                      i));
    }
    if (slice_size > 1) {
      has_stride = true;
    }

    new_dims[i] = slice_size;
    start_offset += row_stride * slice_offset;
    span_size = row_stride * slice_size;
  }

  return device_array(storage().subspan(start_offset, span_size),
                      new_dims.span(), dtype());
}

}  // namespace shortfin::array
