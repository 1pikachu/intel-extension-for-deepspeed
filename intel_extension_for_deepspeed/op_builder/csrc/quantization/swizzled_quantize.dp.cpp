// Copyright (c) Microsoft Corporation.
// SPDX-License-Identifier: Apache-2.0

// DeepSpeed Team

#include <sycl/sycl.hpp>
#include <dpct/dpct.hpp>
#include "memory_access_utils.h"
#include "quantization_utils.h"
#include "reduction_utils.h"

using rop = reduce::ROpType;

namespace swiz_quant {
constexpr int max_threads = 512;
constexpr int min_threads = 32;

constexpr int step_granularity = 2;
constexpr int h_per_step = step_granularity * quantize::h_per_load;
}  // namespace swiz_quant

template <int numBits, int totalChunks, int threads, quantize::Type quantType>
/*
DPCT1110:46: The total declared local variable size in device function swizzled_quant_kernel exceeds
128 bytes and may cause high register pressure. Consult with your hardware vendor to find the total
register size available and adjust the code, or use smaller sub-group size to avoid high register
pressure.
*/
void swizzled_quant_kernel(int8_t* quantized_data,
                           float* quantized_scales,
                           const sycl::half* uncompressed_data,
                           int elems_per_group,
                           int nodes,
                           int devices_per_node)
{
    auto item_ct1 = sycl::ext::oneapi::experimental::this_nd_item<3>();
    sycl::group<3> tb = sycl::ext::oneapi::experimental::this_group<3>();
    sycl::sub_group warp = sycl::ext::oneapi::experimental::this_sub_group();

    // Indexing offsets, same as normal quantization for in-case
    const int block_rank =
        item_ct1.get_group(2) + item_ct1.get_group(1) * item_ct1.get_group_range(2) +
        item_ct1.get_group(0) * item_ct1.get_group_range(2) * item_ct1.get_group_range(1);
    const int block_offset = block_rank * elems_per_group;
    const int elem_offset = tb.get_local_id()[2] * quantize::h_per_load;
    const int base_offset = block_offset + elem_offset;
    const int stride = sycl::ext::oneapi::experimental::this_group<3>().get_local_linear_range() *
                       quantize::h_per_load;
    const sycl::half* input_base = uncompressed_data + base_offset;

    // Local buffer
    sycl::half2 local_buffer[totalChunks * quantize::h2_per_load];

    quantize::GroupStats<quantType> stats;
#pragma unroll
    for (int i = 0; i < totalChunks; i++) {
        sycl::half2* iteration_buffer = local_buffer + i * quantize::h2_per_load;

        mem_access::load_global<quantize::granularity>(
            iteration_buffer, input_base + i * stride, elem_offset + i * stride < elems_per_group);

#pragma unroll
        for (int j = 0; j < quantize::h2_per_load; j++) { stats.update(iteration_buffer[j]); }
    }

    auto params = stats.template get_params<numBits, threads>(tb, warp);

    const int partition_id = item_ct1.get_group(0);
    const int partition_offset = partition_id / devices_per_node;
    const int partition_base = (partition_id % devices_per_node) * nodes;
    const int pipelining_offset = item_ct1.get_group(1) * (devices_per_node * nodes);
    const int output_partition = (pipelining_offset + partition_base + partition_offset);

    constexpr int out_scalar_effect = 8 / numBits;
    const int out_block_rank =
        output_partition * item_ct1.get_group_range(2) + item_ct1.get_group(2);
    const int out_block_offset = out_block_rank * elems_per_group / out_scalar_effect;
    const int out_base_offset = out_block_offset + elem_offset / out_scalar_effect;
    int8_t* out_base = quantized_data + out_base_offset;

    const int out_stride = stride / out_scalar_effect;
    constexpr int num_int8_out = quantize::h_per_load / out_scalar_effect;

    if (tb.get_local_id()[2] == 0) { params.store(quantized_scales, out_block_rank); }

#pragma unroll
    for (int i = 0; i < totalChunks; i++) {
        if (i * stride + elem_offset < elems_per_group) {
            int8_t local_output[quantize::h_per_load / out_scalar_effect];
            quantize::_chunk<numBits, quantType>(
                local_output, local_buffer + i * quantize::h2_per_load, params);
            mem_access::store_global<num_int8_out>(out_base + i * out_stride, local_output);
        }
    }
}

/*
DPCT1049:47: The work-group size passed to the SYCL kernel may exceed the limit. To get the device
limit, query info::device::max_work_group_size. Adjust the work-group size if needed.
*/
#define LAUNCH_SWIZZLE_QUANT(total_chunks, threads)                                               \
  {                                                                                               \
    dpct::has_capability_or_fail(stream->get_device(), {sycl::aspect::fp64, sycl::aspect::fp16}); \
    stream->submit([&](sycl::handler& cgh) {                                                      \
      int8_t* q_data_ct0 = q_data;                                                                \
      float* q_scales_ct1 = q_scales;                                                             \
      const sycl::half* input_data_ct2 = input_data;                                              \
      auto elems_per_group_ct3 = elems_per_group;                                                 \
      auto nodes_ct4 = nodes;                                                                     \
      auto devices_per_node_ct5 = devices_per_node;                                               \
                                                                                                  \
      cgh.parallel_for(sycl::nd_range<3>(grid * block, block),                                    \
                       [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {        \
                         swizzled_quant_kernel<numBits, total_chunks, threads, qType>(            \
                             q_data_ct0,                                                          \
                             q_scales_ct1,                                                        \
                             input_data_ct2,                                                      \
                             elems_per_group_ct3,                                                 \
                             nodes_ct4,                                                           \
                             devices_per_node_ct5);                                               \
                       });                                                                        \
    });                                                                                           \
  }

/*
Swizzled quantization reorganizes the quantized groups in order to better facilitate
communication. As an example of the partitioning scheme we have the following example
of 2 node, 4 device swizzling:

 --- --- --- --- --- --- --- ---
| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
 --- --- --- --- --- --- --- ---
becomes
 --- --- --- --- --- --- --- ---
| 0 | 4 | 1 | 5 | 2 | 6 | 3 | 7 |
 --- --- --- --- --- --- --- ---

Multiple quantization groups may be mapped into a single partition. In order to better support
later pipelining, we may also perform an additional slicing. In two-way slicing, for instance,
the first halves of each partition are concatenated.
*/

template <int numBits, quantize::Type qType>
void launch_swizzled_quant_impl(int8_t* q_data,
                                float* q_scales,
                                const sycl::half* input_data,
                                int groups,
                                int elems_per_group,
                                int pipelining,
                                int nodes,
                                int devices_per_node,
                                dpct::queue_ptr stream)
{
    const int one_step_threads =
        next_pow2((elems_per_group + swiz_quant::h_per_step - 1) / swiz_quant::h_per_step);
    const int max_threads = (one_step_threads < swiz_quant::max_threads) ? one_step_threads
                                                                         : swiz_quant::max_threads;
    const int threads = (max_threads < swiz_quant::min_threads) ? swiz_quant::min_threads
                                                                : max_threads;

    sycl::range<3> block(1, 1, threads);
    const int groups_per_partition = groups / (nodes * devices_per_node);
    assert(groups_per_partition % pipelining == 0);
    const int contiguous_groups = groups_per_partition / pipelining;
    const int partitions = nodes * devices_per_node;
    sycl::range<3> grid(partitions, pipelining, contiguous_groups);

    const int elems_per_step = threads * swiz_quant::h_per_step;
    const int external_unroll = ((elems_per_group + elems_per_step - 1) / elems_per_step);
    const int total_unroll = external_unroll * swiz_quant::step_granularity;

    assert(total_unroll % 2 == 0);

    if (threads == 32) {
        LAUNCH_SWIZZLE_QUANT(2, 32);
    } else if (threads == 64) {
        LAUNCH_SWIZZLE_QUANT(2, 64);
    } else if (threads == 128) {
        LAUNCH_SWIZZLE_QUANT(2, 128);
    } else if (threads == 256) {
        LAUNCH_SWIZZLE_QUANT(2, 256);
    } else if (threads == 512) {
        if (total_unroll == 2) {
            LAUNCH_SWIZZLE_QUANT(2, 512);
        } else if (total_unroll == 4) {
            LAUNCH_SWIZZLE_QUANT(4, 512);
        } else if (total_unroll == 6) {
            LAUNCH_SWIZZLE_QUANT(6, 512);
        } else if (total_unroll == 8) {
            LAUNCH_SWIZZLE_QUANT(8, 512);
        } else if (total_unroll == 10) {
            LAUNCH_SWIZZLE_QUANT(10, 512);
        }
    }
}

#define DISPATCH_SWIZZLE_QUANT(num_bits, qtype)                   \
    launch_swizzled_quant_impl<num_bits, qtype>(q_data,           \
                                                q_scales,         \
                                                input_data,       \
                                                groups,           \
                                                elems_per_group,  \
                                                pipelining,       \
                                                nodes,            \
                                                devices_per_node, \
                                                stream);

void launch_swizzled_quant(int8_t* q_data,
                           float* q_scales,
                           const sycl::half* input_data,
                           int num_bits,
                           quantize::Type q_type,
                           int groups,
                           int elems_per_group,
                           int pipelining,
                           int nodes,
                           int devices_per_node,
                           dpct::queue_ptr stream)
{
    if (num_bits == 4) {
        if (q_type == quantize::Type::Asymmetric) {
            DISPATCH_SWIZZLE_QUANT(4, quantize::Type::Asymmetric);
        } else if (q_type == quantize::Type::Symmetric) {
            DISPATCH_SWIZZLE_QUANT(4, quantize::Type::Symmetric);
        }
    } else if (num_bits == 8) {
        if (q_type == quantize::Type::Asymmetric) {
            DISPATCH_SWIZZLE_QUANT(8, quantize::Type::Asymmetric);
        } else if (q_type == quantize::Type::Symmetric) {
            DISPATCH_SWIZZLE_QUANT(8, quantize::Type::Symmetric);
        }
    }
}
