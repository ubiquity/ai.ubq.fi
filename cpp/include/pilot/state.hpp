#pragma once
#include "pilot/core.hpp"
namespace pilot {
  struct OwnedFd {
  int value = -1;
  operator int() const { return value; }
  OwnedFd& operator=(int descriptor) { value = descriptor; return *this; }
  ~OwnedFd();
};
class State {
    Config const& config_;
    OwnedFd lock_, journal_;
    std::filesystem::path root_;
    std::shared_ptr<AllocationGroup> budget_ = std::make_shared<AllocationGroup>();
    BudgetResource resource_ {
      budget_,8*1024*1024
    };
    json::object data_ {
      json::storage_ptr(&resource_)
    };
    std::uint64_t seq_ = 0;
    std::size_t journal_bytes_ = 0, frames_ = 0;
  bool unavailable_ = false;
    void append(std::string const& op,json::object const& value,bool quota);
    void apply(std::string const& op,json::object const& value,bool quota);
    void load();
    public: explicit State(Config const& config);
    ~State();
    State(State const&) = delete;
    State& operator=(State const&) = delete;
    bool authenticate(std::string_view token) const;
    bool reserve(std::string const& id);
    void dispatch(std::string const& id);
    void release(std::string const& id,bool compensate = false);
    void terminal(std::string const& id,std::string const& kind,json::value const& usage);
    void delivered(std::string const& id,bool success);
    void settle(std::string const& id);
    void revoke();
    json::object inspect(std::string const& id = "") const;
    bool needs_snapshot() const;
    void snapshot_publish();
    void snapshot_sync_directory();
    void snapshot_reset_journal();
    std::uint64_t sequence() const {
      return seq_;
    }
  };
}
