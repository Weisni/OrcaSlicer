#define CURL_STATICLIB
#include <curl/curl.h>
#include <cstdio>
#include <string>
#include "slic3r/Utils/HttpDnsCache.hpp"
static size_t discard(char*, size_t size, size_t count, void*) { return size * count; }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int failures = 0;
    {
        Slic3r::HttpDnsCache cache;
        const std::string url = "http://quack-dns-cache-test.invalid:" + std::string(argv[1]) + "/";
        const std::string mapping = "quack-dns-cache-test.invalid:" + std::string(argv[1]) + ":127.0.0.1";
        for (int index = 0; index < 2; ++index) {
            auto* request = curl_easy_init();
            cache.attach(request);
            curl_easy_setopt(request, CURLOPT_URL, url.c_str());
            curl_easy_setopt(request, CURLOPT_PROXY, "");
            curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, discard);
            curl_easy_setopt(request, CURLOPT_TIMEOUT, 4L);
            auto* addresses = index == 0 ? curl_slist_append(nullptr, mapping.c_str()) : nullptr;
            if (addresses) curl_easy_setopt(request, CURLOPT_RESOLVE, addresses);
            const auto outcome = curl_easy_perform(request);
            if (outcome != CURLE_OK) { ++failures; std::printf("FAIL request %d: %s\n", index, curl_easy_strerror(outcome)); }
            curl_easy_cleanup(request); curl_slist_free_all(addresses);
        }
    }
    curl_global_cleanup();
    std::printf("DNS cache across independent requests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
