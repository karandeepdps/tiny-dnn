/*
    Copyright (c) 2016, Taiga Nomi, Edgar Riba
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

#include "tiny_cnn/core/backend.h"

#include "tiny_cnn/core/kernels/tiny_conv2d_kernel.h"
#include "tiny_cnn/core/kernels/tiny_conv2d_back_kernel.h"
#include "tiny_cnn/core/kernels/tiny_maxpool_kernel.h"
#include "tiny_cnn/core/kernels/tiny_fully_connected_kernel.h"

namespace tiny_cnn {
namespace core {

class tiny_backend : public backend {
 public:
    // context holds solution-dependent parameters
    // context should be able to hold any types of structures (like boost::any)

    // convolution
    tiny_backend(conv_params* params,
                 std::function<void(const vec_t&, int)> f1,
                 std::function<void(const vec_t&, vec_t&)> f2,
                 std::function<void(const vec_t&, const vec_t&, vec_t&)> f3,
                 std::vector<conv_layer_worker_specific_storage>* ptr)
      : params_(params)
      , conv_layer_worker_storage_(ptr),
        copy_and_pad_input(f1)
      , copy_and_unpad_delta(f2)
      , backward_activation(f3) {}

    // maxpooling
    tiny_backend(std::vector<std::vector<cnn_size_t>>* out2in,
                 std::vector<cnn_size_t>* in2out,
                 std::function<void(const vec_t&, const vec_t&, vec_t&)> f,
                 std::vector<max_pooling_layer_worker_specific_storage>* ptr)
      : max_pooling_layer_worker_storage_(ptr)
      , out2in_(out2in)
      , in2out_(in2out)
      , backward_activation(f) {}

    // fully_connected
    tiny_backend(fully_params* params,
                 std::function<void(const vec_t&, const vec_t&, vec_t&)> f)
      : params_f_(params)
      , backward_activation(f) {}

    // core math functions

    void conv2d(cnn_size_t                 index,
                const std::vector<vec_t*>& in_data,
                std::vector<vec_t*>&       out_data) {
        copy_and_pad_input(*in_data[0], static_cast<int>(index));
        const vec_t& W    = *in_data[1];
        const vec_t& bias = *in_data[2];
        vec_t&       a    = *out_data[1];
        const vec_t &in   = *((*conv_layer_worker_storage_)[index].prev_out_padded_); // input // NOLINT

        std::fill(a.begin(), a.end(), float_t(0));

        kernels::tiny_conv2d_kernel(*params_,
                                    in,
                                    W,
                                    bias,
                                    a,
                                    layer_->get_parallelize());
    }

    void conv2d(cnn_size_t                 index,
                const std::vector<vec_t*>& in_data,
                const std::vector<vec_t*>& out_data,
                std::vector<vec_t*>&       out_grad,
                std::vector<vec_t*>&       in_grad) {
        conv_layer_worker_specific_storage& cws =
            (*conv_layer_worker_storage_)[index];

        const vec_t& prev_out = *(cws.prev_out_padded_);
        const vec_t& W  = *in_data[1];
        vec_t&       dW = *in_grad[1];
        vec_t&       db = *in_grad[2];
        vec_t&       curr_delta = *out_grad[1];
        vec_t*       prev_delta = (params_->pad_type == padding::same) ?
                                   &cws.prev_delta_padded_ : in_grad[0];

        assert(W.size() == params_->weight.size());
        assert(dW.size() == params_->weight.size());
        assert(curr_delta.size() ==  layer_->out_shape()[0].size());

        backward_activation(*out_grad[0], *out_data[0], curr_delta);

        std::fill(prev_delta->begin(), prev_delta->end(), float_t(0));

        kernels::tiny_conv2d_back_kernel(*params_,
                                         prev_out,
                                         W,
                                         dW,
                                         db,
                                         curr_delta,
                                         prev_delta);

        if (params_->pad_type == padding::same) {
            copy_and_unpad_delta(cws.prev_delta_padded_, *in_grad[0]);
        }
    }

    void matmul() {
        throw nn_error("not implemented yet.");
    }

    void maxpool(cnn_size_t                 index,
                 const std::vector<vec_t*>& in_data,
                 std::vector<vec_t*>&       out_data) {
        const vec_t& in  = *in_data[0];
        vec_t&       a   = *out_data[1];
        std::vector<cnn_size_t>& max_idx =
            (*max_pooling_layer_worker_storage_)[index].out2inmax_;

        kernels::tiny_maxpool_kernel(in,
                                     a,
                                     max_idx,
                                     *out2in_,
                                     layer_->get_parallelize());
    }

    void maxpool(cnn_size_t                 index,
                 const std::vector<vec_t*>& in_data,
                 const std::vector<vec_t*>& out_data,
                 std::vector<vec_t*>&       out_grad,
                 std::vector<vec_t*>&       in_grad) {
        vec_t&       prev_delta = *in_grad[0];
        vec_t&       curr_delta = *out_grad[1];
        std::vector<cnn_size_t>& max_idx =
            (*max_pooling_layer_worker_storage_)[index].out2inmax_;

        CNN_UNREFERENCED_PARAMETER(in_data);

        backward_activation(*out_grad[0], *out_data[0], curr_delta);

        kernels::tiny_maxpool_back_kernel(prev_delta,
                                          curr_delta,
                                          max_idx,
                                          *in2out_,
                                          layer_->get_parallelize());
    }

    void fully(cnn_size_t                 index,
               const std::vector<vec_t*>& in_data,
               std::vector<vec_t*>&       out_data) {
        const vec_t& in  = *in_data[0];
        const vec_t& W   = *in_data[1];
        vec_t&       b   = *in_data[2];
        vec_t&       a   = *out_data[1];

        CNN_UNREFERENCED_PARAMETER(index);

        kernels::tiny_fully_connected_kernel(*params_f_,
                                             in,
                                             W,
                                             b,
                                             a,
                                             layer_->get_parallelize());
    }

    void fully(cnn_size_t                 index,
               const std::vector<vec_t*>& in_data,
               const std::vector<vec_t*>& out_data,
               std::vector<vec_t*>&       out_grad,
               std::vector<vec_t*>&       in_grad) {
        const vec_t& prev_out   = *in_data[0];
        const vec_t& W          = *in_data[1];
        vec_t&       dW         = *in_grad[1];
        vec_t&       db         = *in_grad[2];
        vec_t&       prev_delta = *in_grad[0];
        vec_t&       curr_delta = *out_grad[1];

        CNN_UNREFERENCED_PARAMETER(index);

        backward_activation(*out_grad[0], *out_data[0], curr_delta);

        kernels::tiny_fully_connected_back_kernel(*params_f_,
                                                  prev_out,
                                                  W,
                                                  dW,
                                                  prev_delta,
                                                  curr_delta,
                                                  db,
                                                  layer_->get_parallelize());
    }

 private:
    /* Pointer to the convolution parameters */
    conv_params* params_;
    fully_params* params_f_;

    /* Pointer to the workers */
    std::vector<conv_layer_worker_specific_storage>* conv_layer_worker_storage_;
    std::vector<max_pooling_layer_worker_specific_storage>* max_pooling_layer_worker_storage_;
    std::vector<std::vector<cnn_size_t>>* out2in_;
    std::vector<cnn_size_t>* in2out_;

    /* Pointers to parent class functions */
    std::function<void(const vec_t&, int)> copy_and_pad_input;
    std::function<void(const vec_t&, vec_t&)> copy_and_unpad_delta;
    std::function<void(const vec_t&, const vec_t&, vec_t&)> backward_activation;
};

}  // namespace core
}  // namespace tiny_cnn