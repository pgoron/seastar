#include <map>

#include <seastar/core/seastar.hh>
#include <seastar/core/print.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/when_all.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/tls.hh>
#include "picojson.h"

using namespace seastar;

class connection {
    private:
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
        http_response_parser _parser;

    public:
        connection(connected_socket&& fd)
            : _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output()) {
        }

        future<> do_req(const sstring& serverName, const sstring& path) {
            auto request = sstring("GET ");
            request += path;
            request += " HTTP/1.1\r\nHost: ";
            request += serverName;
            request += "\r\nAccept: application/json\r\n\r\n";

            std::cout << "sending http request: " << request << std::endl;

            return _write_buf.write(request).then([this] {
                std::cout << "flushing http request" << std::endl;
                return _write_buf.flush();
            }).then([this] {
                std::cout << "reading http response" << std::endl;
                _parser.init();
                return _read_buf.consume(_parser).then([this] {
                    // Read HTTP response header first
                    if (_parser.eof()) {
                        return make_ready_future<>();
                    }
                    auto _rsp = _parser.get_parsed_response();
                    std::cout << "response version: " << _rsp->_version << std::endl;
                    std::cout << "response status: " << _rsp->_status << std::endl;
                    for( const std::pair<sstring, sstring>& n : _rsp->_headers ) {
                        std::cout << n.first << "=" << n.second << std::endl;
                    }

                    auto it = _rsp->_headers.find("Content-Length");
                    if (it == _rsp->_headers.end()) {
                        it = _rsp->_headers.find("content-length");
                        if (it == _rsp->_headers.end()) {
                            fmt::print("Error: HTTP response does not contain: Content-Length\n");
                            return make_ready_future<>();
                        }
                    }
                    auto content_len = std::stoi(it->second);

                    // Read HTTP response body
                    return _read_buf.read_exactly(content_len).then([this, content_len] (temporary_buffer<char> buf) {
                        const char* json = buf.get();
                        picojson::value v;
                        std::string err;
                        picojson::parse(v, json, json + content_len, &err);
                        if (! err.empty()) {
                            std::cerr << err << std::endl;
                        } else {
                            std::cout << "body:" << std::endl << v.serialize() << std::endl;
                        }
                        return make_ready_future();
                    });
                });
            });
        }
    };

namespace bpo = boost::program_options;

int main(int ac, char** av) {
    app_template app;

    app.add_options()
        ("server,s", bpo::value<std::string>()->default_value("localhost"), "Server hostname");
    app.add_options()
        ("path,p", bpo::value<std::string>()->default_value("/"), "Server path");

    return app.run(ac, av, [&app] () -> future<int> {
        auto&& config = app.configuration();
        seastar::shared_ptr<sstring> server;
        seastar::shared_ptr<sstring> path;
        server = seastar::make_shared<sstring>(config["server"].as<std::string>());
        path = seastar::make_shared<sstring>(config["path"].as<std::string>());
        auto creds = make_shared<tls::certificate_credentials>();

        // load system trust store and do dns resolution in parallel
        return when_all(net::dns::resolve_name(*server),
                        creds->set_system_trust())
            .then([server, path, creds] (auto results) -> future<int> {
                // extract ip from first future
                auto ip = std::get<0>(results).get0();
                std::cout << server << " -> " << ip << std::endl;
                socket_address sa(ip, 443);
                return tls::connect(creds, sa, *server)
                    .then([server, path] (connected_socket socket) {
                        std::cout << "Connected" << std::endl;
                        auto c = new connection(std::move(socket));
                        return c->do_req(*server, *path).then([c] () -> future<int> {
                            std::cout << "Done" << std::endl;
                            delete c;
                            return make_ready_future<int>(0);
                        });
                });
            });
    });
}
