#pragma once
#include "uos/authority/contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::authority::durable_store {
  using contracts::AuthorityScope;
  using contracts::AuthoritySnapshot;
  using contracts::AuthorityState;
  using contracts::AuthorityStore;

  /** The actual platform storage is injected; no platform shim or local fallback. */
  class AuthoritySqlStorage {
  public:
    // A query binding: `string | number | null`.
    using Binding = std::variant<std::nullptr_t, std::string, double>;
    // The result of `sql.exec(...)`.
    class Cursor {
    public:
      explicit Cursor(std::vector<json::object> rows_) : rows(std::move(rows_)) {}
      std::vector<json::object> toArray() const { return rows; }

    private:
      std::vector<json::object> rows;
    };
    // `sql`
    class Sql {
    public:
      virtual ~Sql() = default;
      // `sql.exec(query, ...bindings)`
      virtual Cursor exec(std::string_view query, std::vector<Binding> bindings) = 0;
    };

    virtual ~AuthoritySqlStorage() = default;
    virtual Sql& sql() = 0;
    // `transactionSync(callback)`: runs `callback` in one transaction, rolled
    // back when it throws (the exception propagates). A result travels through
    // the callback's captures.
    virtual void transactionSync(const std::function<void()>& callback) = 0;
  };

  /** One bounded coordination atom; migrations, retention and production scale are separate. */
  class DurableAuthorityStore : public AuthorityStore {
  public:
    DurableAuthorityStore(std::shared_ptr<AuthoritySqlStorage> storage_, const AuthorityState& initial, double maximumStateBytes_);

    rt::Promise<AuthoritySnapshot> read() override;
    rt::Promise<bool> compareAndSwap(double revision, AuthorityState state) override;

  private:
    void _bounded(const std::string& encoded) const;
    std::string _encode(const AuthorityState& state) const;
    AuthoritySnapshot _read() const;

    const std::shared_ptr<AuthoritySqlStorage> _storage;
    const double _maximumStateBytes;
    AuthorityScope _scope;
  };
}
