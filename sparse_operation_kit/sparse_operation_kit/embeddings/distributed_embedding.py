#
# Copyright (c) 2021, NVIDIA CORPORATION.
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# 

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import tensorflow as tf

from sparse_operation_kit.kit_lib import create_embedding_sparse, plugin_sparse_fprop
from sparse_operation_kit.core.embedding_variable import EmbeddingVariable
from sparse_operation_kit.embeddings import embedding_ops

class DistributedEmbedding(tf.keras.layers.Layer):
    """
    Abbreviated as ``sok.DistributedEmbedding(*args, **kwargs)``.

    This is a wrapper class for distributed sparse embedding layer.
    It can be used to create a sparse embedding layer which will distribute
    keys based on `gpu_id = key % gpu_num` to each GPU.

    Parameters
    ----------
    combiner: string
              it is used to specify how to combine embedding vectors intra slots.
              Can be `Mean` or `Sum`.
    max_vocabulary_size_per_gpu: integer
            the first dimension of embedding variable whose shape is 
            [max_vocabulary_size_per_gpu, embedding_vec_size].
    embedding_vec_size: integer
            the second dimension of embedding variable whose shape is 
            [max_vocabulary_size_per_gpu, embedding_vec_size].
    slot_num: integer
            the number of feature-fileds which will be processed at the same time in
            each iteration, where all feature-fileds produce embedding vectors
            of the same dimension.
    max_nnz: integer
            the number of maximum valid keys in each slot (feature-filed).
    max_feature_num: integer
            **[Optional]** the maximum valid keys in each sample. It can be used to 
            save GPU memory when this statistic is known. By default, it is equal
            to :math:`max\_feature\_num=slot\_num*max\_nnz`.

    Examples
    --------
    .. code-block:: python

        emb_layer = sok.DistributedEmbedding(combiner, max_vocabulary_size_per_gpu, 
                                             embedding_vec_size, slot_num, max_nnz)
        
        @tf.function
        def _train_step(inputs, labels):
            emb_vectors = emb_layer(inputs)
            ...
        
        for i, (inputs, labels) in enumerate(dataset):
            _train_step(inputs)
    """
    def __init__(self,
                 combiner,
                 max_vocabulary_size_per_gpu,
                 embedding_vec_size,
                 slot_num,
                 max_nnz,
                 max_feature_num = 1,
                 **kwargs):
        super(DistributedEmbedding, self).__init__(**kwargs)

        self.combiner = combiner
        self.max_vocabulary_size_per_gpu = max_vocabulary_size_per_gpu
        self.embedding_vec_size = embedding_vec_size
        self.slot_num = slot_num
        self.max_nnz = max_nnz
        self.max_feature_num = max_feature_num

        self.var = EmbeddingVariable.CreateInstances(shape=[self.max_vocabulary_size_per_gpu, self.embedding_vec_size],
                                                     trainable=True)

        self.emb = create_embedding_sparse(self.var.values[0].emb_handle,
                                           input_dispatcher="all_gather_dispatcher",
                                           input_dispatcher_subsequent_ops=["csr_conversion_distributed"],
                                           embedding_executor="distributed",
                                           output_dispatcher="reduce_scatter_dispatcher",
                                           slot_num=self.slot_num, 
                                           max_nnz=self.max_nnz,
                                           max_feature_num=self.max_feature_num,
                                           combiner=self.combiner)

    @property
    def embedding_variable(self):
        return self.var

    def get_config(self):
        config = super(DistributedEmbedding, self).get_config()
        config.update({})
        return config

    def build(self, input_shape):
        pass

    @tf.function
    def call(self, inputs, training=True):
        """
        The forward logic of this wrapper class.

        Parameters
        ----------
        inputs: tf.sparse.SparseTensor
                keys are stored in SparseTensor.values. SparseTensor.dense_shape is 
                2-dim and denotes [batchsize * slot_num, max_nnz]. Therefore, the rank
                of SparseTensor.indices must be 2 which denotes [row-indices, column-indices]
                in the corresponding dense tensor.

        training: boolean
                whether training or not.

        Returns
        -------
        emb_vector: tf.float
                the embedding vectors for the input keys. Its shape is
                *[batchsize, slot_num, embedding_vec_size]*
        """
        if not isinstance(inputs, tf.SparseTensor):
            raise TypeError("inputs must be SparseTensor")

        values = inputs.values
        row_indices = tf.transpose(inputs.indices, perm=[1, 0])[0]

        # option 2, return grad for self.emb
        emb_vector = plugin_sparse_fprop(self.emb, 
                                         self.var,
                                         values, row_indices, 
                                         embedding_ops.get_global_replica_id(),
                                         slot_num=self.slot_num,
                                         training=training, 
                                         unique_op_name=self.var.name)
        return emb_vector
