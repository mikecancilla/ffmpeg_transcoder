#pragma once

extern "C"
{
    #include <libavutil/rational.h>
}

enum EFrameRateConversionCode
{
    kNoConversion = 0,
    kNTSCFilm_to_PAL,                       // 23.976p to 25 fps
    kNTSCInverseTelecine_to_PAL,            // 30i inverse telecine to 23.976p, then reclock to 25 fps
    kNTSCFilm_to_NTSCBroadcast,             // 23.976p to 30 fps
    kPAL_to_NTSCFilm,                       // 25 fps to 24p
    kPAL_to_NTSCBroadcast,                  // 25 fps to 24p to 30i
    kNTSCInverseTelecine_to_NTSCFilm,       // 30i inverse telecine to 24p
    kNTSCBroadcast_to_PAL,                  // 30 fps to 25 fps
    kNTSCBroadcast_to_NTSC60p,              // 30 fps to 60p
    kNTSC60pInverseTelecine_to_NTSCFilm,    // 60p inverse telecine to 23.976p
    kNTSC60pInverseTelecine_to_PAL,         // 60p inverse telecine to 23.976p, then reclock to 25 fps
    kNTSC60p_to_PAL,                        // 60p to 25 fps
    kNTSC60p_to_NTSCBroadcast,              // 60p to 30 fps
    kFilm_to_NTSCFilm,                      // 24p to 23.976p fps
    kFilm_to_NTSCBroadcast,                 // 24p to 29.97p fps
    kFilm_to_PAL,                           // 24p to 25 fps
    kPAL50p_to_NTSCBroadcast,               // 50p fps to 29.97 fps
    kPAL50p_to_NTSCFilm,                    // 50p fps to 23.976p fps
    kPAL50p_to_PAL,                         // 50p fps to 25 fps
    kPAL50p_to_NTSC60p,                     // 50p fps to 59.94 fps
    kCustomFrameRateConversion
};

enum EScanType
{
	eUndefinedScanType = -1,
	eProgressive = 0,
	eProgressiveHardTelecine,
	eInterlaced,
	eInterlacedSoftTelecine,
	eInterlacedHardTelecine,
	eInterlacedVariable
};

EFrameRateConversionCode CalculateFrameRateConversion(AVRational srcFrameRate, AVRational dstFrameRate, bool bIsTelecine = false);
AVRational FRtoAVRational(double fr);

bool IsDeinterlacing(EFrameRateConversionCode fr_code);

bool IsInterlaced(EScanType es);
bool IsProgressive(EScanType es);
bool IsTelecine(EScanType es);
