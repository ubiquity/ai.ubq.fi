// Port of tests/authority-publication.test.ts.
#include "uos/authority/admission_state.hpp"
#include "uos/authority/contracts.hpp"
#include "uos/authority/credential_publication.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {
  using namespace uos;
  using namespace uos::authority::contracts;
  using namespace uos::authority::credential_publication;
  using uos::authority::admission_state::createAuthorityState;
  using uos::authority::admission_state::PortableAuthority;

  // `await assert.rejects(promise, /pattern/)`: String(error) must match.
  template <class T> rt::Task<void> rejectsMatching(rt::Task<T> task, const char* pattern, const char* file, int line) {
    bool rejected = false;
    std::string text;
    try {
      co_await std::move(task);
    } catch (...) {
      rejected = true;
      text = rt::reasonToString(std::current_exception());
    }
    if (!rejected) rt::test::report(file, line, std::string("expected a rejection matching /") + pattern + "/", false);
    else if (!rt::Regex(pattern).test(text)) rt::test::report(file, line, std::string("expected a rejection matching /") + pattern + "/, got " + text, false);
  }
#define ASSERT_REJECTS(task, pattern) co_await rejectsMatching((task), pattern, __FILE__, __LINE__)

  const AuthorityScope scope{.namespace_ = "synthetic-publication", .epoch = 1};

  struct FixtureStore : AuthorityStore {
    AuthorityState state;
    double revision = 0;
    rt::Promise<AuthoritySnapshot> read() override { return rt::Promise<AuthoritySnapshot>::resolve(AuthoritySnapshot{.revision = revision, .state = state}); }
    rt::Promise<bool> compareAndSwap(double expected, AuthorityState value) override {
      if (expected != revision) return rt::Promise<bool>::resolve(false);
      state = std::move(value);
      revision += 1;
      return rt::Promise<bool>::resolve(true);
    }
  };

  struct Fixture {
    std::shared_ptr<CredentialPublisher> publisher;
    std::shared_ptr<FixtureStore> store;
    CredentialOwnerBinding binding;
    AuthorityState& state() const { return store->state; }
  };

  Fixture fixture() {
    const AccountBinding account{
      .accountId = "a",
      .fingerprint = "old",
      .credentialGeneration = 0,
      .routingGeneration = 1,
      .resetGeneration = 1,
      .overageVersion = 1,
      .overageEnabled = false,
      .enabled = true,
      .models = {"fixture"},
      .proof = std::nullopt,
    };
    auto store = std::make_shared<FixtureStore>();
    store->state = createAuthorityState(scope, {account}, {});
    store->state.active = ActiveAccount{.accountId = "a", .fingerprint = "old", .generation = 7};
    const CredentialOwnerBinding binding{.scope = scope, .ownerId = "owner", .ownerEpoch = 3, .accountId = "a", .workspaceId = "workspace", .minimumNativeRefreshedAtNs = "1000000000000000001"};
    auto publisher = std::make_shared<CredentialPublisher>(store, binding, [](AccessPublication value) {
      return rt::Promise<VerifiedAccessIdentity>::resolve(VerifiedAccessIdentity{
        .accountId = value.accountId,
        .workspaceId = value.workspaceId,
        .fingerprint = value.fingerprint,
        .issuedAtMs = value.issuedAtMs,
        .expiresAtMs = value.expiresAtMs,
      });
    });
    return Fixture{.publisher = publisher, .store = store, .binding = binding};
  }

  const AccessPublication publication{
    .ownerId = "owner",
    .ownerEpoch = 3,
    .accountId = "a",
    .workspaceId = "workspace",
    .generation = 1,
    .nativeRefreshedAtNs = "1000000000000000002",
    .fingerprint = "new",
    .accessToken = "synthetic-access",
    .idToken = "synthetic-id",
    .issuedAtMs = 900,
    .expiresAtMs = 200'000,
    .previousGeneration = 0,
  };
}

ASYNC_TEST("access-only publication advances precise generation once and preserves global active identity") {
  const Fixture f = fixture();
  rt::Task<std::string> first = f.publisher->publish(publication, 1'000);
  const std::string published = co_await std::move(first);
  EXPECT_EQ(published, "published");
  rt::Task<std::string> second = f.publisher->publish(publication, 1'000);
  const std::string duplicate = co_await std::move(second);
  EXPECT_EQ(duplicate, "duplicate");
  EXPECT(f.state().active == (ActiveAccount{.accountId = "a", .fingerprint = "new", .generation = 7}));
  EXPECT_EQ(f.state().accounts[0].credentialGeneration, 1);
  ASSERT(f.state().publications.get("a") != nullptr);
  EXPECT(!rt::hasOwn(toJson(*f.state().publications.get("a")).as_object(), "previousGeneration"));
  const auto authority = std::make_shared<PortableAuthority>(f.store, scope);
  std::string message;
  try {
    rt::Task<bool> checking = authority->resetEligible(AdmissionHandle{.scope = scope, .requestId = "missing", .fence = 1}, 1'000);
    co_await std::move(checking);
  } catch (const rt::JsError& error) {
    message = error.message;
  } catch (...) {
    message = "unexpected";
  }
  EXPECT_EQ(message, "admission_fence_mismatch");
}

ASYNC_TEST("refresh material, arbitrary destinations and unknown credential fields are rejected") {
  const Fixture f = fixture();
  const std::vector<std::string> fields{"refreshToken", "refresh_token", "url", "socketPath", "namespace"};
  for (const std::string& field : fields) {
    AccessPublication smuggled = publication;
    smuggled.extraOwnKeys.push_back(field);
    rt::Task<std::string> publishing = f.publisher->publish(smuggled, 1'000);
    ASSERT_REJECTS(std::move(publishing), "unexpected_credential_field");
  }
  EXPECT_EQ(f.state().publications.keys().size(), 0u);
}

ASYNC_TEST("owner epoch, workspace, identity verifier and native generation cannot be substituted") {
  const Fixture f = fixture();
  AccessPublication otherEpoch = publication;
  otherEpoch.ownerEpoch = 4;
  rt::Task<std::string> epochTask = f.publisher->publish(otherEpoch, 1'000);
  ASSERT_REJECTS(std::move(epochTask), "credential_owner_binding_mismatch");
  AccessPublication otherWorkspace = publication;
  otherWorkspace.workspaceId = "another";
  rt::Task<std::string> workspaceTask = f.publisher->publish(otherWorkspace, 1'000);
  ASSERT_REJECTS(std::move(workspaceTask), "credential_owner_binding_mismatch");
  AccessPublication staleNative = publication;
  staleNative.nativeRefreshedAtNs = "1000000000000000001";
  rt::Task<std::string> nativeTask = f.publisher->publish(staleNative, 1'000);
  ASSERT_REJECTS(std::move(nativeTask), "credential_identity_mismatch");
  const auto badVerifier = std::make_shared<CredentialPublisher>(f.store, f.binding, [](AccessPublication value) {
    return rt::Promise<VerifiedAccessIdentity>::resolve(VerifiedAccessIdentity{
      .accountId = "different",
      .workspaceId = value.workspaceId,
      .fingerprint = value.fingerprint,
      .issuedAtMs = value.issuedAtMs,
      .expiresAtMs = value.expiresAtMs,
    });
  });
  rt::Task<std::string> verifierTask = badVerifier->publish(publication, 1'000);
  ASSERT_REJECTS(std::move(verifierTask), "credential_identity_mismatch");
}

ASYNC_TEST("conflicting replay, expired access and wrong authority namespace fail closed") {
  const Fixture f = fixture();
  rt::Task<std::string> first = f.publisher->publish(publication, 1'000);
  co_await std::move(first);
  AccessPublication conflicting = publication;
  conflicting.accessToken = "conflict";
  rt::Task<std::string> conflictTask = f.publisher->publish(conflicting, 1'000);
  ASSERT_REJECTS(std::move(conflictTask), "credential_generation_conflict");
  AccessPublication expiring = publication;
  expiring.expiresAtMs = 2'000;
  rt::Task<std::string> expiringTask = f.publisher->publish(expiring, 1'000);
  ASSERT_REJECTS(std::move(expiringTask), "credential_generation_invalid");
  AccessPublication renewed = publication;
  renewed.generation = 2;
  renewed.previousGeneration = 1;
  renewed.nativeRefreshedAtNs = "1000000000000000003";
  renewed.expiresAtMs = 150'000;
  rt::Task<std::string> renewedTask = f.publisher->publish(renewed, 1'000);
  ASSERT_REJECTS(std::move(renewedTask), "credential_generation_conflict");
  const Fixture missing = fixture();
  missing.state().accounts = {};
  rt::Task<std::string> missingTask = missing.publisher->publish(publication, 1'000);
  ASSERT_REJECTS(std::move(missingTask), "credential_account_missing");
  f.state().scope = AuthorityScope{.namespace_ = scope.namespace_, .epoch = 2};
  rt::Task<std::string> scopeTask = f.publisher->publish(publication, 1'000);
  ASSERT_REJECTS(std::move(scopeTask), "namespace_or_epoch_mismatch");
}
