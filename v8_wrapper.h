// v8_wrapper.h — stdlib-agnostic public API for the V8 plugin runtime.
//
// Every type exposed here is either a POD (StringView, Span, Error, *Result)
// or a move-only handle whose layout is a single raw pointer (Value, Future,
// Platform, Runtime, ...). None of these mangle differently between libc++
// and libstdc++, so this header can be consumed from either toolchain and the
// wrapper .cc (compiled against V8's libc++) will link cleanly to it.
//
// Do NOT include <string>, <memory>, <expected>, etc. here. That would
// re-introduce the stdlib-identity problem we're deliberately avoiding.

#ifndef V8WRAP_V8_WRAPPER_H_
#define V8WRAP_V8_WRAPPER_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>  // ::free for inline String destructor

namespace v8wrap {

// StringView — non-owning (data, size) pair.
struct StringView {
  const char* data;
  size_t size;

  constexpr StringView() noexcept : data(nullptr), size(0) {}
  constexpr StringView(const char* d, size_t s) noexcept : data(d), size(s) {}

  // Convenience: construct from a NUL-terminated C string (inline strlen).
  explicit StringView(const char* c_str) noexcept : data(c_str), size(0) {
    if (c_str) {
      while (c_str[size]) ++size;
    }
  }
};

// String — owning, heap-allocated via ::malloc/::free.
// Allocated only inside v8_wrapper.cc; users typically receive these in
// results. Movable, non-copyable. Destructor calls ::free which is a libc
// symbol (stdlib-agnostic) so ~String is safe to inline across TUs.
class String {
 public:
  String() noexcept = default;

  String(String&& o) noexcept : data_(o.data_), size_(o.size_) {
    o.data_ = nullptr;
    o.size_ = 0;
  }
  String& operator=(String&& o) noexcept {
    if (this != &o) {
      if (data_) ::free(data_);
      data_ = o.data_;
      size_ = o.size_;
      o.data_ = nullptr;
      o.size_ = 0;
    }
    return *this;
  }
  ~String() {
    if (data_) ::free(data_);
  }

  String(const String&) = delete;
  String& operator=(const String&) = delete;

  // Getters — simple accessors use variable-like naming per Google style.
  const char* data() const noexcept { return data_; }
  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  StringView view() const noexcept { return {data_, size_}; }

  // Factory: copies [src, src+n) into a fresh ::malloc'd buffer. Defined in
  // v8_wrapper.cc to keep <string.h> out of this header.
  static String Own(const char* src, size_t n);

 private:
  char* data_ = nullptr;
  size_t size_ = 0;
};

// Span<T> — non-owning (data, size).
template <class T>
struct Span {
  const T* data;
  size_t size;
  constexpr Span() noexcept : data(nullptr), size(0) {}
  constexpr Span(const T* d, size_t s) noexcept : data(d), size(s) {}
};

// Error — returned by all fallible operations.
struct Error {
  enum Kind : int32_t {
    kNone = 0,
    kCompileError,        // syntax / module-linking error
    kRuntimeError,        // uncaught JS exception
    kTimeout,             // deadline expired
    kNoSuchFunction,      // not exported by plugin
    kNotAFunction,        // export exists but isn't callable
    kInvalidJson,         // JSON.parse or JSON.stringify failed
    kHostNotRegistered,   // JS called host.X but X wasn't RegisterHost'd
    kDeadlock,            // async pending with no host call to service
    kInternalError,       // wrapper or V8 failure
  };
  Kind kind = kNone;
  String message;
  String resource_name;
  int32_t line = -1;    // -1 = not available
  int32_t column = -1;
};

// Forward declarations.
class Runtime;
class Plugin;
class Future;
class HostCall;

// Value — opaque handle to a JS value, backed by v8::Global.
// Must not outlive the Runtime that produced it.
class Value {
 public:
  Value() noexcept = default;
  Value(Value&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
  Value& operator=(Value&& o) noexcept;  // defined in .cc
  ~Value();                               // defined in .cc
  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;

  bool empty() const noexcept { return impl_ == nullptr; }

  // Public for the wrapper .cc; Impl is incomplete externally so opaque.
  struct Impl;
  Impl* impl_ = nullptr;
  explicit Value(Impl* i) noexcept : impl_(i) {}
};

// Result types — specialized per return type (simpler than a templated
// Result<T, E> for our small API surface).
struct ValueResult {
  bool ok = false;
  Value value;
  Error error;
};

struct StringResult {
  bool ok = false;
  String string;
  Error error;
};

struct PluginResult {
  bool ok = false;
  Plugin* plugin = nullptr;  // non-owning; valid until Unload or ~Runtime.
  Error error;
};

// Plugin — loaded ES module; holds the module + its exports.
class Plugin {
 public:
  StringView resource_name() const noexcept;

  // Public for the wrapper .cc; Impl is incomplete externally so opaque.
  struct Impl;
  Impl* impl_ = nullptr;

  Plugin();
  ~Plugin();
  Plugin(const Plugin&) = delete;
  Plugin& operator=(const Plugin&) = delete;
};

// HostCall — handed to caller when JS does `await host.X(params)`.
// Trivially copyable non-owning handle; the underlying Impl lives in the
// Runtime's pending queue until ResolveJson / Reject is called.
class HostCall {
 public:
  HostCall() noexcept = default;
  HostCall(const HostCall&) noexcept = default;
  HostCall& operator=(const HostCall&) noexcept = default;

  StringView name() const noexcept;
  StringView params_json() const noexcept;
  bool valid() const noexcept { return impl_ != nullptr; }

  void ResolveJson(StringView json);
  void Reject(StringView message);

  // Public for the wrapper .cc; Impl is incomplete externally so opaque.
  struct Impl;
  Impl* impl_ = nullptr;
  explicit HostCall(Impl* i) noexcept : impl_(i) {}
};

// PumpStep — what Runtime::Pump returns.
struct PumpStep {
  enum Kind : int32_t { kHostCallPending = 1, kSettled = 2 };
  Kind kind = kSettled;
  HostCall host_call;   // valid iff kind == kHostCallPending
  ValueResult result;   // valid iff kind == kSettled
};

// Future — in-flight async call.
class Future {
 public:
  Future() noexcept = default;
  Future(Future&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
  Future& operator=(Future&& o) noexcept;  // defined in .cc
  ~Future();                                // defined in .cc
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  bool empty() const noexcept { return impl_ == nullptr; }

  // Public for the wrapper .cc; Impl is incomplete externally so opaque.
  struct Impl;
  Impl* impl_ = nullptr;
  explicit Future(Impl* i) noexcept : impl_(i) {}
};

// Platform — V8 process-wide init. Construct once before any Runtime.
class Platform {
 public:
  Platform();
  ~Platform();
  Platform(const Platform&) = delete;
  Platform& operator=(const Platform&) = delete;

  struct Impl;
  Impl* impl_ = nullptr;
};

// RuntimeOptions — configuration knobs for a Runtime.
struct RuntimeOptions {
  size_t max_heap_bytes = 64 * 1024 * 1024;  // 64 MiB default
};

// Runtime — one V8 isolate + context. Create multiple for isolation.
// Single-threaded: do not share a Runtime across threads; do not re-enter a
// Runtime from within a HostCall handler of that same Runtime.
class Runtime {
 public:
  explicit Runtime(Platform* platform, RuntimeOptions opts = {});
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Register a host function name before loading any plugin. JS plugin code
  // can call it via the host global: `await host.<name>(params)`.
  void RegisterHost(StringView name);

  // Compile + evaluate an ES module. `resource_name` appears in
  // Error::resource_name (e.g. "billing/discounts.js").
  PluginResult Load(StringView source, StringView resource_name);

  // Drop a Plugin. All Values derived from it become invalid at the V8 level
  // (their handles will hit dead isolate slots if used after this).
  void Unload(Plugin* plugin);

  // JSON <-> Value.
  ValueResult ValueFromJson(StringView json);
  StringResult ValueToJson(const Value& value);

  // Sync call. Fails with kRuntimeError if the function returns a pending
  // Promise (use CallAsync for async functions).
  ValueResult Call(Plugin* plugin, StringView fn_name,
                   Span<const Value*> args, int64_t deadline_ms);

  // Async call — returns immediately with a Future. Drive via Pump until
  // PumpStep::kSettled.
  Future CallAsync(Plugin* plugin, StringView fn_name,
                   Span<const Value*> args, int64_t deadline_ms);

  // Advance a Future. Returns kHostCallPending if JS suspended on a
  // `await host.X(...)`, or kSettled if the outer call finished (success or
  // error including kTimeout / kDeadlock).
  PumpStep Pump(Future* future);

  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace v8wrap

#endif  // V8WRAP_V8_WRAPPER_H_
