// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/baseline/liftoff-assembler.h"

#include "src/assembler-inl.h"
#include "src/compiler/linkage.h"
#include "src/compiler/wasm-compiler.h"
#include "src/counters.h"
#include "src/macro-assembler-inl.h"
#include "src/wasm/function-body-decoder-impl.h"
#include "src/wasm/wasm-opcodes.h"

namespace v8 {
namespace internal {
namespace wasm {

constexpr auto kRegister = LiftoffAssembler::VarState::kRegister;
constexpr auto kConstant = LiftoffAssembler::VarState::kConstant;
constexpr auto kStack = LiftoffAssembler::VarState::kStack;

namespace {

#define __ asm_->

#define TRACE(...)                                            \
  do {                                                        \
    if (FLAG_trace_liftoff) PrintF("[liftoff] " __VA_ARGS__); \
  } while (false)

class LiftoffCompiler {
 public:
  MOVE_ONLY_NO_DEFAULT_CONSTRUCTOR(LiftoffCompiler);

  // TODO(clemensh): Make this a template parameter.
  static constexpr wasm::Decoder::ValidateFlag validate =
      wasm::Decoder::kValidate;

  using Value = ValueBase;

  struct Control : public ControlWithNamedConstructors<Control, Value> {
    MOVE_ONLY_WITH_DEFAULT_CONSTRUCTORS(Control);

    LiftoffAssembler::CacheState label_state;
    // TODO(clemensh): Labels cannot be moved on arm64, but everywhere else.
    // Find a better solution.
    std::unique_ptr<Label> label = base::make_unique<Label>();
  };

  using Decoder = WasmFullDecoder<validate, LiftoffCompiler>;

  LiftoffCompiler(LiftoffAssembler* liftoff_asm,
                  compiler::CallDescriptor* call_desc, compiler::ModuleEnv* env)
      : asm_(liftoff_asm), call_desc_(call_desc), env_(env) {
    // The ModuleEnv will be used once we implement calls.
    USE(env_);
  }

  bool ok() const { return ok_; }

  void unsupported(Decoder* decoder, const char* reason) {
    ok_ = false;
    TRACE("unsupported: %s\n", reason);
    decoder->errorf(decoder->pc(), "unsupported liftoff operation: %s", reason);
    BindUnboundLabels(decoder);
  }

  void BindUnboundLabels(Decoder* decoder) {
#ifndef DEBUG
    return;
#endif
    // Bind all labels now, otherwise their destructor will fire a DCHECK error
    // if they where referenced before.
    for (uint32_t i = 0, e = decoder->control_depth(); i < e; ++i) {
      Label* label = decoder->control_at(i)->label.get();
      if (!label->is_bound()) __ bind(label);
    }
  }

  void CheckStackSizeLimit(Decoder* decoder) {
    DCHECK_GE(__ cache_state()->stack_height(), __ num_locals());
    int stack_height = __ cache_state()->stack_height() - __ num_locals();
    if (stack_height > kMaxValueStackHeight) {
      unsupported(decoder, "value stack grows too large");
    }
  }

  void StartFunction(Decoder* decoder) {
    if (!kLiftoffAssemblerImplementedOnThisPlatform) {
      unsupported(decoder, "platform");
      return;
    }
    int num_locals = decoder->NumLocals();
    __ set_num_locals(num_locals);
    for (int i = 0; i < num_locals; ++i) {
      __ set_local_type(i, decoder->GetLocalType(i));
    }
  }

  void StartFunctionBody(Decoder* decoder, Control* block) {
    __ EnterFrame(StackFrame::WASM_COMPILED);
    __ ReserveStackSpace(kPointerSize *
                         (__ num_locals() + kMaxValueStackHeight));
    // Param #0 is the wasm context.
    constexpr uint32_t kFirstActualParameterIndex = 1;
    uint32_t num_params = static_cast<uint32_t>(call_desc_->ParameterCount()) -
                          kFirstActualParameterIndex;
    for (uint32_t i = 0; i < __ num_locals(); ++i) {
      // We can currently only handle i32 parameters and locals.
      if (__ local_type(i) != kWasmI32) {
        unsupported(decoder, "non-i32 param/local");
        return;
      }
    }
    uint32_t param_idx = 0;
    for (; param_idx < num_params; ++param_idx) {
      // First input is the call target.
      constexpr int kParameterStartInInputs = kFirstActualParameterIndex + 1;
      compiler::LinkageLocation param_loc =
          call_desc_->GetInputLocation(param_idx + kParameterStartInInputs);
      if (param_loc.IsRegister()) {
        DCHECK(!param_loc.IsAnyRegister());
        Register param_reg = Register::from_code(param_loc.AsRegister());
        if (param_reg.bit() & __ kGpCacheRegs) {
          // This is a cache register, just use it.
          __ PushRegister(param_reg);
        } else {
          // No cache register. Push to the stack.
          __ Spill(param_idx, param_reg);
          __ cache_state()->stack_state.emplace_back();
        }
      } else if (param_loc.IsCallerFrameSlot()) {
        Register tmp_reg = __ GetUnusedRegister(__ local_type(param_idx));
        __ LoadCallerFrameSlot(tmp_reg, -param_loc.AsCallerFrameSlot());
        __ PushRegister(tmp_reg);
      } else {
        UNIMPLEMENTED();
      }
    }
    for (; param_idx < __ num_locals(); ++param_idx) {
      ValueType type = decoder->GetLocalType(param_idx);
      switch (type) {
        case kWasmI32:
          __ cache_state()->stack_state.emplace_back(0);
          break;
        default:
          UNIMPLEMENTED();
      }
    }
    block->label_state.stack_base = __ num_locals();
    DCHECK_EQ(__ num_locals(), param_idx);
    DCHECK_EQ(__ num_locals(), __ cache_state()->stack_height());
    CheckStackSizeLimit(decoder);
  }

  void FinishFunction(Decoder* decoder) {}

  void OnFirstError(Decoder* decoder) {
    ok_ = false;
    BindUnboundLabels(decoder);
  }

  void Block(Decoder* decoder, Control* new_block) {
    // Note: This is called for blocks and loops.
    DCHECK_EQ(new_block, decoder->control_at(0));

    new_block->label_state.stack_base = __ cache_state()->stack_height();

    if (new_block->is_loop()) {
      // Before entering a loop, spill all locals to the stack, in order to free
      // the cache registers, and to avoid unnecessarily reloading stack values
      // into registers at branches.
      // TODO(clemensh): Come up with a better strategy here, involving
      // pre-analysis of the function.
      __ SpillLocals();

      // Loop labels bind at the beginning of the block, block labels at the
      // end.
      __ bind(new_block->label.get());

      new_block->label_state.Split(*__ cache_state());
    }
  }

  void Loop(Decoder* decoder, Control* block) { Block(decoder, block); }

  void Try(Decoder* decoder, Control* block) { unsupported(decoder, "try"); }
  void If(Decoder* decoder, const Value& cond, Control* if_block) {
    unsupported(decoder, "if");
  }

  void FallThruTo(Decoder* decoder, Control* c) {
    if (c->end_merge.reached) {
      __ MergeFullStackWith(c->label_state);
    } else {
      c->label_state.Split(*__ cache_state());
    }
  }

  void PopControl(Decoder* decoder, Control* c) {
    if (!c->is_loop() && c->end_merge.reached) {
      __ cache_state()->Steal(c->label_state);
    }
    if (!c->label->is_bound()) {
      __ bind(c->label.get());
    }
  }

  void EndControl(Decoder* decoder, Control* c) {}

  void UnOp(Decoder* decoder, WasmOpcode opcode, FunctionSig*,
            const Value& value, Value* result) {
    unsupported(decoder, "unary operation");
  }

  void BinOp(Decoder* decoder, WasmOpcode opcode, FunctionSig*,
             const Value& lhs, const Value& rhs, Value* result) {
    void (LiftoffAssembler::*emit_fn)(Register, Register, Register);
#define CASE_EMIT_FN(opcode, fn)            \
  case WasmOpcode::kExpr##opcode:           \
    emit_fn = &LiftoffAssembler::emit_##fn; \
    break;
    switch (opcode) {
      CASE_EMIT_FN(I32Add, i32_add)
      CASE_EMIT_FN(I32Sub, i32_sub)
      CASE_EMIT_FN(I32Mul, i32_mul)
      CASE_EMIT_FN(I32And, i32_and)
      CASE_EMIT_FN(I32Ior, i32_or)
      CASE_EMIT_FN(I32Xor, i32_xor)
      default:
        return unsupported(decoder, WasmOpcodes::OpcodeName(opcode));
    }
#undef CASE_EMIT_FN

    LiftoffAssembler::PinnedRegisterScope pinned_regs;
    Register target_reg =
        pinned_regs.pin(__ GetBinaryOpTargetRegister(kWasmI32));
    Register rhs_reg = pinned_regs.pin(__ PopToRegister(kWasmI32, pinned_regs));
    Register lhs_reg = __ PopToRegister(kWasmI32, pinned_regs);
    (asm_->*emit_fn)(target_reg, lhs_reg, rhs_reg);
    __ PushRegister(std::move(target_reg));
  }

  void I32Const(Decoder* decoder, Value* result, int32_t value) {
    __ cache_state()->stack_state.emplace_back(value);
    CheckStackSizeLimit(decoder);
  }

  void I64Const(Decoder* decoder, Value* result, int64_t value) {
    unsupported(decoder, "i64.const");
  }
  void F32Const(Decoder* decoder, Value* result, float value) {
    unsupported(decoder, "f32.const");
  }
  void F64Const(Decoder* decoder, Value* result, double value) {
    unsupported(decoder, "f64.const");
  }

  void Drop(Decoder* decoder, const Value& value) {
    __ DropStackSlot(&__ cache_state()->stack_state.back());
    __ cache_state()->stack_state.pop_back();
  }

  void DoReturn(Decoder* decoder, Vector<Value> values, bool implicit) {
    if (implicit) {
      DCHECK_EQ(1, decoder->control_depth());
      Control* func_block = decoder->control_at(0);
      __ bind(func_block->label.get());
      __ cache_state()->Steal(func_block->label_state);
    }
    if (!values.is_empty()) {
      if (values.size() > 1) unsupported(decoder, "multi-return");
      // TODO(clemensh): Handle other types.
      DCHECK_EQ(kWasmI32, values[0].type);
      Register reg = __ PopToRegister(kWasmI32);
      __ MoveToReturnRegister(reg);
    }
    __ LeaveFrame(StackFrame::WASM_COMPILED);
    __ Ret();
  }

  void GetLocal(Decoder* decoder, Value* result,
                const LocalIndexOperand<validate>& operand) {
    auto& slot = __ cache_state()->stack_state[operand.index];
    switch (slot.loc) {
      case kRegister:
        __ PushRegister(slot.reg);
        break;
      case kConstant:
        __ cache_state()->stack_state.emplace_back(slot.i32_const);
        break;
      case kStack: {
        Register reg = __ GetUnusedRegister(__ local_type(operand.index));
        __ Fill(reg, operand.index);
        __ PushRegister(reg);
      } break;
    }
    CheckStackSizeLimit(decoder);
  }

  void SetLocal(uint32_t local_index, bool is_tee) {
    auto& state = *__ cache_state();
    auto& source_slot = state.stack_state.back();
    auto& target_slot = state.stack_state[local_index];
    switch (source_slot.loc) {
      case kRegister:
        __ DropStackSlot(&target_slot);
        target_slot = source_slot;
        if (is_tee) state.inc_used(target_slot.reg);
        break;
      case kConstant:
        __ DropStackSlot(&target_slot);
        target_slot = source_slot;
        break;
      case kStack: {
        switch (target_slot.loc) {
          case kRegister:
            if (state.register_use_count[target_slot.reg.code()] == 1) {
              __ Fill(target_slot.reg, state.stack_height() - 1);
              break;
            } else {
              state.dec_used(target_slot.reg);
              // and fall through to use a new register.
            }
          case kConstant:
          case kStack: {
            Register target_reg =
                __ GetUnusedRegister(__ local_type(local_index));
            __ Fill(target_reg, state.stack_height() - 1);
            target_slot = LiftoffAssembler::VarState(target_reg);
            state.inc_used(target_reg);
          } break;
        }
        break;
      }
    }
    if (!is_tee) __ cache_state()->stack_state.pop_back();
  }

  void SetLocal(Decoder* decoder, const Value& value,
                const LocalIndexOperand<validate>& operand) {
    SetLocal(operand.index, false);
  }

  void TeeLocal(Decoder* decoder, const Value& value, Value* result,
                const LocalIndexOperand<validate>& operand) {
    SetLocal(operand.index, true);
  }

  void GetGlobal(Decoder* decoder, Value* result,
                 const GlobalIndexOperand<validate>& operand) {
    unsupported(decoder, "get_global");
    /*
    auto* global = &env_->module->globals[operand.index];
    Address global_addr =
        reinterpret_cast<Address>(env_->globals_start + global->offset);
    if (global->type != kWasmI32) return unsupported(decoder, "non-i32 global");
    Register dst = __ GetUnusedRegister(global->type);
    __ Load(dst, global_addr, RelocInfo::WASM_GLOBAL_REFERENCE);
    __ PushRegister(dst);
    */
  }

  void SetGlobal(Decoder* decoder, const Value& value,
                 const GlobalIndexOperand<validate>& operand) {
    unsupported(decoder, "set_global");
    /*
    auto* global = &env_->module->globals[operand.index];
    Address global_addr =
        reinterpret_cast<Address>(env_->globals_start + global->offset);
    if (global->type != kWasmI32) return unsupported(decoder, "non-i32 global");
    LiftoffAssembler::PinnedRegisterScope pinned_regs;
    Register reg = pinned_regs.pin(__ PopToRegister(global->type));
    __ Store(global_addr, reg, pinned_regs, RelocInfo::WASM_GLOBAL_REFERENCE);
    */
  }

  void Unreachable(Decoder* decoder) { unsupported(decoder, "unreachable"); }

  void Select(Decoder* decoder, const Value& cond, const Value& fval,
              const Value& tval, Value* result) {
    unsupported(decoder, "select");
  }

  void Br(Decoder* decoder, Control* target) {
    if (!target->br_merge()->reached) {
      target->label_state.InitMerge(*__ cache_state(), __ num_locals(),
                                    target->br_merge()->arity);
    }
    __ MergeStackWith(target->label_state, target->br_merge()->arity);
    __ jmp(target->label.get());
  }

  void BrIf(Decoder* decoder, const Value& cond, Control* target) {
    Label cont_false;
    Register value = __ PopToRegister(kWasmI32);
    __ JumpIfZero(value, &cont_false);

    Br(decoder, target);
    __ bind(&cont_false);
  }

  void BrTable(Decoder* decoder, const BranchTableOperand<validate>& operand,
               const Value& key) {
    unsupported(decoder, "br_table");
  }
  void Else(Decoder* decoder, Control* if_block) {
    unsupported(decoder, "else");
  }
  void LoadMem(Decoder* decoder, ValueType type, MachineType mem_type,
               const MemoryAccessOperand<validate>& operand, const Value& index,
               Value* result) {
    unsupported(decoder, "memory load");
  }
  void StoreMem(Decoder* decoder, ValueType type, MachineType mem_type,
                const MemoryAccessOperand<validate>& operand,
                const Value& index, const Value& value) {
    unsupported(decoder, "memory store");
  }
  void CurrentMemoryPages(Decoder* decoder, Value* result) {
    unsupported(decoder, "current_memory");
  }
  void GrowMemory(Decoder* decoder, const Value& value, Value* result) {
    unsupported(decoder, "grow_memory");
  }
  void CallDirect(Decoder* decoder,
                  const CallFunctionOperand<validate>& operand,
                  const Value args[], Value returns[]) {
    unsupported(decoder, "call");
  }
  void CallIndirect(Decoder* decoder, const Value& index,
                    const CallIndirectOperand<validate>& operand,
                    const Value args[], Value returns[]) {
    unsupported(decoder, "call_indirect");
  }
  void SimdOp(Decoder* decoder, WasmOpcode opcode, Vector<Value> args,
              Value* result) {
    unsupported(decoder, "simd");
  }
  void SimdLaneOp(Decoder* decoder, WasmOpcode opcode,
                  const SimdLaneOperand<validate>& operand,
                  const Vector<Value> inputs, Value* result) {
    unsupported(decoder, "simd");
  }
  void SimdShiftOp(Decoder* decoder, WasmOpcode opcode,
                   const SimdShiftOperand<validate>& operand,
                   const Value& input, Value* result) {
    unsupported(decoder, "simd");
  }
  void Simd8x16ShuffleOp(Decoder* decoder,
                         const Simd8x16ShuffleOperand<validate>& operand,
                         const Value& input0, const Value& input1,
                         Value* result) {
    unsupported(decoder, "simd");
  }
  void Throw(Decoder* decoder, const ExceptionIndexOperand<validate>&,
             Control* block, const Vector<Value>& args) {
    unsupported(decoder, "throw");
  }
  void CatchException(Decoder* decoder,
                      const ExceptionIndexOperand<validate>& operand,
                      Control* block, Vector<Value> caught_values) {
    unsupported(decoder, "catch");
  }
  void AtomicOp(Decoder* decoder, WasmOpcode opcode, Vector<Value> args,
                const MemoryAccessOperand<validate>& operand, Value* result) {
    unsupported(decoder, "atomicop");
  }

 private:
  LiftoffAssembler* asm_;
  compiler::CallDescriptor* call_desc_;
  compiler::ModuleEnv* env_;
  bool ok_ = true;

  // TODO(clemensh): Remove this limitation by allocating more stack space if
  // needed.
  static constexpr int kMaxValueStackHeight = 8;
};

}  // namespace

#undef __
#undef TRACE

}  // namespace wasm
}  // namespace internal
}  // namespace v8
