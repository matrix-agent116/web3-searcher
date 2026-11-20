#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_ROUTER_ROUTER_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_ROUTER_ROUTER_H_

#include <filesystem>

#include "common/http.h"

namespace web3::searcher {

class Manager;

void LoadRoutes(const std::filesystem::path& config, const Manager& manager,
                HttpRoutes& routes);

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_ROUTER_ROUTER_H_
