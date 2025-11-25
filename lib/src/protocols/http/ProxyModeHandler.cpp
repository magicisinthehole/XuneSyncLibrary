#include "ProxyModeHandler.h"
#include "HttpClient.h"

ProxyModeHandler::ProxyModeHandler(const ProxyConfig& config)
    : config_(config) {

    // Create HttpClient with server configuration
    HttpClient::ServerConfig client_config;
    client_config.catalog_server = config.catalog_server;
    client_config.image_server = config.image_server;
    client_config.art_server = config.art_server;
    client_config.mix_server = config.mix_server;
    client_config.timeout_ms = config.timeout_ms;

    http_client_ = std::make_unique<HttpClient>(client_config);
}

ProxyModeHandler::~ProxyModeHandler() = default;

HTTPParser::HTTPResponse ProxyModeHandler::HandleRequest(const HTTPParser::HTTPRequest& request) {
    // Handle Microsoft connectivity check endpoint
    if (HttpClient::IsConnectivityCheck(request.path)) {
        Log("Connectivity check: " + request.method + " " + request.path + " -> returning 200 OK");
        return HttpClient::BuildConnectivityResponse();
    }

    std::string host = request.GetHeader("Host");
    std::string server = http_client_->SelectServer(host);

    if (server.empty()) {
        Log("Error: No proxy server configured for host: " + host);
        return HTTPParser::BuildErrorResponse(502, "No proxy server configured");
    }

    std::string full_url = HttpClient::BuildURL(server, request.path, request.query_params);

    Log("Proxy mode: " + request.method + " " + full_url);

    if (request.method == "GET") {
        return http_client_->PerformGET(full_url, request.headers);
    } else {
        Log("Warning: Unsupported HTTP method: " + request.method);
        return HTTPParser::BuildErrorResponse(405, "Method not allowed");
    }
}

void ProxyModeHandler::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
    if (http_client_) {
        http_client_->SetLogCallback(callback);
    }
}

bool ProxyModeHandler::TestConnection() {
    return http_client_ && http_client_->TestConnection();
}

void ProxyModeHandler::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_("[ProxyModeHandler] " + message);
    }
}
