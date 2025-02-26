// Copyright 2024 Advanced Micro Devices, Inc.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "shortfin/local/program.h"

#include "fmt/core.h"
#include "fmt/std.h"
#include "iree/modules/hal/module.h"
#include "iree/vm/bytecode/module.h"
#include "shortfin/local/scope.h"
#include "shortfin/local/system.h"
#include "shortfin/support/logging.h"

namespace shortfin::local {

namespace {
void GetVmModuleExports(iree_vm_module_t *vm_module,
                        std::vector<std::string> &exports) {
  auto sig = iree_vm_module_signature(vm_module);
  for (iree_host_size_t i = 0; i < sig.export_function_count; ++i) {
    iree_vm_function_t f;
    SHORTFIN_THROW_IF_ERROR(iree_vm_module_lookup_function_by_ordinal(
        vm_module, IREE_VM_FUNCTION_LINKAGE_EXPORT, i, &f));
    exports.emplace_back(to_string_view(iree_vm_function_name(&f)));
  }
}
}  // namespace

// -------------------------------------------------------------------------- //
// ProgramFunction
// -------------------------------------------------------------------------- //

ProgramFunction::ProgramFunction(
    std::shared_ptr<Scope> scope, iree::vm_context_ptr vm_context,
    iree_vm_function_t vm_function,
    std::optional<ProgramInvocationModel> invocation_model)
    : scope_(std::move(scope)),
      vm_context_(std::move(vm_context)),
      vm_function_(vm_function),
      invocation_model_(invocation_model
                            ? *invocation_model
                            : GetInvocationModelFromFunction(vm_function)) {}

ProgramInvocationModel ProgramFunction::GetInvocationModelFromFunction(
    iree_vm_function_t &f) {
  iree_string_view_t invocation_model_sv =
      iree_vm_function_lookup_attr_by_name(&f, IREE_SV("iree.abi.model"));
  if (iree_string_view_equal(invocation_model_sv, IREE_SV("coarse-fences"))) {
    return ProgramInvocationModel::COARSE_FENCES;
  } else if (invocation_model_sv.size == 0) {
    return ProgramInvocationModel::NONE;
  } else {
    logging::warn("Unknown function invocation model '{}': '{}'",
                  to_string_view(iree_vm_function_name(&f)),
                  to_string_view(invocation_model_sv));
    return ProgramInvocationModel::UNKNOWN;
  }
}

std::string_view ProgramFunction::name() const {
  if (!*this) return {};
  return to_string_view(iree_vm_function_name(&vm_function_));
}

std::string_view ProgramFunction::calling_convention() const {
  if (!*this) return {};
  return to_string_view(
      iree_vm_function_signature(&vm_function_).calling_convention);
}

ProgramInvocation::Ptr ProgramFunction::CreateInvocation() {
  return ProgramInvocation::New(scope_, vm_context_, vm_function_,
                                invocation_model_);
}

std::string ProgramFunction::to_s() const {
  if (!*this) return std::string("ProgramFunction(NULL)");
  return fmt::format("ProgramFunction({}: {})", name(), calling_convention());
}

// -------------------------------------------------------------------------- //
// ProgramModule
// -------------------------------------------------------------------------- //

ProgramModule ProgramModule::Load(System &system,
                                  const std::filesystem::path &path,
                                  bool mmap) {
  iree::file_contents_ptr contents;
  iree_file_read_flags_t flags =
      mmap ? IREE_FILE_READ_FLAG_MMAP : IREE_FILE_READ_FLAG_PRELOAD;
  SHORTFIN_THROW_IF_ERROR(iree_file_read_contents(
      path.c_str(), flags, system.host_allocator(), contents.for_output()));

  // Ownership hazard: iree_vm_bytecode_module_create only assumes ownership
  // of the contents when it returns *sucessfully*. In the exceptional case,
  // ownership remains with the caller, so we let the RAII wrapper hold on to
  // it until after success.
  iree::vm_module_ptr module;
  SHORTFIN_THROW_IF_ERROR(iree_vm_bytecode_module_create(
      system.vm_instance(), contents.const_buffer(), contents.deallocator(),
      system.host_allocator(), module.for_output()));
  contents.release();  // Must be invoked on success path only.
  return ProgramModule(std::move(module));
}

std::string_view ProgramModule::name() const {
  return to_string_view(iree_vm_module_name(vm_module_));
}

std::string ProgramModule::to_s() const {
  auto sig = iree_vm_module_signature(vm_module_);
  std::vector<std::string> exports;
  for (iree_host_size_t i = 0; i < sig.export_function_count; ++i) {
    iree_vm_function_t f;
    SHORTFIN_THROW_IF_ERROR(iree_vm_module_lookup_function_by_ordinal(
        vm_module_, IREE_VM_FUNCTION_LINKAGE_EXPORT, i, &f));
    exports.push_back(fmt::format(
        "{}({})", to_string_view(iree_vm_function_name(&f)),
        to_string_view(iree_vm_function_signature(&f).calling_convention)));
  }
  return fmt::format("ProgramModule('{}', version={}, exports=[{}])", name(),
                     sig.version, fmt::join(exports, ", "));
}

std::vector<std::string> ProgramModule::exports() const {
  std::vector<std::string> exports;
  GetVmModuleExports(vm_module_, exports);
  return exports;
}

// -------------------------------------------------------------------------- //
// Program
// -------------------------------------------------------------------------- //

Program Program::Load(std::shared_ptr<Scope> scope,
                      std::span<const ProgramModule> modules, Options options) {
  std::vector<iree_vm_module_t *> all_modules;
  std::vector<iree_hal_device_t *> raw_devices;

  // By default, bind all devices in the scope in order to the program.
  for (Device *d : scope->raw_devices()) {
    raw_devices.push_back(d->hal_device());
  }

  // Add a HAL module.
  // TODO: at some point may want to change this to something similar to
  // what the tooling does in iree_tooling_resolve_modules - it uses
  // iree_vm_module_enumerate_dependencies to walk the dependencies and add the
  // required modules only as needed. to start you could use it just to see if
  // the hal is used, but as you add other module types for exposing sharkfin
  // functionality (or module versions; iree_vm_module_dependency_t has the
  // minimum version required so you can switch between them, and whether they
  // are optional/required).
  auto &system = scope->system();
  iree::vm_module_ptr hal_module;
  SHORTFIN_THROW_IF_ERROR(
      iree_hal_module_create(system.vm_instance(), raw_devices.size(),
                             raw_devices.data(), IREE_HAL_MODULE_FLAG_NONE,
                             system.host_allocator(), hal_module.for_output()));
  all_modules.push_back(hal_module);

  // Add explicit modules.
  for (auto &pm : modules) {
    all_modules.push_back(pm.vm_module());
  }

  // Create the context.
  iree::vm_context_ptr context;
  iree_vm_context_flags_t flags = IREE_VM_CONTEXT_FLAG_CONCURRENT;
  if (options.trace_execution) flags |= IREE_VM_CONTEXT_FLAG_TRACE_EXECUTION;
  SHORTFIN_THROW_IF_ERROR(iree_vm_context_create_with_modules(
      system.vm_instance(), flags, all_modules.size(), all_modules.data(),
      system.host_allocator(), context.for_output()));

  return Program(std::move(scope), std::move(context));
}

std::optional<ProgramFunction> Program::LookupFunction(std::string_view name) {
  // By convention, we currently name our coarse-fences function variants
  // as ending in "$async". These are the ones we want but it is inconvenient.
  // Therefore, we probe for that first.
  // TODO: We should add attributes to the function that better describe this
  // relationship.
  iree_vm_function_t f;
  if (!name.ends_with("$async")) {
    std::string async_name(name);
    async_name.append("$async");
    iree_status_t status = iree_vm_context_resolve_function(
        vm_context_, to_iree_string_view(async_name), &f);
    if (iree_status_is_ok(status)) {
      // TODO: Torch import is not setting the coarse-fences abi.model on
      // its functions. Get it from there instead of just assuming based on
      // name.
      return ProgramFunction(scope_, vm_context_, f,
                             ProgramInvocationModel::COARSE_FENCES);
    } else if (!iree_status_is_not_found(status)) {
      SHORTFIN_THROW_IF_ERROR(status);
    }
  }

  // Resolve the exactly named function.
  iree_status_t status = iree_vm_context_resolve_function(
      vm_context_, to_iree_string_view(name), &f);
  if (iree_status_is_not_found(status)) return {};
  SHORTFIN_THROW_IF_ERROR(status);
  return ProgramFunction(scope_, vm_context_, f);
}

ProgramFunction Program::LookupRequiredFunction(std::string_view name) {
  auto f = LookupFunction(name);
  if (!f) {
    throw std::invalid_argument(
        fmt::format("Function '{}' not found in program. Available exports: {}",
                    name, fmt::join(exports(), ", ")));
  }
  return std::move(*f);
}

std::vector<std::string> Program::exports() const {
  std::vector<std::string> results;

  // Iterate in reverse since "user modules" are typically last.
  int module_count = iree_vm_context_module_count(vm_context_);
  for (int i = module_count - 1; i >= 0; --i) {
    auto vm_module = iree_vm_context_module_at(vm_context_, i);
    std::string_view module_name =
        to_string_view(iree_vm_module_name(vm_module));
    std::vector<std::string> names;
    GetVmModuleExports(vm_module, names);
    for (auto &name : names) {
      results.push_back(fmt::format("{}.{}", module_name, name));
    }
  }
  return results;
}

// -------------------------------------------------------------------------- //
// ProgramInvocation
// -------------------------------------------------------------------------- //

iree_vm_list_t *ProgramInvocation::arg_list() {
  // The arg list is located immediately after this, allocated as a trailing
  // data structure.
  return reinterpret_cast<iree_vm_list_t *>(reinterpret_cast<uint8_t *>(this) +
                                            sizeof(*this));
}

void ProgramInvocation::Deleter::operator()(ProgramInvocation *inst) {
  inst->~ProgramInvocation();
  uint8_t *memory = static_cast<uint8_t *>(static_cast<void *>(inst));

  // Trailing arg list and result list. The arg list pointer is only available
  // at construction, so we use the knowledge that it is stored right after
  // the object. The result_list_ is available for the life of the invocation.
  iree_vm_list_deinitialize(static_cast<iree_vm_list_t *>(
      static_cast<void *>(memory + sizeof(ProgramInvocation))));
  iree_vm_list_deinitialize(inst->result_list_);

  // Was allocated in New as a uint8_t[] so delete it by whence it came.
  delete[] memory;
}

ProgramInvocation::ProgramInvocation() = default;
ProgramInvocation::~ProgramInvocation() {
  if (!scheduled()) {
    // This instance was dropped on the floor before scheduling.
    // Clean up the initialization parameters.
    iree::vm_context_ptr drop =
        iree::vm_context_ptr::steal_reference(state.params.context);
  }
}

ProgramInvocation::Ptr ProgramInvocation::New(
    std::shared_ptr<Scope> scope, iree::vm_context_ptr vm_context,
    iree_vm_function_t &vm_function, ProgramInvocationModel invocation_model) {
  auto sig = iree_vm_function_signature(&vm_function);
  iree_host_size_t arg_count;
  iree_host_size_t result_count;
  SHORTFIN_THROW_IF_ERROR(iree_vm_function_call_count_arguments_and_results(
      &sig, &arg_count, &result_count));

  // Compute size of trailing arg/result storage.
  auto variant_type_def = iree_vm_make_undefined_type_def();
  iree_host_size_t arg_storage_size =
      iree_vm_list_storage_size(&variant_type_def, arg_count);
  iree_host_size_t result_storage_size =
      iree_vm_list_storage_size(&variant_type_def, result_count);

  // Allocate storage for the ProgramInvocation, arg, result list and placement
  // new the ProgramInvocation into the storage area.
  std::unique_ptr<uint8_t[]> inst_storage(
      new uint8_t[sizeof(ProgramInvocation) + arg_storage_size +
                  result_storage_size]);
  new (inst_storage.get()) ProgramInvocation();

  // Initialize trailing lists. Abort on failure since this is a bug and we
  // would otherwise leak.
  iree_vm_list_t *arg_list;
  iree_vm_list_t *result_list;
  IREE_CHECK_OK(iree_vm_list_initialize(
      {.data = inst_storage.get() + sizeof(ProgramInvocation),
       .data_length = arg_storage_size},
      &variant_type_def, arg_count, &arg_list));
  IREE_CHECK_OK(iree_vm_list_initialize(
      {.data =
           inst_storage.get() + sizeof(ProgramInvocation) + arg_storage_size,
       .data_length = result_storage_size},
      &variant_type_def, result_count, &result_list));

  Ptr inst(static_cast<ProgramInvocation *>(
               static_cast<void *>(inst_storage.release())),
           Deleter());
  inst->scope_ = std::move(scope);
  inst->state.params.context =
      vm_context.release();  // Ref transfer to ProgramInvocation.
  inst->state.params.function = vm_function;
  inst->state.params.invocation_model = invocation_model;
  inst->result_list_ = result_list;
  return inst;
}

void ProgramInvocation::CheckNotScheduled() {
  if (scheduled()) {
    throw std::logic_error("Cannot mutate an invocation once scheduled.");
  }
}

void ProgramInvocation::AddArg(iree::vm_opaque_ref ref) {
  CheckNotScheduled();
  SHORTFIN_THROW_IF_ERROR(iree_vm_list_push_ref_move(arg_list(), &ref));
}

void ProgramInvocation::AddArg(iree_vm_ref_t *ref) {
  CheckNotScheduled();
  SHORTFIN_THROW_IF_ERROR(iree_vm_list_push_ref_retain(arg_list(), ref));
}

iree_status_t ProgramInvocation::FinalizeCallingConvention(
    iree_vm_list_t *arg_list, iree_vm_function_t &function,
    ProgramInvocationModel invocation_model) {
  // Handle post-processing invocation model setup.
  if (invocation_model == ProgramInvocationModel::COARSE_FENCES) {
    // If we have a device_selection, set up to signal the leader account.
    if (device_selection_) {
      ScopedDevice scoped_device(*scope(), device_selection_);
      auto &sched_account =
          scope()->scheduler().GetDefaultAccount(scoped_device);
      iree_hal_fence_t *wait_fence = this->wait_fence();
      iree_hal_semaphore_t *timeline_sem = sched_account.timeline_sem();
      uint64_t timeline_now = sched_account.timeline_idle_timepoint();
      SHORTFIN_SCHED_LOG("Invocation {}: Wait on account timeline {}@{}",
                         static_cast<void *>(this),
                         static_cast<void *>(timeline_sem), timeline_now);
      IREE_RETURN_IF_ERROR(
          iree_hal_fence_insert(wait_fence, timeline_sem, timeline_now));
      signal_sem_ = sched_account.timeline_sem();
      signal_timepoint_ = sched_account.timeline_acquire_timepoint();
    }

    // Push wait fence (or null if no wait needed).
    ::iree::vm::ref<iree_hal_fence_t> wait_ref;
    if (wait_fence_) {
      ::iree::vm::retain_ref(wait_fence());
    }
    IREE_RETURN_IF_ERROR(iree_vm_list_push_ref_move(arg_list, wait_ref));

    // Create and push signal fence (or null if no signal needed).
    ::iree::vm::ref<iree_hal_fence_t> signal_ref;
    if (signal_sem_) {
      SHORTFIN_SCHED_LOG("Invocation {}: Set signal {}@{}",
                         static_cast<void *>(this),
                         static_cast<void *>(signal_sem_), signal_timepoint_);
      IREE_RETURN_IF_ERROR(
          iree_hal_fence_create_at(signal_sem_, signal_timepoint_,
                                   scope()->host_allocator(), &signal_ref));
    }
    IREE_RETURN_IF_ERROR(iree_vm_list_push_ref_move(arg_list, signal_ref));
  } else {
    logging::warn(
        "Invoking function '{}' with unknown or synchronous invocation model "
        "is not fully supported",
        to_string_view(iree_vm_function_name(&function)));
  }

  return iree_ok_status();
}

ProgramInvocation::Future ProgramInvocation::Invoke(
    ProgramInvocation::Ptr invocation) {
  invocation->CheckNotScheduled();

  Worker &worker = invocation->scope_->worker();
  // We're about to overwrite the instance level storage for params, so move
  // it to the stack and access there.
  Params params = invocation->state.params;

  auto schedule = [](ProgramInvocation *raw_invocation, Worker *worker,
                     iree_vm_context_t *owned_context,
                     iree_vm_function_t function,
                     ProgramInvocationModel invocation_model,
                     std::optional<ProgramInvocation::Future> failure_future) {
    auto complete_callback =
        [](void *user_data, iree_loop_t loop, iree_status_t status,
           iree_vm_list_t *outputs) noexcept -> iree_status_t {
      // Async invocation helpfully gives us a retained reference to the
      // outputs, but we already have one statically on the
      // ProgramInvocation. So release this one, which makes it safe to
      // deallocate the ProgramInvocation at any point after this (there
      // must be no live references to inputs/outputs when the
      // ProgramInvocation::Ptr deleter is invoked).
      iree::vm_list_ptr::steal_reference(outputs);

      // Repatriate the ProgramInvocation.
      ProgramInvocation::Ptr invocation(
          static_cast<ProgramInvocation *>(user_data));
      ProgramInvocation *raw_invocation = invocation.get();
      if (iree_status_is_ok(status)) {
        raw_invocation->future_->set_result(std::move(invocation));
      } else {
        raw_invocation->future_->set_failure(status);
      }

      // Must release the future from the invocation to break the
      // circular reference (we are setting the invocation as the result
      // of the future).
      raw_invocation->future_.reset();

      return iree_ok_status();
    };

    ProgramInvocation::Ptr invocation(raw_invocation);
    iree_status_t status = iree_ok_status();

    // Multiple steps needed to schedule need to all exit via the same
    // path.
    if (iree_status_is_ok(status)) {
      status = invocation->scope()->scheduler().FlushWithStatus();
    }
    if (iree_status_is_ok(status)) {
      status = invocation->FinalizeCallingConvention(
          invocation->arg_list(), function, invocation_model);
    }
    if (iree_status_is_ok(status)) {
      status = iree_vm_async_invoke(worker->loop(),
                                    &invocation->state.async_invoke_state,
                                    owned_context, function,
                                    /*flags=*/IREE_VM_INVOCATION_FLAG_NONE,
                                    /*policy=*/nullptr,
                                    /*inputs=*/invocation->arg_list(),
                                    /*outputs=*/invocation->result_list_,
                                    iree_allocator_system(), +complete_callback,
                                    /*user_data=*/invocation.get());
    }

    // Regardless of status, the context reference we were holding is no
    // longer needed. Drop it on the floor.
    iree::vm_context_ptr::steal_reference(owned_context);

    // On success, then the complete callback takes ownership of the
    // invocation, so we release it here and return. We have to treat
    // the invocation as possibly deallocated at this point, since the
    // async invocation may have finished already.
    if (iree_status_is_ok(status)) {
      invocation.release();
    } else if (failure_future) {
      // Requested to set any failure on the future.
      failure_future->set_failure(status);
    } else {
      // Synchronous: just throw.
      SHORTFIN_THROW_IF_ERROR(status);
    }
  };

  // Transition to the scheduled state.
  invocation->future_.emplace(&worker);
  auto fork_future = *invocation->future_;
  invocation->scheduled_ = true;

  if (&worker == Worker::GetCurrent()) {
    // On the same worker: fast-path directly to the loop.
    schedule(invocation.release(), &worker, params.context, params.function,
             params.invocation_model, /*failure_future=*/{});
  } else {
    // Cross worker coordination: submit an external task to bootstrap.
    auto bound_schedule =
        std::bind(schedule, invocation.release(), &worker, params.context,
                  params.function, params.invocation_model,
                  /*failure_future=*/fork_future);
    worker.CallThreadsafe(bound_schedule);
  }

  return fork_future;
}

iree_host_size_t ProgramInvocation::results_size() {
  return iree_vm_list_size(result_list_);
}

iree::vm_opaque_ref ProgramInvocation::result_ref(iree_host_size_t i) {
  iree::vm_opaque_ref out_value;
  auto status = iree_vm_list_get_ref_retain(result_list_, i, &out_value);
  if (iree_status_is_failed_precondition(status)) return {};
  SHORTFIN_THROW_IF_ERROR(status, "accessing invocation result");
  return out_value;
}

iree_hal_fence_t *ProgramInvocation::wait_fence() {
  if (!wait_fence_) {
    wait_fence_ = scope_->scheduler().NewFence();
  }
  return wait_fence_.get();
}

void ProgramInvocation::wait_insert(iree_hal_semaphore_list_t sem_list) {
  iree_hal_fence_t *f = wait_fence();
  for (iree_host_size_t i = 0; i < sem_list.count; ++i) {
    SHORTFIN_SCHED_LOG("Invocation {}: Wait on {}@{}",
                       static_cast<void *>(this),
                       static_cast<void *>(sem_list.semaphores[i]),
                       sem_list.payload_values[i]);
    SHORTFIN_THROW_IF_ERROR(iree_hal_fence_insert(f, sem_list.semaphores[i],
                                                  sem_list.payload_values[i]));
  }
}

void ProgramInvocation::DeviceSelect(DeviceAffinity device_affinity) {
  CheckNotScheduled();
  SHORTFIN_SCHED_LOG("Invocation {}: DeviceSelect {}",
                     static_cast<void *>(this), device_affinity.to_s());
  device_selection_ |= device_affinity;
}

}  // namespace shortfin::local
