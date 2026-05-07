#pragma once

#include <vector>

#include "Result.hpp"
#include "Endpoint.hpp"

namespace networkpp {

class IResolver {
public:
    virtual ~IResolver() noexcept {}

    virtual Result resolve() noexcept = 0;
    virtual std::vector<Endpoint>& endpoints() noexcept = 0;
};

} // namespace networkpp