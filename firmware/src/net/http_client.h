#pragma once
#include <Arduino.h>

namespace http_client {
    // GET <url>/state with Authorization: Bearer <token>.
    // On success returns true and copies the response body into out (max out_max bytes).
    bool get_state(const String& url, const String& token, char* out, size_t out_max);

    // POST <url>/refresh, no body. Returns true on HTTP 200.
    bool post_refresh(const String& url, const String& token);
}
