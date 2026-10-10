#include "common/os/http_client.hpp"

#ifdef _WIN32
#include "common/os/paths.hpp"
#include "common/os/unique_handle.hpp"
#include "hub/version.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#endif

namespace hub::os {

namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

} // namespace

#ifdef _WIN32
namespace {

/// A WinHTTP session, connection or request, which only WinHttpCloseHandle may close.
struct InternetHandleTraits {
    using pointer = HINTERNET;
    static pointer empty() noexcept { return nullptr; }
    static bool is_valid(pointer handle) noexcept { return handle != nullptr; }
    static void close(pointer handle) noexcept { WinHttpCloseHandle(handle); }
};
using UniqueInternet = BasicUniqueHandle<InternetHandleTraits>;

/// Resolve, connect, send and each receive; a stalled read gives up after this.
constexpr int kTimeoutMs = 10000;

std::string describe(const char* step, DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:           return std::string(step) + " timed out";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return std::string(step) + ": host not found";
        case ERROR_WINHTTP_CANNOT_CONNECT:    return std::string(step) + ": could not connect";
        case ERROR_WINHTTP_SECURE_FAILURE:    return std::string(step) + ": TLS check failed";
        default: return std::string(step) + " failed (error " + std::to_string(code) + ")";
    }
}

UniqueInternet open_session() {
    const std::wstring agent = to_wide("ffxiv-hub/" HUB_VERSION_STRING);
    UniqueInternet session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        // Automatic proxy discovery needs Windows 8.1.
        session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    }
    return session;
}

/// A numeric header, or nullopt when the response has none.
std::optional<DWORD> query_number(HINTERNET request, DWORD header) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(request, header | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &value, &size, WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    return value;
}

bool cancelled(const HttpRequest& request) noexcept {
    return request.cancel != nullptr && request.cancel->load();
}

} // namespace

bool http_supported() noexcept {
    return true;
}

std::optional<HttpResponse> https_get(const HttpRequest& request, std::string* error) {
    if (!request.url.starts_with("https://")) {
        set_error(error, "only https:// URLs are fetched");
        return std::nullopt;
    }
    const std::wstring url = to_wide(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        set_error(error, "malformed URL");
        return std::nullopt;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    const UniqueInternet session = open_session();
    if (!session) {
        set_error(error, describe("WinHttpOpen", GetLastError()));
        return std::nullopt;
    }
    WinHttpSetTimeouts(session.get(), kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    const UniqueInternet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) {
        set_error(error, describe("Connect", GetLastError()));
        return std::nullopt;
    }
    const UniqueInternet handle(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE));
    if (!handle) {
        set_error(error, describe("Request", GetLastError()));
        return std::nullopt;
    }

    std::wstring headers;
    for (const auto& [name, value] : request.headers) {
        headers += to_wide(name) + L": " + to_wide(value) + L"\r\n";
    }
    if (!headers.empty()) {
        WinHttpAddRequestHeaders(handle.get(), headers.c_str(), static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    if (!WinHttpSendRequest(handle.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(handle.get(), nullptr)) {
        set_error(error, describe("Request", GetLastError()));
        return std::nullopt;
    }

    HttpResponse response;
    const auto status = query_number(handle.get(), WINHTTP_QUERY_STATUS_CODE);
    if (!status) {
        set_error(error, "response has no status");
        return std::nullopt;
    }
    response.status = static_cast<int>(*status);
    const size_t total = query_number(handle.get(), WINHTTP_QUERY_CONTENT_LENGTH).value_or(0);
    if (total > request.max_bytes) {
        set_error(error, "response is larger than allowed");
        return std::nullopt;
    }
    response.body.reserve(total);

    for (;;) {
        if (cancelled(request)) {
            set_error(error, "cancelled");
            return std::nullopt;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle.get(), &available)) {
            set_error(error, describe("Download", GetLastError()));
            return std::nullopt;
        }
        if (available == 0) break;
        if (available > request.max_bytes - response.body.size()) {
            set_error(error, "response is larger than allowed");
            return std::nullopt;
        }
        const size_t before = response.body.size();
        response.body.resize(before + available);
        DWORD read = 0;
        if (!WinHttpReadData(handle.get(), response.body.data() + before, available, &read)) {
            set_error(error, describe("Download", GetLastError()));
            return std::nullopt;
        }
        response.body.resize(before + read);
        if (request.on_progress) request.on_progress(response.body.size(), total);
    }
    return response;
}

#else

bool http_supported() noexcept {
    return false;
}

std::optional<HttpResponse> https_get(const HttpRequest&, std::string* error) {
    set_error(error, "no network access on this platform");
    return std::nullopt;
}

#endif

} // namespace hub::os
