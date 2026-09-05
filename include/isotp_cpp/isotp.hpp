/*
 * isotp.hpp - C++ wrapper around the isotp-c (ISO 15765-2) C library.
 *
 * One Link represents one ISO-TP connection: messages are sent on `txId`
 * (also used for the flow-control frames we emit while receiving) and frames
 * from the peer arrive on `rxId`.
 *
 * The wrapper is event driven: call send() to start a transmission and feed
 * received CAN frames to onCanFrame(); the caller should invoke poll()
 * periodically (from its own loop or the RX task) so multi-frame sends are
 * paced and timeouts are detected. Whole received messages and transmission
 * completion arrive via the callbacks configured in the constructor.
 *
 * All timing runs through an injectable 32-bit microsecond clock (see
 * setClock()); pass a fake clock in tests.
 */
#ifndef ISOTP_CPP_ISOTP_H
#define ISOTP_CPP_ISOTP_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace isotp_cpp
{

// Mirrors the ISOTP_RET_* codes of the underlying C library.
enum class Result
{
    Ok = 0,
    Error = -1,
    InProgress = -2,
    Overflow = -3,
    WrongSequence = -4,
    NoData = -5,
    Timeout = -6,
    Length = -7,
    NoSpace = -8
};

// Maximum classic ISO-TP payload (12-bit FF_DL).
constexpr uint32_t kMaxPayload = 4095;

// 32-bit monotonic microseconds (natural wraparound is supported).
using ClockSource = std::function<uint32_t()>;

// Optional global clock/log providers. The default clock uses
// std::chrono::steady_clock.
void setClock(ClockSource clock);
using Logger = std::function<void(const char*)>;
void setLogger(Logger logger);

// The CAN transport the link transmits through. Implement this for your bus.
class CanDriver
{
  public:
    virtual ~CanDriver() = default;

    // Transmit one classic CAN frame. Return false if the frame could not be
    // submitted (e.g. driver full / not started).
    virtual bool sendCan(uint32_t arbitrationId, const uint8_t* data, uint8_t size) = 0;
};

// A single ISO-TP connection.
class Link
{
  public:
    struct Config
    {
        uint32_t txId = 0x7E0; // our request / flow-control id
        uint32_t rxId = 0x7E8; // arbitration id of the peer's frames we accept
        uint32_t sendBufferBytes = kMaxPayload;
        uint32_t receiveBufferBytes = kMaxPayload;
    };

    struct Callbacks
    {
        // A whole ISO-TP message has been received.
        std::function<void(std::vector<uint8_t> payload)> onReceived;
        // A transmission completed successfully (payload size).
        std::function<void(uint32_t size)> onSent;
        // A protocol error occurred on the tx ("tx") or rx ("rx") direction.
        std::function<void(const char* where, int protocolResult)> onError;
    };

    Link(CanDriver& driver, Config config, Callbacks callbacks);
    ~Link();

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    bool start(std::string* err = nullptr);
    void stop();
    bool started() const { return _started; }

    // Feed a CAN frame received by the driver. Only frames whose arbitration
    // id matches `rxId` are consumed.
    void onCanFrame(uint32_t arbitrationId, const uint8_t* data, uint8_t size);

    // Advance the ISO-TP state machine (CF pacing, timeouts). Call regularly.
    void poll();

    // Start sending `payload` (0..sendBufferBytes). Single frames are sent
    // immediately; multi-frame transmission continues under poll() and is
    // reported through onSent/onError.
    Result send(const uint8_t* payload, uint32_t size);
    Result send(const std::vector<uint8_t>& payload);

    bool sendInProgress() const;
    uint32_t maxPayload() const { return _config.sendBufferBytes; }

    // Internal: forwards a frame from the C library's user hook to the link's
    // CanDriver (see src/isotp.cpp). Do not call directly.
    static bool transmitFrame(void* impl, uint32_t arbitrationId, const uint8_t* data,
                              uint8_t size);

    // Private implementation (defined in src/isotp.cpp); declared here so the
    // C callback trampolines in that TU can name it.
    struct Impl;

  private:
    Impl* _impl;
    Config _config;
    bool _started = false;
};

} // namespace isotp_cpp

#endif /* ISOTP_CPP_ISOTP_H */
