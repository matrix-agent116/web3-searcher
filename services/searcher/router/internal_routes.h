#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_ROUTER_INTERNAL_ROUTES_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_ROUTER_INTERNAL_ROUTES_H_

#include "common/http.h"

namespace web3::searcher {

class Manager;

HttpRoutes InternalRoutes(Manager& manager);

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_ROUTER_INTERNAL_ROUTES_H_
