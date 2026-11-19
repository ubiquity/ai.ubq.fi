#include "uos/kv_conversion/legacy_codec.hpp"
#include "runtime/paths.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include <cmath>
#include <filesystem>
#include <optional>
#include <tuple>
#include <utility>

namespace uos::kv_conversion::legacy_codec {
  struct LegacyCodec::State {
    rt::process::Subprocess child;
    std::optional<rt::AbortSignal> signal;
    rt::EventListener interrupted;
    rt::Response stderrResponse;
    rt::Promise<std::string> stderrText;
    rt::ReadableStreamDefaultReader<std::vector<std::uint8_t>> reader;
    rt::TextDecoder decoder;
    std::string buffer;
    bool settled = false;
  };

  namespace {
    using TypedValue = typed_value::TypedValue;

    // `Buffer.from(value, "base64")` for the codec's reply, which the TS casts to string
    // unchecked: Bun takes strings, arrays and array-like objects and throws for the rest.
    std::vector<std::uint8_t> bufferFromBase64(const json::value& value) {
      if (const std::optional<std::string_view> text = rt::getString(value)) return rt::fromBase64(*text);
      const auto toUint8 = [](const json::value* element) { return static_cast<std::uint8_t>(rt::toUint32(rt::toNumber(element)) & 0xFF); };
      if (const json::array* array = rt::getArray(value)) {
        std::vector<std::uint8_t> bytes;
        for (const json::value& element : *array) bytes.push_back(toUint8(&element));
        return bytes;
      }
      std::string received;
      if (const json::object* object = rt::getObject(value)) {
        if (const json::value* length = rt::get(*object, "length")) {
          const std::optional<double> count = rt::getNumber(*length);
          if (!count || !(*count > 0)) return {};
          std::vector<std::uint8_t> bytes;
          for (double index = 0; index < std::trunc(*count); index += 1) bytes.push_back(toUint8(rt::get(*object, rt::numberToString(index))));
          return bytes;
        }
        const json::array* data = rt::getArray(*object, "data");
        if (rt::getString(*object, "type") == std::optional<std::string_view>("Buffer") && data) return bufferFromBase64(*data);
        received = "an instance of Object";
      } else if (value.is_null()) {
        received = "null";
      } else {
        received = "type " + std::string(rt::typeOf(value)) + " (" + rt::toString(value) + ")";
      }
      throw rt::CodedTypeError(
          "The first argument must be of type string or an instance of Buffer, ArrayBuffer, or Array or an Array-like Object. Received " + received,
          "ERR_INVALID_ARG_TYPE");
    }

    rt::Task<json::value> request(std::shared_ptr<LegacyCodec::State> state, json::value input) {
      const std::string line = rt::jsonStringify(input) + "\n";
      state->child.stdin().write(line);
      co_await rt::microtask();
      rt::Promise<void> flushed = state->child.stdin().flush();
      co_await flushed.wait();
      while (state->buffer.find('\n') == std::string::npos) {
        const rt::ReadableStreamReadResult<std::vector<std::uint8_t>> chunk = co_await state->reader.read();
        if (chunk.done) {
          const std::string errors = co_await state->stderrText.wait();
          throw rt::JsError("Legacy codec failed: " + errors);
        }
        rt::append(state->buffer, state->decoder.decode(*chunk.value, {.stream = true}));
      }
      const std::size_t newline = state->buffer.find('\n');
      json::value output = rt::jsonParse(std::string_view(state->buffer).substr(0, newline));
      state->buffer = state->buffer.substr(newline + 1);
      co_return output;
    }

    rt::Task<TypedValue> decodeBytes(std::shared_ptr<LegacyCodec::State> state, std::vector<std::uint8_t> bytes) {
      json::object input;
      input["operation"] = "decode";
      input["bytes"] = json::string(rt::toBase64(bytes));
      json::value payload(std::move(input));
      rt::Task<json::value> exchange = request(std::move(state), std::move(payload));
      TypedValue output = co_await std::move(exchange);
      co_return output;
    }

    rt::Task<std::vector<std::uint8_t>> encodeValue(std::shared_ptr<LegacyCodec::State> state, TypedValue value) {
      json::object input;
      input["operation"] = "encode";
      input["value"] = std::move(value);
      json::value payload(std::move(input));
      rt::Task<json::value> exchange = request(std::move(state), std::move(payload));
      const json::value output = co_await std::move(exchange);
      co_return bufferFromBase64(output);
    }

    rt::Task<void> closeCodec(std::shared_ptr<LegacyCodec::State> state) {
      if (state->settled) co_return;
      state->settled = true;
      if (state->signal) state->signal->removeEventListener("abort", state->interrupted);
      rt::Promise<void> ended = state->child.stdin().end();
      co_await ended.wait();
      rt::Promise<int> exited = state->child.exited();
      auto settledOutputs = rt::all(exited, state->stderrText);
      const auto settledValues = co_await settledOutputs.wait();
      const auto& exit = std::get<0>(settledValues);
      const auto& errors = std::get<1>(settledValues);
      state->reader.releaseLock();
      if (exit != 0) throw rt::JsError("Legacy codec exited " + std::to_string(exit) + ": " + errors);
    }

    rt::Task<void> abortCodec(std::shared_ptr<LegacyCodec::State> state) {
      if (state->settled) co_return;
      state->settled = true;
      if (state->signal) state->signal->removeEventListener("abort", state->interrupted);
      state->child.kill();
      rt::Promise<int> exited = state->child.exited();
      auto settledOutputs = rt::all(exited, state->stderrText);
      co_await settledOutputs.wait();
      state->reader.releaseLock();
    }

    rt::Task<LegacyCodec> open(std::string binary, std::string expectedSha256, std::string cwd, std::optional<rt::AbortSignal> signal) {
      static const rt::Regex SHA256("^[a-f0-9]{64}$");
      const std::string path = rt::path::resolve(binary);
      const std::string binarySha256 = co_await fileDigest(path);
      if (!rt::fs::lstatSync(path).isFile() || !SHA256.test(expectedSha256) || binarySha256 != expectedSha256)
        throw rt::JsError("Pinned legacy Deno binary does not match its SHA-256: expected " + expectedSha256 + ", observed " + binarySha256);
      const std::string helper = (std::filesystem::path(runtime::paths::resourceRoot()) / "src/kv-conversion/v8-helper.mjs").native();
      const std::vector<std::pair<std::string, std::string>> environment{{"DENO_DIR", rt::path::join(cwd, ".deno-codec-cache")}};
      rt::process::SpawnOptions versionOptions;
      versionOptions.cwd = cwd;
      versionOptions.env = environment;
      versionOptions.stdout = rt::process::Stdio::pipe;
      versionOptions.stderr = rt::process::Stdio::pipe;
      const std::vector<std::string> versionCommand{path, "run", "--no-config", "--no-lock", "--no-prompt", helper, "--identity"};
      const rt::process::Subprocess versionChild = rt::process::spawn(versionCommand, versionOptions);
      rt::Response versionStdout(rt::BodyInit(versionChild.stdout()));
      rt::Response versionStderr(rt::BodyInit(versionChild.stderr()));
      rt::Promise<std::string> versionText(versionStdout.text());
      rt::Promise<std::string> versionErrorText(versionStderr.text());
      rt::Promise<int> versionExited = versionChild.exited();
      auto versionOutputs = rt::all(versionText, versionErrorText, versionExited);
      const auto versionValues = co_await versionOutputs.wait();
      const auto& version = std::get<0>(versionValues);
      const auto& versionError = std::get<1>(versionValues);
      const auto& code = std::get<2>(versionValues);
      if (code != 0 || !versionError.empty() || rt::trim(version) != LEGACY_DENO_VERSION) throw rt::JsError("Unsupported pinned legacy Deno version");
      rt::process::SpawnOptions options;
      options.cwd = cwd;
      options.env = environment;
      options.stdin = rt::process::Stdio::pipe;
      options.stdout = rt::process::Stdio::pipe;
      options.stderr = rt::process::Stdio::pipe;
      const std::vector<std::string> command{path, "run", "--no-config", "--no-lock", "--no-prompt", helper};
      const rt::process::Subprocess child = rt::process::spawn(command, options);
      const rt::EventListener interrupted = [child] { child.kill(); };
      if (signal) signal->addEventListener("abort", interrupted, {.once = true});
      rt::Response stderrResponse(rt::BodyInit(child.stderr()));
      rt::Promise<std::string> stderrText(stderrResponse.text());
      auto state = std::make_shared<LegacyCodec::State>(
          LegacyCodec::State{child, signal, interrupted, stderrResponse, stderrText, child.stdout().getReader(), rt::TextDecoder(), "", false});
      LegacyCodec codec(LegacyCodec::Identity{std::string(LEGACY_DENO_VERSION), path, expectedSha256}, std::move(state));
      co_return codec;
    }
  }

  rt::Task<std::string> fileDigest(std::string path) {
    rt::crypto::Hash hash = rt::crypto::createHash("sha256");
    const rt::ReadableStream<std::vector<std::uint8_t>> stream = rt::bun::file(path).stream();
    const rt::ReadableStreamDefaultReader<std::vector<std::uint8_t>> reader = stream.getReader();
    for (;;) {
      const rt::ReadableStreamReadResult<std::vector<std::uint8_t>> chunk = co_await reader.read();
      if (chunk.done) break;
      hash.update(*chunk.value);
    }
    co_return hash.digest("hex");
  }

  LegacyCodec::LegacyCodec(Identity identity_, std::shared_ptr<State> state) : identity(std::move(identity_)), state_(std::move(state)) {}

  rt::Task<TypedValue> LegacyCodec::decode(std::vector<std::uint8_t> bytes) const { return decodeBytes(state_, std::move(bytes)); }
  rt::Task<std::vector<std::uint8_t>> LegacyCodec::encode(TypedValue value) const { return encodeValue(state_, std::move(value)); }
  rt::Task<void> LegacyCodec::close() const { return closeCodec(state_); }
  rt::Task<void> LegacyCodec::abort() const { return abortCodec(state_); }

  rt::Task<LegacyCodec> openLegacyCodec(std::string binary, std::string expectedSha256, std::string cwd) {
    return open(std::move(binary), std::move(expectedSha256), std::move(cwd), std::nullopt);
  }
  rt::Task<LegacyCodec> openLegacyCodec(std::string binary, std::string expectedSha256, std::string cwd, rt::AbortSignal signal) {
    return open(std::move(binary), std::move(expectedSha256), std::move(cwd), std::move(signal));
  }
}
