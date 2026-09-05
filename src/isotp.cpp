#include "isotp_cpp/isotp.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

#include "isotp.h"

namespace isotp_cpp
{

namespace
{

uint32_t defaultClock()
{
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

ClockSource& clockSource()
{
    static ClockSource clock = ClockSource(&defaultClock);
    return clock;
}

Logger& loggerSink()
{
    static Logger logger = Logger();
    return logger;
}

void emitDebug(const char* message, va_list args)
{
    const Logger& logger = loggerSink();
    if (!logger) {
        return;
    }
    char buf[256];
    std::vsnprintf(buf, sizeof(buf), message, args);
    logger(buf);
}

} // namespace

void setClock(ClockSource clock)
{
    clockSource() = std::move(clock);
}

void setLogger(Logger logger)
{
    loggerSink() = std::move(logger);
}

struct Link::Impl
{
    CanDriver& driver;
    Config config;
    Callbacks callbacks;

    std::vector<uint8_t> sendBuf;
    std::vector<uint8_t> receiveBuf;
    IsoTpLink link;

    bool sendActive = false;
    int lastRxResult = 0;
    bool stopped = false;

    Impl(CanDriver& drv, Config cfg, Callbacks cb)
        : driver(drv), config(cfg), callbacks(std::move(cb))
    {
        std::memset(&link, 0, sizeof(link));
    }

    void notifySendError()
    {
        const int rc = link.send_protocol_result;
        sendActive = false;
        if (callbacks.onError) {
            callbacks.onError("tx", rc);
        }
    }

    void noteRxResult()
    {
        const int rc = link.receive_protocol_result;
        if (rc != lastRxResult) {
            lastRxResult = rc;
            if (rc < 0 && callbacks.onError) {
                callbacks.onError("rx", rc);
            }
        }
    }
};

namespace
{

void txDone(void* /*link*/, uint32_t size, void* arg)
{
    Link::Impl* impl = static_cast<Link::Impl*>(arg);
    if (impl == nullptr) {
        return;
    }
    impl->sendActive = false;
    if (impl->callbacks.onSent) {
        impl->callbacks.onSent(size);
    }
}

void rxDone(void* /*link*/, const uint8_t* data, uint32_t size, void* arg)
{
    Link::Impl* impl = static_cast<Link::Impl*>(arg);
    if (impl == nullptr) {
        return;
    }
    if (impl->callbacks.onReceived) {
        impl->callbacks.onReceived(std::vector<uint8_t>(data, data + size));
    }
}

} // namespace

Result toResult(int rc)
{
    switch (rc) {
        case ISOTP_RET_OK:
            return Result::Ok;
        case ISOTP_RET_ERROR:
            return Result::Error;
        case ISOTP_RET_INPROGRESS:
            return Result::InProgress;
        case ISOTP_RET_OVERFLOW:
            return Result::Overflow;
        case ISOTP_RET_WRONG_SN:
            return Result::WrongSequence;
        case ISOTP_RET_NO_DATA:
            return Result::NoData;
        case ISOTP_RET_TIMEOUT:
            return Result::Timeout;
        case ISOTP_RET_LENGTH:
            return Result::Length;
        case ISOTP_RET_NOSPACE:
            return Result::NoSpace;
        default:
            return Result::Error;
    }
}

Link::Link(CanDriver& driver, Config config, Callbacks callbacks)
    : _impl(new Impl(driver, std::move(config), std::move(callbacks))), _config(_impl->config)
{
}

Link::~Link()
{
    stop();
    delete _impl;
}

bool Link::start(std::string* err)
{
    if (_started) {
        return true;
    }
    Impl& impl = *_impl;
    if (impl.config.sendBufferBytes == 0 || impl.config.receiveBufferBytes == 0 ||
        impl.config.sendBufferBytes > kMaxPayload || impl.config.receiveBufferBytes > kMaxPayload) {
        if (err != nullptr) {
            *err = "buffer sizes must be in [1, 4095]";
        }
        return false;
    }

    impl.sendBuf.assign(impl.config.sendBufferBytes, 0);
    impl.receiveBuf.assign(impl.config.receiveBufferBytes, 0);
    isotp_init_link(&impl.link, impl.config.txId, impl.sendBuf.data(),
                    static_cast<uint32_t>(impl.sendBuf.size()), impl.receiveBuf.data(),
                    static_cast<uint32_t>(impl.receiveBuf.size()));
#ifdef ISO_TP_USER_SEND_CAN_ARG
    impl.link.user_send_can_arg = &impl;
#endif
    isotp_set_tx_done_cb(&impl.link, &txDone, &impl);
    isotp_set_rx_done_cb(&impl.link, &rxDone, &impl);
    impl.sendActive = false;
    impl.lastRxResult = 0;
    impl.stopped = false;
    _started = true;
    return true;
}

void Link::stop()
{
    if (!_started) {
        return;
    }
    Impl& impl = *_impl;
    impl.stopped = true;
    isotp_destroy_link(&impl.link);
    _started = false;
}

void Link::onCanFrame(uint32_t arbitrationId, const uint8_t* data, uint8_t size)
{
    if (!_started || data == nullptr) {
        return;
    }
    Impl& impl = *_impl;
    if (arbitrationId != impl.config.rxId) {
        return; // not ours
    }
    isotp_on_can_message(&impl.link, data, size);
    impl.noteRxResult();
}

void Link::poll()
{
    if (!_started) {
        return;
    }
    Impl& impl = *_impl;
    isotp_poll(&impl.link);

    if (impl.sendActive && impl.link.send_status == ISOTP_SEND_STATUS_ERROR) {
        impl.notifySendError();
    }
    impl.noteRxResult();
}

Result Link::send(const uint8_t* payload, uint32_t size)
{
    if (!_started || payload == nullptr) {
        return Result::Error;
    }
    Impl& impl = *_impl;
    if (impl.link.send_status == ISOTP_SEND_STATUS_INPROGRESS) {
        return Result::InProgress;
    }
    impl.sendActive = true;
    const int rc = isotp_send(&impl.link, payload, size);
    const Result res = toResult(rc);
    if (res != Result::Ok && res != Result::InProgress) {
        impl.sendActive = false;
    }
    return res;
}

Result Link::send(const std::vector<uint8_t>& payload)
{
    return send(payload.data(), static_cast<uint32_t>(payload.size()));
}

bool Link::sendInProgress() const
{
    return _started && _impl->sendActive;
}

int Link::lastTxProtocolResult() const
{
    return (_started && _impl != nullptr) ? _impl->link.send_protocol_result : 0;
}

int Link::lastRxProtocolResult() const
{
    return (_started && _impl != nullptr) ? _impl->link.receive_protocol_result : 0;
}

bool Link::transmitFrame(void* arg, uint32_t arbitrationId, const uint8_t* data, uint8_t size)
{
    if (arg == nullptr || data == nullptr) {
        return false;
    }
    Link::Impl* impl = static_cast<Link::Impl*>(arg);
    if (impl->stopped) {
        return false;
    }
    return impl->driver.sendCan(arbitrationId, data, size);
}

} // namespace isotp_cpp

// ---------------------------------------------------------------------------
// C user hooks required by isotp-c. The send hook routes to the link's
// CanDriver through the per-link argument (ISO_TP_USER_SEND_CAN_ARG).
// ---------------------------------------------------------------------------
extern "C" int isotp_user_send_can(const uint32_t arbitration_id, const uint8_t* data,
                                   const uint8_t size
#ifdef ISO_TP_USER_SEND_CAN_ARG
                                   ,
                                   void* arg
#endif
)
{
#ifdef ISO_TP_USER_SEND_CAN_ARG
    return isotp_cpp::Link::transmitFrame(arg, arbitration_id, data, size) ? ISOTP_RET_OK
                                                                          : ISOTP_RET_ERROR;
#else
    (void)arbitration_id;
    (void)data;
    (void)size;
    return ISOTP_RET_ERROR;
#endif
}

extern "C" uint32_t isotp_user_get_us(void)
{
    using namespace isotp_cpp;
    const ClockSource& clock = clockSource();
    if (!clock) {
        return 0;
    }
    return clock();
}

extern "C" void isotp_user_debug(const char* message, ...)
{
    using namespace isotp_cpp;
    va_list args;
    va_start(args, message);
    emitDebug(message, args);
    va_end(args);
}
