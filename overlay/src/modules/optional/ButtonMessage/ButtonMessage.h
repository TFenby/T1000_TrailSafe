#pragma once

// Called from Modules.cpp through upstream's optional-modules hook (bin/optional-modules.py) in every env.
void setupButtonMessage();

#ifdef BUTTON_MESSAGE
#include "ButtonMessageLogic.h"
#include "Observer.h"
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/cannedmessages.pb.h"

#ifndef BUTTON_MESSAGE_RETRY_MS
#define BUTTON_MESSAGE_RETRY_MS 120000 // critical ('!') messages are resent this often until a favorite acks
#endif
#ifndef BUTTON_MESSAGE_MAX_ATTEMPTS
#define BUTTON_MESSAGE_MAX_ATTEMPTS 60
#endif
#ifndef BUTTON_MESSAGE_ACK_TIMEOUT_MS
#define BUTTON_MESSAGE_ACK_TIMEOUT_MS 60000 // normal messages: fail tone when no favorite has acked by then
#endif
#ifndef BUTTON_MESSAGE_BOOT_GUARD_MS
#define BUTTON_MESSAGE_BOOT_GUARD_MS 10000 // ignore presses this long after boot (the power-on press); 0 after a button wake
#endif
#ifndef BUTTON_MESSAGE_MAX_TARGETS
#define BUTTON_MESSAGE_MAX_TARGETS 4
#endif
#ifndef BUTTON_MESSAGE_CLICK_MS
#define BUTTON_MESSAGE_CLICK_MS 250 // max gap between presses of one gesture (stock value); raise if multi-press is fiddly
#endif
#ifndef BUTTON_MESSAGE_MORSE_UNIT_MS
#define BUTTON_MESSAGE_MORSE_UNIT_MS 80 // one morse unit: dot length, gap between elements
#endif

/**
 * Canned messages for screenless devices. N presses (N >= 2) sends canned message N-1 as a direct message to every
 * favorite node and confirms delivery with the buzzer. A message starting with '!' is resent until a favorite acks.
 * Press counts without a message keep the stock behavior. Configured through the stock canned-message settings:
 * canned_message.enabled, canned_message.send_bell and the canned message list.
 */
class ButtonMessageModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    ButtonMessageModule();
    AdminMessageHandleResult handleAdminMessageForModule(const meshtastic_MeshPacket &mp, meshtastic_AdminMessage *request,
                                                         meshtastic_AdminMessage *response) override;

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    static void onDoubleClick(void *self);
    static void onMultiClick(void *self);
    void attachButton();
    void perform(bm::Action a);
    uint8_t sendToFavorites(const char *text);
    void loadMessages();
    bool saveMessages();
    // Power saving: hold off deep sleep (radio off) while a delivery is unresolved
    int preflightSleepCb(void *) { return logic.state != bm::State::IDLE; }

    bm::Logic logic;
    CallbackObserver<ButtonMessageModule, void *> preflightSleepObserver =
        CallbackObserver<ButtonMessageModule, void *>(this, &ButtonMessageModule::preflightSleepCb);
    meshtastic_CannedMessageModuleConfig messages = meshtastic_CannedMessageModuleConfig_init_zero;
    char current[sizeof(messages.messages)] = {0}; // text of the message in flight, for resends
    volatile uint8_t pendingClicks = 0;             // set by the button callbacks, possibly in ISR context
    bool readyTone;                                 // button wake: play '.' once the button is attached
    volatile uint8_t pendingEvents = 0;             // one bit per bm::Event, set by handleReceived
    NodeNum nakNode = 0;
    struct {
        PacketId id;
        NodeNum dest;
    } pending[BUTTON_MESSAGE_MAX_TARGETS];
    uint8_t nPending = 0;
};

extern ButtonMessageModule *buttonMessageModule;
#endif
