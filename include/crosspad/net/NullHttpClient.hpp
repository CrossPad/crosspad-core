// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/net/IHttpClient.hpp"

namespace crosspad {

/**
 * @brief No-op IHttpClient stub for platforms without networking.
 *
 * Used as the default singleton until a real HTTP client is registered,
 * and as the permanent implementation on platforms that lack network
 * connectivity (e.g. PC simulator without WinHTTP/libcurl).
 */
class NullHttpClient : public IHttpClient {
public:
    bool isAvailable() const override { return false; }

    HttpResponse get(const HttpRequest&) override {
        return {0, "", "HTTP not available on this platform"};
    }

    HttpResponse post(const HttpRequest&) override {
        return {0, "", "HTTP not available on this platform"};
    }
};

} // namespace crosspad
