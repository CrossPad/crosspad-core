// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file IHttpClient.hpp
 * @brief Abstract HTTP client interface for portable network requests.
 *
 * Platform implementations:
 *   - ESP-IDF: Esp32HttpClient wrapping esp_http_client
 *   - PC:      NullHttpClient (stub) or PcHttpClient (WinHTTP/libcurl)
 *
 * Synchronous API — blocks until the request completes or times out.
 * Apps should check hasCapability(Capability::Http) or isAvailable()
 * before calling get()/post().
 */

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace crosspad {

/**
 * @brief HTTP request parameters.
 *
 * Maps directly to esp_http_client_config_t fields on ESP-IDF.
 */
struct HttpRequest {
    std::string url;
    std::string body;         ///< POST body (leave empty for GET)
    std::string contentType;  ///< e.g. "application/json"
    std::vector<std::pair<std::string, std::string>> headers;  ///< Additional headers
    uint32_t timeoutMs = 10000;  ///< Request timeout in milliseconds
};

/**
 * @brief HTTP response returned by get()/post().
 */
struct HttpResponse {
    int statusCode = 0;        ///< HTTP status (200, 404, etc.); 0 = transport error
    std::string body;          ///< Response body
    std::string errorMessage;  ///< Human-readable error (empty on success)

    /// True if statusCode is in the 2xx range
    bool success() const { return statusCode >= 200 && statusCode < 300; }
};

/**
 * @brief Abstract HTTP client interface.
 *
 * Implementations:
 *   - Esp32HttpClient (ESP-IDF): wraps esp_http_client, synchronous perform
 *   - NullHttpClient (core): no-op stub for platforms without networking
 */
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    /// Whether the HTTP client is ready (network connected, etc.)
    virtual bool isAvailable() const = 0;

    /// Synchronous HTTP GET request
    virtual HttpResponse get(const HttpRequest& request) = 0;

    /// Synchronous HTTP POST request
    virtual HttpResponse post(const HttpRequest& request) = 0;
};

/// Platform-injected singleton (same pattern as getClock(), getVibration())
IHttpClient& getHttpClient();

} // namespace crosspad
