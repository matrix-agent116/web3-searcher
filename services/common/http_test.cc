#include "common/http.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <hv/HttpClient.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include "common/thread_pool.h"

namespace web3 {
namespace {

using namespace std::chrono_literals;

std::string FreeOrigin() {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("cannot open test socket");
  int result = bind(fd, reinterpret_cast<sockaddr*>(&addr), length);
  if (result == 0) {
    result = getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length);
  }
  close(fd);
  if (result != 0) throw std::runtime_error("cannot allocate test port");
  return "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
}

std::future<Reply> Submit(const std::string& origin, const std::string& path) {
  auto result = std::make_shared<std::promise<Reply>>();
  auto future = result->get_future();
  PostAsync(origin, path, "{}",
            [result](Reply reply) { result->set_value(std::move(reply)); });
  return future;
}

int Health(const std::string& origin) {
  HttpRequest request;
  request.url = origin + "/health";
  request.SetTimeout(1);
  HttpResponse response;
  return http_client_send(&request, &response) == 0 ? response.status_code : 0;
}

class HttpTest : public testing::Test {
 protected:
  void Start(std::map<std::string, Handler> routes) {
    server_ =
        std::jthread([this, routes = std::move(routes)](std::stop_token stop) {
          Serve(origin_, routes, pool_, stop);
        });
    for (int i = 0; i < 100; ++i) {
      if (Health(origin_) == 200) return;
      std::this_thread::sleep_for(10ms);
    }
    FAIL() << "test server did not become healthy";
  }
  std::string origin_ = FreeOrigin();
  ThreadPool pool_{2};
  std::jthread server_;
};

TEST_F(HttpTest, ForwardsWithoutWaitingForDownstream) {
  auto held = std::make_shared<std::promise<Completion>>();
  auto downstream = held->get_future();
  Start({{"/relay",
          [this](Json, Completion done) {
            PostAsync(origin_, "/hold", "{}", std::move(done));
          }},
         {"/hold",
          [held](Json, Completion done) { held->set_value(std::move(done)); }},
         {"/echo", [](Json, Completion done) { done({200, "{}"}); }}});
  auto relay = Submit(origin_, "/relay");
  ASSERT_EQ(downstream.wait_for(3s), std::future_status::ready);
  auto release = downstream.get();
  auto echo = Submit(origin_, "/echo");
  EXPECT_EQ(echo.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(relay.wait_for(0s), std::future_status::timeout);
  release({422, R"({"error":"rejected"})"});
  EXPECT_EQ(echo.get().status, 200);
  auto reply = relay.get();
  EXPECT_EQ(reply.status, 422);
  EXPECT_EQ(reply.body, R"({"error":"rejected"})");
}

TEST_F(HttpTest, OtherRequestsAndHealthRespondWhileWorkerIsBusy) {
  auto started = std::make_shared<std::promise<void>>();
  auto ready = started->get_future();
  std::promise<void> release;
  auto gate = release.get_future().share();
  Start({{"/work",
          [started, gate](Json, Completion done) {
            started->set_value();
            gate.wait_for(5s);
            done({200, "{}"});
          }},
         {"/fast", [](Json, Completion done) { done({200, "{}"}); }}});
  auto work = Submit(origin_, "/work");
  EXPECT_EQ(ready.wait_for(2s), std::future_status::ready);
  auto fast = Submit(origin_, "/fast");
  EXPECT_EQ(fast.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(Health(origin_), 200);
  EXPECT_EQ(work.wait_for(0s), std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(work.get().status, 200);
  EXPECT_EQ(fast.get().status, 200);
}

TEST_F(HttpTest, RejectsInvalidJsonAndKeepsFirstCompletion) {
  Start({{"/once", [](Json, Completion done) {
            done({201, "{}"});
            throw std::runtime_error("late error");
          }}});
  EXPECT_EQ(Post(origin_, "/once", "[").status, 400);
  EXPECT_EQ(Post(origin_, "/once", "[]").status, 400);
  EXPECT_EQ(Post(origin_, "/once", "{}").status, 201);
}

TEST(HttpClientTest, ReportsUnavailableUpstream) {
  auto reply = Post(FreeOrigin(), "/search", "{}");
  EXPECT_EQ(reply.status, 502);
  EXPECT_NE(reply.body.find("error"), std::string::npos);
}

}  // namespace
}  // namespace web3
