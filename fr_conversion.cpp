#include <string>
#include "fr_conversion.h"

bool IsInterlaced(EScanType es) { return es == eInterlaced || es == eInterlacedHardTelecine || es == eInterlacedVariable; }
bool IsProgressive(EScanType es) { return es == eProgressive || es == eProgressiveHardTelecine || es == eInterlacedHardTelecine; }
bool IsTelecine(EScanType es) { return es == eInterlacedHardTelecine || es == eProgressiveHardTelecine; }

EFrameRateConversionCode CalculateFrameRateConversion(AVRational srcFrameRate, AVRational dstFrameRate, bool bIsTelecine)
{
//    pipeElementConfig^ pParams = GetCurTranscodeParams();
//    double DestinationFrameRate = pParams->VideoEncoderSettings->FrameRate;
    EFrameRateConversionCode frameRateConversion = EFrameRateConversionCode::kNoConversion;

//    bool bDropFrameOutput = !IsProgressiveOutput();

//    if (DestinationFrameRate == 0)
//        return frameRateConversion;

    float src = (float) srcFrameRate.num / (float) srcFrameRate.den;
    float dst = (float) dstFrameRate.num / (float) dstFrameRate.den;

    int iSrcFrameRate = (int)(0.5 + (1000.0 * src));
    int iDstFrameRate = (int)(0.5 + (1000.0 * dst));

//    String^ logConversion = "";

    if (iSrcFrameRate == 23976 && iDstFrameRate == 25000)
    {
        frameRateConversion = EFrameRateConversionCode::kNTSCFilm_to_PAL; //logConversion = "23.976 to 25 fps";
    }
    else if (iSrcFrameRate == 23976 && iDstFrameRate == 29970)
    {
        frameRateConversion = EFrameRateConversionCode::kNTSCFilm_to_NTSCBroadcast; //logConversion = "Telecine 23.976 to 30 fps";
    }

    if (iSrcFrameRate == 24000 && iDstFrameRate == 23976)
    {
        frameRateConversion = EFrameRateConversionCode::kFilm_to_NTSCFilm; //logConversion = "24.0p to 23.976 fps";
    }
    if (iSrcFrameRate == 24000 && iDstFrameRate == 25000)
    {
        frameRateConversion = EFrameRateConversionCode::kFilm_to_PAL; //logConversion = "24.0p to 25 fps";
    }
    else if (iSrcFrameRate == 24000 && iDstFrameRate == 29970)
    {
        frameRateConversion = EFrameRateConversionCode::kFilm_to_NTSCBroadcast; //logConversion = "Telecine 24.0p to 30 fps";
    }

    else if (iSrcFrameRate == 25000 && iDstFrameRate == 23976)
    {
        frameRateConversion = EFrameRateConversionCode::kPAL_to_NTSCFilm; //logConversion = "25 fps to 23.976p";
    }
    else if (iSrcFrameRate == 25000 && iDstFrameRate == 29970)
    {
        frameRateConversion = EFrameRateConversionCode::kPAL_to_NTSCBroadcast; //logConversion = "25 fps to 30 fps";
    }

    else if (iSrcFrameRate == 29970 && iDstFrameRate == 25000)
    {
        frameRateConversion = (bIsTelecine) ? EFrameRateConversionCode::kNTSCInverseTelecine_to_PAL : EFrameRateConversionCode::kNTSCBroadcast_to_PAL;
        //logConversion = (bIsTelecine) ? "Inverse Telecine 30i to 25 fps" : "30 fps to 25 fps";
    }
    else if (iSrcFrameRate == 29970 && iDstFrameRate == 59940)
    {
        frameRateConversion = EFrameRateConversionCode::kNTSCBroadcast_to_NTSC60p; //logConversion = "30 fps to 60p";
    }
    else if (iSrcFrameRate == 29970 && bIsTelecine && iDstFrameRate == 23976)
    {
        frameRateConversion = EFrameRateConversionCode::kNTSCInverseTelecine_to_NTSCFilm;
        //logConversion = "Inverse Telecine 30i to 24p";
    }
    else if (iSrcFrameRate == 50000 && iDstFrameRate == 29970)
    {
        frameRateConversion = EFrameRateConversionCode::kPAL50p_to_NTSCBroadcast; //logConversion = "50p fps to 29.97p";
    }
    else if (iSrcFrameRate == 50000 && iDstFrameRate == 25000)
    {
        frameRateConversion = EFrameRateConversionCode::kPAL50p_to_PAL; //logConversion = "50p fps to 25 fps";
    }
    else if (iSrcFrameRate == 50000 && iDstFrameRate == 23976)
    {
        frameRateConversion = EFrameRateConversionCode::kPAL50p_to_NTSCFilm; //logConversion = "50p fps to 23.976p";
    }
    else if (iSrcFrameRate == 50000 && iDstFrameRate == 59940)
    {
        frameRateConversion = EFrameRateConversionCode::kPAL50p_to_NTSC60p; //logConversion = "50p fps to 60p";
    }

    else if (iSrcFrameRate == 59940 && iDstFrameRate == 25000)
    {
        frameRateConversion = (bIsTelecine) ? EFrameRateConversionCode::kNTSC60pInverseTelecine_to_PAL : EFrameRateConversionCode::kNTSC60p_to_PAL;
        //logConversion = (bIsTelecine) ? "Inverse Telecine 60p to 25 fps" : "60p to 25 fps";
    }
    else if (iSrcFrameRate == 59940 && iDstFrameRate == 29970)
    {
        frameRateConversion = EFrameRateConversionCode::kNTSC60p_to_NTSCBroadcast; //logConversion = "60p to 30 fps";
    }
    else if (iSrcFrameRate == 59940 && bIsTelecine)
    {
        frameRateConversion = EFrameRateConversionCode::kNTSC60pInverseTelecine_to_NTSCFilm;
        //logConversion = "Inverse Telecine 60p to 24p";
    }

    if (frameRateConversion == EFrameRateConversionCode::kNoConversion && iSrcFrameRate != iDstFrameRate)
    {
        frameRateConversion = EFrameRateConversionCode::kCustomFrameRateConversion;
        //logConversion = String::Format("custom conversion {0} to {1}", dbInputFrameRate, DestinationFrameRate);
    }

    if (frameRateConversion != EFrameRateConversionCode::kNoConversion)
    {
        //LogStatus("Info", "Frame rate conversion required: " + logConversion);
    }

    return frameRateConversion;
}

EScanType StringToScanType(std::string scanType)
{
    if("Interlaced" == scanType)
        return eInterlaced;

    if("Progressive" == scanType)
        return eProgressive;

    return eUndefinedScanType;
}

bool IsDeinterlacing(EFrameRateConversionCode fr_code)
{
    return false;

#if 0
   // Inverse telecine handled separately with FFDShow filter in ConfigureAVISynthFRConverter()
    switch (fr_code)
    {
        case kNTSCInverseTelecine_to_PAL:
        case kNTSCInverseTelecine_to_NTSCFilm:
        case kNTSC60pInverseTelecine_to_NTSCFilm:
        case kNTSC60pInverseTelecine_to_PAL:
            return false;
    }

    std::string scanType = source scan type; // TODO: fix this

    // Check for interlaced source and interlaced output
    // NOTE: BD spec supports interlaced 1080i and 480i, whereas SmoothStreaming and Widevine have issues with interlaced content
    EScanType inScanType = StringToScanType(scanType);
    bool interlacedInput = IsInterlaced(inScanType);

    scanType = dest scan type; // TODO: fix this

    bool interlacedOutput = IsInterlaced(StringToScanType(scanType));

    // If source interlace field order is not consistent, force de-interlace
    if (inScanType == eInterlacedVariable)
        interlacedOutput = false;

    // If converting NTSC to PAL or vice-versa, de-interlace first as needed
    if (fr_code == EFrameRateConversionCode::kPAL_to_NTSCFilm ||
        fr_code == EFrameRateConversionCode::kPAL_to_NTSCBroadcast ||
        fr_code == EFrameRateConversionCode::kNTSCBroadcast_to_PAL)
        interlacedOutput = false;

    return (interlacedInput && !interlacedOutput);
#endif
}

AVRational FRtoAVRational(double fr)
{
    int iFrameRate = (int) (fr * 1000.0);
    AVRational avRet;

    avRet.num = 0;
    avRet.den = 0;

    switch (iFrameRate)
    {
        case 11988: avRet.den = 1001; avRet.num = 12000; break;
        case 12000: avRet.den = 1; avRet.num = 12; break;
        case 12500: avRet.den = 2; avRet.num = 25; break;
        case 14985: avRet.den = 1001; avRet.num = 15000; break;
        case 15000: avRet.den = 1; avRet.num = 15; break;
        case 23976: avRet.den = 1001; avRet.num = 24000; break;
        case 24000: avRet.den = 1; avRet.num = 24; break;
        case 25000: avRet.den = 1; avRet.num = 25; break;
        case 29970: avRet.den = 1001; avRet.num = 30000; break;
        case 30000: avRet.den = 1; avRet.num = 30; break;
        case 59940: avRet.den = 1001; avRet.num = 60000; break;
    }

    return avRet;
}