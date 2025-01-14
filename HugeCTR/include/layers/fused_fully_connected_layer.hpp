/*
 * Copyright (c) 2023, NVIDIA CORPORATION.
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

#include <cublas_v2.h>

#include <core23/tensor.hpp>
#include <functional>
#include <layer.hpp>
#include <trainable_layer.hpp>
#include <vector>

namespace HugeCTR {

/**
 * @brief
 * This class implements the fully connected layer.
 */
class FusedFullyConnectedLayer : public TrainableLayer<__half> {
  // Optimized cublasGemmEx algorithm selection
  cublasGemmAlgo_t falgo_k_{CUBLAS_GEMM_DEFAULT};
  cublasGemmAlgo_t balgo_k_{CUBLAS_GEMM_DEFAULT};
  cublasGemmAlgo_t balgo_x_{CUBLAS_GEMM_DEFAULT};

  /*
   * stores the references to the intermediate top tensors of this layer.
   */
  core23::Tensor middle_tensor_;

  /*
   * stores the references to the intermediate bias grad tensors of this layer.
   */
  core23::Tensor /*<float>*/ bias_grad_tensor_;

  std::unique_ptr<DataSimulator> get_uniform_initializer(const int index) override;
  std::unique_ptr<DataSimulator> get_xavier_uniform_initializer(const int index) override;
  std::unique_ptr<DataSimulator> get_xavier_norm_initializer(const int index) override;
  std::unique_ptr<DataSimulator> get_default_initializer(const int index) override;

  core23::Tensor& get_bottom_tensor(bool is_train) { return this->input_tensors_[0]; }

 public:
  /**
   * forward pass
   */
  void fprop(bool is_train) final;
  /**
   * backward pass
   */
  void bprop() final;
  /*
   * algorithm search for cublasGemmEx
   */
  void search_algorithm() final;
  /**
   * This is the constructor of the FullyConnectedLayer.
   * It will check whether the format combination of all tensors is supported or not.
   * Only two kinds of tensor formats are supported:
   * (1) weight, input, output, wgrad are all in row-major.
   * (2) weight, input, output, wgrad are all in column-major.
   * @param bottom_tensor: stores the tensor from bottom layer
   * @param top_tensor: stores the tensor to top layer
   * @param tensor_format: specifies the format of the weight tensor, either HW (row major) or WH
   * (col-major)
   */
  FusedFullyConnectedLayer(
      const core23::Tensor& bottom_tensor, const core23::Tensor& top_tensor,
      const std::shared_ptr<GPUResource>& gpu_resource,
      std::vector<Initializer_t> initializer_types = std::vector<Initializer_t>());
  FusedFullyConnectedLayer(const FusedFullyConnectedLayer&) = delete;
  FusedFullyConnectedLayer& operator=(const FusedFullyConnectedLayer&);
};

}  // namespace HugeCTR
