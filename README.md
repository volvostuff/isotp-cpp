# isotp-cpp

C++ wrapper for the [isotp-c](https://github.com/SimonCahill/isotp-c) ISO-TP
(ISO 15765-2) C library.

One `Link` = one ISO-TP connection over classic CAN. Events, not threads:

```cpp
struct Bus : isotp_cpp::CanDriver {
    bool sendCan(uint32_t id, const uint8_t* data, uint8_t size) override { /* TX */ }
};
Bus bus;
isotp_cpp::setClock([]{ /* us since boot */ });

isotp_cpp::Link::Config cfg;          // txId=0x7E0, rxId=0x7E8 by default
isotp_cpp::Link::Callbacks cb;
cb.onReceived = [](std::vector<uint8_t> payload) { /* whole message */ };
cb.onSent     = [](uint32_t size) { /* transmission done */ };
cb.onError    = [](const char* where, int code) { /* protocol error */ };

isotp_cpp::Link link(bus, cfg, cb);
link.start();
link.send(payload.data(), payload.size());  // async
// From your CAN RX task:
link.onCanFrame(id, data, size);
// From your loop / timer:
link.poll();
```

See `AGENTS.md` for build/test instructions and details.

License: BSD 3-Clause.
