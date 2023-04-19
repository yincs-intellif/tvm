/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*!
 * \file src/runtime/relax_vm/lm_support.cc
 * \brief Runtime to support language model related task
 *
 * Including inplace attention kv cache for runtime and simple sampler.
 *
 * This file provides a simple implementation of inplace attention
 * kv cache for relax runtime. The main goal here is to help us enable
 * auto-regressive decoding quickly in relax.
 *
 * This is not the only way to support attention kv-cache.
 * Our support of attention kv-cache can subject to future
 * changes as we build more LM verticals.
 *
 * We will keep the impact minimum by puting it as a private
 * runtime builtin provide as in this file.
 *
 * We can evolve this implementation as we build more LM verticals.
 */
#include <tvm/runtime/container/shape_tuple.h>
#include <tvm/runtime/device_api.h>
#include <tvm/runtime/logging.h>
#include <tvm/runtime/memory.h>
#include <tvm/runtime/ndarray.h>
#include <tvm/runtime/relax_vm/vm.h>

#include <cmath>

namespace tvm {
namespace runtime {
namespace relax_vm {

//-------------------------------------------
// We keep the implementation private as
// they may subject to future changes.
//
// Users can interact with it through the
// runtime API function calls
//-------------------------------------------
/*!
 * \brief An object representing an attention kv cache.
 */
class AttentionKVCacheObj : public Object {
 public:
  /*!
   * \brief Underlying support data.
   */
  NDArray data;

  /*!
   * \brief number of slots already filled.
   */
  int64_t fill_count{0};

  /*!
   * \brief View all current cached values as one array.
   * \param shape The cached values.
   */
  NDArray View(const ShapeTuple& shape) {
    CHECK_EQ(shape[0], fill_count) << "Requested shape do not match the filled count";
    for (int i = 1; i < this->data->ndim; ++i) {
      CHECK_EQ(shape[i], data->shape[i]) << "Dimension " << i << " mismatch";
    }
    return data.CreateView(shape, data->dtype);
  }

  /** Clear the cache */
  void Clear() { this->fill_count = 0; }

  /*!
   * \brief Append value to the cache.
   * \param value The value to be appended.
   */
  void Append(NDArray value) {
    CHECK(data.DataType() == value.DataType()) << "dtype mismatch";
    // reallocate cache
    int64_t reserved_slots = data->shape[0];
    while (fill_count + value->shape[0] > reserved_slots) {
      reserved_slots *= 2;
    }
    if (reserved_slots != data->shape[0]) {
      std::vector<int64_t> new_shape(data->shape, data->shape + data->ndim);
      new_shape[0] = reserved_slots;
      NDArray new_data = NDArray::Empty(new_shape, data->dtype, data->device);
      new_data.CreateView(data.Shape(), data->dtype).CopyFrom(data);
      this->data = new_data;
    }
    // copy into the fill count position.
    ICHECK_LE(fill_count + value->shape[0], data->shape[0]);
    ICHECK(data.IsContiguous());

    int64_t num_filled_elements = fill_count;
    for (int i = 1; i < data->ndim; ++i) {
      CHECK_EQ(value->shape[i], data->shape[i]) << "Dimension " << i << " mismatch";
      num_filled_elements *= data->shape[i];
    }
    // create a view of copy dest to copy the value into it.
    DLTensor copy_dst = *(data.operator->());
    copy_dst.byte_offset = num_filled_elements * ((data->dtype.bits * data->dtype.lanes + 7) / 8);
    copy_dst.shape = value->shape;
    NDArray::CopyFromTo(value.operator->(), &copy_dst);
    this->fill_count += value->shape[0];
  }

  static constexpr const uint32_t _type_index = TypeIndex::kDynamic;
  static constexpr const char* _type_key = "relax.vm.AttentionKVCache";
  TVM_DECLARE_FINAL_OBJECT_INFO(AttentionKVCacheObj, Object);
};

/*! \brief reference to closure. */
class AttentionKVCache : public ObjectRef {
 public:
  /*!
   * \brief Create the attention kv cache.
   * \param init_data The initial reserved.
   */
  static AttentionKVCache Create(NDArray init_data, ShapeTuple reserve_shape, int init_fill_count) {
    auto n = make_object<AttentionKVCacheObj>();
    n->data = NDArray::Empty(reserve_shape, init_data->dtype, init_data->device);
    n->fill_count = 0;
    n->Append(init_data);
    if (init_fill_count >= 0) {
      n->fill_count = init_fill_count;
    }
    return AttentionKVCache(n);
  }

  TVM_DEFINE_MUTABLE_OBJECT_REF_METHODS(AttentionKVCache, ObjectRef, AttentionKVCacheObj);
};

TVM_REGISTER_OBJECT_TYPE(AttentionKVCacheObj);

//-------------------------------------------------
//  Register runtime functions
//-------------------------------------------------
TVM_REGISTER_GLOBAL("vm.builtin.attention_kv_cache_create")
    .set_body_typed(AttentionKVCache::Create);

AttentionKVCache AttentionKVCacheAppend(AttentionKVCache cache, NDArray value) {
  cache->Append(value);
  return cache;
}

TVM_REGISTER_GLOBAL("vm.builtin.attention_kv_cache_append").set_body_typed(AttentionKVCacheAppend);

NDArray AttentionKVCacheView(AttentionKVCache cache, ShapeTuple shape) {
  return cache->View(shape);
}

TVM_REGISTER_GLOBAL("vm.builtin.attention_kv_cache_view").set_body_typed(AttentionKVCacheView);

void AttentionKVCacheArrayClear(Array<AttentionKVCache> caches) {
  for (AttentionKVCache cache : caches) {
    cache->Clear();
  }
}

TVM_REGISTER_GLOBAL("vm.builtin.attention_kv_cache_array_clear")
    .set_body_typed(AttentionKVCacheArrayClear);

// NOTE this is a built-in highly related to LM so we put it here.
int SampleTopPFromLogits(NDArray logits, double temperature, double top_p, double uniform_sample) {
  ICHECK(logits.IsContiguous());
  ICHECK(logits.DataType() == DataType::Float(32));

  if (logits->device.device_type != kDLCPU) {
    logits = logits.CopyTo(DLDevice{kDLCPU, 0});
  }

  ICHECK(logits->device.device_type == kDLCPU);

  for (int i = 0; i < logits->ndim - 1; ++i) {
    ICHECK_EQ(logits->shape[i], 1) << "The leading dimensions of logits must be 1";
  }

  std::vector<std::pair<float, int>> data;
  data.resize(logits->shape[logits->ndim - 1]);
  const float* plogits = static_cast<float*>(logits->data);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = std::make_pair(plogits[i], static_cast<int>(i));
  }
  // sort by logits from smallest to largest
  std::sort(data.begin(), data.end());
  float max_value = data.back().first;
  // argmax
  if (temperature < 1e-6f) {
    return data.back().second;
  }
  // compute expf
  float sum = 0.0f;
  for (size_t i = 0; i < data.size(); ++i) {
    data[i].first = expf(data[i].first - max_value);
    sum += data[i].first;
  }
  // do a cumsum in order of data
  float cum_sum_prob = 0.0f;
  float top_p_sum = 0.0f;
  for (auto rit = data.rbegin(); rit != data.rend(); ++rit) {
    float prob = rit->first / sum;
    if (cum_sum_prob < top_p) {
      top_p_sum += prob;
    }
    cum_sum_prob += prob;
    rit->first = cum_sum_prob;
  }
  // pick a number based on random in (0, 1)
  for (auto rit = data.rbegin(); rit != data.rend(); ++rit) {
    if (uniform_sample < rit->first / top_p_sum) {
      return rit->second;
    }
  }
  ICHECK_LE(uniform_sample, data[0].first);
  return data[0].second;
}

TVM_REGISTER_GLOBAL("vm.builtin.sample_top_p_from_logits").set_body_typed(SampleTopPFromLogits);

}  // namespace relax_vm
}  // namespace runtime
}  // namespace tvm