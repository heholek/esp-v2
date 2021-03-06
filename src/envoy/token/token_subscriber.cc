// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/envoy/token/token_subscriber.h"
#include "absl/strings/str_cat.h"
#include "common/common/enum_to_int.h"
#include "common/http/headers.h"
#include "common/http/message_impl.h"
#include "common/http/utility.h"
#include "envoy/http/async_client.h"

namespace Envoy {
namespace Extensions {
namespace Token {

// Request timeout.
constexpr std::chrono::milliseconds kRequestTimeoutMs(5000);

// Delay after a failed fetch.
constexpr std::chrono::seconds kFailedRequestRetryTime(2);

// Update the token `n` seconds before the expiration.
constexpr std::chrono::seconds kRefreshBuffer(5);

TokenSubscriber::TokenSubscriber(
    Envoy::Server::Configuration::FactoryContext& context,
    const TokenType& token_type, const std::string& token_cluster,
    const std::string& token_url, UpdateTokenCallback callback,
    TokenInfoPtr token_info)
    : context_(context),
      token_type_(token_type),
      token_cluster_(token_cluster),
      token_url_(token_url),
      callback_(callback),
      token_info_(std::move(token_info)),
      active_request_(nullptr),
      init_target_(nullptr) {
  debug_name_ = absl::StrCat("TokenSubscriber(", token_url_, ")");
}

void TokenSubscriber::init() {
  init_target_ = std::make_unique<Envoy::Init::TargetImpl>(
      debug_name_, [this] { refresh(); });
  refresh_timer_ =
      context_.dispatcher().createTimer([this]() -> void { refresh(); });

  context_.initManager().add(*init_target_);
}

TokenSubscriber::~TokenSubscriber() {
  if (active_request_) {
    active_request_->cancel();
  }
}

void TokenSubscriber::handleFailResponse() {
  active_request_ = nullptr;
  refresh_timer_->enableTimer(kFailedRequestRetryTime);
}

void TokenSubscriber::handleSuccessResponse(
    absl::string_view token, const std::chrono::seconds& expires_in) {
  active_request_ = nullptr;

  ENVOY_LOG(debug, "{}: Got token with expiry duration: {} , {} sec",
            debug_name_, token, expires_in.count());
  callback_(token);

  // Use the buffer to set the next refresh time.
  if (expires_in <= kRefreshBuffer) {
    refresh();
  } else {
    refresh_timer_->enableTimer(expires_in - kRefreshBuffer);
  }

  // Signal that we are ready for initialization.
  init_target_->ready();
}

void TokenSubscriber::refresh() {
  if (active_request_) {
    active_request_->cancel();
  }

  ENVOY_LOG(debug, "{}: Sending TokenSubscriber request", debug_name_);

  Envoy::Http::RequestMessagePtr message =
      token_info_->prepareRequest(token_url_);
  if (message == nullptr) {
    // Preconditions in TokenInfo are not met, not an error.
    ENVOY_LOG(warn, "{}: preconditions not met, retrying later", debug_name_);
    handleFailResponse();
    return;
  }

  const struct Envoy::Http::AsyncClient::RequestOptions options =
      Envoy::Http::AsyncClient::RequestOptions()
          .setTimeout(kRequestTimeoutMs)
          // Metadata server rejects X-Forwarded-For requests.
          // https://cloud.google.com/compute/docs/storing-retrieving-metadata#x-forwarded-for_header
          .setSendXff(false);

  active_request_ = context_.clusterManager()
                        .httpAsyncClientForCluster(token_cluster_)
                        .send(std::move(message), *this, options);
}

void TokenSubscriber::processResponse(
    Envoy::Http::ResponseMessagePtr&& response) {
  try {
    const uint64_t status_code =
        Envoy::Http::Utility::getResponseStatus(response->headers());

    if (status_code != enumToInt(Envoy::Http::Code::OK)) {
      ENVOY_LOG(error, "{}: failed: {}", debug_name_, status_code);
      handleFailResponse();
      return;
    }
  } catch (const EnvoyException& e) {
    // This occurs if the status header is missing.
    // Catch the exception to prevent unwinding and skipping cleanup.
    ENVOY_LOG(error, "{}: failed: {}", debug_name_, e.what());
    handleFailResponse();
    return;
  }

  // Delegate parsing the HTTP response.
  TokenResult result{};
  bool success;
  switch (token_type_) {
    case IdentityToken:
      success =
          token_info_->parseIdentityToken(response->bodyAsString(), &result);
      break;
    case AccessToken:
      success =
          token_info_->parseAccessToken(response->bodyAsString(), &result);
      break;
    default:
      ENVOY_LOG(error, "{}: failed with unknown token type {}", debug_name_,
                token_type_);
      return;
  }

  // Determine status.
  if (!success) {
    handleFailResponse();
    return;
  }

  handleSuccessResponse(result.token, result.expiry_duration);
}

void TokenSubscriber::onSuccess(Envoy::Http::ResponseMessagePtr&& response) {
  ENVOY_LOG(debug, "{}: got response: {}", debug_name_,
            response->bodyAsString());
  processResponse(std::move(response));
}

void TokenSubscriber::onFailure(
    Envoy::Http::AsyncClient::FailureReason reason) {
  switch (reason) {
    case Http::AsyncClient::FailureReason::Reset:
      ENVOY_LOG(error, "{}: failed with error: the stream has been reset",
                debug_name_);

      break;
    default:
      ENVOY_LOG(error, "{}: failed with an unknown network failure",
                debug_name_);
      break;
  }

  handleFailResponse();
}

}  // namespace Token
}  // namespace Extensions
}  // namespace Envoy
