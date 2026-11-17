// Differential cases for src/kv-conversion/proof.ts (no bun tests). The proof needs the pinned
// Deno, a prepared native KV backend (src/runtime/native-kv-bootstrap.ts reads the repository's
// .data receipt) and node:v8 deserialize, so only what runs before the backend is prepared is
// exercised: the SHA-256 pin assertion (node:assert/strict's message) and the import.meta.main
// argument check. Expected values were recorded from the TS under Bun 1.4.0
// (`proveLegacyRoundtrip("/nonexistent-proof-directory", "/nonexistent-deno", pin)` and
// `bun src/kv-conversion/proof.ts <args>`).
// Links: tests/kv_conversion_proof_differential_test.cpp src/kv_conversion/proof.cpp src/kv_conversion/convert.cpp src/kv_conversion/cursors.cpp
//        src/kv_conversion/legacy_codec.cpp src/kv_conversion/native_schema.cpp src/kv_conversion/typed_value.cpp src/runtime/native_kv_bootstrap.cpp
//        src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/storage/value_compatibility.cpp src/storage/native_addon.cpp
//        src/storage/key_compatibility.cpp
#include "uos/kv_conversion/proof.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace proof = uos::kv_conversion::proof;

  const std::string USAGE = "Usage: bun --no-env-file src/kv-conversion/proof.ts <NEW-disposable-directory> <pinned-deno> <sha256>";

  struct Failure {
    std::string name;
    std::string message;
  };

  template <class T> rt::Task<std::optional<Failure>> failureOf(rt::Task<T> task) {
    std::optional<Failure> failure;
    try {
      co_await std::move(task);
    } catch (const rt::JsError& error) {
      failure = Failure{error.name, error.message};
    } catch (...) {
      failure = Failure{"?", rt::reasonToString(std::current_exception())};
    }
    co_return failure;
  }

  rt::Task<void> expectPinFailure(std::string pin, std::string message) {
    auto proving = failureOf(proof::proveLegacyRoundtrip("/nonexistent-proof-directory", "/nonexistent-deno", pin));
    const std::optional<Failure> failure = co_await std::move(proving);
    ASSERT(failure.has_value());
    EXPECT_EQ(failure->name, std::string("AssertionError"));
    EXPECT_EQ(failure->message, message);
  }

  rt::Task<void> expectUsage(std::vector<std::string> argv) {
    const rt::process::ScopedArgv scope(std::move(argv));
    auto running = failureOf(proof::importMetaMain());
    const std::optional<Failure> failure = co_await std::move(running);
    ASSERT(failure.has_value());
    EXPECT_EQ(failure->name, std::string("Error"));
    EXPECT_EQ(failure->message, USAGE);
  }
}

ASYNC_TEST("proof: an empty pin is not SHA-256") {
  auto checking = expectPinFailure("", "The explicit legacy codec pin must be SHA-256\n\n0 !== 32\n");
  co_await std::move(checking);
}

ASYNC_TEST("proof: a pin with an incomplete hex pair keeps the complete bytes") {
  auto checking = expectPinFailure("abc", "The explicit legacy codec pin must be SHA-256\n\n1 !== 32\n");
  co_await std::move(checking);
}

ASYNC_TEST("proof: a pin that is not hex decodes to nothing") {
  auto checking = expectPinFailure("zz", "The explicit legacy codec pin must be SHA-256\n\n0 !== 32\n");
  co_await std::move(checking);
}

ASYNC_TEST("proof: 31 and 33 byte pins are refused") {
  auto shorter = expectPinFailure(rt::repeat("0", 62), "The explicit legacy codec pin must be SHA-256\n\n31 !== 32\n");
  co_await std::move(shorter);
  auto longer = expectPinFailure(rt::repeat("0", 66), "The explicit legacy codec pin must be SHA-256\n\n33 !== 32\n");
  co_await std::move(longer);
}

ASYNC_TEST("proof: hex decoding stops at the first invalid pair") {
  auto checking = expectPinFailure(rt::repeat("0", 63) + "g", "The explicit legacy codec pin must be SHA-256\n\n31 !== 32\n");
  co_await std::move(checking);
}

ASYNC_TEST("proof: a 100 byte pin is refused") {
  auto checking = expectPinFailure(rt::repeat("a", 200), "The explicit legacy codec pin must be SHA-256\n\n100 !== 32\n");
  co_await std::move(checking);
}

ASYNC_TEST("proof main: every argument is required") {
  auto none = expectUsage({"/bun", "/proof.ts"});
  co_await std::move(none);
  auto directoryOnly = expectUsage({"/bun", "/proof.ts", "/x"});
  co_await std::move(directoryOnly);
  auto withoutPin = expectUsage({"/bun", "/proof.ts", "/x", "/deno"});
  co_await std::move(withoutPin);
  auto emptyPin = expectUsage({"/bun", "/proof.ts", "/x", "/deno", ""});
  co_await std::move(emptyPin);
}

ASYNC_TEST("proof main: the proof's own failure propagates") {
  const rt::process::ScopedArgv scope({"/bun", "/proof.ts", "/nonexistent-proof-directory", "/nonexistent-deno", "abc"});
  auto running = failureOf(proof::importMetaMain());
  const std::optional<Failure> failure = co_await std::move(running);
  ASSERT(failure.has_value());
  EXPECT_EQ(failure->name, std::string("AssertionError"));
  EXPECT_EQ(failure->message, std::string("The explicit legacy codec pin must be SHA-256\n\n1 !== 32\n"));
}
