#pragma once

#include <functional>
#include <utility>

namespace neoshared::erf {

// Installed for one synchronous archive operation on the caller's worker.
// The callback may throw the caller's cancellation exception. Neither the
// callback nor its captures are retained by an archive after this scope ends.
inline thread_local std::function<void()> operationCheckpoint;

class OperationScope final {
public:
    explicit OperationScope(std::function<void()> callback)
        : previous_(std::move(operationCheckpoint)) {
        operationCheckpoint = std::move(callback);
    }
    ~OperationScope() { operationCheckpoint = std::move(previous_); }
    OperationScope(const OperationScope&) = delete;
    OperationScope& operator=(const OperationScope&) = delete;
private:
    std::function<void()> previous_;
};

inline void checkOperation() {
    if (operationCheckpoint) operationCheckpoint();
}

} // namespace neoshared::erf
