/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/lite/delegates/gpu/cl/kernels/softmax1x1.h"

#include <string>

#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"

namespace tflite {
namespace gpu {
namespace cl {
namespace {

std::string GetSoftmaxKernelCode(
    const OperationDef& op_def,
    const std::vector<ElementwiseOperation*>& linked_operations) {
  TensorCodeGenerator src_tensor("src_data", "tensor_size",
                                 op_def.src_tensors[0]);
  TensorCodeGenerator dst_tensor("dst_data", "tensor_size",
                                 op_def.dst_tensors[0]);

  auto read_src = [&](const std::string& x, const std::string& y,
                      const std::string& z) {
    if (op_def.batch_support) {
      return src_tensor.ReadAsFloat4D(x, y, z, "B");
    } else {
      return src_tensor.ReadAsFloat3D(x, y, z, TextureAddressMode::DONT_CARE);
    }
  };

  std::string c = GetCommonDefines(op_def.precision);
  c += "__kernel void main_function(\n";
  c += src_tensor.GetDeclaration(AccessType::READ);
  c += GetArgsDeclaration(linked_operations);
  c += dst_tensor.GetDeclaration(AccessType::WRITE) + ",\n";
  c += "    int4 tensor_size,\n";
  c += "    int2 size,\n";
  if (op_def.batch_support) {
    c += "    int BATCH_SIZE,  \n";
  }
  c += "    float4 mask\n";
  c += ") {\n";
  if (op_def.batch_support) {
    c += "  int B = get_global_id(1);\n";
    c += "  if (B >= BATCH_SIZE) return;\n";
  }
  c += "  int offset = 0;\n";
  c += "  float sum = 0.0f;\n";
  c += "  int s = 0;\n";
  c += "  int tid = get_local_id(0);\n";
  c += "  do {\n";
  c += "    int z = offset + tid;\n";
  c += "    if (z < size.x) {\n";
  c += "      float4 mask_temp = z == size.x - 1 ? mask : (float4)(1.0f);\n";
  c += "      float4 src = " + read_src("0", "0", "z") + ";\n";
  c += "      sum += dot(mask_temp, exp(src));\n";
  c += "      offset += 32;\n";
  c += "    }\n";
  c += "    s++;\n";
  c += "  } while (s < size.y);\n";
  c += "\n";
  c += "  __local float4 tmp[8];\n";
  c += "  __local float* tmpx1 = (__local float*)tmp;\n";
  c += "  tmpx1[tid] = sum;\n";
  c += "  barrier(CLK_LOCAL_MEM_FENCE);\n";
  c += "  if (tid == 0) {\n";
  c += "    sum = dot((float4)(1.0f), tmp[0]);\n";
  c += "    sum += dot((float4)(1.0f), tmp[1]);\n";
  c += "    sum += dot((float4)(1.0f), tmp[2]);\n";
  c += "    sum += dot((float4)(1.0f), tmp[3]);\n";
  c += "    sum += dot((float4)(1.0f), tmp[4]);\n";
  c += "    sum += dot((float4)(1.0f), tmp[5]);\n";
  c += "    sum += dot((float4)(1.0f), tmp[6]);\n";
  c += "    sum += dot((float4)(1.0f), tmp[7]);\n";
  c += "    tmpx1[0] = 1.0f / sum;\n";
  c += "  }\n";
  c += "  barrier(CLK_LOCAL_MEM_FENCE);\n";
  c += "  sum = tmpx1[0];\n";
  c += "\n";
  c += "  offset = 0;\n";
  c += "  s = 0;\n";
  c += "  do {\n";
  c += "    int z = offset + tid;\n";
  c += "    if (z < size.x) {\n";
  c += "      FLT4 res = TO_FLT4(exp(" + read_src("0", "0", "z") + ")*sum);\n";
  const LinkingContext context{"res", "0", "0", "z"};
  c += PostProcess(linked_operations, context);
  if (op_def.batch_support) {
    c += "    " + dst_tensor.Write4D("res", "0", "0", "z", "B");
  } else {
    c += "    " + dst_tensor.Write3D("res", "0", "0", "z");
  }
  c += "      offset += 32;\n";
  c += "    }\n";
  c += "    s++;\n";
  c += "  } while (s < size.y);\n";
  c += "}\n";
  return c;
}
}  // namespace

Softmax1x1::Softmax1x1(Softmax1x1&& kernel)
    : GPUOperation(std::move(kernel)), kernel_(std::move(kernel.kernel_)) {}

Softmax1x1& Softmax1x1::operator=(Softmax1x1&& kernel) {
  if (this != &kernel) {
    kernel_ = std::move(kernel.kernel_);
    GPUOperation::operator=(std::move(kernel));
  }
  return *this;
}

Status Softmax1x1::Compile(const CreationContext& creation_context) {
  const auto code = GetSoftmaxKernelCode(definition_, linked_operations_);
  return creation_context.cache->GetOrCreateCLKernel(
      code, "main_function", *creation_context.context,
      *creation_context.device, &kernel_);
}

Status Softmax1x1::AddToQueue(CLCommandQueue* queue) {
  kernel_.ResetBindingCounter();
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(src_[0]->GetMemoryPtr()));
  RETURN_IF_ERROR(BindArgs(&kernel_, linked_operations_));
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(dst_[0]->GetMemoryPtrForWriting()));
  RETURN_IF_ERROR(kernel_.SetBytesAuto(src_[0]->GetSizeWithDepth()));
  const int depth = src_[0]->Depth();
  RETURN_IF_ERROR(
      kernel_.SetBytesAuto(int2(depth, IntegralDivideRoundUp(depth, 32))));
  if (definition_.batch_support) {
    RETURN_IF_ERROR(kernel_.SetBytesAuto(dst_[0]->Batch()));
  }
  RETURN_IF_ERROR(
      kernel_.SetBytesAuto(GetMaskForLastPlane(src_[0]->Channels())));

  return queue->DispatchImplicit(kernel_, {32, dst_[0]->Batch(), 1},
                                 {32, 1, 1});
}

Softmax1x1 CreateSoftmax1x1(const OperationDef& definition) {
  return Softmax1x1(definition);
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
