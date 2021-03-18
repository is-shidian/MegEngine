/**
 * \file dnn/src/cuda/conv_bias/fallback_nchw_qs4.cpp
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2021 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 */

#include "./algo.h"
#include "src/cuda/utils.h"

using namespace megdnn;
using namespace cuda;

bool ConvBiasForwardImpl::AlgoFallbackNCHWQS4::is_available(
        const SizeArgs& args) const {
    if (args.bias_layout->ndim <= 0)
        return false;
    using Param = param::ConvBias;
    using Format = Param::Format;
    using Sparse = Param::Sparse;
    using Mode = Param::Mode;
    bool available = true;
    auto&& param = args.opr->param();
    auto&& fm = args.filter_meta;
    if (!conv_bias::check_bias_share_in_channel(*(args.bias_layout),
                                                param.format))
        return false;
    if (param.format != Format::NCHW)
        return false;
    UNPACK_CONV_BIAS_NCHW_PARAM(*(args.src_layout), fm, *(args.dst_layout),
                                param);
    // TODO support group conv
    available &= param.sparse == Sparse::DENSE;
    // mode must be cross correlation
    available &= param.mode == Mode::CROSS_CORRELATION;
    // check data type
    auto src_dtype = args.src_layout->dtype,
         filter_dtype = args.filter_layout->dtype,
         bias_dtype = args.bias_layout->dtype,
         dst_dtype = args.dst_layout->dtype;
    available &= (src_dtype.enumv() == DTypeEnum::QuantizedS4 &&
                  filter_dtype.enumv() == DTypeEnum::QuantizedS4 &&
                  bias_dtype.enumv() == DTypeEnum::QuantizedS32 &&
                  dst_dtype.enumv() == DTypeEnum::QuantizedS4);
    // TODO: support dialtion
    available &= dh == 1 && dw == 1;
    // ensure precomputed offsets are positive integers
    available &= hi >= fh && wi >= fw;
    // only support sm_75 or later, platform should have tensorcore int8
    // support
    available &= is_compute_capability_required(7, 5);
    // param buffer size is 4K, use 3K to store precomputed offset, fh * fw <=
    // (3*1024/4/2/2) - 1
    available &= fh * fw <= 191;
    // channels should be multiples of 64
    available &= ci % 64 == 0 && co % 64 == 0;
    return available;
}

size_t ConvBiasForwardImpl::AlgoFallbackNCHWQS4::get_workspace_in_bytes(
        const SizeArgs& args) const {
    return get_workspace_bundle(nullptr, args).total_size_in_bytes();
}

void ConvBiasForwardImpl::AlgoFallbackNCHWQS4::exec(
        const ExecArgs& args) const {
    auto layouts = make_underlying_tensor_layout(
            *(args.src_layout), *(args.filter_layout), *(args.bias_layout),
            *(args.z_layout), *(args.dst_layout));
    auto ws = get_workspace_bundle(args.workspace.raw_ptr, args);
    auto ws_src = ws.get(0);
    auto ws_filter = ws.get(1);
    auto ws_dst = ws.get(2);
    void* ws_z = nullptr;
    if (args.z_layout->ndim > 0)
        ws_z = ws.get(4);
    // auto&& stream = cuda_stream(args.opr->handle());
    auto nchw2nchw64 = [&args](const TensorND& src, TensorND&& dst) {
        if (dst.raw_ptr == nullptr)
            return;
        auto relayout = args.handle->create_operator<RelayoutFormat>();
        relayout->param() = RelayoutFormat::Param::Mode::NCHW_NCHW64;
        Workspace dummy;
        relayout->exec(src, dst, dummy);
    };
    auto nchw642nchw = [&args](const TensorND& src, TensorND&& dst) {
        auto relayout = args.handle->create_operator<RelayoutFormat>();
        relayout->param() = RelayoutFormat::Param::Mode::NCHW64_NCHW;
        Workspace dummy;
        relayout->exec(src, dst, dummy);
    };
    // reformat src
    nchw2nchw64(*(args.src_tensor), {ws_src, layouts[0]});
    // reformat filter
    nchw2nchw64(*(args.filter_tensor), {ws_filter, layouts[1]});
    // reformat z
    nchw2nchw64(*(args.z_tensor), {ws_z, layouts[3]});
    TensorND src_{ws_src, layouts[0]}, filter_{ws_filter, layouts[1]},
            bias_{args.bias_tensor->raw_ptr, layouts[2]}, z_{ws_z, layouts[3]},
            dst_{ws_dst, layouts[4]};
    ExecArgs args_{args.opr,
                   src_,
                   filter_,
                   bias_,
                   z_,
                   dst_,
                   ws.get_workspace(3),
                   args.preprocessed_filter};
    m_underlying_algo.exec(args);
    // reformat dst
    nchw642nchw(dst_, {args.dst_tensor->raw_ptr, args.dst_tensor->layout});
}

SmallVector<TensorLayout>
ConvBiasForwardImpl::AlgoFallbackNCHWQS4::make_underlying_tensor_layout(
        const TensorLayout& src, const TensorLayout& filter,
        const TensorLayout& bias, const TensorLayout& z,
        const TensorLayout& dst) const {
    size_t n = src[0], ci = src[1], hi = src[2], wi = src[3];
    size_t co = dst[1], ho = dst[2], wo = dst[3];
    size_t fh = filter[2], fw = filter[3];
    SmallVector<TensorLayout> rst;
    rst.emplace_back(TensorLayout{{n, ci / 64, hi, wi, 64}, src.dtype});
    rst.emplace_back(TensorLayout{{co, ci / 64, fh, fw, 64}, filter.dtype});
    rst.emplace_back(TensorLayout{{1, co / 64, 1, 1, 64}, bias.dtype});
    if (z.ndim > 0) {
        rst.emplace_back(TensorLayout{{n, co / 64, ho, wo, 64}, z.dtype});
    } else {
        rst.emplace_back(TensorLayout{});
    }
    rst.emplace_back(TensorLayout{{n, co / 64, ho, wo, 64}, dst.dtype});
    return rst;
}

WorkspaceBundle ConvBiasForwardImpl::AlgoFallbackNCHWQS4::get_workspace_bundle(
        void* raw_ptr, const SizeArgs& args) const {
    size_t ws_size_src = args.src_layout->span().dist_byte();
    size_t ws_size_filter = args.filter_layout->span().dist_byte();
    size_t ws_size_dst = args.dst_layout->span().dist_byte();
    auto layouts = make_underlying_tensor_layout(
            *(args.src_layout), *(args.filter_layout), *(args.bias_layout),
            *(args.z_layout), *(args.dst_layout));
    SizeArgs args_{args.opr,
                   layouts[0],
                   layouts[1],
                   layouts[2],
                   layouts[3],
                   layouts[4],
                   args.preprocessed_filter};
    size_t ws_size_underlying_algo =
            m_underlying_algo.get_workspace_in_bytes(args_);
    if (args.z_layout->ndim > 0) {
        size_t ws_size_z = args.z_layout->span().dist_byte();
        return WorkspaceBundle{raw_ptr,
                               {ws_size_src, ws_size_filter, ws_size_dst,
                                ws_size_underlying_algo, ws_size_z}};
    }
    return WorkspaceBundle{raw_ptr,
                           {ws_size_src, ws_size_filter, ws_size_dst,
                            ws_size_underlying_algo}};
}
// vim: syntax=cpp.doxygen
