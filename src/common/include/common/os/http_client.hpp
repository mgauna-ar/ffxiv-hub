#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hub::os {

struct HttpRequest {
    /// https:// only.
    std::string url;
    /// Sent as given, besides the User-Agent every request carries.
    std::vector<std::pair<std::string, std::string>> headers;
    /// The request fails rather than read a longer body.
    size_t max_bytes{1024 * 1024};
    /// Called on the requesting thread as the body arrives, with the bytes so far
    /// and the Content-Length (0 when the server sent none).
    std::function<void(size_t received, size_t total)> on_progress;
    /// Checked between reads; setting it makes the request fail early.
    const std::atomic<bool>* cancel{nullptr};
};

struct HttpResponse {
    int status{0};
    std::vector<uint8_t> body;
};

/// Whether https_get() can reach the network: WinHTTP on Windows, never on the mock.
[[nodiscard]] bool http_supported() noexcept;

/// A GET over HTTPS, following redirects but never from HTTPS to HTTP. Any HTTP
/// status comes back as a response. nullopt, with the reason in `error`, when no
/// response came, the body passed `max_bytes`, or the request was cancelled.
[[nodiscard]] std::optional<HttpResponse> https_get(const HttpRequest& request, std::string* error = nullptr);

} // namespace hub::os
