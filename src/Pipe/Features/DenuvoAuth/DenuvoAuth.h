#pragma once

#include "Pipe/PipeTypes.h"

namespace PipeManager::DenuvoAuth {

    // Per-handshake entry point: runs the one-time Denuvo detection (cached per
    // process) and advances the authorization state machine.
    void Apply(const PipeContext& ctx);

    // True only while the pipe is the selected authorization pipe and Denuvo has
    // not reached the end-authorization handshake.
    bool IsAuthorizedPipe(const CPipeClient* pipe);

    // 票据请求延长租约（Scheme 1 +300ms / Scheme 2 +3000ms）。
    // GetAppOwnershipTicketExtendedData 的 handler 里调，盖住 Denuvo 的 memcmp 交叉验证。
    void OnOwnershipTicketRequested(const CPipeClient* pipe);

} // namespace PipeManager::DenuvoAuth
