#include "http.h"
#include <3ds.h>
#include <cstring>

// Hard cap on response body to protect against runaway downloads
static constexpr u32 MAX_BODY = 512 * 1024;

void HttpClient::setBaseUrl(const std::string& url) {
    baseUrl_ = url;
    while (!baseUrl_.empty() && baseUrl_.back() == '/')
        baseUrl_.pop_back();
}

void HttpClient::setHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

void HttpClient::clearHeaders() {
    headers_.clear();
}

HttpResponse HttpClient::get(const std::string& path) {
    return request(baseUrl_ + path, Method::Get, "", "");
}

HttpResponse HttpClient::post(const std::string& path, const std::string& body,
                              const std::string& contentType) {
    return request(baseUrl_ + path, Method::Post, body, contentType);
}

HttpResponse HttpClient::del(const std::string& path) {
    return request(baseUrl_ + path, Method::Delete, "", "");
}

HttpResponse HttpClient::request(const std::string& url, Method method,
                                 const std::string& postData,
                                 const std::string& contentType) {
    HttpResponse resp{0, ""};

    httpcContext ctx;
    HTTPC_RequestMethod hm = method == Method::Post   ? HTTPC_METHOD_POST
                           : method == Method::Delete ? HTTPC_METHOD_DELETE
                                                      : HTTPC_METHOD_GET;

    if (R_FAILED(httpcOpenContext(&ctx, hm, url.c_str(), 1))) {
        resp.status = -1;
        return resp;
    }

    httpcAddRequestHeaderField(&ctx, "User-Agent", "3DSFin/0.1");
    httpcAddRequestHeaderField(&ctx, "Accept",     "application/json");

    for (auto& kv : headers_)
        httpcAddRequestHeaderField(&ctx, kv.first.c_str(), kv.second.c_str());

    if (method == Method::Post && !postData.empty()) {
        httpcAddRequestHeaderField(&ctx, "Content-Type", contentType.c_str());
        httpcAddPostDataRaw(&ctx,
            reinterpret_cast<const u32*>(postData.c_str()),
            static_cast<u32>(postData.size()));
    }

    if (R_FAILED(httpcBeginRequest(&ctx))) {
        httpcCloseContext(&ctx);
        resp.status = -2;
        return resp;
    }

    u32 statuscode = 0;
    httpcGetResponseStatusCode(&ctx, &statuscode);
    resp.status = static_cast<int>(statuscode);

    // Stream response in 4 KB chunks
    u8 buf[4096];
    resp.body.reserve(16384);
    Result dlret;
    do {
        u32 read = 0;
        dlret = httpcDownloadData(&ctx, buf, sizeof(buf), &read);
        if (read > 0) {
            resp.body.append(reinterpret_cast<char*>(buf), read);
            if (resp.body.size() >= MAX_BODY) break;
        }
    } while (dlret == static_cast<Result>(HTTPC_RESULTCODE_DOWNLOADPENDING));

    httpcCloseContext(&ctx);
    return resp;
}
