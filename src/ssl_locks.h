#pragma once
/* OpenSSL thread locks + curl_global_init: call once, before any thread. */
int ssl_threads_init(void);
