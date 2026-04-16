// test_main.cc — exercises v8_wrapper.h with stock libstdc++. No V8 or libc++
// anywhere in this file; it only knows v8wrap::* types. This is the proof
// that the boundary works.

#include "v8_wrapper.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace {

v8wrap::StringView ToV8Sv(std::string_view s) { return {s.data(), s.size()}; }

std::string_view ToStdSv(v8wrap::StringView s) { return {s.data, s.size}; }

std::string_view ToStdSv(const v8wrap::String& s) {
  return {s.data(), s.size()};
}

void DumpError(const char* where, const v8wrap::Error& e) {
  std::cout << "  " << where << " FAILED: kind=" << int(e.kind)
            << " msg=" << ToStdSv(e.message);
  if (e.line > 0) {
    std::cout << " at " << ToStdSv(e.resource_name) << ":" << e.line << ":"
              << e.column;
  }
  std::cout << '\n';
}

// Plugin sources.
constexpr std::string_view kPluginA = R"(
let call_count = 0;  // module-level state, persists across calls

export function greet(input) {
  call_count++;
  return { greeting: "hello " + input.name, count: call_count };
}

export function merge(cfg, req) {
  // `cfg` is a persistent Value passed every call; `req` varies.
  return { kind: cfg.kind, sum: req.a + req.b };
}

export async function fetch_and_double(input) {
  const resp = await host.http({ url: input.url });
  return { url: input.url, doubled: resp.value * 2 };
}

export async function many_hops(input) {
  const a = await host.http({ step: 1 });
  const b = await host.xmlrpc({ step: 2, prev: a.v });
  return { total: a.v + b.v + input.extra };
}

export function boom() {
  throw new Error("intentional");
}

export function loop_forever() {
  while (true) {}
}

export async function deadlock() {
  await new Promise(() => { /* never resolves */ });
}
)";

constexpr std::string_view kPluginB = R"(
let seen = [];
export function push(x) { seen.push(x.v); return { all: seen }; }
)";

// Fake host-function implementations. A real embedder would wire these to
// HTTP / XML-RPC / DB clients.
std::string FakeHttp(std::string_view p) {
  if (p.find("\"step\":1") != std::string_view::npos) return R"({"v":10})";
  if (p.find("\"step\":2") != std::string_view::npos) return R"({"v":20})";
  if (p.find("\"url\"") != std::string_view::npos) return R"({"value":21})";
  return R"({"value":0})";
}

std::string FakeXmlrpc(std::string_view p) { return FakeHttp(p); }

using HostFn = std::string (*)(std::string_view);
using HostDispatch = std::map<std::string, HostFn, std::less<>>;

// Drives an async call: services host requests against `dispatch` until the
// outer call settles, then returns the result.
v8wrap::ValueResult Drive(v8wrap::Runtime& rt, v8wrap::Future fut,
                          const HostDispatch& dispatch) {
  for (;;) {
    auto step = rt.Pump(fut);
    if (step.kind == v8wrap::PumpStep::kHostCallPending) {
      auto hc = step.host_call;
      auto name = std::string_view(hc.name().data, hc.name().size);
      auto it = dispatch.find(name);
      if (it == dispatch.end()) {
        hc.Reject(ToV8Sv("unknown host: " + std::string(name)));
      } else {
        auto resp = it->second(
            std::string_view(hc.params_json().data, hc.params_json().size));
        hc.ResolveJson(ToV8Sv(resp));
      }
    } else {
      return std::move(step.result);
    }
  }
}

// Test harness — one Runner per process, holding the Platform.
struct Runner {
  v8wrap::Platform platform;
  int failures = 0;

  void ExpectOk(const char* name, bool cond) {
    if (cond) {
      std::cout << "  " << name << " ok\n";
    } else {
      std::cout << "  " << name << " FAILED\n";
      ++failures;
    }
  }

  void RunAll();
  void SyncTests();
  void PersistentArgTests();
  void AsyncTests();
  void ErrorTests();
  void IsolationTests();
};

void Runner::SyncTests() {
  std::cout << "\n=== Sync ===\n";
  v8wrap::Runtime rt(platform);
  auto pr = rt.Load(ToV8Sv(kPluginA), "pluginA.js");
  if (!pr.ok) {
    DumpError("load pluginA", pr.error);
    ++failures;
    return;
  }

  auto input = rt.ValueFromJson(R"({"name":"V8"})");
  if (!input.ok) {
    DumpError("ValueFromJson", input.error);
    ++failures;
    return;
  }

  const v8wrap::Value* args[] = {&input.value};
  auto r1 = rt.Call(*pr.plugin, "greet",
                    v8wrap::Span<const v8wrap::Value*>{args, 1}, 100);
  if (!r1.ok) {
    DumpError("greet", r1.error);
    ++failures;
    return;
  }

  auto js = rt.ValueToJson(r1.value);
  ExpectOk("greet returned JSON", js.ok);
  std::cout << "    greet => " << ToStdSv(js.string) << "\n";

  // Second call — module-level state persists.
  auto r2 = rt.Call(*pr.plugin, "greet",
                    v8wrap::Span<const v8wrap::Value*>{args, 1}, 100);
  auto js2 = rt.ValueToJson(r2.value);
  ExpectOk("call_count incremented",
           std::string_view(js2.string.data(), js2.string.size())
                   .find("\"count\":2") != std::string::npos);

  rt.Unload(pr.plugin);
}

void Runner::PersistentArgTests() {
  std::cout << "\n=== Persistent arg (parse once, call many) ===\n";
  v8wrap::Runtime rt(platform);
  auto pr = rt.Load(ToV8Sv(kPluginA), "pluginA.js");
  if (!pr.ok) {
    DumpError("load", pr.error);
    ++failures;
    return;
  }

  // Big cfg parsed once.
  auto cfg = rt.ValueFromJson(R"({"kind":"demo"})");
  if (!cfg.ok) {
    DumpError("cfg", cfg.error);
    ++failures;
    return;
  }

  int last_sum = -1;
  for (int i = 0; i < 5; ++i) {
    std::string req_json = "{\"a\":" + std::to_string(i) + ",\"b\":10}";
    auto req = rt.ValueFromJson(ToV8Sv(req_json));
    const v8wrap::Value* args[] = {&cfg.value, &req.value};
    auto r = rt.Call(*pr.plugin, "merge",
                     v8wrap::Span<const v8wrap::Value*>{args, 2}, 100);
    if (!r.ok) {
      DumpError("merge", r.error);
      ++failures;
      return;
    }
    auto js = rt.ValueToJson(r.value);
    std::cout << "    merge iter " << i << " => " << ToStdSv(js.string) << "\n";
    last_sum = i + 10;
  }
  ExpectOk("merge loop completed", last_sum == 14);
  rt.Unload(pr.plugin);
}

void Runner::AsyncTests() {
  std::cout << "\n=== Async (host-backed I/O) ===\n";
  v8wrap::Runtime rt(platform);
  rt.RegisterHost("http");
  rt.RegisterHost("xmlrpc");
  HostDispatch dispatch = {
      {"http", &FakeHttp},
      {"xmlrpc", &FakeXmlrpc},
  };

  auto pr = rt.Load(ToV8Sv(kPluginA), "pluginA.js");
  if (!pr.ok) {
    DumpError("load", pr.error);
    ++failures;
    return;
  }

  // (1) Single host hop.
  auto in1 = rt.ValueFromJson(R"({"url":"/x"})");
  const v8wrap::Value* a1[] = {&in1.value};
  auto f1 = rt.CallAsync(*pr.plugin, "fetch_and_double",
                         v8wrap::Span<const v8wrap::Value*>{a1, 1}, 1000);
  auto r1 = Drive(rt, std::move(f1), dispatch);
  if (!r1.ok) {
    DumpError("fetch_and_double", r1.error);
    ++failures;
    return;
  }
  auto js1 = rt.ValueToJson(r1.value);
  std::cout << "    fetch_and_double => " << ToStdSv(js1.string) << "\n";
  ExpectOk("fetch_and_double doubled=42",
           ToStdSv(js1.string).find("\"doubled\":42") != std::string::npos);

  // (2) Many hops through mixed host functions.
  auto in2 = rt.ValueFromJson(R"({"extra":100})");
  const v8wrap::Value* a2[] = {&in2.value};
  auto f2 = rt.CallAsync(*pr.plugin, "many_hops",
                         v8wrap::Span<const v8wrap::Value*>{a2, 1}, 1000);
  auto r2 = Drive(rt, std::move(f2), dispatch);
  if (!r2.ok) {
    DumpError("many_hops", r2.error);
    ++failures;
    return;
  }
  auto js2 = rt.ValueToJson(r2.value);
  std::cout << "    many_hops => " << ToStdSv(js2.string) << "\n";
  ExpectOk("many_hops total=130",
           ToStdSv(js2.string).find("\"total\":130") != std::string::npos);

  rt.Unload(pr.plugin);
}

void Runner::ErrorTests() {
  std::cout << "\n=== Errors ===\n";
  v8wrap::Runtime rt(platform);

  // (a) Compile error.
  auto bad = rt.Load(ToV8Sv("export functin broken(  { }"), "bad.js");
  ExpectOk("syntax error detected",
           !bad.ok && bad.error.kind == v8wrap::Error::kCompileError);
  if (!bad.ok) {
    std::cout << "    -> " << ToStdSv(bad.error.message) << " at "
              << ToStdSv(bad.error.resource_name) << ":" << bad.error.line
              << ":" << bad.error.column << "\n";
  }

  auto pr = rt.Load(ToV8Sv(kPluginA), "pluginA.js");
  if (!pr.ok) {
    DumpError("load", pr.error);
    ++failures;
    return;
  }

  // (b) Runtime error — expect line+file from the throw site.
  auto r_boom = rt.Call(*pr.plugin, "boom", {}, 100);
  ExpectOk("runtime error detected",
           !r_boom.ok && r_boom.error.kind == v8wrap::Error::kRuntimeError);
  if (!r_boom.ok) {
    std::cout << "    boom -> " << ToStdSv(r_boom.error.message) << " at "
              << ToStdSv(r_boom.error.resource_name) << ":" << r_boom.error.line
              << ":" << r_boom.error.column << "\n";
  }

  // (c) Timeout.
  auto r_loop = rt.Call(*pr.plugin, "loop_forever", {}, 50);
  ExpectOk("infinite loop terminated",
           !r_loop.ok && r_loop.error.kind == v8wrap::Error::kTimeout);

  // (d) NoSuchFunction.
  auto r_missing = rt.Call(*pr.plugin, "does_not_exist", {}, 50);
  ExpectOk("missing function detected",
           !r_missing.ok &&
               r_missing.error.kind == v8wrap::Error::kNoSuchFunction);

  // (e) Deadlock — plugin awaits something that never resolves, no host call.
  auto f_dead = rt.CallAsync(*pr.plugin, "deadlock", {}, 100);
  HostDispatch empty;
  auto r_dead = Drive(rt, std::move(f_dead), empty);
  ExpectOk("deadlock detected (or timed out)",
           !r_dead.ok && (r_dead.error.kind == v8wrap::Error::kDeadlock ||
                          r_dead.error.kind == v8wrap::Error::kTimeout));

  rt.Unload(pr.plugin);
}

void Runner::IsolationTests() {
  std::cout << "\n=== Isolation (multiple Runtimes) ===\n";
  v8wrap::Runtime rt_a(platform);
  v8wrap::Runtime rt_b(platform);

  auto pa = rt_a.Load(ToV8Sv(kPluginB), "pluginB-A.js");
  auto pb = rt_b.Load(ToV8Sv(kPluginB), "pluginB-B.js");
  if (!pa.ok || !pb.ok) {
    ++failures;
    return;
  }

  auto va = rt_a.ValueFromJson(R"({"v":1})");
  auto vb = rt_b.ValueFromJson(R"({"v":99})");
  const v8wrap::Value* aa[] = {&va.value};
  const v8wrap::Value* bb[] = {&vb.value};

  rt_a.Call(*pa.plugin, "push", {aa, 1}, 50);
  rt_a.Call(*pa.plugin, "push", {aa, 1}, 50);
  rt_b.Call(*pb.plugin, "push", {bb, 1}, 50);

  auto r_a = rt_a.Call(*pa.plugin, "push", {aa, 1}, 50);
  auto j_a = rt_a.ValueToJson(r_a.value);
  std::cout << "    rt_a push last => " << ToStdSv(j_a.string) << "\n";

  auto r_b = rt_b.Call(*pb.plugin, "push", {bb, 1}, 50);
  auto j_b = rt_b.ValueToJson(r_b.value);
  std::cout << "    rt_b push last => " << ToStdSv(j_b.string) << "\n";

  ExpectOk("rt_a accumulated 3",
           ToStdSv(j_a.string).find("[1,1,1]") != std::string::npos);
  ExpectOk("rt_b accumulated 2 (independent)",
           ToStdSv(j_b.string).find("[99,99]") != std::string::npos);
}

void Runner::RunAll() {
  SyncTests();
  PersistentArgTests();
  AsyncTests();
  ErrorTests();
  IsolationTests();
}

}  // namespace

int main() {
  Runner r;
  r.RunAll();
  if (r.failures == 0) {
    std::cout << "\nAll tests passed.\n";
  } else {
    std::cout << "\nFAILURES: " << r.failures << "\n";
  }
  return r.failures == 0 ? 0 : 1;
}
