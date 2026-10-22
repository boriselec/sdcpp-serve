// sdcpp-serve name='<sd-server flags>' ...
// One sd-server per model, all on port 8081. /<name>/... goes to that model's
// sd-server API, except our own POST /<name>/generate.

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include "json.hpp"

std::map<std::string, int> ports;  // model name -> sd-server port
std::mutex busy;

std::unique_ptr<httplib::Client> connect(const httplib::Request &req,
                                         httplib::Response &res);
std::string base64_decode(const std::string &in);

// sd-server's own APIs, passed on unchanged
void forward(const httplib::Request &req, httplib::Response &res) {
    auto client = connect(req, res);
    if (!client) {
        return;
    }
    std::string path = req.target.substr(req.target.find('/', 1));
    httplib::Result r;
    if (req.method == "GET") {
        r = client->Get(path);
    } else if (req.is_multipart_form_data()) {
        // httplib splits an upload into fields and files, so rebuild it
        httplib::UploadFormDataItems items;
        for (const auto &[name, field] : req.form.fields) {
            items.push_back({name, field.content, "", ""});
        }
        for (const auto &[name, file] : req.form.files) {
            items.push_back({name, file.content, file.filename, file.content_type});
        }
        r = client->Post(path, items);
    } else {
        r = client->Post(path, req.body, req.get_header_value("Content-Type"));
    }
    if (!r) {
        res.status = 502;
        return;
    }
    res.status = r->status;
    res.set_content(r->body, r->get_header_value("Content-Type"));
}

// Our own API, simpler than sd-server's. The body is the prompt as plain text and
// the answer is a PNG. It calls sd-server's /v1/images/generations and decodes the
// image from the JSON reply.
void generate(const httplib::Request &req, httplib::Response &res) {
    auto client = connect(req, res);
    if (!client) {
        return;
    }
    std::string body = nlohmann::json{{"prompt", req.body}}.dump();
    auto r = client->Post("/v1/images/generations", body, "application/json");
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

// One generation at the same time.
// Others get 503.
httplib::Server::Handler one_at_a_time(httplib::Server::Handler handler) {
    return [handler](const httplib::Request &req, httplib::Response &res) {
        std::unique_lock<std::mutex> lock(busy, std::try_to_lock);
        if (!lock.owns_lock()) {
            res.status = 503;
            return;
        }
        handler(req, res);
    };
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
        if (!ports.emplace(model, port).second) {
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
    server.Post("/([^/]+)/generate", one_at_a_time(generate));
    server.Get("/([^/]+)/.*", forward);
    server.Post("/([^/]+)/.*", one_at_a_time(forward));
    server.listen("0.0.0.0", 8081);
}

// a client for the sd-server of the model named first in the path, or a 404
std::unique_ptr<httplib::Client> connect(const httplib::Request &req,
                                         httplib::Response &res) {
    auto model = ports.find(req.matches[1]);
    if (model == ports.end()) {
        res.status = 404;
        return nullptr;
    }
    auto client = std::make_unique<httplib::Client>("127.0.0.1", model->second);
    client->set_read_timeout(std::chrono::hours(1));
    client->set_path_encode(false);  // the target is passed on as it came
    return client;
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
