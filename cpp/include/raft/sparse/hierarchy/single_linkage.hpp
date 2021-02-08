/*
 * Copyright (c) 2018-2020, NVIDIA CORPORATION.
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

#include <cuml/common/logger.hpp>

#include <cuml/cuml_api.h>
#include <common/cumlHandle.hpp>

#include <raft/cudart_utils.h>
#include <raft/mr/device/buffer.hpp>

#include <cuml/cluster/linkage.hpp>

#include <distance/distance.cuh>
#include <sparse/coo.cuh>

#include <hierarchy/agglomerative.cuh>
#include <hierarchy/connectivities.cuh>
#include <hierarchy/mst.cuh>

namespace raft {
namespace hierarchy {

static const size_t EMPTY = 0;

enum LinkageDistance { PAIRWISE = 0, KNN_GRAPH = 1 };

template <typename value_idx, typename value_t>
struct linkage_output {
  value_idx m;
  value_idx n_clusters;

  value_idx n_leaves;
  value_idx n_connected_components;

  value_idx *labels;  // size: m

  value_idx *children;  // size: (m-1, 2)
};

struct linkage_output_int_float : public linkage_output<int, float> {};
struct linkage_output__int64_float : public linkage_output<int64_t, float> {};

template <typename value_idx, typename value_t,
          LinkageDistance dist_type = LinkageDistance::PAIRWISE>
void single_linkage(const raft::handle_t &handle, const value_t *X, size_t m,
                    size_t n, raft::distance::DistanceType metric,
                    linkage_output<value_idx, value_t> *out, int c,
                    int n_clusters) {
  ASSERT(n_clusters <= m,
         "n_clusters must be less than or equal to the number of data points");

  auto stream = handle.get_stream();
  auto d_alloc = handle.get_device_allocator();

  raft::mr::device::buffer<value_idx> indptr(d_alloc, stream, EMPTY);
  raft::mr::device::buffer<value_idx> indices(d_alloc, stream, EMPTY);
  raft::mr::device::buffer<value_t> pw_dists(d_alloc, stream, EMPTY);

  /**
   * 1. Construct distance graph
   */
  distance::get_distance_graph<value_idx, value_t, dist_type>(
    handle, X, m, n, metric, indptr, indices, pw_dists, c);

  raft::mr::device::buffer<value_idx> mst_rows(d_alloc, stream, EMPTY);
  raft::mr::device::buffer<value_idx> mst_cols(d_alloc, stream, EMPTY);
  raft::mr::device::buffer<value_t> mst_data(d_alloc, stream, EMPTY);

  /**
   * 2. Construct MST, sorted by weights
   */
  mst::build_sorted_mst<value_idx, value_t>(
    handle, indptr.data(), indices.data(), pw_dists.data(), m, mst_rows,
    mst_cols, mst_data, indices.size());

  pw_dists.release();

  /**
   * Perform hierarchical labeling
   */
  size_t n_edges = mst_rows.size();

  raft::mr::device::buffer<value_idx> children(d_alloc, stream, n_edges * 2);
  raft::mr::device::buffer<value_t> out_delta(d_alloc, stream, n_edges);
  raft::mr::device::buffer<value_idx> out_size(d_alloc, stream, n_edges);

  // Create dendrogram
  label::agglomerative::build_dendrogram_host<value_idx, value_t>(
    handle, mst_rows.data(), mst_cols.data(), mst_data.data(), n_edges,
    children, out_delta, out_size);

  label::agglomerative::extract_flattened_clusters(handle, out->labels,
                                                   children, n_clusters, m);
};
};  // namespace hierarchy
};  // namespace raft