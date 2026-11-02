#include "ButtonMessage.h"

#ifdef BUTTON_MESSAGE
#include "FSCommon.h"
#include "GPS.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "buzz.h"
#include "input/ButtonThread.h"
#include "mesh-pb-constants.h"
#include "modules/ExternalNotificationModule.h"
#include "modules/NodeInfoModule.h"
#include "sleep.h"

static const char *messagesFile = "/prefs/cannedConf.proto"; // same file and proto as the stock CannedMessageModule

// playTones() and ToneDuration live in buzz.cpp without a header entry; mirrored here verbatim.
struct ToneDuration {
    int frequency_khz;
    int duration_ms;
};
void playTones(const ToneDuration *tone_durations, int size);

// Morse-ish tones: B3 like the stock beep; a 1 Hz note is a rest (nRF52 tone() ignores anything under 20 Hz).
// playTones already waits 1.3x each note, so a 1-unit rest after a dot or dash gives a clear gap.
static const int U = BUTTON_MESSAGE_MORSE_UNIT_MS, B3 = 247, REST = 1;
static const ToneDuration SENT[] = {{B3, U}, {REST, U}, {B3, U}};                                // ..
static const ToneDuration SENT_CRITICAL[] = {{B3, 3 * U}, {REST, U}, {B3, 3 * U}};               // --
static const ToneDuration ACKED[] = {{B3, 3 * U}, {REST, U}, {B3, 3 * U}, {REST, U}, {B3, U}};   // --. (G, good)
static const ToneDuration FAILED[] = {{B3, 7 * U}, {REST, 4 * U}, {B3, 7 * U}}; // two 7-unit tones, ~7 units apart
#define PLAY(m) playTones(m, sizeof(m) / sizeof(m[0]))

extern ButtonThread *UserButtonThread; // defined in input/InputBroker.cpp; no header declares it
ButtonMessageModule *buttonMessageModule;

void setupButtonMessage()
{
    buttonMessageModule = new ButtonMessageModule();
}

ButtonMessageModule::ButtonMessageModule()
    : SinglePortModule("buttonmsg", meshtastic_PortNum_TEXT_MESSAGE_APP), OSThread("ButtonMessage"),
      logic(BUTTON_MESSAGE_BOOT_GUARD_MS, BUTTON_MESSAGE_ACK_TIMEOUT_MS, BUTTON_MESSAGE_RETRY_MS, BUTTON_MESSAGE_MAX_ATTEMPTS)
{
    loadMessages();
    preflightSleepObserver.observe(&preflightSleep);
    if (moduleConfig.canned_message.enabled) {
        LOG_INFO("ButtonMessage enabled: '%s'", messages.messages);
    } else {
        LOG_INFO("ButtonMessage disabled (canned_message.enabled is false)");
        disable();
    }
}

void ButtonMessageModule::loadMessages()
{
    if (nodeDB->loadProto(messagesFile, meshtastic_CannedMessageModuleConfig_size, sizeof(messages),
                          &meshtastic_CannedMessageModuleConfig_msg, &messages) != LoadFileResult::LOAD_SUCCESS)
        messages.messages[0] = 0; // no list: every press count keeps its stock behavior
}

bool ButtonMessageModule::saveMessages()
{
#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif
    return nodeDB->saveProto(messagesFile, meshtastic_CannedMessageModuleConfig_size, &meshtastic_CannedMessageModuleConfig_msg,
                             &messages);
}

// --- button ---------------------------------------------------------------------------------------------------------

// Both callbacks can run from the button pin ISR: store the count and wake the thread, nothing else.
void ButtonMessageModule::onDoubleClick(void *self)
{
    auto *m = static_cast<ButtonMessageModule *>(self);
    m->pendingClicks = 2;
    m->setIntervalFromNow(0);
}

void ButtonMessageModule::onMultiClick(void *self)
{
    auto *m = static_cast<ButtonMessageModule *>(self);
    int n = UserButtonThread->userButton.getNumberClicks();
    m->pendingClicks = n > 255 ? 255 : (uint8_t)n;
    m->setIntervalFromNow(0);
}

// Takes over the double/multi-click slots ButtonThread::initButton attached (the parameterized ones; the plain slots
// stay empty, and both would fire). Single press and long press stay stock. Idempotent, so it runs every pass.
void ButtonMessageModule::attachButton()
{
    if (!UserButtonThread)
        return;
    UserButtonThread->userButton.attachDoubleClick(onDoubleClick, this);
    UserButtonThread->userButton.attachMultiClick(onMultiClick, this);
    UserButtonThread->userButton.setClickMs(BUTTON_MESSAGE_CLICK_MS);
}

// --- thread ---------------------------------------------------------------------------------------------------------

int32_t ButtonMessageModule::runOnce()
{
    if (!moduleConfig.canned_message.enabled)
        return disable();
    attachButton();
    uint32_t now = millis();

    uint8_t clicks = pendingClicks;
    pendingClicks = 0;
    if (clicks) {
        bool crit = false;
        char text[sizeof(current)];
        bool has = clicks >= 2 && bm::messageAt(messages.messages, clicks - 2, text, sizeof(text), &crit);
        LOG_INFO("ButtonMessage: %u presses -> %s", clicks, has ? text : "stock action");
        if (has)
            strcpy(current, text);
        perform(logic.onClicks(clicks, has, crit, now));
    }

    uint8_t events = pendingEvents;
    pendingEvents = 0;
    for (int b = 0; b < 4; b++)
        if (events & (1 << b))
            perform(logic.onEvent((bm::Event)b, now));

    perform(logic.onTick(now));

    uint32_t wake = logic.nextWakeMs(now);
    return wake > 60000 ? 60000 : (int32_t)wake;
}

void ButtonMessageModule::perform(bm::Action a)
{
    switch (a) {
    case bm::Action::NONE:
        break;
    case bm::Action::SEND:
    case bm::Action::RESEND:
#if !MESHTASTIC_EXCLUDE_GPS
        if (logic.critical && gps && config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_DISABLED)
            gps->toggleGpsMode(); // a critical message should be able to carry a position
#endif
        if (sendToFavorites(current) == 0) {
            LOG_WARN("ButtonMessage: no favorite node to send to");
            logic.abort();
            PLAY(FAILED);
            break;
        }
        if (logic.critical)
            PLAY(SENT_CRITICAL);
        else
            PLAY(SENT);
        service->refreshLocalMeshNode();
        service->trySendPosition(NODENUM_BROADCAST, false); // stock position broadcast on the configured position channel
        break;
    case bm::Action::STOCK_PING: // what the stock double press does (ButtonThread + SystemCommandsModule)
        playComboTune();
        service->refreshLocalMeshNode();
        service->trySendPosition(NODENUM_BROADCAST, true);
        break;
    case bm::Action::STOCK_GPS: // what the stock triple press does
        playComboTune();
#if !MESHTASTIC_EXCLUDE_GPS
        if (gps)
            gps->toggleGpsMode();
#endif
        break;
    case bm::Action::STOCK_MUTE: // what the stock quadruple press does
        if (moduleConfig.external_notification.enabled && externalNotificationModule) {
            externalNotificationModule->setMute(!externalNotificationModule->getMute());
            if (externalNotificationModule->getMute())
                play4ClickDown();
            else
                play4ClickUp();
        }
        break;
    case bm::Action::TONE_ACK:
        nPending = 0;
        PLAY(ACKED);
        break;
    case bm::Action::NODEINFO_FAIL:
        if (nodeInfoModule && nakNode)
            nodeInfoModule->sendOurNodeInfo(nakNode, true); // starts the key exchange so the next attempt can work
        [[fallthrough]];
    case bm::Action::TONE_FAIL:
        if (logic.state == bm::State::IDLE)
            nPending = 0;
        PLAY(FAILED);
        break;
    }
}

uint8_t ButtonMessageModule::sendToFavorites(const char *text)
{
    uint8_t body[meshtastic_Constants_DATA_PAYLOAD_LEN];
    size_t len = 0;
    if (moduleConfig.canned_message.send_bell)
        body[len++] = 7; // BEL: the receiver's External Notification bell alert
    size_t n = strlen(text);
    if (n > sizeof(body) - len)
        n = sizeof(body) - len;
    memcpy(body + len, text, n);
    len += n;

    nPending = 0;
    NodeNum me = nodeDB->getNodeNum();
    for (size_t i = 0; i < nodeDB->getNumMeshNodes() && nPending < BUTTON_MESSAGE_MAX_TARGETS; i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!nodeInfoLiteIsFavorite(node) || node->num == me)
            continue;
        meshtastic_MeshPacket *p = allocDataPacket();
        if (!p)
            break;
        p->to = node->num;
        p->want_ack = true;
        p->decoded.payload.size = len;
        memcpy(p->decoded.payload.bytes, body, len);
        pending[nPending++] = {p->id, node->num}; // before sending: a missing key NAKs synchronously inside sendToMesh
        LOG_INFO("ButtonMessage: DM to 0x%08x id %u", node->num, p->id);
        service->sendToMesh(p, RX_SRC_LOCAL, true);
    }
    return nPending;
}

// --- acks -----------------------------------------------------------------------------------------------------------

bool ButtonMessageModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return nPending && p->decoded.portnum == meshtastic_PortNum_ROUTING_APP;
}

// Only latches an event; tones and sends happen in runOnce. A NAK for a missing key arrives while sendToMesh is still
// on the stack, and the tones block.
ProcessMessage ButtonMessageModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (!isToUs(&mp))
        return ProcessMessage::CONTINUE;
    uint8_t i = 0;
    while (i < nPending && pending[i].id != mp.decoded.request_id)
        i++;
    if (i == nPending)
        return ProcessMessage::CONTINUE;

    meshtastic_Routing r = meshtastic_Routing_init_default;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Routing_msg, &r))
        return ProcessMessage::CONTINUE;

    bm::Event e;
    if (r.which_variant == meshtastic_Routing_error_reason_tag && r.error_reason != meshtastic_Routing_Error_NONE) {
        e = r.error_reason == meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY ? bm::Event::NAK_PKI : bm::Event::NAK_OTHER;
        nakNode = pending[i].dest;
        LOG_WARN("ButtonMessage: NAK %d for id %u", r.error_reason, mp.decoded.request_id);
    } else {
        e = getFrom(&mp) == pending[i].dest ? bm::Event::ACK_HOME : bm::Event::ACK_RELAY;
        LOG_INFO("ButtonMessage: %s ack for id %u", e == bm::Event::ACK_HOME ? "home" : "relay", mp.decoded.request_id);
    }
    pendingEvents |= 1 << (int)e;
    setIntervalFromNow(0);
    return ProcessMessage::CONTINUE; // RoutingModule must still see it to stop the radio's own retransmits
}

// --- configuration --------------------------------------------------------------------------------------------------

// The stock canned-message admin messages (phone app editor, `meshtastic --set-canned-message`), handled here because
// the stock CannedMessageModule is not built for screenless devices.
AdminMessageHandleResult ButtonMessageModule::handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                          meshtastic_AdminMessage *request,
                                                                          meshtastic_AdminMessage *response)
{
    switch (request->which_payload_variant) {
    case meshtastic_AdminMessage_get_canned_message_module_messages_request_tag:
        if (mp.decoded.want_response) {
            response->which_payload_variant = meshtastic_AdminMessage_get_canned_message_module_messages_response_tag;
            strncpy(response->get_canned_message_module_messages_response, messages.messages,
                    sizeof(response->get_canned_message_module_messages_response));
        }
        return AdminMessageHandleResult::HANDLED_WITH_RESPONSE;

    case meshtastic_AdminMessage_set_canned_message_module_messages_tag: {
        const char *in = request->set_canned_message_module_messages;
        if (*in && strcmp(messages.messages, in) != 0) {
            strncpy(messages.messages, in, sizeof(messages.messages));
            messages.messages[sizeof(messages.messages) - 1] = 0;
            saveMessages();
            LOG_INFO("ButtonMessage: messages set to '%s'", messages.messages);
        }
        // As the stock module does, a new list turns the module on for this session. Save canned_message.enabled
        // (app toggle, or `meshtastic --set canned_message.enabled true`) to keep it on across reboots.
        if (*in && !moduleConfig.canned_message.enabled) {
            moduleConfig.canned_message.enabled = true;
            enabled = true;
            setIntervalFromNow(0);
        }
        return AdminMessageHandleResult::HANDLED;
    }

    default:
        return AdminMessageHandleResult::NOT_HANDLED;
    }
}

#else
void setupButtonMessage() {}
#endif
