#include "sdrplay_client.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#include "sdrplay_const.hpp"

namespace {

/** rsp_tcp.c's own bandwidth-from-samplerate ladder (see init_rsp_device()). */
int bwTypeForSampleRate(double sr) {
  if (sr < 300e3) return sdrplay_const::kBw0_200;
  if (sr < 600e3) return sdrplay_const::kBw0_300;
  if (sr < 1536e3) return sdrplay_const::kBw0_600;
  if (sr < 5e6) return sdrplay_const::kBw5_000;
  if (sr < 6e6) return sdrplay_const::kBw6_000;
  if (sr < 7e6) return sdrplay_const::kBw7_000;
  if (sr < 8e6) return sdrplay_const::kBw8_000;
  return sdrplay_const::kBw8_000;
}

struct DecimationResult {
  double adcSampleRateHz;
  int decimationFactor;
};

/**
 * The RSP ADC's minimum sample rate is 2 MSPS - lower output rates are
 * achieved via the tuner's own hardware decimation, same as rsp_tcp.c's
 * init_rsp_device(): double the ADC rate and the decimation factor until
 * the ADC rate clears 2 MSPS.
 */
DecimationResult decimationForSampleRate(double sr) {
  double adcSampleRateHz = sr;
  int decimationFactor = 1;
  while (adcSampleRateHz < 2e6) {
    adcSampleRateHz *= 2;
    decimationFactor *= 2;
  }
  return {adcSampleRateHz, decimationFactor};
}

} // namespace

SdrplayApiClient::SdrplayApiClient(const SdrplayConfig& cfg) : cfg_(cfg) {}

SdrplayApiClient::~SdrplayApiClient() {
  try {
    close();
  } catch (...) {
    // destructors must not throw
  }
}

void SdrplayApiClient::connect() {
  checkOrThrow(sdrplay_api_Open(), "Open");
  opened_ = true;

  try {
    checkOrThrow(sdrplay_api_LockDeviceApi(), "LockDeviceApi");

    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int numDevices = 0;
    checkOrThrow(sdrplay_api_GetDevices(devices, &numDevices, SDRPLAY_MAX_DEVICES), "GetDevices");
    if (numDevices == 0) {
      throw std::runtime_error("no RSP devices found");
    }

    int selectedIdx = 0;
    if (cfg_.serialNumber.has_value()) {
      selectedIdx = -1;
      for (unsigned int i = 0; i < numDevices; ++i) {
        if (cfg_.serialNumber.value() == devices[i].SerNo) {
          selectedIdx = static_cast<int>(i);
          break;
        }
      }
      if (selectedIdx < 0) {
        std::string available;
        for (unsigned int i = 0; i < numDevices; ++i) {
          if (i > 0) available += ", ";
          available += devices[i].SerNo;
        }
        throw std::runtime_error("RSP with serial \"" + *cfg_.serialNumber + "\" not found. Available: " +
                                  available);
      }
    }

    device_ = devices[static_cast<unsigned int>(selectedIdx)];

    // Single-tuner mode. On RSPduo, this must be forced explicitly - see
    // rsp_tcp.c's main(), which always runs single-tuner too.
    if (device_.hwVer == sdrplay_const::kDeviceRspDuo) {
      device_.rspDuoMode = static_cast<decltype(device_.rspDuoMode)>(sdrplay_const::kRspDuoModeSingleTuner);
      device_.tuner = static_cast<decltype(device_.tuner)>(sdrplay_const::kTunerA);
    }

    checkOrThrow(sdrplay_api_SelectDevice(&device_), "SelectDevice");
    deviceSelected_ = true;

    checkOrThrow(sdrplay_api_UnlockDeviceApi(), "UnlockDeviceApi");

    sdrplay_api_DeviceParamsT* deviceParams = nullptr;
    checkOrThrow(sdrplay_api_GetDeviceParams(device_.dev, &deviceParams), "GetDeviceParams");
    deviceParams_ = deviceParams;

    configureInitialParams();

    sdrplay_api_CallbackFnsT callbackFns{};
    callbackFns.StreamACbFn = &SdrplayApiClient::streamCallbackTrampoline;
    callbackFns.StreamBCbFn = nullptr;
    callbackFns.EventCbFn = &SdrplayApiClient::eventCallbackTrampoline;

    checkOrThrow(sdrplay_api_Init(device_.dev, &callbackFns, this), "Init");

    std::cout << "[sdrplay] streaming from " << device_.SerNo << " (hwVer " << static_cast<int>(device_.hwVer)
              << "): " << (cfg_.centerFrequencyHz / 1e6) << " MHz, " << cfg_.sampleRateHz << " sps\n";
  } catch (...) {
    // Best-effort cleanup so a failed connect() doesn't leave the API locked/open.
    try {
      close();
    } catch (...) {
      // already in an error path; nothing more useful to do
    }
    throw;
  }
}

void SdrplayApiClient::configureInitialParams() {
  const DecimationResult dec = decimationForSampleRate(cfg_.sampleRateHz);

  sdrplay_api_DevParamsT* devParams = deviceParams_->devParams;
  devParams->ppm = cfg_.ppm;
  devParams->fsFreq.fsHz = dec.adcSampleRateHz;

  sdrplay_api_RxChannelParamsT* rx = deviceParams_->rxChannelA;
  rx->tunerParams.bwType = static_cast<decltype(rx->tunerParams.bwType)>(bwTypeForSampleRate(cfg_.sampleRateHz));
  // Zero-IF: baseband IQ centered at 0 Hz, matching this project's NCO/decimator,
  // which expect a wideband capture centered on centerFrequencyHz with no residual IF.
  rx->tunerParams.ifType = static_cast<decltype(rx->tunerParams.ifType)>(sdrplay_const::kIfZero);
  rx->tunerParams.loMode = static_cast<decltype(rx->tunerParams.loMode)>(sdrplay_const::kLoAuto);
  rx->tunerParams.rfFreq.rfHz = cfg_.centerFrequencyHz;
  rx->tunerParams.gain.gRdB = cfg_.gainReductionDb;
  rx->tunerParams.gain.LNAstate = static_cast<decltype(rx->tunerParams.gain.LNAstate)>(cfg_.lnaState);
  rx->tunerParams.gain.minGr = static_cast<decltype(rx->tunerParams.gain.minGr)>(sdrplay_const::kMinGrNormal);

  rx->ctrlParams.dcOffset.DCenable = 1;
  rx->ctrlParams.dcOffset.IQenable = 1;
  rx->ctrlParams.decimation.enable = dec.decimationFactor > 1 ? 1 : 0;
  rx->ctrlParams.decimation.decimationFactor =
      static_cast<decltype(rx->ctrlParams.decimation.decimationFactor)>(dec.decimationFactor);
  rx->ctrlParams.decimation.wideBandSignal = 1;
  rx->ctrlParams.agc.enable = static_cast<decltype(rx->ctrlParams.agc.enable)>(
      cfg_.agc ? sdrplay_const::kAgcCtrlEn : sdrplay_const::kAgcDisable);
  rx->ctrlParams.agc.setPoint_dBfs = cfg_.agcSetPointDbfs;

  if (cfg_.biasT) applyBiasTField(rx);
  if (cfg_.antenna.has_value()) applyAntennaField(devParams, rx);

  // No decode/mutate/encode dance needed here (unlike the FFI prototype this
  // was ported from) - these are direct writes through the pointers the
  // driver handed back via GetDeviceParams(), applied on the upcoming Init().
}

void SdrplayApiClient::applyBiasTField(sdrplay_api_RxChannelParamsT* rx) {
  switch (device_.hwVer) {
    case sdrplay_const::kDeviceRsp1A:
    case sdrplay_const::kDeviceRsp1B:
      rx->rsp1aTunerParams.biasTEnable = 1;
      break;
    case sdrplay_const::kDeviceRsp2:
      rx->rsp2TunerParams.biasTEnable = 1;
      break;
    case sdrplay_const::kDeviceRspDuo:
      rx->rspDuoTunerParams.biasTEnable = 1;
      break;
    default:
      // RSPdx/RSPdxR2 bias-T lives on devParams->rspDxParams, not rxChannel - see setBiasTee().
      break;
  }
}

void SdrplayApiClient::applyAntennaField(sdrplay_api_DevParamsT* devParams, sdrplay_api_RxChannelParamsT* rx) {
  switch (device_.hwVer) {
    case sdrplay_const::kDeviceRsp2:
      rx->rsp2TunerParams.antennaSel = static_cast<decltype(rx->rsp2TunerParams.antennaSel)>(*cfg_.antenna);
      break;
    case sdrplay_const::kDeviceRspDx:
    case sdrplay_const::kDeviceRspDxR2:
      devParams->rspDxParams.antennaSel =
          static_cast<decltype(devParams->rspDxParams.antennaSel)>(*cfg_.antenna);
      break;
    default:
      std::cerr << "[sdrplay] antenna selection not supported on hwVer " << static_cast<int>(device_.hwVer)
                << "\n";
  }
}

void SdrplayApiClient::streamCallbackTrampoline(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                                                 unsigned int numSamples, unsigned int /*reset*/,
                                                 void* cbContext) {
  (void)params; // available if fs/rf/gr-changed flags become useful later
  auto* self = static_cast<SdrplayApiClient*>(cbContext);
  self->handleStream(xi, xq, numSamples);
}

void SdrplayApiClient::handleStream(const short* xi, const short* xq, unsigned int numSamples) {
  const size_t needed = static_cast<size_t>(numSamples) * 2;
  if (sampleBuf_.size() < needed) sampleBuf_.resize(needed);

  for (unsigned int i = 0; i < numSamples; ++i) {
    sampleBuf_[i * 2] = static_cast<float>(xi[i]) / 32768.0f;
    sampleBuf_[i * 2 + 1] = static_cast<float>(xq[i]) / 32768.0f;
  }
  if (onIq_) onIq_(sampleBuf_.data(), numSamples);
}

void SdrplayApiClient::eventCallbackTrampoline(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT /*tuner*/,
                                                sdrplay_api_EventParamsT* params, void* cbContext) {
  auto* self = static_cast<SdrplayApiClient*>(cbContext);
  self->handleEvent(static_cast<int>(eventId), params);
}

void SdrplayApiClient::handleEvent(int eventId, sdrplay_api_EventParamsT* params) {
  switch (eventId) {
    case sdrplay_const::kEventPowerOverloadChange: {
      const int changeType = static_cast<int>(params->powerOverloadParams.powerOverloadChangeType);
      std::cout << "[sdrplay] overload event: "
                << (changeType == sdrplay_const::kPowerOverloadDetected ? "Detected" : "Corrected") << "\n";
      break;
    }
    case sdrplay_const::kEventDeviceRemoved:
      std::cerr << "[sdrplay] device removed\n";
      break;
    case sdrplay_const::kEventDeviceFailure:
      std::cerr << "[sdrplay] device failure\n";
      break;
    default:
      break;
  }
}

void SdrplayApiClient::retune(double centerFrequencyHz) {
  deviceParams_->rxChannelA->tunerParams.rfFreq.rfHz = centerFrequencyHz;
  update(sdrplay_const::kUpdateTunerFrf);
}

void SdrplayApiClient::setGainReduction(int gRdB) {
  deviceParams_->rxChannelA->tunerParams.gain.gRdB = gRdB;
  update(sdrplay_const::kUpdateTunerGr);
}

void SdrplayApiClient::setLnaState(int lnaState) {
  auto* rx = deviceParams_->rxChannelA;
  rx->tunerParams.gain.LNAstate = static_cast<decltype(rx->tunerParams.gain.LNAstate)>(lnaState);
  update(sdrplay_const::kUpdateTunerGr);
}

void SdrplayApiClient::setAgc(bool enabled, std::optional<int> setPointDbfs) {
  auto* rx = deviceParams_->rxChannelA;
  rx->ctrlParams.agc.enable =
      static_cast<decltype(rx->ctrlParams.agc.enable)>(enabled ? sdrplay_const::kAgcCtrlEn : sdrplay_const::kAgcDisable);
  if (setPointDbfs.has_value()) rx->ctrlParams.agc.setPoint_dBfs = *setPointDbfs;
  update(sdrplay_const::kUpdateCtrlAgc);
}

void SdrplayApiClient::setBiasTee(bool enabled) {
  const auto hwVer = device_.hwVer;

  if (hwVer == sdrplay_const::kDeviceRspDx || hwVer == sdrplay_const::kDeviceRspDxR2) {
    // RSPdx/RSPdxR2 bias-T is a device-level (not per-channel) param.
    deviceParams_->devParams->rspDxParams.biasTEnable = enabled ? 1 : 0;
    update(sdrplay_const::kUpdateNone, sdrplay_const::kUpdateExt1RspDxBiasTControl);
    return;
  }

  auto* rx = deviceParams_->rxChannelA;
  uint32_t reason = sdrplay_const::kUpdateNone;
  switch (hwVer) {
    case sdrplay_const::kDeviceRsp1A:
    case sdrplay_const::kDeviceRsp1B:
      rx->rsp1aTunerParams.biasTEnable = enabled ? 1 : 0;
      reason = sdrplay_const::kUpdateRsp1aBiasTControl;
      break;
    case sdrplay_const::kDeviceRsp2:
      rx->rsp2TunerParams.biasTEnable = enabled ? 1 : 0;
      reason = sdrplay_const::kUpdateRsp2BiasTControl;
      break;
    case sdrplay_const::kDeviceRspDuo:
      rx->rspDuoTunerParams.biasTEnable = enabled ? 1 : 0;
      reason = sdrplay_const::kUpdateRspDuoBiasTControl;
      break;
    default:
      std::cerr << "[sdrplay] bias-T not supported on hwVer " << static_cast<int>(hwVer) << " (RSP1 has none)\n";
      return;
  }
  update(reason, sdrplay_const::kUpdateExt1None);
}

void SdrplayApiClient::update(uint32_t reasonForUpdate, uint32_t reasonForUpdateExt1) {
  if (!deviceSelected_) return;
  const sdrplay_api_ErrT err = sdrplay_api_Update(device_.dev, device_.tuner, reasonForUpdate, reasonForUpdateExt1);
  if (err != sdrplay_const::kErrSuccess) {
    std::cerr << "[sdrplay] Update failed: " << sdrplay_api_GetErrorString(err) << "\n";
  }
}

void SdrplayApiClient::checkOrThrow(sdrplay_api_ErrT err, const char* what) {
  if (err != sdrplay_const::kErrSuccess) {
    throw std::runtime_error(std::string("sdrplay_api_") + what + " failed: " + sdrplay_api_GetErrorString(err) +
                              " (" + std::to_string(static_cast<int>(err)) + ")");
  }
}

void SdrplayApiClient::close() {
  if (!opened_) return;

  if (deviceSelected_ && device_.dev != nullptr) {
    const sdrplay_api_ErrT err = sdrplay_api_Uninit(device_.dev);
    if (err != sdrplay_const::kErrSuccess) {
      std::cerr << "[sdrplay] Uninit failed: " << sdrplay_api_GetErrorString(err) << "\n";
    }
  }

  if (deviceSelected_) {
    const sdrplay_api_ErrT err = sdrplay_api_ReleaseDevice(&device_);
    if (err != sdrplay_const::kErrSuccess) {
      std::cerr << "[sdrplay] ReleaseDevice failed: " << sdrplay_api_GetErrorString(err) << "\n";
    }
    deviceSelected_ = false;
  }

  sdrplay_api_Close();
  opened_ = false;
  deviceParams_ = nullptr;
}
