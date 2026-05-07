#pragma once
#include <memory>
#include "Result.hpp"
#include "ITransport.hpp"

namespace networkpp {

class IAcceptor {
public:
    virtual ~IAcceptor() = default;

    virtual Result bind() noexcept = 0;
    virtual Result listen(int backlog = 128) noexcept = 0;
    virtual Result accept(std::unique_ptr<ITransport>& client) noexcept = 0;
    virtual void close() noexcept = 0;
    virtual int fd() noexcept = 0;
};

} // namespace networkpp