#pragma once
#include <boost/json.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace pilot {
  namespace json = boost::json;
  struct Failure : std::runtime_error {
    int status;
    std::string code, param;
    Failure(int status, std::string message, std::string code = "invalid_request_error", std::string param = "");
  };
  std::string text(json::value const& value);
  std::string field(json::object const& value, std::string_view key, std::string fallback = "");
  json::value const& member(json::object const& value, std::string_view key);
  bool flag(json::object const& value, std::string_view key, bool fallback = false);
  std::int64_t integer(json::object const& value, std::string_view key, std::int64_t fallback = 0);
  std::int64_t now_ms();
  std::string sha256(std::string_view value);
  std::string random_id(std::string_view prefix);
std::string repair_utf8(std::string_view input);
bool nonblank_utf16(std::string_view input);
  json::object error_payload(Failure const& failure);
  struct IoCounters {
    std::string epoch=random_id("io_");
    std::uint64_t upstream_read_starts=0,upstream_read_completions=0,pending_upstream_reads=0;
    std::uint64_t downstream_write_starts=0,downstream_write_completions=0,pending_downstream_writes=0;
    std::uint64_t downstream_write_successes=0,actual_downstream_send_buffer_bytes=0,active_requests=0;
  };
  struct Limits {
    std::size_t request_bytes = 4 * 1024 * 1024, frame_bytes = 1024 * 1024, terminal_bytes = 2 * 1024 * 1024;
    std::size_t events = 65536, nodes = 65536, depth = 64;
    std::chrono::milliseconds header {
      5000
    }, body {
      30000
    }, connect {
      10000
    }, headers {
      120000
    }, frame {
      1800000
    }, write {
      30000
    };
  };
  struct Config {
    std::string host = "127.0.0.1", upstream_host, upstream_port, upstream_target = "/chat/completions", upstream_key;
    std::string ca_file, token_hash, key_id = "responses-conformance-key", state_directory;
    unsigned short port = 0;
    bool tls = true, fixture = false;
    std::int64_t quota_limit = 50;
    int control_in = -1, control_out = -1;
    Limits limits;
  };
  Config read_config(std::filesystem::path const& path);
  struct AllocationGroup {
    std::size_t current = 0, peak = 0, limit = 24 * 1024 * 1024;
  };
  class BudgetResource final : public json::memory_resource {
    std::shared_ptr<AllocationGroup> group_;
    std::size_t limit_, current_ = 0, peak_ = 0;
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
    bool do_is_equal(json::memory_resource const& other) const noexcept override;
    public: BudgetResource(std::shared_ptr<AllocationGroup> group, std::size_t limit) : group_(std::move(group)), limit_(limit) {
    } std::size_t peak() const {
      return peak_;
    }
  };
  struct JsonScan {
    std::size_t bytes = 0, nodes = 0, depth = 0;
    bool quoted = false, escaped = false, atom = false;
    void feed(std::string_view input, Limits const& limits);
  };
  struct ToolName {
    std::string chat, name;
    std::shared_ptr<std::string const> space;
    bool custom = false;
  };
  struct Prepared {
    json::object body, echo;
    std::vector<ToolName> names;
    std::string model;
    bool stream = true;
    std::string warning;
    explicit Prepared(json::storage_ptr storage) : body(storage), echo(storage) {
    }
  };
  Prepared project(json::object const& raw, Limits const& limits, json::storage_ptr storage);
  void project_input(json::object const& raw, json::array& messages, bool reject);
  void project_tools(json::object const& raw, Prepared& result);
  std::string bounded_payload(std::string value, std::string const& path, bool reject, bool arguments = false);
  std::size_t estimate_tokens(std::string_view input);
  inline constexpr std::string_view summary_prefix = "Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work. Here is the summary produced by the other language model, use the information in this summary to assist with your own analysis:";
}
