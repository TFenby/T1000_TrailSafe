// Host test for ButtonMessageLogic.h: g++ -std=c++17 -I overlay/src/modules/optional/ButtonMessage test/test_logic.cpp
#include "ButtonMessageLogic.h"
#include <cassert>
#include <cstdio>

using namespace bm;

int main()
{
    char out[64];
    bool crit = true;
    const char *list = "Check-in OK|!HELP - need assistance|I am OK";
    assert(messageAt(list, 0, out, sizeof out, &crit) && !strcmp(out, "Check-in OK") && !crit);
    assert(messageAt(list, 1, out, sizeof out, &crit) && !strcmp(out, "HELP - need assistance") && crit);
    assert(messageAt(list, 2, out, sizeof out, &crit) && !strcmp(out, "I am OK") && !crit);
    assert(!messageAt(list, 3, out, sizeof out, &crit));
    assert(!messageAt("", 0, out, sizeof out, &crit));
    assert(!messageAt("a||b", 1, out, sizeof out, &crit));
    assert(messageAt("abcdef", 0, out, 4, &crit) && !strcmp(out, "abc")); // truncation

    const uint32_t GUARD = 10000, ACK = 60000, RETRY = 120000, MAX = 3;
    Logic l(GUARD, ACK, RETRY, MAX);

    // boot guard
    assert(l.onClicks(2, true, false, GUARD - 1) == Action::NONE && l.state == State::IDLE);
    // unconfigured counts keep stock behavior
    assert(l.onClicks(2, false, false, GUARD) == Action::STOCK_PING);
    assert(l.onClicks(3, false, false, GUARD) == Action::STOCK_GPS);
    assert(l.onClicks(4, false, false, GUARD) == Action::STOCK_MUTE);
    assert(l.onClicks(5, false, false, GUARD) == Action::NONE && l.state == State::IDLE);

    // normal message: send, relay ack is silent, home ack ends it
    assert(l.onClicks(2, true, false, 20000) == Action::SEND && l.state == State::WAIT);
    assert(l.onEvent(Event::ACK_RELAY, 20001) == Action::NONE && l.state == State::WAIT);
    assert(l.nextWakeMs(20001) == ACK - 1);
    assert(l.onEvent(Event::ACK_HOME, 20002) == Action::TONE_ACK && l.state == State::IDLE);
    assert(l.nextWakeMs(20002) == UINT32_MAX);

    // normal message: relay-only then timeout fails
    assert(l.onClicks(2, true, false, 30000) == Action::SEND);
    assert(l.onTick(30000 + ACK - 1) == Action::NONE);
    assert(l.onTick(30000 + ACK) == Action::TONE_FAIL && l.state == State::IDLE);

    // normal message: NAK fails immediately; PKI NAK asks for node info
    assert(l.onClicks(2, true, false, 40000) == Action::SEND);
    assert(l.onEvent(Event::NAK_OTHER, 40001) == Action::TONE_FAIL && l.state == State::IDLE);
    assert(l.onClicks(2, true, false, 41000) == Action::SEND);
    assert(l.onEvent(Event::NAK_PKI, 41001) == Action::NODEINFO_FAIL && l.state == State::IDLE);

    // critical: resend at RETRY only, NAKs don't stop it, home ack does
    assert(l.onClicks(3, true, true, 50000) == Action::SEND && l.state == State::CRITICAL && l.attempts == 1);
    assert(l.onTick(50000 + RETRY - 1) == Action::NONE);
    assert(l.onTick(50000 + RETRY) == Action::RESEND && l.attempts == 2);
    assert(l.onEvent(Event::NAK_PKI, 50000 + RETRY + 1) == Action::NODEINFO_FAIL && l.state == State::CRITICAL);
    assert(l.onEvent(Event::NAK_OTHER, 50000 + RETRY + 2) == Action::TONE_FAIL && l.state == State::CRITICAL);
    assert(l.onEvent(Event::ACK_HOME, 50000 + RETRY + 3) == Action::TONE_ACK && l.state == State::IDLE);

    // critical: a new send replaces it
    assert(l.onClicks(3, true, true, 60000) == Action::SEND && l.state == State::CRITICAL);
    assert(l.onClicks(4, true, false, 60500) == Action::SEND && l.state == State::WAIT && !l.critical);
    assert(l.onEvent(Event::ACK_HOME, 60600) == Action::TONE_ACK);

    // critical: max attempts then fail
    uint32_t t = 70000;
    assert(l.onClicks(3, true, true, t) == Action::SEND);
    for (uint32_t i = 1; i < MAX; i++) {
        t += RETRY;
        assert(l.onTick(t) == Action::RESEND && l.attempts == i + 1);
    }
    t += RETRY;
    assert(l.onTick(t) == Action::TONE_FAIL && l.state == State::IDLE);

    // events while idle are ignored
    assert(l.onEvent(Event::ACK_HOME, t) == Action::NONE);

    puts("test_logic: ok");
    return 0;
}
