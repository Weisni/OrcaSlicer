#pragma once
#include <curl/curl.h>
#include <mutex>
#include <stdexcept>

namespace Slic3r {
// Opt-in DNS reuse across independent HTTP handles. Cookies, connections and
// authentication headers remain private to each request. libcurl retains its
// normal DNS expiry, so a changed HA address is resolved again.
class HttpDnsCache {
public:
    HttpDnsCache() : m_share(curl_share_init())
    {
        if (!m_share) throw std::runtime_error("Cannot create the HTTP DNS cache");
        curl_share_setopt(m_share, CURLSHOPT_USERDATA, this);
        curl_share_setopt(m_share, CURLSHOPT_LOCKFUNC, &lock);
        curl_share_setopt(m_share, CURLSHOPT_UNLOCKFUNC, &unlock);
        curl_share_setopt(m_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    }
    ~HttpDnsCache() { curl_share_cleanup(m_share); }
    HttpDnsCache(const HttpDnsCache&) = delete;
    HttpDnsCache& operator=(const HttpDnsCache&) = delete;
    void attach(CURL* request) { curl_easy_setopt(request, CURLOPT_SHARE, m_share); }
private:
    static void lock(CURL*, curl_lock_data data, curl_lock_access, void* user)
    { static_cast<HttpDnsCache*>(user)->mutex(data).lock(); }
    static void unlock(CURL*, curl_lock_data data, void* user)
    { static_cast<HttpDnsCache*>(user)->mutex(data).unlock(); }
    std::mutex& mutex(curl_lock_data data)
    { return data == CURL_LOCK_DATA_DNS ? m_dns_mutex : m_share_mutex; }
    CURLSH* m_share;
    std::mutex m_dns_mutex, m_share_mutex;
};
}
