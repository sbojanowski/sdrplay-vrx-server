#pragma once

#include <cstdint>

/**
 * Numeric constants mirroring sdrplay_api's enums (SDRplay "Software
 * Defined Radio API" spec v3.15). Declared as our own named constants
 * instead of depending on the exact spelling of the vendor header's enum
 * members: these are the same values the project's original Node/koffi FFI
 * implementation used successfully against real RSPdx hardware, so getting
 * the *value* right (already proven) matters more here than guessing the
 * vendor's exact identifier spelling. See sdrplay_client.cpp for where
 * these get cast to whatever type sdrplay_api.h actually declares each
 * field/parameter as.
 */
namespace sdrplay_const {

constexpr uint8_t kDeviceRsp1 = 1;
constexpr uint8_t kDeviceRsp1A = 255;
constexpr uint8_t kDeviceRsp2 = 2;
constexpr uint8_t kDeviceRspDuo = 3;
constexpr uint8_t kDeviceRspDx = 4;
constexpr uint8_t kDeviceRsp1B = 6;
constexpr uint8_t kDeviceRspDxR2 = 7;

constexpr int kErrSuccess = 0;

// sdrplay_api_ReasonForUpdateT (bitmask)
constexpr uint32_t kUpdateNone = 0x00000000;
constexpr uint32_t kUpdateRsp1aBiasTControl = 0x00000010;
constexpr uint32_t kUpdateRsp2BiasTControl = 0x00000080;
constexpr uint32_t kUpdateRsp2AntennaControl = 0x00000200;
constexpr uint32_t kUpdateTunerGr = 0x00008000;
constexpr uint32_t kUpdateTunerFrf = 0x00020000;
constexpr uint32_t kUpdateCtrlAgc = 0x01000000;
constexpr uint32_t kUpdateRspDuoBiasTControl = 0x08000000;

// sdrplay_api_ReasonForUpdateExtension1T (bitmask)
constexpr uint32_t kUpdateExt1None = 0x00000000;
constexpr uint32_t kUpdateExt1RspDxBiasTControl = 0x00000002;
constexpr uint32_t kUpdateExt1RspDxAntennaControl = 0x00000004;

// sdrplay_api_Bw_MHzT
constexpr int kBw0_200 = 200;
constexpr int kBw0_300 = 300;
constexpr int kBw0_600 = 600;
constexpr int kBw1_536 = 1536;
constexpr int kBw5_000 = 5000;
constexpr int kBw6_000 = 6000;
constexpr int kBw7_000 = 7000;
constexpr int kBw8_000 = 8000;

// sdrplay_api_If_kHzT
constexpr int kIfZero = 0;

// sdrplay_api_LoModeT
constexpr int kLoAuto = 1;

// sdrplay_api_MinGainReductionT
constexpr int kMinGrNormal = 20;

// sdrplay_api_TunerSelectT
constexpr int kTunerA = 1;

// sdrplay_api_RspDuoModeT
constexpr int kRspDuoModeSingleTuner = 1;

// sdrplay_api_AgcControlT
constexpr int kAgcDisable = 0;
constexpr int kAgcCtrlEn = 4;

// sdrplay_api_Rsp2_AntennaSelectT
constexpr int kRsp2AntennaA = 5;
constexpr int kRsp2AntennaB = 6;

// sdrplay_api_RspDx_AntennaSelectT
constexpr int kRspDxAntennaA = 0;
constexpr int kRspDxAntennaB = 1;
constexpr int kRspDxAntennaC = 2;

// sdrplay_api_EventT
constexpr int kEventGainChange = 0;
constexpr int kEventPowerOverloadChange = 1;
constexpr int kEventDeviceRemoved = 2;
constexpr int kEventRspDuoModeChange = 3;
constexpr int kEventDeviceFailure = 4;

// sdrplay_api_PowerOverloadCbEventIdT
constexpr int kPowerOverloadDetected = 0;
constexpr int kPowerOverloadCorrected = 1;

} // namespace sdrplay_const
