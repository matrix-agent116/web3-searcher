#ifndef WEB3_SEARCHER_SERVICES_COMMON_HTTP_H_
#define WEB3_SEARCHER_SERVICES_COMMON_HTTP_H_

#include <simdjson.h>

#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>

namespace web3 {

class ThreadPool;

using Json = simdjson::dom::element;

struct Reply {
  int status;
  std::string body;
};

using Completion = std::function<void(Reply)>;
// The JSON view lives until the handler returns. A handler may retain the
// completion and invoke it from any thread when its reply is ready.
using Handler = std::function<void(Json, Completion)>;

std::string Quote(std::string_view value);
// Validation happens on the caller's thread. Network replies arrive on the
// client event loop, so callbacks must return promptly.
void PostAsync(const std::string& origin, const std::string& path,
               std::string body, Completion done);
// Blocking convenience for command-line callers.
Reply Post(const std::string& origin, const std::string& path,
           std::string body);
int PrintResponse(const Reply& reply);
// Business routes run on the supplied pool. The standalone overload uses a
// dedicated serial worker. Socket I/O and /health run on the HTTP loop.
void Serve(const std::string& origin,
           const std::map<std::string, Handler>& routes, ThreadPool& pool,
           std::stop_token stop = {});
void Serve(const std::string& origin,
           const std::map<std::string, Handler>& routes,
           std::stop_token stop = {});

}  // namespace web3

#endif  // WEB3_SEARCHER_SERVICES_COMMON_HTTP_H_
