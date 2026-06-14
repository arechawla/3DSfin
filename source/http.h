#pragma once
#include <string>
#include <map>

struct HttpResponse {
    int         status;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

class HttpClient {
public:
    void setBaseUrl(const std::string& url);
    void setHeader(const std::string& key, const std::string& value);
    void clearHeaders();

    HttpResponse get(const std::string& path);
    HttpResponse post(const std::string& path, const std::string& body,
                      const std::string& contentType = "application/json");

private:
    std::string                        baseUrl_;
    std::map<std::string, std::string> headers_;

    HttpResponse request(const std::string& url, bool isPost,
                         const std::string& postData,
                         const std::string& contentType);
};
