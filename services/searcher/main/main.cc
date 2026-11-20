#include <iostream>
#include <iresearch/utils/cpu_features.hpp>
#include <string_view>

#include "common/http.h"
#include "common/system.h"
#include "common/thread_pool.h"
#include "searcher/manager/manager.h"
#include "searcher/router/internal_routes.h"
#include "searcher/router/router.h"

int main(int argc, char** argv) {
  try {
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (command == "query" && argc == 5 &&
        std::string_view(argv[2]).starts_with('/'))
      return web3::PrintResponse(web3::Post(argv[4], argv[2], argv[3]));
    if (!((command == "build" && argc == 3) ||
          (command == "serve" && (argc == 4 || argc == 5)))) {
      std::cerr << "usage: searcher serve INDEX_DIR URL [ROUTER.conf]\n"
                   "       searcher build INDEX_DIR\n"
                   "       searcher query PATH JSON URL\n";
      return 2;
    }

    irs::CheckCpuFeatures(argv[0]);
    web3::searcher::Manager manager(argv[2]);
    if (command == "build")
      return web3::PrintResponse({200, manager.EnsureTables()});

    auto routes = web3::searcher::InternalRoutes(manager);
    if (argc == 5) web3::searcher::LoadRoutes(argv[4], manager, routes);
    manager.EnsureTables();
    manager.StartMerger();
    web3::ThreadPool pool(web3::CpuCoreCount());
    web3::ServeRoutes(argv[3], routes, pool);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
