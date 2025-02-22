// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <arm_neon.h>
#include "lite/backends/arm/math/conv_block_utils.h"
#include "lite/backends/arm/math/conv_depthwise.h"
#include "lite/backends/arm/math/conv_impl.h"
#include "lite/core/context.h"
#include "lite/operators/op_params.h"
#ifdef ARM_WITH_OMP
#include <omp.h>
#endif

namespace paddle {
namespace lite {
namespace arm {
namespace math {

#define ROUNDUP(a, b) ((((a) + (b)-1) / (b)) * (b))

template <typename Dtype>
void conv_depthwise_3x3s1_int8(Dtype* dout,
                               const int8_t* din,
                               const int8_t* weights,
                               const float* scale,
                               const float* bias,
                               bool flag_bias,
                               bool flag_relu,
                               int num,
                               int chin,
                               int hin,
                               int win,
                               int hout,
                               int wout,
                               int padw,
                               int padh,
                               ARMContext* ctx) {
  const int threads = ctx->threads();
  int llc_size = ctx->llc_size() / 4;

  const int hout_c_block = 8;
  const int hout_r_kernel = 1;
  const int wout_block = 4;
  const int wout_round = ((wout + wout_block - 1) / wout_block) * wout_block;
  const int win_round = wout_round + 2;

  //! get h block
  //! llc_size = threads * win_round * hin_r_block * sizeof(int8_t) + wout_round
  //! * hout_c_block * hout_r_block * threads * sizeof(int32_t)
  //! win_round = wout_round + 2
  //! hin_r_block = hout_r_block + 2
  int hout_r_block =
      (llc_size - 2 * win_round * threads) /
      (win_round * threads + hout_c_block * wout_round * threads * 4);
  hout_r_block = hout_r_block > hout ? hout : hout_r_block;
  hout_r_block =
      ((hout_r_block + hout_r_kernel - 1) / hout_r_kernel) * hout_r_kernel;
  hout_r_block = hout_r_block < hout_r_kernel ? hout_r_kernel : hout_r_block;

  const int hin_r_block = hout_r_block + 2;

  auto tmp_work_space = ctx->workspace_data<int8_t>();
  int8_t ptr_zero[win_round];  // NOLINT
  memset(ptr_zero, 0, sizeof(int8_t) * win_round);
  Dtype ptr_write[wout_round];  // NOLINT

  int in_len = win_round * hout_c_block;
  int pre_in_size = hin_r_block * in_len;
  pre_in_size = ROUNDUP(pre_in_size, 4);
  int pre_out_size = hout_c_block * hout_r_block * wout_round;

  int8_t* tmp_din = tmp_work_space;

  int size_in_channel = win * hin;
  int size_out_channel = wout * hout;
  int w_stride = 9;  // kernel_w * kernel_h;

  int ws = -padw;
  int we = ws + win_round;
  int w_loop = wout_round / 4;
  int chout = chin;

  int out_row_stride = hout_c_block * wout_round;

  for (int n = 0; n < num; ++n) {
    const int8_t* din_batch = din + n * chin * size_in_channel;
    int8_t* dout_batch = reinterpret_cast<int8_t*>(dout) +
                         n * chout * size_out_channel * sizeof(Dtype);
    for (int h = 0; h < hout; h += hout_r_block) {
      int h_kernel = hout_r_block;
      if (h + hout_r_block > hout) {
        h_kernel = hout - h;
      }
      int hs = h - padh;
      int he = hs + h_kernel + 2;

#pragma omp parallel for num_threads(threads)
      for (int c = 0; c < chout; c += hout_c_block) {
#ifdef ARM_WITH_OMP
        int8_t* pre_din =
            tmp_din + omp_get_thread_num() * (pre_in_size + pre_out_size * 4);
        int32_t* pre_out = reinterpret_cast<int*>(pre_din + pre_in_size);
#else
        int32_t* pre_out = reinterpret_cast<int32_t*>(tmp_din + pre_in_size);
        auto pre_din = tmp_din;
#endif
        prepack_input_nxw_c8_int8(din_batch,
                                  pre_din,
                                  c,
                                  c + hout_c_block,
                                  hs,
                                  he,
                                  ws,
                                  we,
                                  chin,
                                  win,
                                  hin);
        const int8_t* block_inr0 = pre_din;
        const int8_t* block_inr1 = block_inr0 + in_len;
        const int8_t* block_inr2 = block_inr1 + in_len;

        const int8_t* weight_c = weights + c * w_stride;
        float bias_local[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        if (flag_bias) {
          bias_local[0] = bias[c];
          bias_local[1] = bias[c + 1];
          bias_local[2] = bias[c + 2];
          bias_local[3] = bias[c + 3];
          bias_local[4] = bias[c + 4];
          bias_local[5] = bias[c + 5];
          bias_local[6] = bias[c + 6];
          bias_local[7] = bias[c + 7];
        }
#ifdef __aarch64__
        int8x8_t vw0 = vld1_s8(weight_c);
        int8x8_t vw1 = vld1_s8(weight_c + 8);
        int8x8_t vw2 = vld1_s8(weight_c + 16);
        int8x8_t vw3 = vld1_s8(weight_c + 24);
        int8x8_t vw4 = vld1_s8(weight_c + 32);
        int8x8_t vw5 = vld1_s8(weight_c + 40);
        int8x8_t vw6 = vld1_s8(weight_c + 48);
        int8x8_t vw7 = vld1_s8(weight_c + 56);
        int8x8_t vw8 = vld1_s8(weight_c + 64);
#endif
        for (int hk = 0; hk < h_kernel; hk += hout_r_kernel) {
          int cnt = w_loop;
          const int8_t* inr0 = block_inr0;
          const int8_t* inr1 = block_inr1;
          const int8_t* inr2 = block_inr2;
          int32_t* ptr_out0 = pre_out + hk * out_row_stride;
#ifdef __aarch64__
          asm volatile(
              "ld1  {v0.8b, v1.8b, v2.8b, v3.8b}, [%[r0]], #32\n"
              "1:\n"
              /* inr0 -> outr0 */
              "ldp  d4, d5, [%[r0]]\n"           /* load r0, 4 */
              "smull v20.8h, v0.8b,  %[w0].8b\n" /* int16, out0 */
              "smull v21.8h, v1.8b,  %[w0].8b\n" /* int16, out1 */
              "smull v22.8h, v2.8b,  %[w0].8b\n" /* int16, out2 */
              "smull v23.8h, v3.8b,  %[w0].8b\n" /* int16, out3 */
              "smlal v20.8h, v1.8b,  %[w1].8b\n" /* int16, out0 */
              "smlal v21.8h, v2.8b,  %[w1].8b\n" /* int16, out1 */
              "smlal v22.8h, v3.8b,  %[w1].8b\n" /* int16, out2 */
              "smlal v23.8h, v4.8b,  %[w1].8b\n" /* int16, out3 */
              "ldp  d0, d1, [%[r1]], #16\n"      /* load r1, 0,1 */
              "sxtl  v24.4s, v20.4h\n"
              "sxtl2 v25.4s, v20.8h\n"
              "sxtl  v26.4s, v21.4h\n"
              "sxtl2 v27.4s, v21.8h\n"
              "sxtl  v28.4s, v22.4h\n"
              "sxtl2 v29.4s, v22.8h\n"
              "sxtl  v30.4s, v23.4h\n"
              "sxtl2 v31.4s, v23.8h\n"
              "smull v20.8h, v2.8b,  %[w2].8b\n" /* int16, out0 */
              "smull v21.8h, v3.8b,  %[w2].8b\n" /* int16, out1 */
              "smull v22.8h, v4.8b,  %[w2].8b\n" /* int16, out2 */
              "smull v23.8h, v5.8b,  %[w2].8b\n" /* int16, out3 */
              "ldp  d2, d3, [%[r1]], #16\n"      /* load r1, 2,3 */
              "smlal v20.8h, v0.8b,  %[w3].8b\n" /* int16, out0 */
              "smlal v21.8h, v1.8b,  %[w3].8b\n" /* int16, out1 */
              "smlal v22.8h, v2.8b,  %[w3].8b\n" /* int16, out2 */
              "smlal v23.8h, v3.8b,  %[w3].8b\n" /* int16, out3 */
              "saddw  v24.4s, v24.4s, v20.4h\n"
              "saddw2 v25.4s, v25.4s, v20.8h\n"
              "saddw  v26.4s, v26.4s, v21.4h\n"
              "saddw2 v27.4s, v27.4s, v21.8h\n"
              "ldp  d4, d5, [%[r1]]\n" /* load r1, 4,5 */
              "saddw  v28.4s, v28.4s, v22.4h\n"
              "saddw2 v29.4s, v29.4s, v22.8h\n"
              "saddw  v30.4s, v30.4s, v23.4h\n"
              "saddw2 v31.4s, v31.4s, v23.8h\n"
              "smull v20.8h, v1.8b,  %[w4].8b\n" /* int16, out0 */
              "smull v21.8h, v2.8b,  %[w4].8b\n" /* int16, out1 */
              "smull v22.8h, v3.8b,  %[w4].8b\n" /* int16, out1 */
              "smull v23.8h, v4.8b,  %[w4].8b\n" /* int16, out1 */
              "ldp  d0, d1, [%[r2]], #16\n"      /* load r2, 0,1 */
              "smlal v20.8h, v2.8b,  %[w5].8b\n" /* int16, out0 */
              "smlal v21.8h, v3.8b,  %[w5].8b\n" /* int16, out1 */
              "smlal v22.8h, v4.8b,  %[w5].8b\n" /* int16, out2 */
              "smlal v23.8h, v5.8b,  %[w5].8b\n" /* int16, out3 */
              "ldp  d2, d3, [%[r2]], #16\n"      /* load r2, 2,3 */
              "saddw  v24.4s, v24.4s, v20.4h\n"
              "saddw2 v25.4s, v25.4s, v20.8h\n"
              "saddw  v26.4s, v26.4s, v21.4h\n"
              "saddw2 v27.4s, v27.4s, v21.8h\n"
              "ldp  d4, d5, [%[r2]]\n" /* load r2 */
              "saddw  v28.4s, v28.4s, v22.4h\n"
              "saddw2 v29.4s, v29.4s, v22.8h\n"
              "saddw  v30.4s, v30.4s, v23.4h\n"
              "saddw2 v31.4s, v31.4s, v23.8h\n"
              "smull v20.8h, v0.8b,  %[w6].8b\n" /* int16, out0 */
              "smull v21.8h, v1.8b,  %[w6].8b\n" /* int16, out1 */
              "smull v22.8h, v2.8b,  %[w6].8b\n" /* int16, out1 */
              "smull v23.8h, v3.8b,  %[w6].8b\n" /* int16, out1 */
              "smlal v20.8h, v1.8b,  %[w7].8b\n" /* int16, out0 */
              "smlal v21.8h, v2.8b,  %[w7].8b\n" /* int16, out1 */
              "smlal v22.8h, v3.8b,  %[w7].8b\n" /* int16, out1 */
              "smlal v23.8h, v4.8b,  %[w7].8b\n" /* int16, out1 */
              "ldp  d0, d1, [%[r0]], #16\n"      /* load r0, 0,1 */
              "saddw  v24.4s, v24.4s, v20.4h\n"
              "saddw2 v25.4s, v25.4s, v20.8h\n"
              "saddw  v26.4s, v26.4s, v21.4h\n"
              "saddw2 v27.4s, v27.4s, v21.8h\n"
              "saddw  v28.4s, v28.4s, v22.4h\n"
              "saddw2 v29.4s, v29.4s, v22.8h\n"
              "saddw  v30.4s, v30.4s, v23.4h\n"
              "saddw2 v31.4s, v31.4s, v23.8h\n"
              "smull v20.8h, v2.8b,  %[w8].8b\n" /* int16, out0 */
              "smull v21.8h, v3.8b,  %[w8].8b\n" /* int16, out1 */
              "smull v22.8h, v4.8b,  %[w8].8b\n" /* int16, out1 */
              "smull v23.8h, v5.8b,  %[w8].8b\n" /* int16, out1 */
              "ldp  d2, d3, [%[r0]], #16\n"      /* load r0, 2,3 */
              "saddw  v24.4s, v24.4s, v20.4h\n"
              "saddw2 v25.4s, v25.4s, v20.8h\n"
              "saddw  v26.4s, v26.4s, v21.4h\n"
              "saddw2 v27.4s, v27.4s, v21.8h\n"
              "stp    q24, q25, [%[ptr_out0]], #32\n"
              "saddw  v28.4s, v28.4s, v22.4h\n"
              "saddw2 v29.4s, v29.4s, v22.8h\n"
              "stp    q26, q27, [%[ptr_out0]], #32\n"
              "saddw  v30.4s, v30.4s, v23.4h\n"
              "saddw2 v31.4s, v31.4s, v23.8h\n"
              "subs    %w[cnt], %w[cnt], #1\n"
              "stp    q28, q29, [%[ptr_out0]], #32\n"
              "stp    q30, q31, [%[ptr_out0]], #32\n"
              "bne    1b\n"
              : [cnt] "+r"(cnt),
                [r0] "+r"(inr0),
                [r1] "+r"(inr1),
                [r2] "+r"(inr2),
                [ptr_out0] "+r"(ptr_out0)
              : [w0] "w"(vw0),
                [w1] "w"(vw1),
                [w2] "w"(vw2),
                [w3] "w"(vw3),
                [w4] "w"(vw4),
                [w5] "w"(vw5),
                [w6] "w"(vw6),
                [w7] "w"(vw7),
                [w8] "w"(vw8)
              : "cc",
                "memory",
                "v0",
                "v1",
                "v2",
                "v3",
                "v4",
                "v5",
                "v20",
                "v21",
                "v22",
                "v23",
                "v24",
                "v25",
                "v26",
                "v27",
                "v28",
                "v29",
                "v30",
                "v31"

              );
#else
          auto wptr = weight_c;
          asm volatile(
              "vld1.32    {d0-d3}, [%[r0]]!\n"   /* load r0, 0-4 */
              "vld1.32    {d6-d7}, [%[wptr]]!\n" /* load w0-w1 */
              "1:\n"
              /* inr0 -> outr0 */
              "vld1.32    {d4-d5}, [%[r0]]\n"   /* load r0, 5-6 */
              "vmull.s8 q4, d0,  d6\n"          /* int16, out0 */
              "vmull.s8 q5, d1,  d6\n"          /* int16, out1 */
              "vmull.s8 q6, d2,  d6\n"          /* int16, out2 */
              "vmull.s8 q7, d3,  d6\n"          /* int16, out3 */
              "vld1.32    {d6},   [%[wptr]]!\n" /* load w2 */
              "vmlal.s8 q4, d1,  d7\n"          /* int16, out0 */
              "vmlal.s8 q5, d2,  d7\n"          /* int16, out1 */
              "vmlal.s8 q6, d3,  d7\n"          /* int16, out2 */
              "vmlal.s8 q7, d4,  d7\n"          /* int16, out3 */
              "vld1.32    {d7},   [%[wptr]]!\n" /* load w3 */
              "vmovl.s16  q8, d8\n"
              "vmovl.s16  q9, d9\n"
              "vmovl.s16  q10, d10\n"
              "vmovl.s16  q11, d11\n"
              "vld1.32    {d0-d1}, [%[r1]]!\n" /* load r1, 0-1 */
              "vmovl.s16  q12, d12\n"
              "vmovl.s16  q13, d13\n"
              "vmovl.s16  q14, d14\n"
              "vmovl.s16  q15, d15\n"
              "vmull.s8 q4, d2,  d6\n"          /* int16, out0 */
              "vmull.s8 q5, d3,  d6\n"          /* int16, out1 */
              "vld1.32    {d2-d3}, [%[r1]]!\n"  /* load r1, 2-3 */
              "vmull.s8 q6, d4,  d6\n"          /* int16, out2 */
              "vmull.s8 q7, d5,  d6\n"          /* int16, out3 */
              "vld1.32    {d6},   [%[wptr]]!\n" /* load w4 */
              /* inr1 -> outr0 */
              "vmlal.s8 q4, d0,  d7\n"        /* int16, out0 */
              "vmlal.s8 q5, d1,  d7\n"        /* int16, out1 */
              "vmlal.s8 q6, d2,  d7\n"        /* int16, out2 */
              "vmlal.s8 q7, d3,  d7\n"        /* int16, out3 */
              "vld1.32    {d4-d5}, [%[r1]]\n" /* load r1, 4-5 */
              "vaddw.s16  q8, q8, d8\n"
              "vaddw.s16  q9, q9, d9\n"
              "vaddw.s16  q10, q10, d10\n"
              "vaddw.s16  q11, q11, d11\n"
              "vld1.32    {d7},   [%[wptr]]!\n" /* load w5 */
              "vaddw.s16  q12, q12, d12\n"
              "vaddw.s16  q13, q13, d13\n"
              "vaddw.s16  q14, q14, d14\n"
              "vaddw.s16  q15, q15, d15\n"
              "vmull.s8 q4, d1,  d6\n"          /* int16, out0 */
              "vmull.s8 q5, d2,  d6\n"          /* int16, out1 */
              "vmull.s8 q6, d3,  d6\n"          /* int16, out2 */
              "vmull.s8 q7, d4,  d6\n"          /* int16, out3 */
              "vld1.32    {d6},   [%[wptr]]!\n" /* load w6 */
              "vld1.32    {d0-d1}, [%[r2]]!\n"  /* load r2, 0-1 */
              "vmlal.s8 q4, d2,  d7\n"          /* int16, out0 */
              "vmlal.s8 q5, d3,  d7\n"          /* int16, out1 */
              "vmlal.s8 q6, d4,  d7\n"          /* int16, out2 */
              "vmlal.s8 q7, d5,  d7\n"          /* int16, out3 */
              "vld1.32    {d7},   [%[wptr]]!\n" /* load w7 */
              "vaddw.s16  q8, q8, d8\n"
              "vaddw.s16  q9, q9, d9\n"
              "vaddw.s16  q10, q10, d10\n"
              "vaddw.s16  q11, q11, d11\n"
              "vld1.32    {d2-d3}, [%[r2]]!\n" /* load r2, 2-3 */
              "vaddw.s16  q12, q12, d12\n"
              "vaddw.s16  q13, q13, d13\n"
              "vaddw.s16  q14, q14, d14\n"
              "vaddw.s16  q15, q15, d15\n"
              "vld1.32    {d4-d5}, [%[r2]]\n" /* load r2, 4-5 */
              /* inr2 -> outr0 */
              "vmull.s8 q4, d0,  d6\n"          /* int16, out0 */
              "vmull.s8 q5, d1,  d6\n"          /* int16, out1 */
              "vmull.s8 q6, d2,  d6\n"          /* int16, out2 */
              "vmull.s8 q7, d3,  d6\n"          /* int16, out3 */
              "vld1.32    {d6},   [%[wptr]]!\n" /* load w8 */
              "vmlal.s8 q4, d1,  d7\n"          /* int16, out0 */
              "vmlal.s8 q5, d2,  d7\n"          /* int16, out1 */
              "vmlal.s8 q6, d3,  d7\n"          /* int16, out2 */
              "vmlal.s8 q7, d4,  d7\n"          /* int16, out3 */
              "vaddw.s16  q8, q8, d8\n"
              "vaddw.s16  q9, q9, d9\n"
              "vaddw.s16  q10, q10, d10\n"
              "vaddw.s16  q11, q11, d11\n"
              "vld1.32    {d0-d1}, [%[r0]]!\n" /* load r0, 0-1 */
              "vaddw.s16  q12, q12, d12\n"
              "vaddw.s16  q13, q13, d13\n"
              "vaddw.s16  q14, q14, d14\n"
              "vaddw.s16  q15, q15, d15\n"
              "sub %[wptr],   %[wptr],    #72\n"
              "vmull.s8 q4, d2,  d6\n"         /* int16, out0 */
              "vmull.s8 q5, d3,  d6\n"         /* int16, out1 */
              "vmull.s8 q6, d4,  d6\n"         /* int16, out2 */
              "vmull.s8 q7, d5,  d6\n"         /* int16, out3 */
              "vld1.32    {d2-d3}, [%[r0]]!\n" /* load r0, 2-3 */
              "vaddw.s16  q8, q8, d8\n"
              "vaddw.s16  q9, q9, d9\n"
              "vaddw.s16  q10, q10, d10\n"
              "vaddw.s16  q11, q11, d11\n"
              "vst1.32    {d16-d19},  [%[ptr_out0]]!\n"
              "vld1.32    {d6-d7},   [%[wptr]]!\n" /* load w0-w1 */
              "vaddw.s16  q12, q12, d12\n"
              "vaddw.s16  q13, q13, d13\n"
              "vst1.32    {d20-d23},  [%[ptr_out0]]!\n"
              "vaddw.s16  q14, q14, d14\n"
              "vaddw.s16  q15, q15, d15\n"
              "subs    %[cnt], #1\n"
              "vst1.32    {d24-d27},  [%[ptr_out0]]!\n"
              "vst1.32    {d28-d31},  [%[ptr_out0]]!\n"
              "bne    1b\n"
              : [cnt] "+r"(cnt),
                [r0] "+r"(inr0),
                [r1] "+r"(inr1),
                [r2] "+r"(inr2),
                [ptr_out0] "+r"(ptr_out0),
                [wptr] "+r"(wptr)
              :
              : "cc",
                "memory",
                "q0",
                "q1",
                "q2",
                "q3",
                "q4",
                "q5",
                "q6",
                "q7",
                "q8",
                "q9",
                "q10",
                "q11",
                "q12",
                "q13",
                "q14",
                "q15");
#endif
          block_inr0 = block_inr1;
          block_inr1 = block_inr2;
          block_inr2 = block_inr1 + in_len;
        }
        write_int32_nchwc8_to_nchw<Dtype>(pre_out,
                                          reinterpret_cast<Dtype*>(dout_batch),
                                          c,
                                          c + hout_c_block,
                                          h,
                                          h + h_kernel,
                                          0,
                                          wout_round,
                                          chout,
                                          hout,
                                          wout,
                                          flag_relu,
                                          bias_local,
                                          flag_bias,
                                          ptr_write,
                                          scale + c);
      }
    }
  }
}

template void conv_depthwise_3x3s1_int8<int8_t>(int8_t* dout,
                                                const int8_t* din,
                                                const int8_t* weights,
                                                const float* scale,
                                                const float* bias,
                                                bool flag_bias,
                                                bool flag_relu,
                                                int num,
                                                int chin,
                                                int hin,
                                                int win,
                                                int hout,
                                                int wout,
                                                int padw,
                                                int padh,
                                                ARMContext* ctx);

template void conv_depthwise_3x3s1_int8<float>(float* dout,
                                               const int8_t* din,
                                               const int8_t* weights,
                                               const float* scale,
                                               const float* bias,
                                               bool flag_bias,
                                               bool flag_relu,
                                               int num,
                                               int chin,
                                               int hin,
                                               int win,
                                               int hout,
                                               int wout,
                                               int padw,
                                               int padh,
                                               ARMContext* ctx);
}  // namespace math
}  // namespace arm
}  // namespace lite
}  // namespace paddle
