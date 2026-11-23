#pragma once
// Port of src/authority/native-authority-sql.ts.
#include "uos/authority/native_authority_contracts.hpp"
#include "uos/rt/json.hpp"
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace uos::authority::native_authority_sql {
  std::string nativeKey(const native_authority_contracts::NativeKey& key);
  struct NativeIndex {
    std::optional<std::string> scope{};
    std::optional<std::string> parent{};
    std::optional<double> due{};
  };
  template<class T = json::value> struct NativePageEntry {
    native_authority_contracts::NativeKey key;
    std::string encoded;
    native_authority_contracts::NativeRead<T> row;
  };
  class NativeAuthoritySql {
    native_authority_contracts::NativeSqlStorage _storage;
    std::vector<json::object> _exec(std::string query, std::vector<native_authority_contracts::NativeSqlStorage::Binding> bindings = {}) const;
  public:
    const std::string namespace_;
    const std::string epoch;
    NativeAuthoritySql(native_authority_contracts::NativeSqlStorage storage, std::string namespaceValue, std::string epochValue);
    void assertEpoch() const;
    template<class Callback> auto transaction(Callback callback) const {
      return _storage.transactionSync([&]() -> std::invoke_result_t<Callback> {
        assertEpoch();
        if constexpr (std::is_void_v<std::invoke_result_t<Callback>>) callback();
        else {
          auto result = callback();
          if constexpr (rt::detail::IsPromise<decltype(result)>::value || rt::detail::IsTask<decltype(result)>::value)
            throw native_authority_contracts::NativeAuthorityError("async_transaction_forbidden", 500);
          return result;
        }
      });
    }
    native_authority_contracts::NativeRead<json::value> read(const std::string& table, const native_authority_contracts::NativeKey& key, double now) const;
    template<class T> native_authority_contracts::NativeRead<T> readAs(const std::string& table, const native_authority_contracts::NativeKey& key, double now) const {
      const auto row = read(table, key, now);
      return {row.value && !row.value->is_null() ? std::optional<T>(json::value_to<T>(*row.value)) : std::nullopt, row.revision, row.versionstamp, row.expiresAtMs};
    }
    void write(const std::string& table, const native_authority_contracts::NativeKey& key, const json::value& value, std::optional<double> expiresAtMs = std::nullopt, const NativeIndex& index = {});
    template<class T> void write(const std::string& table, const native_authority_contracts::NativeKey& key, const T& value, std::optional<double> expiresAtMs = std::nullopt, const NativeIndex& index = {}) {
      write(table, key, json::value_from(value), expiresAtMs, index);
    }
    void delete_(const std::string& table, const native_authority_contracts::NativeKey& key);
    std::vector<NativePageEntry<>> page(const std::string& table, double now, const std::string& after, double limit, const NativeIndex& filter = {}) const;
    template<class T> std::vector<NativePageEntry<T>> pageAs(const std::string& table, double now, const std::string& after, double limit, const NativeIndex& filter = {}) const {
      std::vector<NativePageEntry<T>> result;
      for (const auto& item : page(table, now, after, limit, filter))
        result.push_back({item.key, item.encoded, {item.row.value && !item.row.value->is_null() ? std::optional<T>(json::value_to<T>(*item.row.value)) : std::nullopt, item.row.revision, item.row.versionstamp, item.row.expiresAtMs}});
      return result;
    }
    void initialize(const std::function<void()>& callback);
    std::optional<double> earliestPaidDue(double now) const;
    std::optional<native_authority_contracts::NativeAttempt> paidAttempt(const std::string& keyId, const std::string& billingRequestId, double now) const;
    native_authority_contracts::NativeCapability capability(const std::string& id, double now) const;
    std::optional<native_principal_contracts::NativePrincipalResolution> principalReference(const std::string& id, const std::string& keyId, double now) const;
    std::optional<native_authority_contracts::NativeRead<native_authority_contracts::NativeKeyPolicy>> currentKeyPolicy(const std::optional<std::string>& tokenHash, const std::optional<std::string>& keyId, double now) const;
  };
}
