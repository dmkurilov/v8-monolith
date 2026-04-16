// v8_wrapper.cc — implementation. Compiled with V8's bundled libc++; all
// std types inside this TU live in std::__Cr::*. The wrapper's public API
// uses only v8wrap:: types so nothing libc++ leaks across TUs.

#include "v8_wrapper.h"

#include <stdio.h>   // fprintf for fatal Platform init error
#include <stdlib.h>  // abort
#include <string.h>  // memcpy

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "libplatform/libplatform.h"
#include "v8-context.h"
#include "v8-exception.h"
#include "v8-function.h"
#include "v8-isolate.h"
#include "v8-json.h"
#include "v8-microtask-queue.h"
#include "v8-persistent-handle.h"
#include "v8-primitive.h"
#include "v8-promise.h"
#include "v8-script.h"
#include "v8-template.h"
#include "v8.h"

#ifndef V8_ENABLE_SANDBOX
#error "Build V8 with v8_enable_sandbox=true for untrusted plugin execution."
#endif

namespace v8wrap {

// ---------------------------------------------------------------------------
// String::Own
// ---------------------------------------------------------------------------
String String::Own(const char* src, size_t n) {
  String s;
  if (n > 0 && src) {
    s.data_ = static_cast<char*>(::malloc(n + 1));
    if (s.data_) {
      ::memcpy(s.data_, src, n);
      s.data_[n] = '\0';
      s.size_ = n;
    }
  }
  return s;
}

// ---------------------------------------------------------------------------
// Internal helpers for converting between v8 and v8wrap types.
// ---------------------------------------------------------------------------
namespace {

String CopyV8Utf8(v8::Isolate* iso, v8::Local<v8::Value> val) {
  if (val.IsEmpty()) return {};
  v8::String::Utf8Value u(iso, val);
  if (*u == nullptr) return {};
  return String::Own(*u, static_cast<size_t>(u.length()));
}

v8::Local<v8::String> MakeV8String(v8::Isolate* iso, StringView sv) {
  return v8::String::NewFromUtf8(iso, sv.data, v8::NewStringType::kNormal,
                                 static_cast<int>(sv.size))
      .ToLocalChecked();
}

// Populates an Error from a TryCatch. Caller supplies the Kind and a
// fallback resource_name (used if the TryCatch's Message doesn't carry one,
// e.g. for errors not tied to a script line).
Error MakeError(Error::Kind kind, v8::Isolate* iso, v8::TryCatch& tc,
                StringView fallback_resource) {
  Error err;
  err.kind = kind;
  err.resource_name = String::Own(fallback_resource.data, fallback_resource.size);

  if (!tc.HasCaught()) {
    err.message = String::Own("(no exception info)", 20);
    return err;
  }

  auto ctx = iso->GetCurrentContext();
  err.message = CopyV8Utf8(iso, tc.Exception());

  auto msg = tc.Message();
  if (!msg.IsEmpty()) {
    int line = msg->GetLineNumber(ctx).FromMaybe(0);
    int col = msg->GetStartColumn(ctx).FromMaybe(-1);
    if (line > 0) err.line = line;
    if (col >= 0) err.column = col;

    auto res = msg->GetScriptResourceName();
    if (!res.IsEmpty() && res->IsString()) {
      err.resource_name = CopyV8Utf8(iso, res);
    }
  }
  return err;
}

// Build an Error from a JS exception value (when we don't have a TryCatch,
// e.g. for rejected promises).
Error MakeErrorFromException(Error::Kind kind, v8::Isolate* iso,
                             v8::Local<v8::Value> exc,
                             StringView fallback_resource) {
  Error err;
  err.kind = kind;
  err.resource_name = String::Own(fallback_resource.data, fallback_resource.size);
  err.message = CopyV8Utf8(iso, exc);

  // Best-effort: extract line/col from an Error object's stack frame.
  if (!exc.IsEmpty() && exc->IsObject()) {
    auto ctx = iso->GetCurrentContext();
    auto obj = exc.As<v8::Object>();
    auto get_int = [&](const char* key) -> int {
      v8::Local<v8::Value> v;
      if (!obj->Get(ctx, v8::String::NewFromUtf8(iso, key).ToLocalChecked())
               .ToLocal(&v)) {
        return -1;
      }
      if (!v->IsInt32()) return -1;
      return v.As<v8::Int32>()->Value();
    };
    int line = get_int("lineNumber");
    int col = get_int("columnNumber");
    if (line > 0) err.line = line;
    if (col >= 0) err.column = col;
  }
  return err;
}

}  // namespace

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------
struct Value::Impl {
  v8::Isolate* isolate{};
  v8::Global<v8::Value> value;
};

Value& Value::operator=(Value&& o) noexcept {
  if (this != &o) {
    if (impl_) delete impl_;
    impl_ = o.impl_;
    o.impl_ = nullptr;
  }
  return *this;
}

Value::~Value() {
  if (impl_) delete impl_;
}

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------
struct Plugin::Impl {
  v8::Isolate* isolate{};
  v8::Global<v8::Module> module;
  v8::Global<v8::Object> exports;
  std::string resource_name;
};

Plugin::Plugin() : impl_(new Impl()) {}
Plugin::~Plugin() { delete impl_; }

StringView Plugin::resource_name() const noexcept {
  return {impl_->resource_name.data(), impl_->resource_name.size()};
}

// ---------------------------------------------------------------------------
// HostCall
// ---------------------------------------------------------------------------
struct HostCall::Impl {
  Runtime* runtime{};
  std::string name;
  std::string params_json;
  v8::Global<v8::Promise::Resolver> resolver;
  bool settled{false};
};

// ---------------------------------------------------------------------------
// Future
// ---------------------------------------------------------------------------
struct Future::Impl {
  Runtime* runtime{};
  Plugin* plugin{};
  std::string fn_name;
  std::vector<const Value*> args;
  std::chrono::steady_clock::time_point deadline;
  bool started{false};
  v8::Global<v8::Promise> outer_promise;
};

Future& Future::operator=(Future&& o) noexcept {
  if (this != &o) {
    if (impl_) delete impl_;
    impl_ = o.impl_;
    o.impl_ = nullptr;
  }
  return *this;
}

Future::~Future() {
  if (impl_) delete impl_;
}

// ---------------------------------------------------------------------------
// Platform
// ---------------------------------------------------------------------------
struct Platform::Impl {
  std::unique_ptr<v8::Platform> platform;
};

Platform::Platform() : impl_(new Impl()) {
  // NewDefaultPlatform gives V8 its own internal worker threads for GC and
  // compilation. Your plugin code still runs on a single thread — these
  // workers are invisible to the embedder. NewSingleThreadedDefaultPlatform
  // is tempting but V8's Isolate teardown posts delayed tasks that require
  // a worker pool, so it crashes in the destructor.
  impl_->platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(impl_->platform.get());
  v8::V8::Initialize();

  // V8_ENABLE_SANDBOX (compile-time) only says V8 was built with sandbox
  // support. The runtime check below verifies the sandbox's virtual address
  // reservation actually succeeded on THIS host — e.g. on some 32-bit
  // configurations or VMs with constrained virtual memory, the reservation
  // can fail silently, leaving V8 running without isolation. For untrusted
  // code this is an all-or-nothing precondition; fail loudly rather than
  // degrade to an unsafe mode.
  if (!v8::V8::IsSandboxConfiguredSecurely()) {
    ::fprintf(stderr,
              "v8wrap: V8 sandbox is not configured securely on this host; "
              "refusing to run untrusted plugins.\n");
    ::abort();
  }

  // Observability — emit the sandbox reservation size on startup so ops/logs
  // can confirm the expected isolation size on this host (typically 1 TiB on
  // 64-bit Linux). Doesn't affect correctness; IsSandboxConfiguredSecurely
  // above is the actual security gate.
  ::fprintf(stderr,
            "v8wrap: V8 sandbox reserved %llu MiB of virtual address space\n",
            static_cast<unsigned long long>(
                v8::V8::GetSandboxSizeInBytes() >> 20));
}

Platform::~Platform() {
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  delete impl_;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
struct Runtime::Impl {
  v8::Isolate* isolate{};
  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
  v8::Global<v8::Context> context;
  v8::Global<v8::Object> host_object;
  std::vector<std::string> host_names;
  std::deque<std::unique_ptr<HostCall::Impl>> pending_host_calls;
};

namespace {

// Host function callback — installed as a method on the `host` global.
// Captures the name (from callback Data), serializes args[0] via
// JSON.stringify, enqueues a HostCall::Impl with a fresh Promise::Resolver,
// and returns the Promise to JS.
void HostFunctionCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* iso = info.GetIsolate();
  auto ctx = iso->GetCurrentContext();
  auto* runtime = static_cast<Runtime*>(iso->GetData(0));

  v8::Local<v8::String> name_str = info.Data().As<v8::String>();
  v8::String::Utf8Value name_utf8(iso, name_str);
  std::string name(*name_utf8, static_cast<size_t>(name_utf8.length()));

  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) {
    info.GetReturnValue().SetUndefined();
    return;
  }

  std::string params_json;
  if (info.Length() > 0) {
    v8::Local<v8::String> json_str;
    if (v8::JSON::Stringify(ctx, info[0]).ToLocal(&json_str)) {
      v8::String::Utf8Value u(iso, json_str);
      if (*u) params_json.assign(*u, static_cast<size_t>(u.length()));
    }
  }

  auto hc = std::make_unique<HostCall::Impl>();
  hc->runtime = runtime;
  hc->name = std::move(name);
  hc->params_json = std::move(params_json);
  hc->resolver.Reset(iso, resolver);
  runtime->impl_->pending_host_calls.push_back(std::move(hc));

  info.GetReturnValue().Set(resolver->GetPromise());
}

// Module resolution callback — we refuse all imports.
v8::MaybeLocal<v8::Module> ResolveModuleCallback(
    v8::Local<v8::Context> /*context*/, v8::Local<v8::String> /*specifier*/,
    v8::Local<v8::FixedArray> /*import_attributes*/,
    v8::Local<v8::Module> /*referrer*/) {
  auto* iso = v8::Isolate::GetCurrent();
  iso->ThrowException(v8::Exception::Error(
      v8::String::NewFromUtf8Literal(iso, "imports are not allowed in plugins")));
  return v8::MaybeLocal<v8::Module>();
}

// Get the function exported under fn_name by the plugin module namespace.
v8::MaybeLocal<v8::Function> GetExportedFunction(v8::Isolate* iso,
                                                 v8::Local<v8::Context> ctx,
                                                 v8::Local<v8::Object> exports,
                                                 StringView name) {
  auto name_str = MakeV8String(iso, name);
  v8::Local<v8::Value> val;
  if (!exports->Get(ctx, name_str).ToLocal(&val)) return {};
  if (!val->IsFunction()) return {};
  return val.As<v8::Function>();
}

// Run a watchdog around a V8 call; if the deadline expires, TerminateExecution.
template <class F>
auto RunUnderDeadline(v8::Isolate* iso,
                      std::chrono::steady_clock::time_point deadline, F&& f) {
  std::atomic<bool> done{false};
  std::thread watchdog([&] {
    while (!done.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        iso->TerminateExecution();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  auto result = std::forward<F>(f)();
  done.store(true, std::memory_order_release);
  watchdog.join();
  return result;
}

}  // namespace

Runtime::Runtime(Platform&, RuntimeOptions opts) : impl_(new Impl()) {
  impl_->allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());

  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = impl_->allocator.get();
  params.constraints.ConfigureDefaultsFromHeapSize(0, opts.max_heap_bytes);

  impl_->isolate = v8::Isolate::New(params);
  impl_->isolate->SetData(0, this);  // back-ref for host callback

  v8::Isolate::Scope iso_scope(impl_->isolate);
  v8::HandleScope hs(impl_->isolate);
  auto ctx = v8::Context::New(impl_->isolate);
  impl_->context.Reset(impl_->isolate, ctx);
}

Runtime::~Runtime() {
  if (impl_) {
    impl_->pending_host_calls.clear();
    impl_->host_object.Reset();
    impl_->context.Reset();
    if (impl_->isolate) impl_->isolate->Dispose();
    delete impl_;
  }
}

void Runtime::RegisterHost(StringView name) {
  v8::Isolate::Scope iso_scope(impl_->isolate);
  v8::HandleScope hs(impl_->isolate);
  auto ctx = impl_->context.Get(impl_->isolate);
  v8::Context::Scope ctx_scope(ctx);

  v8::Local<v8::Object> host_obj;
  if (impl_->host_object.IsEmpty()) {
    host_obj = v8::Object::New(impl_->isolate);
    auto host_name = v8::String::NewFromUtf8Literal(impl_->isolate, "host");
    ctx->Global()->Set(ctx, host_name, host_obj).Check();
    impl_->host_object.Reset(impl_->isolate, host_obj);
  } else {
    host_obj = impl_->host_object.Get(impl_->isolate);
  }

  auto name_str = MakeV8String(impl_->isolate, name);
  auto tmpl = v8::FunctionTemplate::New(impl_->isolate, HostFunctionCallback,
                                        name_str);
  auto fn = tmpl->GetFunction(ctx).ToLocalChecked();
  host_obj->Set(ctx, name_str, fn).Check();

  impl_->host_names.emplace_back(name.data, name.size);
}

PluginResult Runtime::Load(StringView source, StringView resource_name) {
  PluginResult out;

  v8::Isolate::Scope iso_scope(impl_->isolate);
  v8::HandleScope hs(impl_->isolate);
  auto ctx = impl_->context.Get(impl_->isolate);
  v8::Context::Scope ctx_scope(ctx);
  v8::TryCatch tc(impl_->isolate);

  auto source_str = MakeV8String(impl_->isolate, source);
  auto rn_str = MakeV8String(impl_->isolate, resource_name);

  v8::ScriptOrigin origin(rn_str, 0, 0,
                          /*resource_is_shared_cross_origin=*/false,
                          /*script_id=*/-1,
                          /*source_map_url=*/v8::Local<v8::Value>(),
                          /*resource_is_opaque=*/false,
                          /*is_wasm=*/false,
                          /*is_module=*/true);

  v8::ScriptCompiler::Source script_source(source_str, origin);
  v8::Local<v8::Module> module;
  if (!v8::ScriptCompiler::CompileModule(impl_->isolate, &script_source)
           .ToLocal(&module)) {
    out.error = MakeError(Error::kCompileError, impl_->isolate, tc,
                          resource_name);
    return out;
  }

  if (module->InstantiateModule(ctx, ResolveModuleCallback).IsNothing()) {
    out.error = MakeError(Error::kCompileError, impl_->isolate, tc,
                          resource_name);
    return out;
  }

  v8::Local<v8::Value> eval_result;
  if (!module->Evaluate(ctx).ToLocal(&eval_result)) {
    out.error = MakeError(Error::kRuntimeError, impl_->isolate, tc,
                          resource_name);
    return out;
  }
  impl_->isolate->PerformMicrotaskCheckpoint();

  // In modern V8 Evaluate returns a Promise that's resolved immediately
  // unless the module uses top-level await (which we don't support).
  if (eval_result->IsPromise()) {
    auto p = eval_result.As<v8::Promise>();
    if (p->State() == v8::Promise::kPending) {
      out.error.kind = Error::kCompileError;
      out.error.message = String::Own("top-level await is not supported", 32);
      out.error.resource_name = String::Own(resource_name.data,
                                            resource_name.size);
      return out;
    }
    if (p->State() == v8::Promise::kRejected) {
      out.error = MakeErrorFromException(Error::kRuntimeError, impl_->isolate,
                                         p->Result(), resource_name);
      return out;
    }
  }

  auto ns = module->GetModuleNamespace();
  if (!ns->IsObject()) {
    out.error.kind = Error::kInternalError;
    out.error.message = String::Own("module namespace is not an object", 33);
    out.error.resource_name = String::Own(resource_name.data,
                                          resource_name.size);
    return out;
  }

  auto plugin = new Plugin();
  plugin->impl_->isolate = impl_->isolate;
  plugin->impl_->module.Reset(impl_->isolate, module);
  plugin->impl_->exports.Reset(impl_->isolate, ns.As<v8::Object>());
  plugin->impl_->resource_name.assign(resource_name.data, resource_name.size);

  out.ok = true;
  out.plugin = plugin;
  return out;
}

void Runtime::Unload(Plugin* plugin) {
  if (!plugin) return;
  v8::Isolate::Scope iso_scope(impl_->isolate);
  delete plugin;
}

ValueResult Runtime::ValueFromJson(StringView json) {
  ValueResult out;
  v8::Isolate::Scope iso_scope(impl_->isolate);
  v8::HandleScope hs(impl_->isolate);
  auto ctx = impl_->context.Get(impl_->isolate);
  v8::Context::Scope ctx_scope(ctx);
  v8::TryCatch tc(impl_->isolate);

  auto str = MakeV8String(impl_->isolate, json);
  v8::Local<v8::Value> parsed;
  if (!v8::JSON::Parse(ctx, str).ToLocal(&parsed)) {
    out.error = MakeError(Error::kInvalidJson, impl_->isolate, tc, {});
    return out;
  }
  auto vi = new Value::Impl();
  vi->isolate = impl_->isolate;
  vi->value.Reset(impl_->isolate, parsed);
  out.ok = true;
  out.value = Value(vi);
  return out;
}

StringResult Runtime::ValueToJson(const Value& value) {
  StringResult out;
  if (value.empty()) {
    out.error.kind = Error::kInternalError;
    out.error.message = String::Own("empty Value", 11);
    return out;
  }
  v8::Isolate::Scope iso_scope(impl_->isolate);
  v8::HandleScope hs(impl_->isolate);
  auto ctx = impl_->context.Get(impl_->isolate);
  v8::Context::Scope ctx_scope(ctx);
  v8::TryCatch tc(impl_->isolate);

  auto local = value.impl_->value.Get(impl_->isolate);
  v8::Local<v8::String> str;
  if (!v8::JSON::Stringify(ctx, local).ToLocal(&str)) {
    out.error = MakeError(Error::kInvalidJson, impl_->isolate, tc, {});
    return out;
  }
  v8::String::Utf8Value u(impl_->isolate, str);
  out.string = String::Own(*u, static_cast<size_t>(u.length()));
  out.ok = true;
  return out;
}

ValueResult Runtime::Call(Plugin& plugin, StringView fn_name,
                          Span<const Value*> args, int64_t deadline_ms) {
  ValueResult out;

  v8::Isolate::Scope iso_scope(impl_->isolate);
  v8::HandleScope hs(impl_->isolate);
  auto ctx = impl_->context.Get(impl_->isolate);
  v8::Context::Scope ctx_scope(ctx);
  v8::TryCatch tc(impl_->isolate);

  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(deadline_ms);

  auto exports = plugin.impl_->exports.Get(impl_->isolate);
  v8::Local<v8::Function> fn;
  if (!GetExportedFunction(impl_->isolate, ctx, exports, fn_name).ToLocal(&fn)) {
    out.error.kind = Error::kNoSuchFunction;
    out.error.message = String::Own(fn_name.data, fn_name.size);
    out.error.resource_name =
        String::Own(plugin.impl_->resource_name.data(),
                    plugin.impl_->resource_name.size());
    return out;
  }

  std::vector<v8::Local<v8::Value>> argv;
  argv.reserve(args.size);
  for (size_t i = 0; i < args.size; ++i) {
    argv.push_back(args.data[i]->impl_->value.Get(impl_->isolate));
  }

  v8::MaybeLocal<v8::Value> maybe =
      RunUnderDeadline(impl_->isolate, deadline, [&] {
        return fn->Call(ctx, v8::Undefined(impl_->isolate),
                        static_cast<int>(argv.size()),
                        argv.empty() ? nullptr : argv.data());
      });

  if (tc.HasTerminated()) {
    impl_->isolate->CancelTerminateExecution();
    out.error.kind = Error::kTimeout;
    out.error.message = String::Own("deadline exceeded", 17);
    out.error.resource_name =
        String::Own(plugin.impl_->resource_name.data(),
                    plugin.impl_->resource_name.size());
    return out;
  }
  v8::Local<v8::Value> ret;
  if (!maybe.ToLocal(&ret)) {
    out.error = MakeError(
        Error::kRuntimeError, impl_->isolate, tc,
        {plugin.impl_->resource_name.data(),
         plugin.impl_->resource_name.size()});
    return out;
  }

  if (ret->IsPromise()) {
    auto p = ret.As<v8::Promise>();
    impl_->isolate->PerformMicrotaskCheckpoint();
    if (p->State() == v8::Promise::kPending) {
      out.error.kind = Error::kRuntimeError;
      out.error.message = String::Own(
          "sync call returned a pending Promise — use CallAsync", 53);
      out.error.resource_name =
          String::Own(plugin.impl_->resource_name.data(),
                      plugin.impl_->resource_name.size());
      return out;
    }
    if (p->State() == v8::Promise::kRejected) {
      out.error = MakeErrorFromException(
          Error::kRuntimeError, impl_->isolate, p->Result(),
          {plugin.impl_->resource_name.data(),
           plugin.impl_->resource_name.size()});
      return out;
    }
    ret = p->Result();
  }

  auto vi = new Value::Impl();
  vi->isolate = impl_->isolate;
  vi->value.Reset(impl_->isolate, ret);
  out.ok = true;
  out.value = Value(vi);
  return out;
}

Future Runtime::CallAsync(Plugin& plugin, StringView fn_name,
                          Span<const Value*> args, int64_t deadline_ms) {
  auto fi = new Future::Impl();
  fi->runtime = this;
  fi->plugin = &plugin;
  fi->fn_name.assign(fn_name.data, fn_name.size);
  fi->args.assign(args.data, args.data + args.size);
  fi->deadline = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(deadline_ms);
  return Future(fi);
}

PumpStep Runtime::Pump(Future& future) {
  PumpStep step;

  auto make_timeout = [&](StringView resource) {
    impl_->isolate->CancelTerminateExecution();
    step.kind = PumpStep::kSettled;
    step.result.ok = false;
    step.result.error.kind = Error::kTimeout;
    step.result.error.message = String::Own("deadline exceeded", 17);
    step.result.error.resource_name = String::Own(resource.data, resource.size);
  };

  if (std::chrono::steady_clock::now() >= future.impl_->deadline) {
    make_timeout({future.impl_->plugin->impl_->resource_name.data(),
                  future.impl_->plugin->impl_->resource_name.size()});
    return step;
  }

  v8::Isolate::Scope iso_scope(impl_->isolate);
  v8::HandleScope hs(impl_->isolate);
  auto ctx = impl_->context.Get(impl_->isolate);
  v8::Context::Scope ctx_scope(ctx);
  v8::TryCatch tc(impl_->isolate);
  StringView resource{future.impl_->plugin->impl_->resource_name.data(),
                      future.impl_->plugin->impl_->resource_name.size()};

  if (!future.impl_->started) {
    future.impl_->started = true;

    auto exports = future.impl_->plugin->impl_->exports.Get(impl_->isolate);
    v8::Local<v8::Function> fn;
    if (!GetExportedFunction(
             impl_->isolate, ctx, exports,
             {future.impl_->fn_name.data(), future.impl_->fn_name.size()})
             .ToLocal(&fn)) {
      step.kind = PumpStep::kSettled;
      step.result.ok = false;
      step.result.error.kind = Error::kNoSuchFunction;
      step.result.error.message = String::Own(future.impl_->fn_name.data(),
                                              future.impl_->fn_name.size());
      step.result.error.resource_name = String::Own(resource.data,
                                                    resource.size);
      return step;
    }

    std::vector<v8::Local<v8::Value>> argv;
    argv.reserve(future.impl_->args.size());
    for (const Value* v : future.impl_->args) {
      argv.push_back(v->impl_->value.Get(impl_->isolate));
    }

    v8::MaybeLocal<v8::Value> maybe = RunUnderDeadline(
        impl_->isolate, future.impl_->deadline, [&] {
          return fn->Call(ctx, v8::Undefined(impl_->isolate),
                          static_cast<int>(argv.size()),
                          argv.empty() ? nullptr : argv.data());
        });

    if (tc.HasTerminated()) {
      make_timeout(resource);
      return step;
    }
    v8::Local<v8::Value> ret;
    if (!maybe.ToLocal(&ret)) {
      step.kind = PumpStep::kSettled;
      step.result.ok = false;
      step.result.error = MakeError(Error::kRuntimeError, impl_->isolate, tc,
                                    resource);
      return step;
    }

    if (!ret->IsPromise()) {
      // Sync function returned a value — wrap and settle immediately.
      auto vi = new Value::Impl();
      vi->isolate = impl_->isolate;
      vi->value.Reset(impl_->isolate, ret);
      step.kind = PumpStep::kSettled;
      step.result.ok = true;
      step.result.value = Value(vi);
      return step;
    }
    future.impl_->outer_promise.Reset(impl_->isolate, ret.As<v8::Promise>());
  }

  // Run any microtasks queued by previous host-call resolutions, then check
  // the queue / outer promise state.
  impl_->isolate->PerformMicrotaskCheckpoint();

  if (!impl_->pending_host_calls.empty()) {
    step.kind = PumpStep::kHostCallPending;
    step.host_call = HostCall(impl_->pending_host_calls.front().get());
    return step;
  }

  auto promise = future.impl_->outer_promise.Get(impl_->isolate);
  switch (promise->State()) {
    case v8::Promise::kPending:
      step.kind = PumpStep::kSettled;
      step.result.ok = false;
      step.result.error.kind = Error::kDeadlock;
      step.result.error.message = String::Own(
          "plugin is awaiting but no host call was issued", 46);
      step.result.error.resource_name = String::Own(resource.data,
                                                    resource.size);
      return step;
    case v8::Promise::kFulfilled: {
      auto vi = new Value::Impl();
      vi->isolate = impl_->isolate;
      vi->value.Reset(impl_->isolate, promise->Result());
      step.kind = PumpStep::kSettled;
      step.result.ok = true;
      step.result.value = Value(vi);
      return step;
    }
    case v8::Promise::kRejected:
      step.kind = PumpStep::kSettled;
      step.result.ok = false;
      step.result.error = MakeErrorFromException(
          Error::kRuntimeError, impl_->isolate, promise->Result(), resource);
      return step;
  }
  // Unreachable.
  step.kind = PumpStep::kSettled;
  step.result.ok = false;
  step.result.error.kind = Error::kInternalError;
  step.result.error.message = String::Own("Pump: unknown promise state", 27);
  return step;
}

// ---------------------------------------------------------------------------
// HostCall
// ---------------------------------------------------------------------------
StringView HostCall::name() const noexcept {
  return impl_ ? StringView{impl_->name.data(), impl_->name.size()}
               : StringView{};
}

StringView HostCall::params_json() const noexcept {
  return impl_
             ? StringView{impl_->params_json.data(), impl_->params_json.size()}
             : StringView{};
}

namespace {

void PopFromQueue(HostCall::Impl* hc) {
  auto& q = hc->runtime->impl_->pending_host_calls;
  for (auto it = q.begin(); it != q.end(); ++it) {
    if (it->get() == hc) {
      q.erase(it);
      return;
    }
  }
}

}  // namespace

void HostCall::ResolveJson(StringView json) {
  if (!impl_ || impl_->settled) return;
  impl_->settled = true;
  auto* iso = impl_->runtime->impl_->isolate;

  v8::Isolate::Scope iso_scope(iso);
  v8::HandleScope hs(iso);
  auto ctx = impl_->runtime->impl_->context.Get(iso);
  v8::Context::Scope ctx_scope(ctx);
  v8::TryCatch tc(iso);

  auto resolver = impl_->resolver.Get(iso);
  auto json_str = MakeV8String(iso, json);
  v8::Local<v8::Value> parsed;
  if (!v8::JSON::Parse(ctx, json_str).ToLocal(&parsed)) {
    auto msg = v8::String::NewFromUtf8Literal(iso, "invalid JSON from host");
    resolver->Reject(ctx, v8::Exception::Error(msg)).Check();
  } else {
    resolver->Resolve(ctx, parsed).Check();
  }
  PopFromQueue(impl_);
  impl_ = nullptr;
}

void HostCall::Reject(StringView message) {
  if (!impl_ || impl_->settled) return;
  impl_->settled = true;
  auto* iso = impl_->runtime->impl_->isolate;

  v8::Isolate::Scope iso_scope(iso);
  v8::HandleScope hs(iso);
  auto ctx = impl_->runtime->impl_->context.Get(iso);
  v8::Context::Scope ctx_scope(ctx);

  auto resolver = impl_->resolver.Get(iso);
  auto msg_str = MakeV8String(iso, message);
  resolver->Reject(ctx, v8::Exception::Error(msg_str)).Check();
  PopFromQueue(impl_);
  impl_ = nullptr;
}

}  // namespace v8wrap
