// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2022 Google Inc. All rights reserved.
// http://ceres-solver.org/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Authors: dmitriy.korchemkin@gmail.com (Dmitriy Korchemkin)

#include "ceres/cuda_kernels_bsm_to_crs.h"

#include <cuda_runtime.h>
#include <thrust/execution_policy.h>
#include <thrust/scan.h>

#include "ceres/block_structure.h"
#include "ceres/cuda_kernels_utils.h"

namespace ceres {
namespace internal {

// Fill row block id and nnz for each row using block-sparse structure
// represented by a set of flat arrays.
// Inputs:
// - num_row_blocks: number of row-blocks in block-sparse structure
// - first_cell_in_row_block: index of the first cell of the row-block; size:
// num_row_blocks + 1
// - cells: cells of block-sparse structure as a continuous array
// - row_blocks: row blocks of block-sparse structure stored sequentially
// - col_blocks: column blocks of block-sparse structure stored sequentially
// Outputs:
// - rows: rows[i + 1] will contain number of non-zeros in i-th row, rows[0]
// will be set to 0; rows are filled with a shift by one element in order
// to obtain row-index array of CRS matrix with a inclusive scan afterwards
// - row_block_ids: row_block_ids[i] will be set to index of row-block that
// contains i-th row.
// Computation is perform row-block-wise
__global__ void RowBlockIdAndNNZ(
    int num_row_blocks,
    const int* __restrict__ first_cell_in_row_block,
    const Cell* __restrict__ cells,
    const Block* __restrict__ row_blocks,
    const Block* __restrict__ col_blocks,
    int* __restrict__ rows,
    int* __restrict__ row_block_ids) {
  const int row_block_id = blockIdx.x * blockDim.x + threadIdx.x;
  if (row_block_id > num_row_blocks) {
    // No synchronization is performed in this kernel, thus it is safe to return
    return;
  }
  if (row_block_id == num_row_blocks) {
    // one extra thread sets the first element
    rows[0] = 0;
    return;
  }
  const auto& row_block = row_blocks[row_block_id];
  int row_nnz = 0;
  const auto first_cell = cells + first_cell_in_row_block[row_block_id];
  const auto last_cell = cells + first_cell_in_row_block[row_block_id + 1];
  for (auto cell = first_cell; cell < last_cell; ++cell) {
    row_nnz += col_blocks[cell->block_id].size;
  }
  const int first_row = row_block.position;
  const int last_row = first_row + row_block.size;
  for (int i = first_row; i < last_row; ++i) {
    rows[i + 1] = row_nnz;
    row_block_ids[i] = row_block_id;
  }
}

// Row-wise creation of CRS structure
// Inputs:
// - num_rows: number of rows in matrix
// - first_cell_in_row_block: index of the first cell of the row-block; size:
// num_row_blocks + 1
// - cells: cells of block-sparse structure as a continuous array
// - row_blocks: row blocks of block-sparse structure stored sequentially
// - col_blocks: column blocks of block-sparse structure stored sequentially
// - row_block_ids: index of row-block that corresponds to row
// - rows: row-index array of CRS structure
// Outputs:
// - cols: column-index array of CRS structure
// Computaion is perform row-wise
__global__ void ComputeColumnsAndPermutation(
    int num_rows,
    const int* __restrict__ first_cell_in_row_block,
    const Cell* __restrict__ cells,
    const Block* __restrict__ row_blocks,
    const Block* __restrict__ col_blocks,
    const int* __restrict__ row_block_ids,
    const int* __restrict__ rows,
    int* __restrict__ cols) {
  const int row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= num_rows) {
    // No synchronization is performed in this kernel, thus it is safe to return
    return;
  }
  const int row_block_id = row_block_ids[row];
  // position in crs matrix
  int crs_position = rows[row];
  const auto first_cell = cells + first_cell_in_row_block[row_block_id];
  const auto last_cell = cells + first_cell_in_row_block[row_block_id + 1];
  // For reach cell of row-block only current row is being filled
  for (auto cell = first_cell; cell < last_cell; ++cell) {
    const auto& col_block = col_blocks[cell->block_id];
    const int col_block_size = col_block.size;
    int column_idx = col_block.position;
    // Column indices for each element of row_in_block row of current cell
    for (int i = 0; i < col_block_size; ++i, ++crs_position) {
      cols[crs_position] = column_idx++;
    }
  }
}

void FillCRSStructure(const int num_row_blocks,
                      const int num_rows,
                      const int* first_cell_in_row_block,
                      const Cell* cells,
                      const Block* row_blocks,
                      const Block* col_blocks,
                      int* rows,
                      int* cols,
                      cudaStream_t stream) {
  // Set number of non-zeros per row in rows array and row to row-block map in
  // row_block_ids array
  int* row_block_ids;
  cudaMallocAsync(&row_block_ids, sizeof(int) * num_rows, stream);
  const int num_blocks_blockwise = NumBlocksInGrid(num_row_blocks + 1);
  RowBlockIdAndNNZ<<<num_blocks_blockwise, kCudaBlockSize, 0, stream>>>(
      num_row_blocks,
      first_cell_in_row_block,
      cells,
      row_blocks,
      col_blocks,
      rows,
      row_block_ids);
  // Finalize row-index array of CRS strucure by computing prefix sum
  thrust::inclusive_scan(
      thrust::cuda::par_nosync.on(stream), rows, rows + num_rows + 1, rows);

  // Fill cols array of CRS structure and permutation from block-sparse to CRS
  const int num_blocks_rowwise = NumBlocksInGrid(num_rows);
  ComputeColumnsAndPermutation<<<num_blocks_rowwise,
                                 kCudaBlockSize,
                                 0,
                                 stream>>>(num_rows,
                                           first_cell_in_row_block,
                                           cells,
                                           row_blocks,
                                           col_blocks,
                                           row_block_ids,
                                           rows,
                                           cols);
  cudaFreeAsync(row_block_ids, stream);
}

template <typename T, typename Predicate>
__device__ int PartitionPoint(const T* data,
                              int first,
                              int last,
                              Predicate&& predicate) {
  if (!predicate(data[first])) {
    return first;
  }
  while (last - first > 1) {
    const auto midpoint = first + (last - first) / 2;
    if (predicate(data[midpoint])) {
      first = midpoint;
    } else {
      last = midpoint;
    }
  }
  return last;
}

// Element-wise reordering of block-sparse values
// - first_cell_pos - position of the first cell of corresponding sub-matrix;
// only used if use_first_cell is true; otherwise the first cell in row-block is
// used
// - block_sparse_values - segment of block-sparse values starting from
// block_sparse_offset, containing num_values
__global__ void PermuteToCrsKernel(
    const int block_sparse_offset,
    const int num_values,
    const int num_row_blocks,
    const int* __restrict__ first_cell_in_row_block,
    const Cell* __restrict__ cells,
    const Block* __restrict__ row_blocks,
    const Block* __restrict__ col_blocks,
    const int* __restrict__ crs_rows,
    const double* __restrict__ block_sparse_values,
    double* __restrict__ crs_values) {
  const int value_id = blockIdx.x * blockDim.x + threadIdx.x;
  if (value_id >= num_values) {
    return;
  }
  const int block_sparse_value_id = value_id + block_sparse_offset;
  // Find the corresponding row-block with a binary search
  const int row_block_id =
      PartitionPoint(
          first_cell_in_row_block,
          0,
          num_row_blocks,
          [cells,
           block_sparse_value_id] __device__(const int row_block_offset) {
            return cells[row_block_offset].position <= block_sparse_value_id;
          }) -
      1;
  // Find cell and calculate offset within the row with a linear scan
  const auto& row_block = row_blocks[row_block_id];
  const auto first_cell = cells + first_cell_in_row_block[row_block_id];
  const auto last_cell = cells + first_cell_in_row_block[row_block_id + 1];
  const int row_block_size = row_block.size;
  int num_cols_before = 0;
  for (const Cell* cell = first_cell; cell < last_cell; ++cell) {
    const auto& col_block = col_blocks[cell->block_id];
    const int col_block_size = col_block.size;
    const int cell_size = row_block_size * col_block_size;
    if (cell->position + cell_size > block_sparse_value_id) {
      const int pos_in_cell = block_sparse_value_id - cell->position;
      const int row_in_cell = pos_in_cell / col_block_size;
      const int col_in_cell = pos_in_cell % col_block_size;
      const int row = row_in_cell + row_block.position;
      crs_values[crs_rows[row] + num_cols_before + col_in_cell] =
          block_sparse_values[value_id];
      break;
    }
    num_cols_before += col_block_size;
  }
}

void PermuteToCRS(const int block_sparse_offset,
                  const int num_values,
                  const int num_row_blocks,
                  const int* first_cell_in_row_block,
                  const Cell* cells,
                  const Block* row_blocks,
                  const Block* col_blocks,
                  const int* crs_rows,
                  const double* block_sparse_values,
                  double* crs_values,
                  cudaStream_t stream) {
  const int num_blocks_valuewise = NumBlocksInGrid(num_values);
  PermuteToCrsKernel<<<num_blocks_valuewise, kCudaBlockSize, 0, stream>>>(
      block_sparse_offset,
      num_values,
      num_row_blocks,
      first_cell_in_row_block,
      cells,
      row_blocks,
      col_blocks,
      crs_rows,
      block_sparse_values,
      crs_values);
}

}  // namespace internal
}  // namespace ceres
