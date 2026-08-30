// TASK-007 · CommandSource 名称化（日志 / 审计输出用）。

#include "mmo/core/bus/command.h"

namespace mmo::core {

const char* ToString(CommandSource source) noexcept {
    switch (source) {
        case CommandSource::kInternal:
            return "INTERNAL";
        case CommandSource::kClient:
            return "CLIENT";
        case CommandSource::kRpc:
            return "RPC";
        case CommandSource::kConsole:
            return "CONSOLE";
    }
    return "UNKNOWN";
}

}  // namespace mmo::core
