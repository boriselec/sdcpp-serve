// sdcpp-serve name='<sd-server flags>' ...
// One sd-server per model, all on port 8081. The only API is POST /<name>/generate:
// synchronous, one image at a time, with the settings given at startup.

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include "json.hpp"

std::map<std::string, int> model_to_port;

bool is_openai_request(const std::string &body);
std::string base64_decode(const std::string &in);

// plain text prompt -> PNG
void generate(const httplib::Request &req, httplib::Response &res) {
    auto model_and_port = model_to_port.find(req.path_params.at("model"));
    if (model_and_port == model_to_port.end()) {
        res.status = 404;
        return;
    }
    // the body is the prompt, as text: curl's default form type is fine, but not JSON,
    // or multipart, which httplib takes apart and leaves the body empty
    std::string type = req.get_header_value("Content-Type");
    type = type.substr(0, type.find(';'));
    if (type != "" &&
        type != "text/plain" &&
        type != "application/x-www-form-urlencoded") {
        res.status = 415;
        return;
    }
    // a request for OpenAI-style APIs
    if (is_openai_request(req.body)) {
        res.status = 400;
        res.set_content(R"({"error":"send the prompt itself, not a JSON request"})",
                        "application/json");
        return;
    }
    // sd-server would take settings from this block in the prompt, see
    // extract_and_remove_sd_cpp_extra_args() in examples/server/routes_openai.cpp
    if (req.body.contains("<sd_cpp_extra_args>")) {
        res.status = 400;
        res.set_content(R"({"error":"generation settings are fixed at startup"})",
                        "application/json");
        return;
    }
    // one generation at the same time, across all models, others get 503
    static std::mutex busy;
    std::unique_lock lock(busy, std::try_to_lock);
    if (!lock.owns_lock()) {
        res.status = 503;
        return;
    }
    // no keep-alive: sd-server doesn't set TCP_NODELAY, reused connections wait ~40 ms
    httplib::Client client("127.0.0.1", model_and_port->second);
    client.set_read_timeout(std::chrono::hours(1));
    std::string body = nlohmann::json{{"prompt", req.body}}.dump();
    auto r = client.Post("/v1/images/generations", body, "application/json");
    lock.unlock();

    if (!r) {
        res.status = 502;
        return;
    }
    if (r->status != 200) {
        // sd-server's error, passed on
        res.status = r->status;
        res.set_content(r->body, r->get_header_value("Content-Type"));
        return;
    }
    auto png = nlohmann::json::parse(r->body)["data"][0]["b64_json"];
    res.set_content(base64_decode(png.get<std::string>()), "image/png");
}

int main(int argc, char **argv) {
    auto stop = [](int) { _exit(0); };
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    if (argc < 2) {
        fprintf(stderr, "usage: sdcpp-serve name='<sd-server flags>' ...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        size_t eq = arg.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "expected name=<sd-server flags>, got: %s\n", argv[i]);
            return 1;
        }
        int port = 1234 + (i - 1);
        std::string model = arg.substr(0, eq);
        if (!model_to_port.emplace(model, port).second) {
            fprintf(stderr, "model named twice: %s\n", model.c_str());
            return 1;
        }
        std::vector<std::string> args = {
            "/opt/sd.cpp/sd-server",
            // not sd-server's default: mapped weights of idle models can be dropped
            // and re-read later, so several models can share RAM.
            "--mmap"};
        std::istringstream words(arg.substr(eq + 1));
        for (std::string word; words >> word;) {
            args.push_back(word);
        }
        args.insert(args.end(), {
            "--listen-ip", "127.0.0.1",
            "--listen-port", std::to_string(port)});
        if (fork() == 0) {
            std::vector<char *> exec_args;
            for (auto &a : args) {
                exec_args.push_back(a.data());
            }
            exec_args.push_back(nullptr);
            execv(exec_args[0], exec_args.data());
            _exit(127);
        }
    }
    // an sd-server exited: exit too, so that the container restarts
    std::thread([] {
        wait(nullptr);
        _exit(1);
    }).detach();

    httplib::Server server;
    // the same as httplib's fixed limit for application/x-www-form-urlencoded
    server.set_payload_max_length(8 * 1024);
    server.Post("/:model/generate", generate);
    server.listen("0.0.0.0", 8081);
}

bool is_openai_request(const std::string &body) {
    // cheap exit for plain prompts
    size_t start = body.starts_with("\xEF\xBB\xBF") ? 3 : 0;
    start = body.find_first_not_of(" \t\r\n", start);
    if (start == std::string::npos || body[start] != '{') {
        return false;
    }

    auto json = nlohmann::json::parse(body, nullptr, false);
    // structured prompts are JSON objects too, without "prompt"
    auto prompt = json.find("prompt");
    return prompt != json.end() && prompt->is_string();
}

std::string base64_decode(const std::string &in) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned buffer = 0, bits = 0;
    for (char c : in) {
        size_t i = chars.find(c);
        if (i == std::string::npos) {
            continue;  // padding
        }
        buffer = (buffer << 6 | i) & 0xffff;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>(buffer >> bits);
        }
    }
    return out;
}
