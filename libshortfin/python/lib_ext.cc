// Copyright 2024 Advanced Micro Devices, Inc.
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "./lib_ext.h"

#include "./utils.h"
#include "shortfin/array/array.h"
#include "shortfin/array/storage.h"
#include "shortfin/local/async.h"
#include "shortfin/local/messaging.h"
#include "shortfin/local/process.h"
#include "shortfin/local/program.h"
#include "shortfin/local/scope.h"
#include "shortfin/local/system.h"
#if defined(SHORTFIN_HAVE_AMDGPU)
#include "shortfin/local/systems/amdgpu.h"
#endif  // SHORTFIN_HAVE_AMDGPU
#include "shortfin/local/systems/host.h"
#include "shortfin/support/globals.h"
#include "shortfin/support/logging.h"

namespace shortfin::python {

namespace {

static const char DOCSTRING_PROGRAM_FUNCTION_INVOCATION[] =
    R"(Creates an invocation object targeting the function.

This is a low-level interface for performing an invocation, and it should be
used when precise, non-default control is needed.
)";

class Refs {
 public:
  py::object asyncio_create_task =
      py::module_::import_("asyncio").attr("create_task");
  py::object asyncio_set_event_loop =
      py::module_::import_("asyncio").attr("set_event_loop");
  py::object asyncio_get_running_loop =
      py::module_::import_("asyncio.events").attr("get_running_loop");
  py::object asyncio_set_running_loop =
      py::module_::import_("asyncio.events").attr("_set_running_loop");
  py::object threading_Thread =
      py::module_::import_("threading").attr("Thread");
  py::object threading_current_thread =
      py::module_::import_("threading").attr("current_thread");
  py::object threading_main_thread =
      py::module_::import_("threading").attr("main_thread");

  py::handle lazy_PyWorkerEventLoop() {
    if (!lazy_PyWorkerEventLoop_.is_valid()) {
      lazy_PyWorkerEventLoop_ = py::module_::import_("_shortfin.asyncio_bridge")
                                    .attr("PyWorkerEventLoop");
    }
    return lazy_PyWorkerEventLoop_;
  }

 private:
  py::object lazy_PyWorkerEventLoop_;
};

// We need a fair bit of accounting additions in order to make Workers usable
// as asyncio loops. This extension holds that.
class PyWorkerExtension : public local::Worker::Extension {
 public:
  PyWorkerExtension(local::Worker &worker, PyInterpreterState *interp,
                    std::shared_ptr<Refs> refs)
      : Extension(worker), interp_(interp), refs_(std::move(refs)) {
    py::object worker_obj = py::cast(worker, py::rv_policy::reference);
    loop_ = refs_->lazy_PyWorkerEventLoop()(worker_obj);
  }

  static PyWorkerExtension &GetCurrent() {
    PyWorkerExtension *ext =
        local::Worker::GetCurrentExtension<PyWorkerExtension>();
    if (!ext) {
      throw std::logic_error(
          "There is no shortfin worker associated with this thread.");
    }
    return *ext;
  }

  py::handle loop() { return loop_; }

  void OnThreadStart() noexcept override {
    // Python threading initialization.
    // If our own thread, teach Python about it. Not done for donated.
    if (worker().options().owned_thread) {
      PyThreadState_New(interp_);
    }

    py::gil_scoped_acquire g;
    // Aside from set_event_loop being old and _set_running_loop being new
    // it isn't clear to me that either can be left off.
    refs_->asyncio_set_event_loop(loop_);
    refs_->asyncio_set_running_loop(loop_);
  }

  void OnThreadStop() noexcept override {
    {
      // Do Python level thread cleanup.
      py::gil_scoped_acquire g;
      loop_.reset();

      // reset the event loop.
      refs_->asyncio_set_event_loop(py::none());
      refs_->asyncio_set_running_loop(py::none());
    }

    // And destroy our thread state (if not donated).
    if (worker().options().owned_thread) {
      // Ordinarily PyGILState_Ensure must be balanced with PyGILState_Release,
      // by PyThreadState_DeleteCurrent() implicitly releases it as part of
      // its cleanup process.
      PyGILState_STATE gil_state = PyGILState_Ensure();
      PyThreadState_Clear(PyThreadState_Get());
      PyThreadState_DeleteCurrent();
    }
  }

  // Because shutdown happens on the main thread under the purview of the
  // GIL, we must make arrangements to release it. Otherwise shutdown can
  // deadlock with processing.
  void OnBeforeShutdownWait() noexcept override {
    shutdown_wait_gil_state = PyEval_SaveThread();
  }
  void OnAfterShutdownWait() noexcept override {
    PyEval_RestoreThread(shutdown_wait_gil_state);
    shutdown_wait_gil_state = nullptr;
  }

 private:
  py::object loop_;
  PyInterpreterState *interp_ = nullptr;
  std::shared_ptr<Refs> refs_;

  PyThreadState *shutdown_wait_gil_state = nullptr;
};

class PyProcess : public local::detail::BaseProcess {
 public:
  PyProcess(std::shared_ptr<local::Scope> scope, std::shared_ptr<Refs> refs)
      : BaseProcess(std::move(scope)), refs_(std::move(refs)) {}
  using BaseProcess::Launch;

  void ScheduleOnWorker() override {
    // This is tricky: We need to retain the object reference across the
    // thread transition, but on the receiving side, the GIL will not be
    // held initially, so we must avoid any refcount maintenance until it
    // is acquired. Therefore, we manually borrow a reference and steal it in
    // the callback.
    py::handle self_object = py::cast(this, py::rv_policy::none);
    self_object.inc_ref();
    scope()->worker().CallThreadsafe(
        std::bind(&PyProcess::RunOnWorker, self_object));
  }
  static void RunOnWorker(py::handle self_handle) {
    py::gil_scoped_acquire g;
    // Steal the reference back from ScheduleOnWorker. Important: this is
    // very likely the last reference to the process. So self must not be
    // touched after self_object goes out of scope.
    py::object self_object = py::steal(self_handle);
    PyProcess *self = py::cast<PyProcess *>(self_handle);
    // We assume that the run method either returns None (def) or a coroutine
    // (async def).
    auto coro = self_object.attr("run")();
    if (!coro.is_none()) {
      auto task = self->refs_->asyncio_create_task(coro);
      // Capture the self object to avoid lifetime hazzard with PyProcess
      // going away before done.
      task.attr("add_done_callback")(
          py::cpp_function([self_object](py::handle future) {
            PyProcess *done_self = py::cast<PyProcess *>(self_object);
            done_self->Terminate();
            // The result of the process future doesn't matter to us, but it
            // may be carrying an exception and this is our only chance to
            // bubble it. If it is, this will throw and be handled by the
            // last chance exception handler in the worker.
            // TODO: Route process termination and exceptions to a supervisor.
            future.attr("result")();
          }));
    } else {
      // Synchronous termination.
      self->Terminate();
    }
  }

  std::shared_ptr<Refs> refs_;
};

void PyAddProgramInvocationArg(py::capsule &inv_capsule, py::handle arg) {
  // See if the object implements our marshaling protocol. If it does, then
  // We invoke the marshaling method with the Invocation wrapped as a capsule
  // and the ProgramResourceBarrier.
  py::object marshaler = py::getattr(arg, "__sfinv_marshal__", py::none());
  if (!marshaler.is_none()) {
    marshaler(inv_capsule,
              static_cast<int>(local::ProgramResourceBarrier::DEFAULT));
    return;
  }

  throw std::invalid_argument(
      fmt::format("Unsupported argument type {} in call to ProgramFunction",
                  py::cast<std::string>(py::repr(arg.type()))));
}

local::ProgramInvocation::Future PyFunctionCall(local::ProgramFunction &self,
                                                py::args args) {
  auto inv = self.CreateInvocation();
  py::capsule inv_capsule(inv.get());
  for (py::handle arg : args) {
    PyAddProgramInvocationArg(inv_capsule, arg);
  }
  return local::ProgramInvocation::Invoke(std::move(inv));
}

py::object PyRehydrateRef(local::ProgramInvocation *inv,
                          iree::vm_opaque_ref ref) {
  auto type = ref.get()->type;
  // Note that these accessors are dangerous as they assert/abort if
  // process-wide registration is not done properly. We assume here that
  // since we got a ref out that the basics are set up soundly, but if actually
  // doing this on user/dynamic types, we would want to be more defensive.
  // TODO: Don't just do a linear scan if we have more than a couple.
  // TODO: Find a reliable way to statically cache the type id.
  if (local::ProgramInvocationMarshalableFactory::invocation_marshalable_type<
          array::device_array>() == type) {
    // device_array
    return py::cast(local::ProgramInvocationMarshalableFactory::
                        CreateFromInvocationResultRef<array::device_array>(
                            inv, std::move(ref)));
  } else if (local::ProgramInvocationMarshalableFactory::
                 invocation_marshalable_type<array::storage>() == type) {
    // storage
    return py::cast(
        local::ProgramInvocationMarshalableFactory::
            CreateFromInvocationResultRef<array::storage>(inv, std::move(ref)));
  }
  throw std::invalid_argument(
      fmt::format("Cannot marshal ref type {} to Python",
                  to_string_view(iree_vm_ref_type_name(type))));
}

py::object RunInForeground(std::shared_ptr<Refs> refs, local::System &self,
                           py::object coro) {
  bool is_main_thread =
      refs->threading_current_thread().is(refs->threading_main_thread());

  local::Worker &worker = self.init_worker();
  py::object result = py::none();
  py::object py_exception = py::none();
  auto done_callback = [&](py::handle future) {
    worker.Kill();
    py_exception = future.attr("exception")();
    if (py_exception.is_none()) {
      result = future.attr("result")();
    }
  };
  worker.CallThreadsafe([&]() {
    // Run within the worker we are about to donate to.
    py::gil_scoped_acquire g;
    auto task = refs->asyncio_create_task(coro);
    task.attr("add_done_callback")(py::cpp_function(done_callback));
  });

  auto run = py::cpp_function([&]() {
    // Release GIL and run until the worker exits.
    {
      py::gil_scoped_release g;
      worker.RunOnCurrentThread();
    }
  });

  // If running on the main thread, we spawn a background thread and join
  // it because that shields it from receiving spurious KeyboardInterrupt
  // exceptions at inopportune points.
  if (is_main_thread) {
    auto thread = refs->threading_Thread(/*group=*/py::none(), /*target=*/run);
    thread.attr("start")();
    try {
      thread.attr("join")();
    } catch (...) {
      logging::warn("Exception caught in run(). Shutting down.");
      // Leak warnings are hopeless in exceptional termination.
      py::set_leak_warnings(false);
      // Give it a go waiting for the worker thread to exit.
      worker.Kill();
      thread.attr("join")();
      self.Shutdown();
      throw;
    }
  } else {
    run();
  }

  self.Shutdown();

  if (!py_exception.is_none()) {
    // We got this exception from a future/user code, which could have done
    // something nefarious. So type check it.
    if (PyObject_IsInstance(py_exception.ptr(), PyExc_Exception)) {
      PyErr_SetObject(py_exception.type().ptr(), py_exception.ptr());
    } else {
      PyErr_SetObject(PyExc_RuntimeError, py_exception.ptr());
    }
    throw py::python_error();
  }
  return result;
}

}  // namespace

NB_MODULE(lib, m) {
  py::register_exception_translator(
      [](const std::exception_ptr &p, void * /*unused*/) {
        try {
          std::rethrow_exception(p);
        } catch (shortfin::iree::error &e) {
          PyObject *exc_type;
          switch (e.code()) {
            case IREE_STATUS_INVALID_ARGUMENT:
            case IREE_STATUS_OUT_OF_RANGE:
              exc_type = PyExc_ValueError;
              break;
            case IREE_STATUS_FAILED_PRECONDITION:
              exc_type = PyExc_AssertionError;
              break;
            case IREE_STATUS_UNIMPLEMENTED:
              exc_type = PyExc_NotImplementedError;
              break;
            default:
              exc_type = PyExc_RuntimeError;
          }
          PyErr_SetString(PyExc_ValueError, e.what());
        }
      });

  py::class_<iree::vm_opaque_ref>(m, "_OpaqueVmRef");
  auto local_m = m.def_submodule("local");
  BindLocal(local_m);
  BindHostSystem(local_m);
#if defined(SHORTFIN_HAVE_AMDGPU)
  BindAMDGPUSystem(local_m);
#endif  // SHORTFIN_HAVE_AMDGPU

  auto array_m = m.def_submodule("array");
  BindArray(array_m);
}

void BindLocal(py::module_ &m) {
  // Keep weak refs to key objects that need explicit atexit shutdown.
  auto weakref = py::module_::import_("weakref");
  py::object live_system_refs = weakref.attr("WeakSet")();
  auto atexit = py::module_::import_("atexit");
  // Manually shutdown all System instances atexit if still alive (it is
  // not reliable to shutdown during interpreter finalization).
  atexit.attr("register")(py::cpp_function([](py::handle live_system_refs) {
                            for (auto it = live_system_refs.begin();
                                 it != live_system_refs.end(); ++it) {
                              (*it).attr("shutdown")();
                            }
                          }),
                          live_system_refs);
  auto refs = std::make_shared<Refs>();
  auto worker_initializer =
      [refs, interp_state = PyInterpreterState_Get()](local::Worker &worker) {
        worker.SetExtension(
            std::make_unique<PyWorkerExtension>(worker, interp_state, refs));
      };

  py::class_<local::SystemBuilder>(m, "SystemBuilder")
      .def("create_system", [live_system_refs,
                             worker_initializer](local::SystemBuilder &self) {
        auto system_ptr = self.CreateSystem();
        system_ptr->AddWorkerInitializer(worker_initializer);
        auto system_obj = py::cast(system_ptr, py::rv_policy::take_ownership);
        live_system_refs.attr("add")(system_obj);
        return system_obj;
      });
  py::class_<local::System>(m, "System", py::is_weak_referenceable())
      .def("shutdown", &local::System::Shutdown)
      // Access devices by list, name, or lookup.
      .def_prop_ro("device_names",
                   [](local::System &self) {
                     py::list names;
                     for (auto &it : self.named_devices()) {
                       names.append(it.first);
                     }
                     return names;
                   })
      .def_prop_ro("devices", &local::System::devices,
                   py::rv_policy::reference_internal)
      .def(
          "device",
          [](local::System &self, std::string_view key) {
            local::Device *device = self.FindDeviceByName(key);
            if (!device) {
              throw std::invalid_argument(fmt::format("No device '{}'", key));
            }
            return device;
          },
          py::rv_policy::reference_internal)
      .def(
          "create_queue",
          [](local::System &self, std::string name) -> local::Queue & {
            local::Queue::Options options;
            options.name = std::move(name);
            return self.CreateQueue(std::move(options));
          },
          py::arg("name"), py::rv_policy::reference_internal)
      .def("named_queue", &local::System::named_queue, py::arg("name"),
           py::rv_policy::reference_internal)
      .def(
          "create_scope",
          [](local::System &self, local::Worker *worker,
             py::handle raw_devices) {
            // TODO: I couldn't really figure out how to directly accept an
            // optional kw-only arg without it just being a raw object/handle.
            // If the passed devices is none, then we create the scope with
            // all devices in the system. Otherwise, with those explicitly
            // given.
            std::vector<local::Device *> devices;
            if (raw_devices.is_none()) {
              devices.assign(self.devices().begin(), self.devices().end());
            } else {
              devices = py::cast<std::vector<local::Device *>>(raw_devices);
            }

            // If no worker, default to the init worker.
            if (!worker) {
              worker = dynamic_cast<local::Worker *>(&self.init_worker());
            }

            return self.CreateScope(*worker, devices);
          },
          py::rv_policy::reference_internal,
          py::arg("worker").none() = py::none(), py::kw_only(),
          py::arg("devices") = py::none())
      .def(
          "create_worker",
          [refs](local::System &self, std::string name) -> local::Worker & {
            local::Worker::Options options(self.host_allocator(),
                                           std::move(name));
            return self.CreateWorker(options);
          },
          py::arg("name"), py::rv_policy::reference_internal)
      .def_prop_ro("init_worker", &local::System::init_worker,
                   py::rv_policy::reference_internal)
      .def(
          "run",
          [refs](local::System &self, py::object coro) {
            return RunInForeground(refs, self, std::move(coro));
          },
          py::arg("coro"))
      // Methods not on System but on child objects, taking System as an arg.
      // Emitted here for convenience.
      .def("load_module", &local::ProgramModule::Load, py::arg("path"),
           py::arg("mmap") = true);

  // Support classes.
  py::class_<local::Node>(m, "Node")
      .def_prop_ro("node_num", &local::Node::node_num)
      .def("__repr__", [](local::Node &self) {
        return fmt::format("local::Node({})", self.node_num());
      });
  py::class_<local::Device>(m, "Device")
      .def_prop_ro("name", &local::Device::name)
      .def_prop_ro("node_affinity", &local::Device::node_affinity)
      .def_prop_ro("node_locked", &local::Device::node_locked)
      .def(py::self == py::self)
      .def("__repr__", &local::Device::to_s);
  py::class_<local::DeviceAffinity>(m, "DeviceAffinity")
      .def(py::init<>())
      .def(py::init<local::Device *>())
      .def(py::self == py::self)
      .def("add", &local::DeviceAffinity::AddDevice)
      .def("__add__", &local::DeviceAffinity::AddDevice)
      .def("__repr__", &local::DeviceAffinity::to_s);

  py::class_<local::Program>(m, "Program")
      .def(py::new_([](std::span<const local::ProgramModule> modules,
                       local::Scope &scope, bool trace_execution) {
             local::Program::Options options;
             options.trace_execution = trace_execution;
             return local::Program::Load(scope.shared_from_this(), modules,
                                         std::move(options));
           }),
           py::arg("modules"), py::arg("scope"), py::kw_only(),
           py::arg("trace_execution") = false)
      .def_prop_ro("exports", &local::Program::exports)
      .def("lookup_function", &local::Program::LookupRequiredFunction)
      .def("__getitem__", &local::Program::LookupRequiredFunction);
  py::class_<local::ProgramFunction>(m, "ProgramFunction")
      .def_prop_ro("name", &local::ProgramFunction::name)
      .def_prop_ro("calling_convention",
                   &local::ProgramFunction::calling_convention)
      .def("invocation", &local::ProgramFunction::CreateInvocation,
           DOCSTRING_PROGRAM_FUNCTION_INVOCATION)
      .def("__call__", PyFunctionCall, py::arg("args"))
      .def("__repr__", &local::ProgramFunction::to_s);
  py::class_<local::ProgramModule>(m, "ProgramModule")
      .def_prop_ro("exports", &local::ProgramModule::exports)
      .def("__repr__", &local::ProgramModule::to_s)
      .def_static("load", &local::ProgramModule::Load, py::arg("system"),
                  py::arg("path"), py::arg("mmap") = true);
  py::class_<local::ProgramInvocation::Ptr>(m, "ProgramInvocation")
      .def("invoke",
           [](local::ProgramInvocation::Ptr &self) {
             if (!self) throw std::invalid_argument("Deallocated invocation");
             return local::ProgramInvocation::Invoke(std::move(self));
           })
      .def("add_arg",
           [](local::ProgramInvocation::Ptr &self, py::handle arg) {
             if (!self) throw std::invalid_argument("Deallocated invocation");
             py::capsule inv_capsule(self.get());
             PyAddProgramInvocationArg(inv_capsule, arg);
           })
      .def("__iter__",
           [](local::ProgramInvocation::Ptr &self) {
             if (!self) throw std::invalid_argument("Deallocated invocation");
             size_t size = self->results_size();
             py::object tp = py::steal(PyTuple_New(size));
             for (size_t i = 0; i < size; ++i) {
               iree::vm_opaque_ref ref = self->result_ref(i);
               if (!ref) {
                 throw new std::logic_error(
                     "Program returned unsupported Python type");
               }
               py::object item = PyRehydrateRef(self.get(), std::move(ref));
               PyTuple_SET_ITEM(tp.ptr(), i, item.release().ptr());
             }
             return tp.attr("__iter__")();
           })
      .def(
          "__len__",
          [](local::ProgramInvocation::Ptr &self) {
            if (!self) throw std::invalid_argument("Deallocated invocation");
            return self->results_size();
          },
          "The number of results in this invocation")
      .def(
          "__getitem__",
          [](local::ProgramInvocation::Ptr &self, iree_host_size_t i) {
            if (!self) throw std::invalid_argument("Deallocated invocation");
            iree::vm_opaque_ref ref = self->result_ref(i);
            if (!ref) {
              throw new std::logic_error(
                  "Program returned unsupported Python type");
            }
            return PyRehydrateRef(self.get(), std::move(ref));
          },
          "Gets the i'th result");

  struct DevicesSet {
    DevicesSet(local::Scope &scope) : scope(scope) {}
    local::Scope &scope;
  };
  py::class_<local::Scope>(m, "Scope")
      .def("__repr__", &local::Scope::to_s)
      .def_prop_ro("raw_devices", &local::Scope::raw_devices,
                   py::rv_policy::reference_internal)
      .def(
          "raw_device",
          [](local::Scope &self, int index) { return self.raw_device(index); },
          py::rv_policy::reference_internal)
      .def(
          "raw_device",
          [](local::Scope &self, std::string_view name) {
            return self.raw_device(name);
          },
          py::rv_policy::reference_internal)
      .def_prop_ro(
          "devices", [](local::Scope &self) { return DevicesSet(self); },
          py::rv_policy::reference_internal)
      .def_prop_ro("device_names", &local::Scope::device_names)
      .def_prop_ro("named_devices", &local::Scope::named_devices,
                   py::rv_policy::reference_internal)
      .def(
          "device",
          [](local::Scope &self, py::args args) {
            return CastDeviceAffinity(self, args);
          },
          py::rv_policy::reference_internal);

  py::class_<local::ScopedDevice>(m, "ScopedDevice")
      .def_prop_ro("scope", &local::ScopedDevice::scope,
                   py::rv_policy::reference)
      .def_prop_ro("affinity", &local::ScopedDevice::affinity,
                   py::rv_policy::reference_internal)
      .def_prop_ro("raw_device", &local::ScopedDevice::raw_device,
                   py::rv_policy::reference_internal)
      .def(py::self == py::self)
      .def("__await__",
           [](local::ScopedDevice &self) {
             py::object future = py::cast(self.OnSync(), py::rv_policy::move);
             return future.attr("__await__")();
           })
      .def("__repr__", &local::ScopedDevice::to_s);

  py::class_<DevicesSet>(m, "_ScopeDevicesSet")
      .def("__len__",
           [](DevicesSet &self) { return self.scope.raw_devices().size(); })
      .def(
          "__getitem__",
          [](DevicesSet &self, int index) { return self.scope.device(index); },
          py::rv_policy::reference_internal)
      .def(
          "__getitem__",
          [](DevicesSet &self, std::string_view name) {
            return self.scope.device(name);
          },
          py::rv_policy::reference_internal)
      .def(
          "__getattr__",
          [](DevicesSet &self, std::string_view name) {
            return self.scope.device(name);
          },
          py::rv_policy::reference_internal);

  py::class_<local::Worker>(m, "Worker", py::is_weak_referenceable())
      .def_prop_ro("loop",
                   [](local::Worker &self) {
                     auto *ext = self.GetExtension<PyWorkerExtension>();
                     if (!ext) {
                       throw std::logic_error(
                           "Worker was not initialized for access from Python");
                     }
                     return ext->loop();
                   })
      .def("call_threadsafe", &local::Worker::CallThreadsafe)
      .def("call",
           [](local::Worker &self, py::handle callable) {
             callable.inc_ref();  // Stolen within the callback.
             auto thunk = +[](void *user_data, iree_loop_t loop,
                              iree_status_t status) noexcept -> iree_status_t {
               py::gil_scoped_acquire g;
               py::object user_callable =
                   py::steal(static_cast<PyObject *>(user_data));
               IREE_RETURN_IF_ERROR(status);
               try {
                 user_callable();
               } catch (std::exception &e) {
                 return iree::exception_to_status(e);
               }
               return iree_ok_status();
             };
             SHORTFIN_THROW_IF_ERROR(self.CallLowLevel(thunk, callable.ptr()));
           })
      .def("delay_call",
           [](local::Worker &self, iree_time_t deadline_ns,
              py::handle callable) {
             callable.inc_ref();  // Stolen within the callback.
             auto thunk = +[](void *user_data, iree_loop_t loop,
                              iree_status_t status) noexcept -> iree_status_t {
               py::gil_scoped_acquire g;
               py::object user_callable =
                   py::steal(static_cast<PyObject *>(user_data));
               IREE_RETURN_IF_ERROR(status);
               try {
                 user_callable();
               } catch (std::exception &e) {
                 return iree_make_status(
                     IREE_STATUS_UNKNOWN,
                     "Python exception raised from async callback: %s",
                     e.what());
               }
               return iree_ok_status();
             };
             SHORTFIN_THROW_IF_ERROR(self.WaitUntilLowLevel(
                 iree_make_deadline(deadline_ns), thunk, callable.ptr()));
           })
      .def("_delay_to_deadline_ns",
           [](local::Worker &self, double delay_seconds) {
             return self.ConvertRelativeTimeoutToDeadlineNs(
                 static_cast<iree_duration_t>(delay_seconds * 1e9));
           })
      .def("_now", [](local::Worker &self) { return self.now(); })
      .def("__repr__", &local::Worker::to_s);

  py::class_<PyProcess>(m, "Process")
      .def("__init__", [](py::args, py::kwargs) {})
      .def_static(
          "__new__",
          [refs](py::handle py_type, py::args,
                 std::shared_ptr<local::Scope> scope, py::kwargs) {
            return custom_new<PyProcess>(py_type, std::move(scope), refs);
          },
          py::arg("type"), py::arg("args"), py::arg("scope"), py::arg("kwargs"))
      .def_prop_ro("pid", &PyProcess::pid)
      .def_prop_ro("scope", &PyProcess::scope)
      .def("launch",
           [](py::object self_obj) {
             PyProcess &self = py::cast<PyProcess &>(self_obj);
             self.Launch();
             return self_obj;
           })
      .def("__await__",
           [](PyProcess &self) {
             py::object future =
                 py::cast(local::CompletionEvent(self.OnTermination()),
                          py::rv_policy::move);
             return future.attr("__await__")();
           })
      .def("__repr__", &PyProcess::to_s);

  py::class_<local::CompletionEvent>(m, "CompletionEvent")
      .def(py::init<>())
      .def("__await__", [](py::handle self_obj) {
        auto &worker_ext = PyWorkerExtension::GetCurrent();
        auto &self = py::cast<local::CompletionEvent &>(self_obj);
        py::object future = worker_ext.loop().attr("create_future")();
        // Stashing self as an attribute on the future, keeps us alive,
        // which transitively means that the wait source we get from self
        // stays alive. This can be done better later with a custom
        // Future.
        future.attr("_sf_event") = self_obj;
        py::object iter_ret = future.attr("__iter__")();

        // Pass the future object as void* user data to the C callback
        // interface. This works because we release the object pointer going
        // in and then steal it back to a py::object once the GIL has been
        // acquired (since dec_ref'ing it must happen within the GIL).
        // Because the self object was stashed on an attribute above, the
        // wait_source is valid for the entire sequence.
        SHORTFIN_THROW_IF_ERROR(worker_ext.worker().WaitOneLowLevel(
            /*wait_source=*/
            self, iree_infinite_timeout(),
            +[](void *future_vp, iree_loop_t loop,
                iree_status_t status) noexcept -> iree_status_t {
              py::gil_scoped_acquire g;
              py::object future = py::steal(static_cast<PyObject *>(future_vp));
              try {
                SHORTFIN_THROW_IF_ERROR(status);
                future.attr("set_result")(py::none());
              } catch (std::exception &e) {
                auto RuntimeError = py::handle(PyExc_RuntimeError);
                future.attr("set_exception")(RuntimeError(e.what()));
              }
              return iree_ok_status();
            },
            static_cast<void *>(future.release().ptr())));
        return iter_ret;
      });

  // ------------------------------------------------------------------------ //
  // Messaging
  // ------------------------------------------------------------------------ //
  py::class_<local::Message>(
      m, "Message",
      // Message is special in that it supports vague ownership and can be
      // transferred to the Python side, sharing one reference count and
      // lifetime. This is done the first time a Message is seen by the Python
      // side (either by in-place construction in a Python object or by
      // taking ownership of an object originating on the C++ side). When this
      // happens, the owner struct is replaced and any C++ side reference counts
      // are turned into Python reference counts. Once transferred, only Python
      // reference counting is used, even if referenced from the C++ side.
      py::intrusive_ptr<local::Message>([](local::Message *self,
                                           PyObject *self_py) noexcept {
        local::detail::MessageLifetimeController owner(
            +[](local::detail::MessageLifetimeController::Request req,
                const local::Message &msg) {
              py::gil_scoped_acquire g;
              PyObject *msg_object = reinterpret_cast<PyObject *>(
                  local::detail::MessageLifetimeController::AccessOwnedRefData(
                      msg));
              if (req ==
                  local::detail::MessageLifetimeController::Request::RETAIN) {
                py::handle(msg_object).inc_ref();
              } else {
                py::handle(msg_object).dec_ref();
              }
            });
        intptr_t orig_ref_data =
            owner.TakeOwnership(*self, reinterpret_cast<intptr_t>(self_py));
        // Transfer any prior C++ references to the Python side (less 1
        // since we start with a live reference).
        for (int i = 0; i < orig_ref_data - 1; ++i) {
          py::handle(self_py).inc_ref();
        }
      }))
      .def(py::init<>());

  py::class_<local::Queue>(m, "Queue")
      .def("__repr__", &local::Queue::to_s)
      .def("close", &local::Queue::Close)
      .def("writer",
           [](local::Queue &self) {
             return custom_new_keep_alive<local::QueueWriter>(
                 py::type<local::QueueWriter>(),
                 /*keep_alive=*/self, /*queue=*/self);
           })
      .def("reader", [](local::Queue &self) {
        return custom_new_keep_alive<local::QueueReader>(
            py::type<local::QueueReader>(),
            /*keep_alive=*/self, /*queue=*/self);
      });
  py::class_<local::QueueWriter>(m, "QueueWriter")
      .def("__call__",
           [](local::QueueWriter &self, local::Message &message) {
             return self.Write(local::Message::Ref(message));
           })
      .def("close", &local::QueueWriter::Close);
  py::class_<local::QueueReader>(m, "QueueReader")
      .def("__call__", [](local::QueueReader &self) { return self.Read(); });

  // ------------------------------------------------------------------------ //
  // Futures
  // ------------------------------------------------------------------------ //
  py::class_<local::Future>(m, "Future")
      // The generic await support relies on the result override from a
      // subclass. Here and for VoidFuture, it is always none.
      .def("result",
           [](local::Future &self) {
             self.ThrowFailure();
             return py::none();
           })
      .def("__await__", [](py::handle self_obj) {
        // TODO: We should make our C++ future able to be used directly
        // vs needing to bridge it like this.
        auto &worker_ext = PyWorkerExtension::GetCurrent();
        auto &self = py::cast<local::Future &>(self_obj);
        py::object future = worker_ext.loop().attr("create_future")();
        // Stashing self as an attribute on the future, keeps us alive,
        // which transitively means that the wait source we get from self
        // stays alive. This can be done better later with a custom
        // Future.
        future.attr("_sf_future") = self_obj;
        py::object iter_ret = future.attr("__iter__")();

        // Pass the future object as void* user data to the C callback
        // interface. This works because we release the object pointer going
        // in and then steal it back to a py::object once the GIL has been
        // acquired (since dec_ref'ing it must happen within the GIL).
        // Because the self object was stashed on an attribute above, the
        // wait_source is valid for the entire sequence.
        self.AddCallback(
            [py_future_vp = static_cast<void *>(future.release().ptr())](
                local::Future &sf_future) {
              py::gil_scoped_acquire g;
              py::object py_future =
                  py::steal(static_cast<PyObject *>(py_future_vp));
              try {
                // Consult the Python level "result" on our local::Future
                // binding as it will override properly to get the right
                // Python casted type, allowing us to just have this one
                // __await__ implementation for every typed future.
                py_future.attr("set_result")(
                    py_future.attr("_sf_future").attr("result")());
              } catch (std::exception &e) {
                auto RuntimeError = py::handle(PyExc_RuntimeError);
                py_future.attr("set_exception")(RuntimeError(e.what()));
              }
            });
        return iter_ret;
      });
  py::class_<local::VoidFuture, local::Future>(m, "VoidFuture");
  py::class_<local::ProgramInvocation::Future, local::Future>(
      m, "ProgramInvocationFuture")
      .def("result", [](local::ProgramInvocation::Future &self) {
        local::ProgramInvocation::Ptr &result = self.result();
        if (!result) return py::none();
        // Sharp edge: ProgramInvocationFutures are read-once since we move the
        // ProgramInvocation::Ptr out of the future here and transfer ownership
        // to a Python object. There isn't a better way to do this without
        // increasing overhead on this hot path or doing something more
        // expensive in the C++ API: essentially, ProgramInvocations flow
        // through the system precisely one way. As a low level facility, this
        // is deemed acceptable.
        return py::cast(std::move(result));
      });
  py::class_<local::MessageFuture, local::Future>(m, "MessageFuture")
      .def("result", [](local::MessageFuture &self) {
        // Get a raw backing msg (without an increased refcount). When cast
        // to a py::object, it will get a new reference.
        local::Message::Ref &result = self.result();
        if (!result) return py::none();
        return py::cast(result.get());
      });
}

void BindHostSystem(py::module_ &global_m) {
  auto m = global_m.def_submodule("host", "Host device management");
  py::class_<local::systems::HostSystemBuilder, local::SystemBuilder>(
      m, "SystemBuilder");
  py::class_<local::systems::HostCPUSystemBuilder,
             local::systems::HostSystemBuilder>(m, "CPUSystemBuilder")
      .def(py::init<>());
  py::class_<local::systems::HostCPUDevice, local::Device>(m, "HostCPUDevice");
}

#if defined(SHORTFIN_HAVE_AMDGPU)
void BindAMDGPUSystem(py::module_ &global_m) {
  auto m = global_m.def_submodule("amdgpu", "AMDGPU system config");
  py::class_<local::systems::AMDGPUSystemBuilder,
             local::systems::HostCPUSystemBuilder>(m, "SystemBuilder")
      .def(py::init<>())
      .def_rw("cpu_devices_enabled",
              &local::systems::AMDGPUSystemBuilder::cpu_devices_enabled);
  py::class_<local::systems::AMDGPUDevice, local::Device>(m, "AMDGPUDevice");
}
#endif  // SHORTFIN_HAVE_AMDGPU

}  // namespace shortfin::python
