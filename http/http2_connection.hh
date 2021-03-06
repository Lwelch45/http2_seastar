﻿/*
 * Copyright (C) 2018 ScyllaDB
 */

/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#pragma once

#include "http2_file_handler.hh"
#include "http2_request_response.hh"
#include "core/iostream.hh"
#include "http/routes.hh"
#include "net/api.hh"
#include "core/future-util.hh"
#include "core/temporary_buffer.hh"
#include "core/sstring.hh"
#include "core/iostream.hh"
#include "core/app-template.hh"
#include "core/distributed.hh"
#include "net/socket_defs.hh"
#include "net/api.hh"
#include "net/tls.hh"
#include <nghttp2/nghttp2.h>
#include <boost/intrusive/list.hpp>
#include <optional>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <array>
#include <vector>
#include <stdexcept>

namespace seastar {
namespace httpd {

class session : public boost::intrusive::list_base_hook<> {
public:
    virtual future<> process() = 0;
    virtual void shutdown() = 0;
    virtual output_stream<char>& out() = 0;
    virtual ~session() = default;
};
}
}

namespace seastar {
namespace httpd2 {

using user_callback = std::function<
                        future<
                            std::tuple<
                                lw_shared_ptr<request>, std::unique_ptr<response>>>
    (lw_shared_ptr<request>, std::unique_ptr<response>)>;
using client_callback = std::function<void(const sstring&)>;

using dhandler = seastar::httpd2::directory_handler;

class routes {
public:
    user_callback handle(const sstring &path);
    user_callback handle_push();
    routes& add(const method type, const sstring &path, user_callback handler);
    routes& add_on_push(const sstring &path, user_callback handler, user_callback push_handler);
    routes& add_on_client(client_callback handler);
    sstring& get_push_path() { return _push_path; }
    routes& add_directory_handler(dhandler *handler);
    ~routes() {
        delete _directory_handler;
    }
private:
    std::unordered_map<sstring, user_callback> _path_to_handler;
    user_callback _push_handler;
    sstring _push_path;
public:
    dhandler *_directory_handler {nullptr};
    sstring *_date {nullptr};
public:
    client_callback _client_handler;
};

class http2_stream {
public:
    http2_stream() = default;
    http2_stream(const int32_t id, routes &routes_)
        : _id(id), _req(make_lw_shared<request>()), _routes(routes_) {}

    http2_stream(const int32_t id, lw_shared_ptr<request> req, routes &routes_)
        : _id(id), _req(req), _routes(routes_) {
        assert(_req);
    }
    int32_t get_id() const {
        return _id;
    }
    future<> eat_request(bool promised_stream = false);
    bool pushable() const {
        return _req->_path == _routes.get_push_path();
    }
    void update_request(request_feed &data) {
        _req->add_header(data);
    }
    void commit_response(bool promised = false);
    void migrate_to_promise() {
        _promised_rep = std::move(_rep);
        _rep = std::make_unique<response>();
    }
    const response &get_response() const {
        return *_rep;
    }
private:
    int32_t _id {0};
    lw_shared_ptr<request> _req;
    std::unique_ptr<response> _rep, _promised_rep;
    routes &_routes;
};

enum class session_t {client, server};
enum class ops {
    on_begin_headers,
    on_frame_recv,
    on_header,
    on_data_chunk_recv,
    on_stream_close,
    on_frame_send,
    on_frame_not_send
};

namespace legacy = seastar::httpd;

template<session_t session_type = session_t::server>
class http2_connection final : public legacy::session {
public:
    explicit http2_connection(routes &routes_, connected_socket&& fd, socket_address addr = socket_address());
    future<> process() override;
    void shutdown() override;
    output_stream<char>& out() override;
    ~http2_connection();
    future<> process_internal(bool start_with_reading = true);
    int resume(const http2_stream &stream);
    int submit_response(http2_stream &stream);
    int submit_push_promise(http2_stream &stream);
    int submit_request(lw_shared_ptr<request> _request);
    void create_stream(const int32_t stream_id, lw_shared_ptr<request> req);
    void eat_server_rep(data_chunk_feed data);
    unsigned pending_streams() const {
        return _streams.size();
    }
private:
    nghttp2_session *_session {nullptr};
    bool _done {false};
    std::unordered_map<int32_t, std::unique_ptr<http2_stream>> _streams;
    std::pair<int32_t, http2_stream*> last_active_stream {-1, nullptr};
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    routes &_routes;
    constexpr static auto _streams_limit = 100u;
    std::vector<lw_shared_ptr<request>> _remaining_reqs;
    bool _start_with_reading;

    future<> process_send();
    int submit_request_nghttp2(lw_shared_ptr<request> _request);
    void reset_stream(int32_t stream_id, uint32_t error_code);
    future<> internal_process();
    void dump_frame(nghttp2_frame_type frame_type, const char *direction = "---------------------------->");
    void receive_nghttp2(const uint8_t *data, size_t len);
    int send_nghttp2(const uint8_t **data);
    int handle_remaining_reqs();
    template<ops state>
    int consume_frame(nghttp2_internal_data &&data);
    void create_stream(const int32_t stream_id);
    void close_stream(const int32_t stream_id);
    http2_stream *find_stream(const int32_t stream_id);
};

}
}
