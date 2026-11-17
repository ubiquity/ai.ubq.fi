#include "uos/worker/entrypoint.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"

#include <utility>

namespace uos::worker::entrypoint {
  using authority::admission_state::Selection;
  using authority::contracts::AdmissionHandle;
  using authority::contracts::AdmitInput;
  using authority::contracts::AttemptInput;
  using authority::contracts::AuthorityError;
  using authority::contracts::GatewayRoute;
  using authority::contracts::TerminalEvidence;
  using authority::delivery_outbox::DeliveryOutbox;
  using transport::UpstreamAttempt;
  using transport::ValidatedGatewayInput;

  namespace {
    rt::Response errorResponse(const std::string& code, double status) {
      json::object error;
      error["code"] = code;
      error["message"] = code;
      error["type"] = "gateway_error";
      json::object body;
      body["error"] = std::move(error);
      rt::ResponseInit init;
      init.status = status;
      return rt::Response::json(body, std::move(init));
    }

    rt::Task<std::string> digest(std::string body) {
      const rt::crypto::Bytes hashed = rt::crypto::subtle::digest("SHA-256", rt::encodeUtf8(body));
      co_await rt::microtask();
      co_return rt::toHex(hashed);
    }

    rt::Task<json::object> readBoundedJson(rt::Request request) {
      request.signal().throwIfAborted();
      const std::optional<rt::ByteStream> stream = request.body();
      if (!stream) throw AuthorityError("request_body_required", 400);
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = stream->getReader();
      rt::TextDecoder decoder;
      double size = 0;
      std::string text;
      const rt::AbortSignal signal = request.signal();
      const rt::EventListener cancel([reader, signal] { rt::spawn(reader.cancel(signal.reason()), rt::ignoreRejection); });
      request.signal().addEventListener("abort", cancel, {.once = true});
      rt::Reason failure;
      json::object result;
      try {
        for (;;) {
          rt::Task<rt::ReadableStreamReadResult<rt::ByteChunk>> reading = reader.read();
          rt::ReadableStreamReadResult<rt::ByteChunk> chunk = co_await std::move(reading);
          request.signal().throwIfAborted();
          if (chunk.done) break;
          size += static_cast<double>(chunk.value->size());
          if (size > 1'048'576) {
            rt::Task<void> cancelling = reader.cancel(rt::thrown(json::value("request_too_large")));
            co_await std::move(cancelling);
            throw AuthorityError("request_too_large", 413);
          }
          text += decoder.decode(*chunk.value, {.stream = true});
        }
        text += decoder.decode();
        json::value value = rt::jsonParse(text);
        if (!value.is_object()) throw AuthorityError("invalid_request_body", 400);
        result = std::move(value.get_object());
      } catch (...) {
        failure = std::current_exception();
      }
      request.signal().removeEventListener("abort", cancel);
      reader.releaseLock();
      if (failure) {
        try {
          std::rethrow_exception(failure);
        } catch (const rt::SyntaxError&) {
          throw AuthorityError("invalid_json", 400);
        }
      }
      co_return result;
    }

    json::value validatedJson(const ValidatedGatewayInput& input) {
      json::object out;
      out["route"] = input.route;
      out["model"] = input.model;
      out["stream"] = input.stream;
      out["body"] = input.body;
      return out;
    }

    // The closure state of one delivered response (`deliverResponse`'s locals).
    struct Delivery {
      UpstreamResult upstream;
      rt::Request request;
      rt::AbortController abort;
      rt::EventListener abortFromRequest;
      std::shared_ptr<DeliveryOutbox> outbox;
      BoundaryLifetime lifetime;
      std::optional<BoundaryDelivery> delivery;
      bool bodyDone = false;
      std::optional<rt::Promise<void>> completed;
      std::optional<rt::ReadableStreamDefaultReader<rt::ByteChunk>> reader;
      rt::EventListener signalCancellation;
    };

    rt::Promise<void> finish(const std::shared_ptr<Delivery>& state, DeliveryOutcome outcome) {
      if (!state->completed) {
        const rt::Request request = state->request;
        const rt::EventListener abortFromRequest = state->abortFromRequest;
        state->completed =
          rt::Promise<void>(state->outbox->delivery(std::move(outcome))).finally_([request, abortFromRequest] { request.signal().removeEventListener("abort", abortFromRequest); });
      }
      return *state->completed;
    }

    // `abort.signal.removeEventListener("abort", signalCancellation)`; the
    // listener also holds this state, so the reference is dropped with it.
    void releaseCancellation(const std::shared_ptr<Delivery>& state) {
      state->abort.signal().removeEventListener("abort", state->signalCancellation);
      state->signalCancellation = rt::EventListener();
    }

    rt::Task<void> cancelDelivery(std::shared_ptr<Delivery> state, rt::Reason reason) {
      if (state->bodyDone) co_return;
      state->bodyDone = true;
      state->abort.abort(reason);
      rt::Reason failure;
      try {
        // An already aborted fetch may reject cancellation with the abort reason.
        if (state->reader) {
          rt::Promise<void> cancelling = rt::Promise<void>(state->reader->cancel(reason)).catch_([](const rt::Reason&) {});
          co_await cancelling.wait();
        } else {
          co_await rt::microtask();
        }
      } catch (...) {
        failure = std::current_exception();
      }
      rt::Promise<void> finishing = finish(state, "interrupted");
      co_await finishing.wait();
      if (failure) std::rethrow_exception(failure);
    }

    rt::Task<void> pullBody(std::shared_ptr<Delivery> state, rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      rt::Reason failure;
      try {
        rt::Task<rt::ReadableStreamReadResult<rt::ByteChunk>> reading = state->reader->read();
        rt::ReadableStreamReadResult<rt::ByteChunk> chunk = co_await std::move(reading);
        if (chunk.done) {
          state->bodyDone = true;
          releaseCancellation(state);
          if (!state->delivery) {
            rt::Promise<void> finishing = finish(state, state->abort.signal().aborted() ? "interrupted" : "unobserved");
            co_await finishing.wait();
          }
          controller.close();
        } else {
          controller.enqueue(std::move(*chunk.value));
        }
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        state->bodyDone = true;
        releaseCancellation(state);
        rt::Promise<void> finishing = finish(state, "interrupted");
        co_await finishing.wait();
        controller.error(failure);
      }
    }

    rt::Response deliverResponse(UpstreamResult upstream, const std::string& requestId, rt::Request request, rt::AbortController abort, rt::EventListener abortFromRequest,
                                 std::shared_ptr<DeliveryOutbox> outbox, BoundaryLifetime lifetime, std::optional<BoundaryDelivery> delivery) {
      auto state = std::make_shared<Delivery>(Delivery{
        .upstream = std::move(upstream),
        .request = std::move(request),
        .abort = std::move(abort),
        .abortFromRequest = std::move(abortFromRequest),
        .outbox = std::move(outbox),
        .lifetime = std::move(lifetime),
        .delivery = std::move(delivery),
        .bodyDone = false,
        .completed = std::nullopt,
        .reader = std::nullopt,
        .signalCancellation = rt::EventListener(),
      });
      if (state->delivery) {
        state->lifetime.waitUntil(state->delivery->completed.then([state](DeliveryOutcome outcome) { return finish(state, std::move(outcome)); }));
      }
      if (const std::optional<rt::ByteStream> body = state->upstream.response.body()) state->reader = body->getReader();
      state->signalCancellation = rt::EventListener([state] {
        state->lifetime.waitUntil(rt::Promise<void>(cancelDelivery(state, state->abort.signal().reason())));
        state->signalCancellation = rt::EventListener();
      });
      state->abort.signal().addEventListener("abort", state->signalCancellation, {.once = true});
      std::optional<rt::ByteStream> responseBody;
      if (state->reader) {
        rt::UnderlyingSource<rt::ByteChunk> source;
        source.pull = [state](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { return pullBody(state, std::move(controller)); };
        source.cancel = [state](rt::Reason reason) { return cancelDelivery(state, std::move(reason)); };
        responseBody.emplace(std::move(source), rt::QueuingStrategy{.highWaterMark = 0});
      }
      if (!state->reader) {
        state->bodyDone = true;
        releaseCancellation(state);
        if (!state->delivery) state->lifetime.waitUntil(finish(state, "unobserved"));
      }
      if (state->abort.signal().aborted()) state->lifetime.waitUntil(rt::Promise<void>(cancelDelivery(state, state->abort.signal().reason())));
      rt::Headers headers(state->upstream.response.headers());
      headers.set("x-request-id", requestId);
      rt::ResponseInit init;
      init.status = static_cast<double>(state->upstream.response.status());
      init.statusText = state->upstream.response.statusText();
      init.headers = std::move(headers);
      return rt::Response(rt::bodyInit(std::move(responseBody)), std::move(init));
    }

    struct DispatchState {
      bool intent = false;
    };

    rt::Task<rt::Response> refuseRequest(rt::Reason error, rt::Request request, rt::AbortController abort, rt::EventListener abortFromRequest,
                                         std::optional<AdmissionHandle> handle, std::shared_ptr<DispatchState> dispatchState, std::shared_ptr<AuthorityOperations> authority) {
      request.signal().removeEventListener("abort", abortFromRequest);
      try {
        if (handle && !dispatchState->intent) {
          rt::Task<void> cancelling = authority->cancelUndispatched(*handle);
          co_await std::move(cancelling);
        }
        if (handle && dispatchState->intent) {
          rt::Task<void> observing = authority->observeDelivery(*handle, abort.signal().aborted() ? "interrupted" : "unobserved");
          co_await std::move(observing);
        }
      } catch (...) {
        // The durable admission/attempt remains unresolved; cleanup failure cannot replace the original refusal.
      }
      std::optional<std::pair<std::string, double>> refusal;
      if (error) {
        try {
          std::rethrow_exception(error);
        } catch (const AuthorityError& authorityError) {
          refusal.emplace(authorityError.code, authorityError.status);
        } catch (...) {
        }
      }
      if (refusal) co_return errorResponse(refusal->first, refusal->second);
      const bool aborted = abort.signal().aborted();
      co_return errorResponse(aborted ? "request_cancelled" : "upstream_unavailable", aborted ? 499 : 502);
    }

    rt::Task<void> markDispatched(std::shared_ptr<const WorkerBoundaryDependencies> dependencies, std::shared_ptr<DispatchState> dispatchState, AdmissionHandle admitted,
                                  std::string dispatchedAttempt) {
      if (dispatchState->intent) throw AuthorityError("dispatch_callback_repeated");
      rt::Task<bool> beginning = dependencies->authority->beginDispatch(admitted, dispatchedAttempt, dependencies->now());
      const bool begun = co_await std::move(beginning);
      if (!begun) throw AuthorityError("dispatch_already_claimed");
      dispatchState->intent = true;
      rt::Task<void> confirming = dependencies->authority->confirmDispatched(admitted, dispatchedAttempt);
      co_await std::move(confirming);
    }

    rt::Task<rt::Response> boundaryFetch(std::shared_ptr<const WorkerBoundaryDependencies> dependencies, rt::Request request, BoundaryLifetime lifetime,
                                         std::optional<BoundaryDelivery> delivery) {
      const std::string path = rt::URL(request.url()).pathname();
      if (path != "/v1/responses" && path != "/v1/chat/completions") co_return errorResponse("route_not_implemented", 501);
      if (request.method() != "POST") co_return errorResponse("method_not_allowed", 405);
      const GatewayRoute route = path;
      std::optional<AdmissionHandle> handle;
      const auto dispatchState = std::make_shared<DispatchState>();
      const rt::AbortController abort;
      const rt::EventListener abortFromRequest([abort, request] { abort.abort(request.signal().reason()); });
      request.signal().addEventListener("abort", abortFromRequest, {.once = true});
      if (request.signal().aborted()) abortFromRequest();
      rt::Reason failure;
      try {
        rt::Task<std::optional<BoundaryPrincipal>> authenticating = dependencies->authenticate(request);
        const std::optional<BoundaryPrincipal> principal = co_await std::move(authenticating);
        if (!principal) {
          request.signal().removeEventListener("abort", abortFromRequest);
          co_return errorResponse("invalid_api_key", 401);
        }
        rt::Task<json::object> reading = readBoundedJson(request);
        json::object body = co_await std::move(reading);
        rt::Task<ValidatedGatewayInput> validating = dependencies->upstream->validate(route, std::move(body));
        const ValidatedGatewayInput input = co_await std::move(validating);
        const std::string requestId = rt::crypto::randomUUID();
        rt::Task<std::string> digesting = digest(rt::jsonStringify(validatedJson(input)));
        const std::string digested = co_await std::move(digesting);
        const AdmitInput admission{
          .scope = dependencies->scope,
          .requestId = requestId,
          .principalId = principal->principalId,
          .policyVersion = principal->policyVersion,
          .digest = digested,
          .route = route,
          .model = input.model,
          .nowMs = dependencies->now(),
        };
        rt::Task<AdmissionHandle> admitting = dependencies->authority->admit(admission);
        handle = co_await std::move(admitting);
        const AdmissionHandle admitted = *handle;
        rt::Task<Selection> selecting = dependencies->authority->selection(admitted);
        const Selection selection = co_await std::move(selecting);
        rt::Task<UpstreamAttempt> attempting = dependencies->upstream->attempt(input, selection.account);
        const UpstreamAttempt selected = co_await std::move(attempting);
        const std::string dispatchedAttempt = rt::crypto::randomUUID();
        const AttemptInput attempt{
          .attemptId = dispatchedAttempt,
          .provider = selected.provider,
          .maximumExposureMicrocredits = selected.maximumExposureMicrocredits,
          .account = selected.account,
        };
        rt::Task<void> reserving = dependencies->authority->reserveAttempt(admitted, attempt, dependencies->now());
        co_await std::move(reserving);
        abort.signal().throwIfAborted();
        const auto outbox = std::make_shared<DeliveryOutbox>(dependencies->authority, admitted);
        std::function<rt::Task<void>()> onDispatched = [dependencies, dispatchState, admitted, dispatchedAttempt] {
          return markDispatched(dependencies, dispatchState, admitted, dispatchedAttempt);
        };
        rt::Task<UpstreamResult> dispatching = dependencies->upstream->dispatch(input, selected, abort.signal(), std::move(onDispatched));
        const UpstreamResult upstream = co_await std::move(dispatching);
        if (!dispatchState->intent) throw AuthorityError("upstream_dispatch_unobserved", 502);
        rt::Promise<void> terminal = upstream.terminal.then([outbox, dispatchedAttempt](TerminalEvidence evidence) { return outbox->terminal(dispatchedAttempt, std::move(evidence)); });
        // Persist terminal observation through the actual request lifetime, even on client abort.
        lifetime.waitUntil(terminal);
        co_return deliverResponse(upstream, requestId, request, abort, abortFromRequest, outbox, lifetime, delivery);
      } catch (...) {
        failure = std::current_exception();
      }
      rt::Task<rt::Response> refusing = refuseRequest(failure, request, abort, abortFromRequest, handle, dispatchState, dependencies->authority);
      rt::Response refused = co_await std::move(refusing);
      co_return refused;
    }
  }

  WorkerBoundary::WorkerBoundary(std::shared_ptr<const WorkerBoundaryDependencies> dependencies_) : dependencies(std::move(dependencies_)) {}

  rt::Task<rt::Response> WorkerBoundary::fetch(rt::Request request, BoundaryLifetime lifetime) const {
    return boundaryFetch(dependencies, std::move(request), std::move(lifetime), std::nullopt);
  }

  rt::Task<rt::Response> WorkerBoundary::fetch(rt::Request request, BoundaryLifetime lifetime, BoundaryDelivery delivery) const {
    return boundaryFetch(dependencies, std::move(request), std::move(lifetime), std::move(delivery));
  }

  WorkerBoundary createWorkerBoundary(WorkerBoundaryDependencies dependencies) {
    return WorkerBoundary(std::make_shared<const WorkerBoundaryDependencies>(std::move(dependencies)));
  }
}
