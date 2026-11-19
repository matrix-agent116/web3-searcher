#include "common/http.h"

#include <hv/AsyncHttpClient.h>
#include <hv/HttpServer.h>
#include <hv/hlog.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "common/thread_pool.h"

namespace web3 {
namespace {

constexpr size_t kMaxBody = 16 * 1024 * 1024;
constexpr std::string_view kLoopback = "http://127.0.0.1:";
std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void Stop(int) { stopping.store(true, std::memory_order_relaxed); }

int Port(std::string_view origin) {
  if (origin.starts_with(kLoopback)) {
    origin.remove_prefix(kLoopback.size());
    unsigned port = 0;
    const char* end = origin.data() + origin.size();
    auto parsed = std::from_chars(origin.data(), end, port);
    if (parsed.ec == std::errc{} && parsed.ptr == end && port > 0 &&
        port <= 65535) {
      return static_cast<int>(port);
    }
  }
  throw std::invalid_argument("expected http://127.0.0.1:PORT");
}

Reply Error(int status, std::string_view message) {
  return {status, R"({"ok":false,"error":)" + Quote(message) + "}"};
}

int Send(HttpResponse* response, Reply reply) {
  response->SetContentType(APPLICATION_JSON);
  response->body = std::move(reply.body);
  return reply.status;
}

void SetUpLogging() {
  static std::once_flag once;
  std::call_once(once, [] { hlog_set_handler(stderr_logger); });
}

void Dispatch(const Handler& handler, const std::string& body,
              const Completion& done) {
  try {
    simdjson::dom::parser parser;
    auto json = parser.parse(body).value();
    if (!json.is_object()) {
      done(Error(400, "expected a JSON object"));
      return;
    }
    handler(json, done);
  } catch (const std::length_error& error) {
    done(Error(413, error.what()));
  } catch (const std::exception& error) {
    done(Error(400, error.what()));
  } catch (...) {
    done(Error(500, "request handler failed"));
  }
}

// Only the HTTP loop touches the writer. While a handler owns its completion,
// pause socket reads so libhv cannot reuse this writer for the next request.
Completion MakeCompletion(const HttpContextPtr& ctx,
                          std::atomic_size_t& pending) {
  auto writer = ctx->writer;
  writer->stopRead();
  return [writer, loop = hv::tlsEventLoop(),
          sent = std::make_shared<std::atomic_flag>(), &pending](Reply reply) {
    if (sent->test_and_set()) return;
    if (reply.body.size() > kMaxBody)
      reply = Error(500, "response exceeds 16 MiB");
    loop->queueInLoop([writer, reply = std::move(reply), &pending]() mutable {
      if (writer->isConnected()) {
        auto& response = *writer->response;
        response.status_code =
            static_cast<http_status>(Send(&response, std::move(reply)));
        bool keep_alive = response.IsKeepAlive();
        writer->End();
        if (keep_alive && writer->isConnected()) writer->startRead();
      }
      --pending;
    });
  };
}

}  // namespace

std::string Quote(std::string_view value) {
  simdjson::builder::string_builder json;
  json.escape_and_append_with_quotes(value);
  return std::string(json.view().value());
}

void PostAsync(const std::string& origin, const std::string& path,
               std::string body, Completion done) {
  Port(origin);
  if (body.size() > kMaxBody) {
    done(Error(413, "request exceeds 16 MiB"));
    return;
  }
  SetUpLogging();
  static hv::AsyncHttpClient client;
  auto request = std::make_shared<HttpRequest>();
  request->method = HTTP_POST;
  request->url = origin + path;
  request->SetContentType(APPLICATION_JSON);
  request->SetConnectTimeout(3);
  request->SetTimeout(30);
  request->SetRetry(0);
  request->AllowRedirect(false);
  request->body = std::move(body);
  int error =
      client.send(request, [done, origin](const HttpResponsePtr& response) {
        if (!response) {
          done(Error(502, "request to " + origin + " failed"));
        } else if (response->ContentType() != APPLICATION_JSON ||
                   response->body.size() > kMaxBody) {
          done(Error(502, "invalid HTTP response from " + origin));
        } else {
          done({response->status_code, std::move(response->body)});
        }
      });
  if (error != 0) done(Error(502, "cannot submit request to " + origin));
}

Reply Post(const std::string& origin, const std::string& path,
           std::string body) {
  std::promise<Reply> result;
  auto future = result.get_future();
  PostAsync(origin, path, std::move(body),
            [&result](Reply reply) { result.set_value(std::move(reply)); });
  return future.get();
}

int PrintResponse(const Reply& reply) {
  std::cout << reply.body << '\n';
  return reply.status >= 200 && reply.status < 300 ? 0 : 1;
}

namespace {

using Schedule = std::function<void(std::function<void()>)>;

void ServeWithScheduler(const std::string& origin,
                        const std::map<std::string, Handler>& routes,
                        const Schedule& schedule, std::stop_token stop) {
  const int port = Port(origin);
  SetUpLogging();
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  std::atomic_size_t pending{0};
  hv::HttpService service;
  service.document_root.clear();
  service.GET("/health", [](HttpRequest*, HttpResponse* response) {
    return Send(response, {200, R"({"ok":true})"});
  });
  for (const auto& [path, handler] : routes) {
    service.POST(path.c_str(), [&, handler](const HttpContextPtr& ctx,
                                            http_parser_state state,
                                            const char* data, size_t size) {
      auto& body = ctx->request->body;
      if (state == HP_BODY && ctx->response->status_code == HTTP_STATUS_OK) {
        if (size > kMaxBody - body.size()) {
          ctx->response->status_code = HTTP_STATUS_PAYLOAD_TOO_LARGE;
          return Send(ctx->response.get(),
                      Error(413, "request exceeds 16 MiB"));
        }
        body.append(data, size);
      }
      if (state == HP_MESSAGE_COMPLETE) {
        if (ctx->request->ContentType() != APPLICATION_JSON) {
          return Send(ctx->response.get(),
                      Error(415, "expected application/json"));
        }
        constexpr size_t kMaxInFlight = 64;
        if (pending.fetch_add(1) >= kMaxInFlight || stopping.load() ||
            stop.stop_requested()) {
          --pending;
          return Send(ctx->response.get(),
                      Error(503, "service busy or stopping"));
        }
        auto done = MakeCompletion(ctx, pending);
        try {
          schedule([handler, body = std::move(body), done] {
            Dispatch(handler, body, done);
          });
        } catch (const std::exception& error) {
          done(Error(503, error.what()));
        }
      }
      return 0;
    });
  }
  service.preprocessor = [&service](HttpRequest* request,
                                    HttpResponse* response) {
    http_handler* route = nullptr;
    int status = service.GetRoute(request, &route);
    return status ? Send(response,
                         Error(status, status == 405 ? "method not allowed"
                                                     : "unknown route"))
                  : 0;
  };
  hv::HttpServer server(&service);
  server.setHost("127.0.0.1");
  server.setPort(port);
  server.setThreadNum(1);
  if (server.start() != 0) {
    throw std::runtime_error("cannot listen on " + origin);
  }
  std::cerr << "Listening on " << origin << '\n';
  while ((!stopping.load() && !stop.stop_requested()) || pending.load() != 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace

void Serve(const std::string& origin,
           const std::map<std::string, Handler>& routes, ThreadPool& pool,
           std::stop_token stop) {
  ServeWithScheduler(
      origin, routes,
      [&pool](std::function<void()> task) { pool.Commit(std::move(task)); },
      stop);
}

void Serve(const std::string& origin,
           const std::map<std::string, Handler>& routes, std::stop_token stop) {
  SetUpLogging();
  hv::EventLoopThread worker;
  worker.start();
  ServeWithScheduler(
      origin, routes,
      [&worker](std::function<void()> task) {
        worker.loop()->queueInLoop(std::move(task));
      },
      stop);
}

}  // namespace web3
