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

#include <core23_helper.hpp>
#include <embeddings/distributed_slot_sparse_embedding_hash.hpp>
#include <embeddings/hybrid_sparse_embedding.hpp>
#include <embeddings/localized_slot_sparse_embedding_hash.hpp>
#include <embeddings/localized_slot_sparse_embedding_one_hot.hpp>
#include <loss.hpp>
#include <optimizer.hpp>
#include <pybind/model.hpp>
#ifdef ENABLE_MPI
#include <mpi.h>
#endif

namespace HugeCTR {

SparseEmbedding get_sparse_embedding_from_json(const nlohmann::json& j_sparse_embedding) {
  auto bottom_name = get_value_from_json<std::string>(j_sparse_embedding, "bottom");
  auto top_name = get_value_from_json<std::string>(j_sparse_embedding, "top");
  auto embedding_type_name = get_value_from_json<std::string>(j_sparse_embedding, "type");
  Embedding_t embedding_type = Embedding_t::None;
  if (!find_item_in_map(embedding_type, embedding_type_name, EMBEDDING_TYPE_MAP)) {
    HCTR_OWN_THROW(Error_t::WrongInput, "No such embedding type: " + embedding_type_name);
  }
  auto j_hparam = get_json(j_sparse_embedding, "sparse_embedding_hparam");

  if (!has_key_(j_hparam, "workspace_size_per_gpu_in_mb") &&
      !has_key_(j_hparam, "slot_size_array")) {
    HCTR_OWN_THROW(Error_t::WrongInput, "need workspace_size_per_gpu_in_mb or slot_size_array");
  }
  size_t workspace_size_per_gpu_in_mb =
      get_value_from_json_soft<size_t>(j_hparam, "workspace_size_per_gpu_in_mb", 0);

  size_t embedding_vec_size = get_value_from_json<size_t>(j_hparam, "embedding_vec_size");
  if (embedding_vec_size == 0 || embedding_vec_size > 1024) {
    HCTR_OWN_THROW(Error_t::WrongInput, "Embedding vector size(" +
                                            std::to_string(embedding_vec_size) +
                                            ") is invalid. It cannot be zero nor exceed 1024.");
  }
  if (embedding_vec_size % 32 != 0) {
    HCTR_LOG(WARNING, WORLD,
             "Embedding vector size(%zu) is not a multiple of 32, which may affect the GPU "
             "resource utilization.\n",
             embedding_vec_size);
  }

  auto combiner_str = get_value_from_json<std::string>(j_hparam, "combiner");

  std::vector<size_t> slot_size_array;
  if (has_key_(j_hparam, "slot_size_array")) {
    auto slots = get_json(j_hparam, "slot_size_array");
    assert(slots.is_array());
    for (auto slot : slots) {
      slot_size_array.emplace_back(slot.get<size_t>());
    }
  }

  std::shared_ptr<OptParamsPy> embedding_opt_params(new OptParamsPy());
  if (has_key_(j_sparse_embedding, "optimizer")) {
    auto j_optimizer = get_json(j_sparse_embedding, "optimizer");
    auto optimizer_type_name = get_value_from_json<std::string>(j_optimizer, "type");
    auto update_type_name = get_value_from_json<std::string>(j_optimizer, "update_type");
    embedding_opt_params->initialized = true;
    if (!find_item_in_map(embedding_opt_params->optimizer, optimizer_type_name,
                          OPTIMIZER_TYPE_MAP)) {
      HCTR_OWN_THROW(Error_t::WrongInput, "No such optimizer: " + optimizer_type_name);
    }
    if (!find_item_in_map(embedding_opt_params->update_type, update_type_name, UPDATE_TYPE_MAP)) {
      HCTR_OWN_THROW(Error_t::WrongInput, "No such update type: " + update_type_name);
    }
    OptHyperParams hyperparams;
    switch (embedding_opt_params->optimizer) {
      case Optimizer_t::Ftrl: {
        auto j_optimizer_hparam = get_json(j_optimizer, "ftrl_hparam");
        const float beta = get_value_from_json<float>(j_optimizer_hparam, "beta");
        const float lambda1 = get_value_from_json<float>(j_optimizer_hparam, "lambda1");
        const float lambda2 = get_value_from_json<float>(j_optimizer_hparam, "lambda2");
        hyperparams.ftrl.beta = beta;
        hyperparams.ftrl.lambda1 = lambda1;
        hyperparams.ftrl.lambda2 = lambda2;
        embedding_opt_params->hyperparams = hyperparams;
      } break;

      case Optimizer_t::Adam: {
        auto j_optimizer_hparam = get_json(j_optimizer, "adam_hparam");
        auto beta1 = get_value_from_json<float>(j_optimizer_hparam, "beta1");
        auto beta2 = get_value_from_json<float>(j_optimizer_hparam, "beta2");
        auto epsilon = get_value_from_json<float>(j_optimizer_hparam, "epsilon");
        hyperparams.adam.beta1 = beta1;
        hyperparams.adam.beta2 = beta2;
        hyperparams.adam.epsilon = epsilon;
        embedding_opt_params->hyperparams = hyperparams;
      } break;

      case Optimizer_t::AdaGrad: {
        auto j_optimizer_hparam = get_json(j_optimizer, "adagrad_hparam");
        auto initial_accu_value =
            get_value_from_json<float>(j_optimizer_hparam, "initial_accu_value");
        auto epsilon = get_value_from_json<float>(j_optimizer_hparam, "epsilon");
        hyperparams.adagrad.initial_accu_value = initial_accu_value;
        hyperparams.adagrad.epsilon = epsilon;
        embedding_opt_params->hyperparams = hyperparams;
      } break;

      case Optimizer_t::MomentumSGD: {
        auto j_optimizer_hparam = get_json(j_optimizer, "momentum_sgd_hparam");
        auto factor = get_value_from_json<float>(j_optimizer_hparam, "momentum_factor");
        hyperparams.momentum.factor = factor;
        embedding_opt_params->hyperparams = hyperparams;
      } break;

      case Optimizer_t::Nesterov: {
        auto j_optimizer_hparam = get_json(j_optimizer, "nesterov_hparam");
        auto mu = get_value_from_json<float>(j_optimizer_hparam, "momentum_factor");
        hyperparams.nesterov.mu = mu;
        embedding_opt_params->hyperparams = hyperparams;
      } break;

      case Optimizer_t::SGD: {
        auto j_optimizer_hparam = get_json(j_optimizer, "sgd_hparam");
        auto atomic_update = get_value_from_json<bool>(j_optimizer_hparam, "atomic_update");
        hyperparams.sgd.atomic_update = atomic_update;
        embedding_opt_params->hyperparams = hyperparams;
      } break;

      default: {
        assert(!"Error: no such optimizer && should never get here!");
      }
    }
  }
  HybridEmbeddingParam hybrid_embedding_param;
  hybrid_embedding_param.max_num_frequent_categories =
      get_value_from_json_soft<size_t>(j_hparam, "max_num_frequent_categories", 1);
  hybrid_embedding_param.max_num_infrequent_samples =
      get_value_from_json_soft<int64_t>(j_hparam, "max_num_infrequent_samples", -1);
  hybrid_embedding_param.p_dup_max =
      get_value_from_json_soft<double>(j_hparam, "p_dup_max", 1. / 100);
  hybrid_embedding_param.max_all_reduce_bandwidth =
      get_value_from_json_soft<double>(j_hparam, "max_all_reduce_bandwidth", 1.3e11);
  hybrid_embedding_param.max_all_to_all_bandwidth =
      get_value_from_json_soft<double>(j_hparam, "max_all_to_all_bandwidth", 1.9e11);
  hybrid_embedding_param.efficiency_bandwidth_ratio =
      get_value_from_json_soft<double>(j_hparam, "efficiency_bandwidth_ratio", 1.0);
  std::string communication_type_string =
      get_value_from_json_soft<std::string>(j_hparam, "communication_type", "IB_NVLink");
  std::string hybrid_embedding_type_string =
      get_value_from_json_soft<std::string>(j_hparam, "hybrid_embedding_type", "Distributed");
  if (!find_item_in_map(hybrid_embedding_param.communication_type, communication_type_string,
                        COMMUNICATION_TYPE_MAP)) {
    HCTR_OWN_THROW(Error_t::WrongInput, "No such communication type: " + communication_type_string);
  }
  if (!find_item_in_map(hybrid_embedding_param.hybrid_embedding_type, hybrid_embedding_type_string,
                        HYBRID_EMBEDDING_TYPE_MAP)) {
    HCTR_OWN_THROW(Error_t::WrongInput,
                   "No such hybrid embedding type: " + hybrid_embedding_type_string);
  }
  SparseEmbedding sparse_embedding = SparseEmbedding(
      embedding_type, workspace_size_per_gpu_in_mb, embedding_vec_size, combiner_str, top_name,
      bottom_name, slot_size_array, embedding_opt_params, hybrid_embedding_param);
  return sparse_embedding;
}

template <typename TypeKey, typename TypeFP>
void add_sparse_embedding(SparseEmbedding& sparse_embedding,
                          std::map<std::string, SparseInput<TypeKey>>& sparse_input_map,
                          std::vector<std::vector<TensorEntity>>& train_tensor_entries_list,
                          std::vector<std::vector<TensorEntity>>& evaluate_tensor_entries_list,
                          std::vector<std::shared_ptr<IEmbedding>>& embeddings,
                          const std::shared_ptr<ResourceManager>& resource_manager,
                          size_t batch_size, size_t batch_size_eval,
                          OptParams& embedding_opt_params,
                          std::shared_ptr<ExchangeWgrad>& exchange_wgrad, bool use_cuda_graph,
                          bool grouped_all_reduce, size_t num_iterations_statistics,
                          GpuLearningRateSchedulers& gpu_lr_sches) {
  Embedding_t embedding_type = sparse_embedding.embedding_type;
  std::string bottom_name = sparse_embedding.bottom_name;
  std::string top_name = sparse_embedding.sparse_embedding_name;
  size_t max_vocabulary_size_per_gpu = sparse_embedding.max_vocabulary_size_per_gpu;
  size_t embedding_vec_size = sparse_embedding.embedding_vec_size;
  int combiner = sparse_embedding.combiner;

  SparseInput<TypeKey> sparse_input;
  if (!find_item_in_map(sparse_input, bottom_name, sparse_input_map)) {
    HCTR_OWN_THROW(Error_t::WrongInput, "Cannot find bottom");
  }

  switch (embedding_type) {
    case Embedding_t::DistributedSlotSparseEmbeddingHash: {
      const SparseEmbeddingHashParams embedding_params = {batch_size,
                                                          batch_size_eval,
                                                          max_vocabulary_size_per_gpu,
                                                          {},
                                                          embedding_vec_size,
                                                          sparse_input.max_feature_num_per_sample,
                                                          sparse_input.slot_num,
                                                          combiner,  // combiner: 0-sum, 1-mean
                                                          embedding_opt_params};
      embeddings.emplace_back(new DistributedSlotSparseEmbeddingHash<TypeKey, TypeFP>(
          core_helper::convert_sparse_tensors23_to_sparse_tensors<TypeKey>(
              sparse_input.train_sparse_tensors),
          core_helper::convert_sparse_tensors23_to_sparse_tensors<TypeKey>(
              sparse_input.evaluate_sparse_tensors),
          embedding_params, resource_manager));
      break;
    }
    case Embedding_t::LocalizedSlotSparseEmbeddingHash: {
      const SparseEmbeddingHashParams embedding_params = {batch_size,
                                                          batch_size_eval,
                                                          max_vocabulary_size_per_gpu,
                                                          sparse_embedding.slot_size_array,
                                                          embedding_vec_size,
                                                          sparse_input.max_feature_num_per_sample,
                                                          sparse_input.slot_num,
                                                          combiner,  // combiner: 0-sum, 1-mean
                                                          embedding_opt_params};
      embeddings.emplace_back(new LocalizedSlotSparseEmbeddingHash<TypeKey, TypeFP>(
          core_helper::convert_sparse_tensors23_to_sparse_tensors<TypeKey>(
              sparse_input.train_sparse_tensors),
          core_helper::convert_sparse_tensors23_to_sparse_tensors<TypeKey>(
              sparse_input.evaluate_sparse_tensors),
          embedding_params, resource_manager));
      break;
    }
    case Embedding_t::LocalizedSlotSparseEmbeddingOneHot: {
      const SparseEmbeddingHashParams embedding_params = {batch_size,
                                                          batch_size_eval,
                                                          0,
                                                          sparse_embedding.slot_size_array,
                                                          embedding_vec_size,
                                                          sparse_input.max_feature_num_per_sample,
                                                          sparse_input.slot_num,
                                                          combiner,  // combiner: 0-sum, 1-mean
                                                          embedding_opt_params};
      embeddings.emplace_back(new LocalizedSlotSparseEmbeddingOneHot<TypeKey, TypeFP>(
          core_helper::convert_sparse_tensors23_to_sparse_tensors<TypeKey>(
              sparse_input.train_sparse_tensors),
          core_helper::convert_sparse_tensors23_to_sparse_tensors<TypeKey>(
              sparse_input.evaluate_sparse_tensors),
          embedding_params, resource_manager));
      break;
    }
    case Embedding_t::HybridSparseEmbedding: {
      auto& embed_wgrad_buff =
          (grouped_all_reduce)
              ? std::dynamic_pointer_cast<GroupedExchangeWgrad<TypeFP>>(exchange_wgrad)
                    ->get_embed_wgrad_buffs()
              : std::dynamic_pointer_cast<NetworkExchangeWgrad<TypeFP>>(exchange_wgrad)
                    ->get_embed_wgrad_buffs();

      const HybridSparseEmbeddingParams embedding_params = {
          batch_size,
          batch_size_eval,
          num_iterations_statistics,  // TBD
          sparse_embedding.hybrid_embedding_param.max_num_frequent_categories *
              std::max(batch_size, batch_size_eval),                           // TBD
          sparse_embedding.hybrid_embedding_param.max_num_infrequent_samples,  // TBD
          sparse_embedding.hybrid_embedding_param.p_dup_max,
          embedding_vec_size,
          sparse_input.slot_num,
          sparse_embedding.slot_size_array,
          sparse_embedding.hybrid_embedding_param.communication_type,
          sparse_embedding.hybrid_embedding_param.max_all_reduce_bandwidth,
          sparse_embedding.hybrid_embedding_param.max_all_to_all_bandwidth,  // TBD
          sparse_embedding.hybrid_embedding_param.efficiency_bandwidth_ratio,
          sparse_embedding.hybrid_embedding_param.hybrid_embedding_type,
          embedding_opt_params};
      embeddings.emplace_back(new HybridSparseEmbedding<TypeKey, TypeFP>(
          core_helper::convert_sparse_tensors23_to_sparse_tensors<TypeKey>(
              sparse_input.train_sparse_tensors),
          core_helper::convert_sparse_tensors23_to_sparse_tensors<TypeKey>(
              sparse_input.evaluate_sparse_tensors),
          embedding_params, embed_wgrad_buff, gpu_lr_sches, use_cuda_graph, resource_manager));
      break;
    }
    default:
      HCTR_OWN_THROW(Error_t::UnspecificError,
                     "add_sparse_embedding with no specified embedding type.");
  }  // switch

  for (size_t i = 0; i < resource_manager->get_local_gpu_count(); i++) {
    auto gpu_id = resource_manager->get_local_gpu(i)->get_device_id();
    core23::Device device(core23::DeviceType::GPU, gpu_id);
    core23::Tensor train_sparse = core_helper::convert_tensorbag_to_core23_tensor<TypeFP>(
        (embeddings.back()->get_train_output_tensors())[i], device);
    core23::Tensor eval_sparse = core_helper::convert_tensorbag_to_core23_tensor<TypeFP>(
        (embeddings.back()->get_evaluate_output_tensors())[i], device);
    train_tensor_entries_list[i].push_back({top_name, train_sparse});
    evaluate_tensor_entries_list[i].push_back({top_name, eval_sparse});
  }
}

template void add_sparse_embedding<long long, float>(
    SparseEmbedding&, std::map<std::string, SparseInput<long long>>&,
    std::vector<std::vector<TensorEntity>>&, std::vector<std::vector<TensorEntity>>&,
    std::vector<std::shared_ptr<IEmbedding>>&, const std::shared_ptr<ResourceManager>&, size_t,
    size_t, OptParams&, std::shared_ptr<ExchangeWgrad>&, bool, bool, size_t,
    GpuLearningRateSchedulers&);
template void add_sparse_embedding<long long, __half>(
    SparseEmbedding&, std::map<std::string, SparseInput<long long>>&,
    std::vector<std::vector<TensorEntity>>&, std::vector<std::vector<TensorEntity>>&,
    std::vector<std::shared_ptr<IEmbedding>>&, const std::shared_ptr<ResourceManager>&, size_t,
    size_t, OptParams&, std::shared_ptr<ExchangeWgrad>&, bool, bool, size_t,
    GpuLearningRateSchedulers&);
template void add_sparse_embedding<unsigned int, float>(
    SparseEmbedding&, std::map<std::string, SparseInput<unsigned int>>&,
    std::vector<std::vector<TensorEntity>>&, std::vector<std::vector<TensorEntity>>&,
    std::vector<std::shared_ptr<IEmbedding>>&, const std::shared_ptr<ResourceManager>&, size_t,
    size_t, OptParams&, std::shared_ptr<ExchangeWgrad>&, bool, bool, size_t,
    GpuLearningRateSchedulers&);
template void add_sparse_embedding<unsigned int, __half>(
    SparseEmbedding&, std::map<std::string, SparseInput<unsigned int>>&,
    std::vector<std::vector<TensorEntity>>&, std::vector<std::vector<TensorEntity>>&,
    std::vector<std::shared_ptr<IEmbedding>>&, const std::shared_ptr<ResourceManager>&, size_t,
    size_t, OptParams&, std::shared_ptr<ExchangeWgrad>&, bool, bool, size_t,
    GpuLearningRateSchedulers&);
}  // namespace HugeCTR
