# sdrplay-vrx-server (C++ / Raspberry Pi)

Backend service that drives an SDRplay RSP directly through the native
**SDRplay API**, takes its wideband IQ capture, and channelizes it into
independently tunable **virtual receivers**, each exposed as its own
`rtl_tcp`-compatible TCP server *and*, optionally, its own **SpyServer**
TCP server (8-bit IQ). Any librtlsdr-based client (SDR++, AIS-catcher,
GQRX, ...) connects to a VRX's rtl_tcp port exactly as it would to a real
RTL-SDR dongle, and any SpyServer client (SDR++, SDR#, CubicSDR, ...) can
connect to its SpyServer port instead/as well - both protocols are fed by
the same VRX, so a client on either one can retune within the wideband
capture's span; unlike a plain `rtlmux` relay, retuning here is real (it
moves the VRX's internal NCO), so a client's frequency assumptions are
actually correct.

This is a from-scratch C++ port of an earlier Node/TypeScript prototype
(kept for reference in `../legacy-ts/`), written for continuous unattended
operation on a Raspberry Pi.

## Structure

```
cpp/
  config.yaml                Wideband capture + VRX definitions (edit this)
  src/
    config.hpp/.cpp           YAML config loader
    sdrplay_const.hpp         sdrplay_api enum values as named constants
    sdrplay_client.hpp/.cpp   Owns the RSP device end-to-end via the native API
    dsp/nco.hpp               Digital downconversion mixer (frequency shift)
    dsp/decimator.hpp/.cpp    Windowed-sinc FIR + integer decimation
    dsp/virtual_receiver.hpp/.cpp   Wires NCO + decimator into one VRX
    rtltcp/protocol.hpp/.cpp  rtl_tcp wire format constants/helpers
    rtltcp/server.hpp/.cpp    rtl_tcp-compatible TCP server per VRX (POSIX sockets)
    spyserver/protocol.hpp    SpyServer wire format structs/enums/constants
    spyserver/server.hpp/.cpp SpyServer-protocol TCP server per VRX (optional, alongside rtl_tcp)
    main.cpp                  Wires it all together
```

## Building

Requires:

- A C++17 compiler and CMake >= 3.16
- **The official SDRplay API SDK** installed (https://www.sdrplay.com/api/ -
  Raspberry Pi/ARM builds are available from SDRplay directly). This
  provides `libsdrplay_api.so` and `sdrplay_api.h` (plus the `sdrplay_api_*.h`
  headers it includes), and the `sdrplay_apiService` background
  daemon that must be running - same requirement as the original prototype.
- `libyaml-cpp-dev` (`sudo apt install libyaml-cpp-dev` on Raspberry Pi OS)

```bash
sudo apt install cmake libyaml-cpp-dev build-essential
# install the SDRplay API SDK per SDRplay's instructions, then:

cd cpp
cmake -B build -S .
# if the SDK isn't in /usr/local/{include,lib}, point CMake at it:
#   cmake -B build -S . -DSDRPLAY_API_DIR=/path/to/sdk
cmake --build build -j4

./build/sdrplay-vrx-server config.yaml
```

Run it as a systemd service for unattended operation on the Pi (not
included here - a plain `ExecStart=/path/to/sdrplay-vrx-server
/path/to/config.yaml` unit with `Restart=on-failure` is enough for a
starter setup).

## Config (`config.yaml`)

See the annotated example in `config.yaml`. Fields map directly to the
original TS version's `SdrplayConfig`/`VirtualReceiverConfig`:

- `serial_number` - pick a specific RSP if more than one is connected.
- `center_frequency_hz` / `sample_rate_hz` - wideband capture; sample rates
  below the RSP ADC's 2 MSPS floor are handled via hardware decimation
  automatically (mirrors `rsp_tcp.c`'s own logic).
- `gain_reduction_db` / `lna_state` - manual gain (`sdrplay_api_GainT`).
- `agc` / `agc_set_point_dbfs` - use the RSP's AGC instead.
- `ppm`, `bias_t` - frequency correction and bias-tee (model-dependent).
- `antenna` - RSP2 (A=5, B=6) / RSPdx & RSPdxR2 (A=0, B=1, C=2). Omit on
  single-antenna models.
- `virtual_receivers[].sample_rate_hz` must evenly divide
  `sdrplay.sample_rate_hz` (integer decimation only - see
  `dsp/decimator.hpp`).
- `virtual_receivers[].spyserver_port` - optional; starts a SpyServer
  endpoint for that VRX (streaming its IQ as 8-bit unsigned samples,
  identical format to the rtl_tcp side) alongside its required `tcp_port`.
  Omit/`0` to skip it - the rtl_tcp endpoint alone is unaffected either way.

## Known risk: `sdrplay_client.cpp` is not verified against a real build

This file `#include <sdrplay_api.h>` and writes directly through the
pointers `sdrplay_api_GetDeviceParams()` hands back - no manual struct
decode/encode dance like the old FFI prototype needed, since C++ can
dereference native pointers the way C does. That also means, unlike the FFI
version, most struct-layout mistakes here are **compile errors**, not
silent memory corruption - a real improvement in safety.

What's *not* independently verified: the real vendor header's exact type
names for a few enum-typed function/struct parameters (e.g. whether
`sdrplay_api_Update()`'s `reasonForUpdate` parameter is a plain `unsigned
int` or a specific enum type). This project's own local syntax/link check
used a hand-written stub header (not the real SDK) built from this
project's prior working Node/koffi FFI bindings
(`../legacy-ts/src/sdrplay/api.ts`, which those field names/order/numeric
enum values were themselves transcribed from the official "Software
Defined Radio API" spec v3.15 and validated against real RSPdx hardware).
If the installed SDK disagrees on an exact parameter type, the compiler
will point at the exact line - add a `static_cast<>` to the type the
compiler names. Before trusting this against real hardware:

- Confirm your installed `sdrplay_api.h`'s version - if it's not v3.15,
  diff the "API Data Types" section of its spec PDF against
  `sdrplay_const.hpp`'s comments for any reordered/added/removed fields or
  changed enum values.
- Start with logging and a low-risk path (e.g. just `sdrplay_api_Open()` +
  `sdrplay_api_GetDevices()` + `sdrplay_api_GetErrorString()` on any error)
  before relying on the full streaming path.

## Known risk: `spyserver/` is not verified against a real SpyServer client

The SpyServer wire protocol has no public server-side reference (the real
Airspy `spyserver` binary is closed-source) and no formal spec document.
This was implemented against the protocol's structs/enums as vendored into
SDR++'s open-source client (`spyserver_protocol.h`, written by the
protocol's original author) and that same client's parsing logic
(`spyserver_client.cpp`) - i.e. against what a real, widely-deployed client
actually expects on the wire, not against the original server's own
(unavailable) implementation. `spyserver/protocol.hpp` flags the couple of
specific fields that aren't nailed down by any public source (mainly
`MessageHeader::ProtocolID`'s exact expected value, which SDR++'s client
never actually validates). Before trusting this against a real client:

- Test against at least one real SpyServer client (SDR++ is the one this was
  built against) connecting over the network, not just localhost - retune,
  toggle streaming on/off, and disconnect/reconnect a few times.
- If a different client (SDR#, CubicSDR, gqrx) fails to connect where SDR++
  works, capture its traffic and diff against what's implemented here -
  same approach as this project's own sdrconnect protocol investigation.

## Known simplifications (see comments in code)

- **Integer decimation only.** A VRX's sample rate must evenly divide the
  wideband capture's sample rate (`config.cpp` enforces this at startup).
  Arbitrary output rates need a rational resampler (interpolate by L,
  decimate by M) instead of `dsp/decimator.hpp`'s plain decimator.
- **Fixed decimation factor at startup.** A client's `SET_SAMPLE_RATE`
  rtl_tcp command is logged but not acted on. Retuning *frequency* works
  live; changing a VRX's *bandwidth* live would need the decimator/filter
  rebuilt on the fly.
- **Backpressure drops rather than blocks.** `RtlTcpServer` writes to each
  client with `MSG_DONTWAIT`; a slow/stalled client has its chunk dropped
  for that round rather than blocking the shared SDRplay streaming thread
  or growing memory without bound.
- **Gain/AGC/bias-tee/direct-sampling/offset-tuning rtl_tcp commands are
  accepted and ignored.** There's no physical tuner behind a VRX - if you
  want per-VRX gain requests to do something, they'd need to be forwarded
  via `SdrplayApiClient::setGainReduction()`/`setLnaState()`/`setAgc()`/
  `setBiasTee()`, which affect the *whole* wideband capture, not just one
  VRX.
- **SpyServer only ever streams IQ, always as 8-bit unsigned.** AF
  (demodulated audio) and FFT (spectrum display) stream types aren't
  implemented - `DeviceInfo.ForcedIqFormat` tells well-behaved clients not to
  bother asking for a different IQ format, and settings for the unsupported
  stream types are accepted-and-ignored the same way out-of-scope rtl_tcp
  commands are.
- **A VRX's SpyServer and rtl_tcp endpoints share one NCO/decimator.**
  Retuning from either protocol (rtl_tcp `SET_FREQUENCY` or SpyServer's
  `IQ_FREQUENCY` setting) moves the same shared VRX, so it retunes *every*
  client connected to that VRX on either endpoint - same tradeoff the
  rtl_tcp side already had with multiple simultaneous rtl_tcp clients, just
  now spanning both protocols too. Also, a SpyServer client's `ClientSync`
  frequency fields are a snapshot sent once at connect time - a later retune
  from elsewhere isn't pushed to already-connected SpyServer clients, so
  their own UI may show a stale center frequency even though the stream
  itself did retune.
