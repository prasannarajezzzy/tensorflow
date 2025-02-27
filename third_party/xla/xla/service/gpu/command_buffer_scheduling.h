/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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
#ifndef XLA_SERVICE_GPU_COMMAND_BUFFER_SCHEDULING_H_
#define XLA_SERVICE_GPU_COMMAND_BUFFER_SCHEDULING_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/ir/hlo_schedule.h"
#include "xla/service/hlo_pass_interface.h"
#include "xla/status.h"
#include "xla/statusor.h"

namespace xla::gpu {

// Lift fusion instructions to command buffers.
//
// Before the pass:
//   %fused_computation (param_0: s32[], param_1: s32[]) -> s32[] {
//     ...
//   }
//
//   ENTRY %main (a: s32[], b: s32[]) -> s32[] {
//     %a = s32[] parameter(0)
//     %b = s32[] parameter(1)
//     ROOT %fusion = s32[] fusion(s32[] %a, s32[] %b), kind=kLoop,
//       calls=%fused_computation
//   }
//
// After the pass:
//   %fused_computation (param_0: s32[], param_1: s32[]) -> s32[] {
//     ...
//   }
//
//   %command_buffer (param_0: s32[], param_1: s32[]) -> s32[] {
//     %param_0 = s32[] parameter(0)
//     %param_1 = s32[] parameter(1)
//     ROOT %fusion = s32[] fusion(s32[] %param_0, s32[] %param_1), kind=kLoop,
//       calls=%fused_computation
//   }
//
//   ENTRY %main (a: s32[], b: s32[]) -> s32[] {
//     %a = s32[] parameter(0)
//     %b = s32[] parameter(1)
//     ROOT %call = s32[] call(s32[] %a, s32[] %b), to_apply=%command_buffer
//  }
//
// We currently do not have a command_buffer HLO operation, so we'll start with
// a kCall op code with an attached HLO computation. We'll consider graduating
// custom call to a first class operation later.
class CommandBufferScheduling : public HloModulePass {
 public:
  // DebugOptions control which commands are enabled. Long term we want to
  // remove that flag and enable all supported commands by default.
  using CommandBufferConfig =
      absl::flat_hash_set<DebugOptions::CommandBufferCmdType>;

  CommandBufferScheduling(int32_t gpu_toolkit_version,
                          int32_t gpu_driver_version);

  absl::string_view name() const override {
    return "command-buffer-scheduling";
  }

  using HloPassInterface::Run;
  StatusOr<bool> Run(
      HloModule* module,
      const absl::flat_hash_set<absl::string_view>& execution_threads) override;

  static std::vector<HloInstructionSequence> CollectCommandBufferSequences(
      HloInstructionSequence inst_sequence, const CommandBufferConfig& config,
      absl::flat_hash_set<HloComputation*>& processed_command_buffers,
      int32_t min_num_commands = 1);

  // Moves kParameter and kConstant instructions in a computation to
  // the beginning of the computation. This simplifies the construction of
  // command buffer computations because we don't need to deal with parameters
  // and constants that have users outside of a command buffer.
  static Status MoveParametersAndConstantsToFront(HloComputation* computation);

  struct CommandBuffer {
    // Command buffer arguments (call instruction arguments).
    std::vector<HloInstruction*> arguments;

    // Command buffer result (call instruction result tuple).
    std::vector<HloInstruction*> results;

    // Hlo computation corresponding to a command buffer body.
    std::unique_ptr<HloComputation> computation;

    // Mapping from original instruction to their clones in the command buffer.
    absl::flat_hash_map<HloInstruction*, HloInstruction*> inst_mapping;
  };

  // Prepares a command buffer from the instruction sequence. Used values
  // constructed by instructions outside of the sequence are passed in as
  // parameters. Results of instructions in the sequence are returned in a tuple
  // (if command buffer has a single result we don't wrap it into tuple).
  static StatusOr<CommandBuffer> PrepareCommandBuffer(
      const HloInstructionSequence& seq);

  // Rewrites prepared command buffer computation into Hlo operations in the
  // parent computation (calls command buffer and replaced all users).
  static Status RewriteCommandBuffer(HloComputation* parent,
                                     const HloInstructionSequence& seq,
                                     CommandBuffer command_buffer);

 private:
  // For NVIDIA gpus XLA can be compiled with a CUDA version that is larger than
  // the version supported by the driver, e.g. we can compile for CUDA 12.3 but
  // have 12.1 driver installed. When deciding what command buffer features we
  // can use we have to consider both versions.
  int32_t gpu_toolkit_version_;
  int32_t gpu_driver_version_;
};

}  // namespace xla::gpu

#endif  // XLA_SERVICE_GPU_COMMAND_BUFFER_SCHEDULING_H_
