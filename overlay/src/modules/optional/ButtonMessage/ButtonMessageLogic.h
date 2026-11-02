// ButtonMessage: pure decision logic. No Meshtastic or Arduino includes, so test/test_logic.cpp can run on any host.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bm
{

// Copy message `idx` (0-based) of a '|'-separated list into `out`. A leading '!' marks a critical message
// (resend until acked); it is stripped and reported through *critical. Returns false when there is no such message.
inline bool messageAt(const char *list, unsigned idx, char *out, size_t n, bool *critical)
{
    const char *p = list;
    for (unsigned i = 0; i < idx; i++) {
        p = strchr(p, '|');
        if (!p)
            return false;
        p++;
    }
    const char *end = strchr(p, '|');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    bool crit = len > 0 && p[0] == '!';
    if (crit) {
        p++;
        len--;
    }
    if (len == 0 || n == 0)
        return false;
    if (len >= n)
        len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    if (critical)
        *critical = crit;
    return true;
}

enum class State { IDLE, WAIT, CRITICAL };
enum class Event { ACK_HOME = 0, ACK_RELAY, NAK_PKI, NAK_OTHER };
enum class Action { NONE, SEND, RESEND, STOCK_PING, STOCK_GPS, STOCK_MUTE, TONE_ACK, TONE_FAIL, NODEINFO_FAIL };

struct Logic {
    const uint32_t bootGuardMs, ackTimeoutMs, retryMs, maxAttempts;
    State state = State::IDLE;
    bool critical = false;
    uint32_t attempts = 0, lastSend = 0;

    Logic(uint32_t bootGuard, uint32_t ackTimeout, uint32_t retry, uint32_t maxAtt)
        : bootGuardMs(bootGuard), ackTimeoutMs(ackTimeout), retryMs(retry), maxAttempts(maxAtt)
    {
    }

    // n presses were counted. hasMessage: a message exists at index n-2. Any send replaces whatever was pending.
    Action onClicks(unsigned n, bool hasMessage, bool msgCritical, uint32_t now)
    {
        if (now < bootGuardMs || n < 2)
            return Action::NONE;
        if (!hasMessage)
            return n == 2 ? Action::STOCK_PING : n == 3 ? Action::STOCK_GPS : n == 4 ? Action::STOCK_MUTE : Action::NONE;
        critical = msgCritical;
        attempts = 1;
        lastSend = now;
        state = critical ? State::CRITICAL : State::WAIT;
        return Action::SEND;
    }

    // ponytail: one NAK ends a WAIT even with several favorites; per-target bookkeeping if a second home node matters.
    Action onEvent(Event e, uint32_t)
    {
        if (state == State::IDLE)
            return Action::NONE;
        switch (e) {
        case Event::ACK_HOME:
            state = State::IDLE;
            return Action::TONE_ACK;
        case Event::ACK_RELAY:
            return Action::NONE;
        case Event::NAK_PKI:
            if (state == State::WAIT)
                state = State::IDLE;
            return Action::NODEINFO_FAIL;
        case Event::NAK_OTHER:
            if (state == State::WAIT)
                state = State::IDLE;
            return Action::TONE_FAIL;
        }
        return Action::NONE;
    }

    Action onTick(uint32_t now)
    {
        if (state == State::WAIT && now - lastSend >= ackTimeoutMs) {
            state = State::IDLE;
            return Action::TONE_FAIL;
        }
        if (state == State::CRITICAL && now - lastSend >= retryMs) {
            if (attempts >= maxAttempts) {
                state = State::IDLE;
                return Action::TONE_FAIL;
            }
            attempts++;
            lastSend = now;
            return Action::RESEND;
        }
        return Action::NONE;
    }

    void abort() { state = State::IDLE; }

    // ms until onTick has something to do; UINT32_MAX when idle.
    uint32_t nextWakeMs(uint32_t now) const
    {
        uint32_t period = state == State::WAIT ? ackTimeoutMs : state == State::CRITICAL ? retryMs : 0;
        if (!period)
            return UINT32_MAX;
        uint32_t elapsed = now - lastSend;
        return elapsed >= period ? 0 : period - elapsed;
    }
};

} // namespace bm
