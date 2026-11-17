#include "uos/codex/supervisor_config.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include <exception>

namespace uos::codex::supervisor_config {
  namespace {
    constexpr std::string_view SUPERVISOR_CONFIG_PATH = ".data/codex-supervisor.json";

    constexpr double MAX_SOURCES = 8;
    constexpr double MAX_SOURCE_NAME_LENGTH = 64;
  }

  std::string_view toString(SupervisorState state) {
    switch (state) {
      case SupervisorState::active:
        return "active";
      case SupervisorState::idle:
        return "idle";
      case SupervisorState::stale:
        return "stale";
      case SupervisorState::unknown:
        return "unknown";
      case SupervisorState::system_error:
        return "system_error";
    }
    return "unknown";
  }

  std::optional<SupervisorState> parseSupervisorState(std::string_view text) {
    for (const SupervisorState state : {SupervisorState::active, SupervisorState::idle, SupervisorState::stale, SupervisorState::unknown, SupervisorState::system_error})
      if (toString(state) == text) return state;
    return std::nullopt;
  }

  bool isRecord(const json::value& value) { return rt::isRecord(value); }
  bool isRecord(const json::value* value) { return rt::isRecord(value); }

  bool isUnknownArray(const json::value& value) { return value.is_array(); }
  bool isUnknownArray(const json::value* value) { return value != nullptr && value->is_array(); }

  std::optional<std::string> asString(const json::value& value) { return asString(&value); }
  std::optional<std::string> asString(const json::value* value) {
    const std::optional<std::string_view> text = rt::getString(value);
    return text ? std::optional<std::string>(std::string(*text)) : std::nullopt;
  }

  std::optional<double> asFiniteNumber(const json::value& value) { return asFiniteNumber(&value); }
  std::optional<double> asFiniteNumber(const json::value* value) {
    const std::optional<double> number = rt::getNumber(value);
    return number && rt::Number::isFinite(*number) ? number : std::nullopt;
  }

  std::optional<bool> asBoolean(const json::value& value) { return asBoolean(&value); }
  std::optional<bool> asBoolean(const json::value* value) { return rt::getBool(value); }

  std::string joinPath(std::string_view base, std::string_view leaf) {
    return rt::endsWith(base, "/") ? rt::concat(base, leaf) : rt::concat(std::string(base) + "/", leaf);
  }

  std::optional<double> normalizeEpochMs(const json::value& value) { return normalizeEpochMs(&value); }
  std::optional<double> normalizeEpochMs(const json::value* value) {
    const std::optional<double> numeric = asFiniteNumber(value);
    if (!numeric || *numeric <= 0) return std::nullopt;
    return *numeric < 1'000'000'000'000 ? rt::Math::round(*numeric * 1000) : rt::Math::round(*numeric);
  }

  std::optional<std::string> textOrNull(const json::value& value) { return textOrNull(&value); }
  std::optional<std::string> textOrNull(const json::value* value) {
    const std::optional<std::string> text = asString(value);
    if (!text) return std::nullopt;
    std::string trimmed = rt::trim(*text);
    return !trimmed.empty() ? std::optional<std::string>(std::move(trimmed)) : std::nullopt;
  }

  std::vector<std::string> uniqueStrings(const std::vector<std::string>& values) {
    std::vector<std::string> unique;
    std::unordered_set<std::string> seen;
    for (const std::string& value : values)
      if (seen.insert(value).second) unique.push_back(value);
    return unique;
  }

  std::string boundedText(std::string_view value, double limit) {
    return static_cast<double>(rt::length(value)) <= limit ? std::string(value) : rt::concat(rt::slice(value, 0, limit), "…");
  }

  double nowMs() { return rt::dateNow(); }

  std::optional<std::string> readHostname() {
    try {
      return rt::os::hostname();
    } catch (...) {
      return std::nullopt;
    }
  }

  rt::Promise<void> waitForPoll(double ms, rt::AbortSignal signal) {
    return rt::newPromise<void>([ms, signal](rt::Resolve<void> resolve, rt::Reject) {
      const auto timer = std::make_shared<rt::Timeout>();
      const rt::EventListener onAbort([timer, resolve] {
        rt::clearTimeout(*timer);
        resolve();
      });
      *timer = rt::setTimeout(
        [signal, onAbort, resolve] {
          signal.removeEventListener("abort", onAbort);
          resolve();
        },
        ms);
      signal.addEventListener("abort", onAbort, {.once = true});
    });
  }

  /* ------------------------------------------------------------------ config */

  namespace {
    std::optional<SupervisorSource> supervisorSourceOf(const json::value& entry, const std::unordered_set<std::string>& seen, std::vector<std::string>& notes) {
      if (!isRecord(entry)) {
        notes.emplace_back("ignored a source that is not an object");
        return std::nullopt;
      }
      const json::object& record = entry.as_object();
      const std::optional<std::string> id = textOrNull(rt::get(record, "id"));
      const std::optional<std::string> name = textOrNull(rt::get(record, "name"));
      const std::optional<std::string> socketPath = textOrNull(rt::get(record, "socketPath"));
      const json::value* codexHomeValue = rt::get(record, "codexHome");
      const std::optional<std::string> codexHome = rt::isNullish(codexHomeValue) ? std::nullopt : textOrNull(codexHomeValue);
      if (!id || !SOURCE_ID_PATTERN.test(*id)) {
        notes.emplace_back("ignored a source whose id must match /^[a-z0-9][a-z0-9_-]{0,31}$/");
        return std::nullopt;
      }
      if (seen.contains(*id)) {
        notes.push_back("ignored duplicate source id " + *id);
        return std::nullopt;
      }
      if (!name || static_cast<double>(rt::length(*name)) > MAX_SOURCE_NAME_LENGTH) {
        notes.push_back("ignored source " + *id + ": name must be 1-" + rt::numberToString(MAX_SOURCE_NAME_LENGTH) + " characters");
        return std::nullopt;
      }
      if (!socketPath || !rt::startsWith(*socketPath, "/")) {
        notes.push_back("ignored source " + *id + ": socketPath must be an absolute path");
        return std::nullopt;
      }
      if (codexHome && !rt::startsWith(*codexHome, "/")) {
        notes.push_back("ignored source " + *id + ": codexHome must be an absolute path");
        return std::nullopt;
      }
      return SupervisorSource{.id = *id, .name = *name, .socketPath = *socketPath, .codexHome = codexHome};
    }
  }

  ParsedSupervisorConfig parseSupervisorConfig(const json::value& raw) {
    std::vector<std::string> notes;
    if (!isRecord(raw)) return ParsedSupervisorConfig{.sources = {}, .notes = {"configuration must be a JSON object"}};
    const json::value* sourcesValue = rt::get(raw, "sources");
    const json::array noEntries;
    const json::array& entries = isUnknownArray(sourcesValue) ? sourcesValue->as_array() : noEntries;
    if (entries.empty()) return ParsedSupervisorConfig{.sources = {}, .notes = {"configuration lists no sources"}};
    std::vector<SupervisorSource> sources;
    std::unordered_set<std::string> seen;
    for (std::size_t index = 0; index < entries.size() && static_cast<double>(index) < MAX_SOURCES; ++index) {
      std::optional<SupervisorSource> source = supervisorSourceOf(entries[index], seen, notes);
      if (!source) continue;
      seen.insert(source->id);
      sources.push_back(std::move(*source));
    }
    if (static_cast<double>(entries.size()) > MAX_SOURCES) notes.push_back("only the first " + rt::numberToString(MAX_SOURCES) + " sources are used");
    return ParsedSupervisorConfig{.sources = std::move(sources), .notes = std::move(notes)};
  }

  std::optional<SupervisorSource> defaultSupervisorSource(const EnvironmentReader& env, std::vector<std::string>& notes) {
    const std::string home = rt::trim(env("HOME").value_or(""));
    std::string codexHome = rt::trim(env("CODEX_HOME").value_or(""));
    if (codexHome.empty()) codexHome = !home.empty() ? joinPath(home, ".codex") : "";
    if (codexHome.empty()) {
      notes.emplace_back("HOME and CODEX_HOME are both unavailable, so no local source could be derived");
      return std::nullopt;
    }
    return SupervisorSource{.id = "local", .name = "Local Codex", .socketPath = joinPath(codexHome, "app-server-control/app-server-control.sock"), .codexHome = codexHome};
  }

  namespace {
    std::optional<std::string> readEnvironment(std::string_view name) {
      try {
        return rt::env::get(name);
      } catch (...) {
        return std::nullopt;
      }
    }
  }

  rt::Task<SupervisorConfig> resolveSupervisorConfig(ResolveSupervisorConfigOptions options) {
    const std::string cwd = options.cwd ? *options.cwd : rt::process::cwd();
    const EnvironmentReader env = options.env ? options.env : EnvironmentReader(readEnvironment);
    const std::string configPath = joinPath(cwd, SUPERVISOR_CONFIG_PATH);
    std::vector<std::string> notes;
    json::value raw = nullptr;
    bool configLoaded = false;
    // `await readFile(...)`: the read itself is synchronous here; its outcome is taken
    // up after the suspension, where the TS resumes.
    std::optional<std::string> text;
    rt::Reason readFailure;
    try {
      text = rt::fs::promises::readFile(configPath, "utf8");
    } catch (...) {
      readFailure = std::current_exception();
    }
    co_await rt::microtask();
    try {
      if (readFailure) std::rethrow_exception(readFailure);
      raw = rt::jsonParse(*text);
      configLoaded = true;
    } catch (const rt::fs::ErrnoError& error) {
      if (error.code != "ENOENT") notes.push_back(std::string(SUPERVISOR_CONFIG_PATH) + " could not be read; using the default local source");
    } catch (...) {
      notes.push_back(std::string(SUPERVISOR_CONFIG_PATH) + " could not be read; using the default local source");
    }
    if (configLoaded) {
      ParsedSupervisorConfig parsed = parseSupervisorConfig(raw);
      notes.insert(notes.end(), parsed.notes.begin(), parsed.notes.end());
      if (!parsed.sources.empty()) {
        SupervisorConfig loaded;
        loaded.sources = std::move(parsed.sources);
        loaded.configPath = configPath;
        loaded.configLoaded = configLoaded;
        loaded.notes = std::move(notes);
        co_return loaded;
      }
      notes.emplace_back("no usable source in the configuration file; using the default local source");
    }
    const std::optional<SupervisorSource> fallback = defaultSupervisorSource(env, notes);
    SupervisorConfig resolved;
    if (fallback) resolved.sources.push_back(*fallback);
    resolved.configPath = configPath;
    resolved.configLoaded = configLoaded;
    resolved.notes = std::move(notes);
    co_return resolved;
  }

  rt::Task<SupervisorConfig> resolveSupervisorConfig() {
    ResolveSupervisorConfigOptions options;
    return resolveSupervisorConfig(std::move(options));
  }
}
