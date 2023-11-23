/*
 * Copyright 2021 BlueStack Systems, Inc.
 * All Rights Reserved
 *
 * THIS IS UNPUBLISHED PROPRIETARY SOURCE CODE OF BLUESTACK SYSTEMS, INC.
 * The copyright notice above does not evidence any actual or intended
 * publication of such source code.
 */
#include <sys/time.h>
#include <dlfcn.h>
#include <map>
#include "HwAVCEnc.h"
#include "nvEncodeAPI.h"
#include "ColorBuffer.h"
#include "FrameBuffer.h"
#include "OpenGLESDispatch/EGLDispatch.h"
#include "RenderThreadInfo.h"
#include <signal.h>
#include <csignal>


#define NVENC_API_CALL(nvencAPI)                                                                                   \
    do                                                                                                             \
    {                                                                                                              \
        NVENCSTATUS errorCode = nvencAPI;                                                                          \
        if (errorCode != NV_ENC_SUCCESS) {                                                                         \
            HDLOGE(":::: %s: %s returned error=%d\n", __FUNCTION__, #nvencAPI, errorCode);                         \
        }                                                                                                          \
    } while (0)

#define NVENC_API_CALL_RET(nvencAPI, ret)                                                                          \
    do                                                                                                             \
    {                                                                                                              \
        NVENCSTATUS errorCode = nvencAPI;                                                                          \
        if (errorCode != NV_ENC_SUCCESS) {                                                                         \
            HDLOGE(":::: %s: %s returned error=%d\n", __FUNCTION__, #nvencAPI, errorCode);                         \
            return ret;                                                                                            \
        }                                                                                                          \
    } while (0)

#define NVENC_API_CALL_GOTO(nvencAPI, err)                                                                         \
    do                                                                                                             \
    {                                                                                                              \
        NVENCSTATUS errorCode = nvencAPI;                                                                          \
        if (errorCode != NV_ENC_SUCCESS) {                                                                         \
            HDLOGE(":::: %s: %s returned error=%d\n", __FUNCTION__, #nvencAPI, errorCode);                         \
            goto err;                                                                                              \
        }                                                                                                          \
    } while (0)

#define AVC_ENCODE_GUID NV_ENC_CODEC_H264_GUID
#define AVC_PRESET_GUID NV_ENC_PRESET_P2_GUID
#define AVC_TURING_INFO NV_ENC_TUNING_INFO_LOW_LATENCY
#define STATISTIC_TIME_MS     1000
#define LowBitrateInBits      1000 * 1000
#define MediumBitrateInBits   2000 * 1000
#define RatedBitrateInBits    2500 * 1000
#define HighBitrateInBits     3500 * 1000
#define ExHighBitrateInBits   5000 * 1000
#define QPDELTA_MIN_THRESHOLD -20
#define QPDELTA_MAX_THRESHOLD 0
#define TIMERDEBUG            0
typedef struct _extraEncodeMessage{
    uint32_t widthInMBs;
    uint32_t heightInMBs;
    uint32_t qpDeltaMapArraySize;
    int8_t*  qpDeltaMapArray;
    int      low_bit_qpValue;
    int      medium_bit_qpValue;
    int      high_bit_qpValue;
    int      qpValueOffset;
    // TODO:  more encoder features can be stored here later...
} extraEncodeMessage;

typedef enum {
    LOW_BITRATE,
    MEDIUM_BITRATE,
    HIGH_BITRATE,
} bitrateCondition;

typedef enum {
    REMAIN,
    INCREASE_STEADILY,
    INCREASE_RAPIDLY,
    DECREASE_STEADILY,
    DECREASE_RAPIDLY,
} qpDeltaMode;

static extraEncodeMessage * extraEncMsg = NULL;
static timeval  startTime               = {0};
static uint32_t totalEncodedSizeInBytes = 0;
static bool     dynamicQpAdjustReady    = false;
static bool     dynamicQpAdjustAllowed  = false;

#if TIMERDEBUG
extern qpDeltaMode ope;
timer_t timerid;
struct sigevent sigEv;
struct itimerspec its;
#endif

static FILE* inputFile = NULL;
static FILE* outPutFile = NULL;

extern const GLint* getGlesMaxContextAttribs();

typedef struct
{
    NV_ENC_INPUT_RESOURCE_OPENGL_TEX        resource;           // input resource
    NV_ENC_MAP_INPUT_RESOURCE               mapInputResource;
    NV_ENC_PIC_PARAMS                       picParams;
    NV_ENC_LOCK_BITSTREAM                   lockBitstreamData;  // output data
} NvEncBufferInfo;

typedef NVENCSTATUS NVENCAPI (*NvEncodeAPICreateInstance_t)(NV_ENCODE_API_FUNCTION_LIST *functionList);
typedef std::unordered_map<ColorBufferPtr, NvEncBufferInfo*> BufferMap_t;

typedef struct
{
    void*                           encoder;
    NV_ENCODE_API_FUNCTION_LIST     nvenc;
    int                             width;
    int                             height;
    int                             bitrate;
    int                             minBitrate;
    NV_ENC_RECONFIGURE_PARAMS       reconfigParams;
    NV_ENC_BUFFER_FORMAT            format;
    EGLSurface                      eglSurface;
    EGLContext                      eglContext;
    BufferMap_t                     bufferMap;
} AVCEncoderContext;

typedef struct{
    int currentFrameCount = 0;
    int frameCount;
    int width = 0;
    int height = 0;
    uint32_t bitrate = 0;
    int frameCountPerSecond = 0;
    int IDRCountPerSecond = 0;
    bool isDumpEnable = false;
    FILE *fpRaw = NULL;
    FILE *fpEnc = NULL;
    FILE *fpInfo = NULL;
    GLubyte* pixels = NULL;
    timeval startTime;
    std::string bitrateString;
    std::string IDRString;
} AVCDumpInfo;

static AVCDumpInfo gDumpInfo;

void AVCEnableDump(const char *renderID) {
    std::string dir = "/data/";
    std::string path = dir.append(renderID).append("/").append(renderID);
    std::string rawName = path + ".rgba";
    std::string encName = path + ".h264";
    std::string infoName = path + ".txt";

    gDumpInfo.fpRaw = fopen(rawName.c_str(), "wb");
    gDumpInfo.fpEnc = fopen(encName.c_str(), "wb");
    gDumpInfo.fpInfo = fopen(infoName.c_str(), "w");
    if (gDumpInfo.fpRaw && gDumpInfo.fpEnc && gDumpInfo.fpInfo)
        gDumpInfo.isDumpEnable = true;
    else
        gDumpInfo.isDumpEnable = false;
    gettimeofday(&gDumpInfo.startTime, 0);
    HDLOGE("jayden test H264 Dump enable=%d Frame Count: %d\n", gDumpInfo.isDumpEnable, gDumpInfo.frameCount);
}

static int SearchCapabilitySupported(AVCEncoderContext* ctx, GUID encodeGUID, NV_ENC_CAPS capsToQuery)
{
    if (!ctx) {
        HDLOGE(":::: %s but ctx is null, return directly....", __FUNCTION__);
        return 0;
    }

    NV_ENC_CAPS_PARAM capParam = { 0 };
    capParam.version     = NV_ENC_CAPS_PARAM_VER;
    capParam.capsToQuery = capsToQuery;
    int capVals          = 0;
    ctx->nvenc.nvEncGetEncodeCaps(ctx->encoder, AVC_ENCODE_GUID, &capParam, &capVals);
    return capVals;
}

#if TIMERDEBUG
void timerHandler(union sigval sv) {
    qpDeltaMode* opeValue = reinterpret_cast<qpDeltaMode*>(sv.sival_ptr);
    ope = *opeValue;
    HDLOGE("jayden test dynamic qp, %s, ope: %d, totalEncodedSize :%d", __FUNCTION__, ope, totalEncodedSize);
    int EncodedSizeInBits = totalEncodedSize * 8;
    if (EncodedSizeInBits > MediumBitrateInBits && EncodedSizeInBits < HighBitrateInBits){
        ope = REMAIN;
    } else if (EncodedSizeInBits > HighBitrateInBits) {
        ope = (EncodedSizeInBits > ExHighBitrateInBits ? DECREASE_RAPIDLY : DECREASE_STEADILY);
    } else {
        ope = EncodedSizeInBits < LowBitrateInBits ? INCREASE_RAPIDLY: INCREASE_STEADILY;
    }
    totalEncodedSize = 0;
}
#endif
static void AVCDisableDump() {
    gDumpInfo.currentFrameCount = 0;
    if (gDumpInfo.fpRaw) {
        fclose(gDumpInfo.fpRaw);
        gDumpInfo.fpRaw =  NULL;
    }
    if (gDumpInfo.fpEnc) {
        fclose(gDumpInfo.fpEnc);
        gDumpInfo.fpEnc = NULL;
    }
    if (gDumpInfo.fpInfo) {
        fclose(gDumpInfo.fpInfo);
        gDumpInfo.fpInfo =  NULL;
    }
    if (gDumpInfo.pixels) {
        free(gDumpInfo.pixels);
        gDumpInfo.pixels = NULL;
    }
    gDumpInfo.isDumpEnable = false;
    gDumpInfo.frameCountPerSecond = 0;
    gDumpInfo.IDRCountPerSecond = 0;
    gDumpInfo.bitrate = 0;
    gDumpInfo.bitrateString.clear();
    gDumpInfo.IDRString.clear();
    HDLOGI("AVC Dump complete\n");
}

static void calculateAVCStats(int resIDRFrame, uint32_t bitrate){
    gDumpInfo.currentFrameCount++;
    gDumpInfo.frameCountPerSecond++;

    if(resIDRFrame == 1)
        gDumpInfo.IDRCountPerSecond++;

    gDumpInfo.bitrate += bitrate;
    timeval currTime;
    gettimeofday(&currTime, 0);
    int milli = (currTime.tv_sec - gDumpInfo.startTime.tv_sec) * 1000.0f + (currTime.tv_usec - gDumpInfo.startTime.tv_usec) / 1000.0f;

    if(milli >= 1000){
        int bitrateperSecond = gDumpInfo.bitrate/gDumpInfo.frameCountPerSecond/1000; // in kbps+
        gDumpInfo.bitrateString += std::to_string(bitrateperSecond) + ",";
        gDumpInfo.IDRString += std::to_string(gDumpInfo.IDRCountPerSecond) + ",";
        gDumpInfo.IDRCountPerSecond = 0;
        gDumpInfo.frameCountPerSecond = 0;
        gDumpInfo.bitrate = 0;
        gDumpInfo.startTime = currTime;
    }

    if (gDumpInfo.currentFrameCount == gDumpInfo.frameCount) {
        if(gDumpInfo.frameCountPerSecond > 0) {
            int bitrateperSecond = gDumpInfo.bitrate/gDumpInfo.frameCountPerSecond/1000; // in kbps+
            gDumpInfo.bitrateString += std::to_string(bitrateperSecond);
        } else {
            gDumpInfo.bitrateString += "0";
        }
        gDumpInfo.IDRString += std::to_string(gDumpInfo.IDRCountPerSecond);
        std::string resolutionStr = std::to_string(gDumpInfo.width) + " " + std::to_string(gDumpInfo.height);
        fwrite(resolutionStr.c_str(), 1, resolutionStr.length(), gDumpInfo.fpInfo);
        fprintf(gDumpInfo.fpInfo, "\n");
        fwrite(gDumpInfo.bitrateString.c_str(), 1, gDumpInfo.bitrateString.length(), gDumpInfo.fpInfo);
        fprintf(gDumpInfo.fpInfo, "\n");
        fwrite(gDumpInfo.IDRString.c_str(), 1, gDumpInfo.IDRString.length(), gDumpInfo.fpInfo);
        fprintf(gDumpInfo.fpInfo, "\n");
        AVCDisableDump();
    }
}


static bool setupEGLResources(AVCEncoderContext* ctx, int width, int height)
{
    EGLConfig config;
    EGLDisplay dpy = FrameBuffer::getFB()->getDisplay();

    static const GLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };

    int n;
    if (!s_egl.eglChooseConfig(dpy, configAttribs, &config, 1, &n) ||
            n == 0) {
        HDLOGE(":::: %s: Could not find GLES 2.x config!\n", __FUNCTION__);
        return false;
    }

    static const EGLint pbufAttribs[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE
    };

    ctx->eglSurface = s_egl.eglCreatePbufferSurface(dpy, config, pbufAttribs);
    if (ctx->eglSurface == EGL_NO_SURFACE) {
        HDLOGE(":::: %s: Could not create GLES 2.x Pbuffer!\n", __FUNCTION__);
        return false;
    }

    ctx->eglContext = s_egl.eglCreateContext(dpy, config, EGL_NO_CONTEXT, getGlesMaxContextAttribs());
    if (ctx->eglContext == EGL_NO_CONTEXT) {
        HDLOGE(":::: %s: Could not create GLES 2.x Context!\n", __FUNCTION__);
        s_egl.eglDestroySurface(dpy, ctx->eglSurface);
        return false;
    }

    if (!s_egl.eglMakeCurrent(dpy, ctx->eglSurface, ctx->eglSurface, ctx->eglContext)) {
        HDLOGE(":::: %s: Could not make GLES 2.x context current!\n", __FUNCTION__);
        s_egl.eglDestroySurface(dpy, ctx->eglSurface);
        s_egl.eglDestroyContext(dpy, ctx->eglContext);
        return false;
    }

    return true;
}

static void destroyEGLResources(AVCEncoderContext* ctx)
{
    EGLDisplay dpy = FrameBuffer::getFB()->getDisplay();
    s_egl.eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (ctx->eglContext != EGL_NO_CONTEXT)
        eglDestroyContext(dpy, ctx->eglContext);

    if (ctx->eglSurface != EGL_NO_SURFACE)
        eglDestroySurface(dpy, ctx->eglSurface);
}

AVCEncCtx AVCCreateEncoder(int width, int height, int fps, int bitrate, qpValueMsg* msg)
{   qpValueMsg msgTmp = {-14, -10, 0, -2}; msg = &msgTmp;
    if (msg)
        HDLOGE("jayden test qpdela value debugMsg output, highbit: %d, mediumbit:%d, lowbit:%d, offset: %d, msg: %p",msg->high_bit_qpValue, msg->medium_bit_qpValue, msg->low_bit_qpValue, msg->qpValueOffset, msg);
    AVCEncoderContext* ctx = new AVCEncoderContext;
    ctx->width = width;
    ctx->height = height;
    ctx->bitrate = bitrate;
    ctx->minBitrate = bitrate / 2.5;
    ctx->format = NV_ENC_BUFFER_FORMAT_ABGR;
    gDumpInfo.frameCount = fps*20;
    if (!setupEGLResources(ctx, width, height))
        return 0;

    void* handle = dlopen("libnvidia-encode.so", RTLD_LAZY);
    if (!handle) {
        HDLOGE(":::: %s dlopen libnvidia-encode.so failed error=%s\n", __FUNCTION__, dlerror());
        return 0;
    }

    NvEncodeAPICreateInstance_t nvEncodeAPICreateInstance = (NvEncodeAPICreateInstance_t) dlsym(handle, "NvEncodeAPICreateInstance");
    if (!nvEncodeAPICreateInstance) {
        HDLOGE(":::: %s dlsym NvEncodeAPICreateInstance failed error=%s\n", __FUNCTION__, dlerror());
        return 0;
    }

    ctx->nvenc = { NV_ENCODE_API_FUNCTION_LIST_VER };
    NVENC_API_CALL_RET(nvEncodeAPICreateInstance(&ctx->nvenc), 0);

    if (!ctx->nvenc.nvEncOpenEncodeSessionEx) {
        HDLOGE(":::: %s EncodeAPI not found\n", __FUNCTION__);
        return 0;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS encodeSessionExParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    encodeSessionExParams.device = NULL;
    encodeSessionExParams.deviceType = NV_ENC_DEVICE_TYPE_OPENGL;
    encodeSessionExParams.apiVersion = NVENCAPI_VERSION;
    ctx->encoder = NULL;
    NVENC_API_CALL_RET(ctx->nvenc.nvEncOpenEncodeSessionEx(&encodeSessionExParams, &ctx->encoder), 0);

    // track encoder
    RenderThreadInfo* const tinfo = RenderThreadInfo::get();
    FrameBuffer::getFB()->lock();
    tinfo->m_avcEncSet.insert((AVCEncCtx)ctx);
    FrameBuffer::getFB()->unlock();

    ctx->reconfigParams = { NV_ENC_RECONFIGURE_PARAMS_VER };
    ctx->reconfigParams.reInitEncodeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    ctx->reconfigParams.reInitEncodeParams.encodeGUID = AVC_ENCODE_GUID;
    ctx->reconfigParams.reInitEncodeParams.presetGUID = AVC_PRESET_GUID;
    ctx->reconfigParams.reInitEncodeParams.encodeWidth = width;
    ctx->reconfigParams.reInitEncodeParams.encodeHeight = height;
    ctx->reconfigParams.reInitEncodeParams.darWidth = width;
    ctx->reconfigParams.reInitEncodeParams.darHeight = height;
    ctx->reconfigParams.reInitEncodeParams.frameRateNum = fps;
    ctx->reconfigParams.reInitEncodeParams.frameRateDen = 1;
    ctx->reconfigParams.reInitEncodeParams.enableEncodeAsync = 0;
    ctx->reconfigParams.reInitEncodeParams.enablePTD = 1;
    ctx->reconfigParams.reInitEncodeParams.reportSliceOffsets = 0;
    ctx->reconfigParams.reInitEncodeParams.enableSubFrameWrite = 0;
    ctx->reconfigParams.reInitEncodeParams.enableMEOnlyMode = 0;
    ctx->reconfigParams.reInitEncodeParams.enableOutputInVidmem = 0;

    NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } };
    NVENC_API_CALL_RET(ctx->nvenc.nvEncGetEncodePresetConfigEx(ctx->encoder, AVC_ENCODE_GUID, AVC_PRESET_GUID, AVC_TURING_INFO, &presetConfig), 0);
    ctx->reconfigParams.reInitEncodeParams.encodeConfig = new NV_ENC_CONFIG;
    memcpy(ctx->reconfigParams.reInitEncodeParams.encodeConfig, &(presetConfig.presetCfg), sizeof(NV_ENC_CONFIG));
    ctx->reconfigParams.reInitEncodeParams.encodeConfig->profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.averageBitRate = bitrate;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.maxBitRate = bitrate;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.idrPeriod = 60;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.enableAQ = 1;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.enableTemporalAQ = 1;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->gopLength = 30;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.enableLookahead = 1;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.disableIadapt = 0;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.disableBadapt = 1;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->frameIntervalP = 1;
    ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_H264_42;
    ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.disableSPSPPS = 0;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.enableConstrainedEncoding = 1;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.enableIntraRefresh = 1;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.intraRefreshPeriod = 30;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.enableScalabilityInfoSEI = 1;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.enableTemporalSVC = 1;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.numTemporalLayers = 2;
    //ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.maxTemporalLayers = 2;
   // gettimeofday(&startTime, NULL);
    if (msg) {
        ctx->reconfigParams.reInitEncodeParams.encodeConfig->profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
        ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
        ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA;
        extraEncMsg = (struct _extraEncodeMessage*)malloc(sizeof(extraEncodeMessage));
        memset(extraEncMsg, 0, sizeof(extraEncodeMessage));
        extraEncMsg->widthInMBs	 = ((width + 15) & ~15) >> 4;
        extraEncMsg->heightInMBs = ((height + 15) & ~15) >> 4;
        extraEncMsg->qpDeltaMapArraySize  = extraEncMsg->widthInMBs * extraEncMsg->heightInMBs;
        extraEncMsg->qpDeltaMapArray      = (int8_t*)malloc(extraEncMsg->qpDeltaMapArraySize * sizeof(int8_t));
        extraEncMsg->high_bit_qpValue     = msg->high_bit_qpValue;
        extraEncMsg->medium_bit_qpValue   = msg->medium_bit_qpValue;
        extraEncMsg->low_bit_qpValue      = msg->low_bit_qpValue;
        extraEncMsg->qpValueOffset        = msg->qpValueOffset;
        memset(extraEncMsg->qpDeltaMapArray, 0, extraEncMsg->qpDeltaMapArraySize);
    }
    ctx->reconfigParams.reInitEncodeParams.maxEncodeWidth = width;
    ctx->reconfigParams.reInitEncodeParams.maxEncodeHeight = height;
    ctx->reconfigParams.reInitEncodeParams.tuningInfo = AVC_TURING_INFO;

    NVENC_API_CALL_RET(ctx->nvenc.nvEncInitializeEncoder(ctx->encoder, &(ctx->reconfigParams.reInitEncodeParams)), 0);

    HDLOGE("...jayden test AVC encoder created=0x%" PRIx64 " width=%d height=%d fps=%d bitrate=%d minBitrate=%d\n", (AVCEncCtx)ctx, width, height, fps, bitrate, ctx->minBitrate);

#if TIMERDEBUG
    ope = REMAIN;
    sigEv.sigev_notify = SIGEV_THREAD;
    sigEv.sigev_notify_function   = timerHandler;
    sigEv.sigev_notify_attributes = NULL;
    sigEv.sigev_value.sival_ptr   = &ope;
    timer_create(CLOCK_REALTIME, &sigEv, &timerid);
#else 
    gettimeofday(&startTime, NULL);
#endif

    return (AVCEncCtx) ctx;
}

static int elapsedTimeMs(timeval currentTime, timeval startTime) {
    gettimeofday(&currentTime, NULL);
    int elapsed_time = (currentTime.tv_sec - startTime.tv_sec)*1000.0f +
                       (currentTime.tv_usec - startTime.tv_usec) / 1000.0f;
    return elapsed_time;
}

bool checkDynamicQpAdjustAllowed(int& suitableBrtNumInSec, int minFpsRequired) {
    bool allowed                    = false;
    static int continousBrtSuitable = 0;
    static timeval nowTimeMs        = startTime;
    timeval firTimeValMs, secTimeValMs;

    if (elapsedTimeMs(firTimeValMs, startTime) > STATISTIC_TIME_MS * 60 * 5) {
        HDLOGE("jayden test dynamic qp, checkDynamicQpAdjustAllowed by 5 mins");
        allowed             = true;
        suitableBrtNumInSec = 0;
        return allowed;
    }

    if (elapsedTimeMs(secTimeValMs, nowTimeMs) > STATISTIC_TIME_MS) {
        if (suitableBrtNumInSec > minFpsRequired) {
            HDLOGE("jayden test dynamic qp, checkDynamicQpAdjustAllowed, suitableBrtNumInSec: %d, continousBrtSuitable: %d", suitableBrtNumInSec, continousBrtSuitable);
            continousBrtSuitable++;
            gettimeofday(&nowTimeMs, NULL);
            if (continousBrtSuitable > 30) {
                HDLOGE("jayden test dynamic qp, checkDynamicQpAdjustAllowed by continousBrtSuitable more than specified times, suitableBrtNumInSec: %d, continousBrtSuitable: %d", suitableBrtNumInSec, continousBrtSuitable);
                allowed             = true;
                suitableBrtNumInSec = 0;
                return allowed;
            }
        } else {
            continousBrtSuitable = 0;
            gettimeofday(&nowTimeMs, NULL);
        }
        suitableBrtNumInSec = 0;
    }
    return allowed;
}

void qpDeltaModeSelect(uint32_t bitrate, int &suitableBrtNumInSec, qpDeltaMode &ope) {
    timeval nowTimeMs;
    if (elapsedTimeMs(nowTimeMs, startTime) >= STATISTIC_TIME_MS) {
        uint32_t EncodedSizeInBits = totalEncodedSizeInBytes * 8;
        dynamicQpAdjustReady       = true;
        if (EncodedSizeInBits > HighBitrateInBits) {
            ope = (EncodedSizeInBits > ExHighBitrateInBits ? INCREASE_RAPIDLY : INCREASE_STEADILY);
        } else if ((EncodedSizeInBits >= RatedBitrateInBits && EncodedSizeInBits <= HighBitrateInBits) 
                    /*|| EncodedSizeInBits <= LowBitrateInBits || bitrate <= LowBitrateInBits*/) {
            ope = REMAIN;
        } else {
            ope = EncodedSizeInBits >= MediumBitrateInBits ? DECREASE_STEADILY: DECREASE_RAPIDLY;
        }
        HDLOGE("jayden test dynamic qp, bitrate: %d, totalEncoded size: %f M, brtAdjustThreshold: %d, ope: %d", bitrate, (float)EncodedSizeInBits/(1000*1000), suitableBrtNumInSec, (int)ope);
        totalEncodedSizeInBytes = 0;
        suitableBrtNumInSec	    = 0;
        gettimeofday(&startTime, NULL);
    } else {
        dynamicQpAdjustReady = false;
        ope = REMAIN;
    }
}
static int qpDeltaValueOperation(bool bitrateNotJump, qpDeltaMode opt, int &qpValue) {
    if (bitrateNotJump == false || dynamicQpAdjustReady == false) 
        goto err;
    switch (opt) {
        case REMAIN:
            return qpValue;
        case INCREASE_STEADILY: {
            if (qpValue + 1 < QPDELTA_MAX_THRESHOLD)
                qpValue = qpValue + 1;
            return qpValue;
        }
        case INCREASE_RAPIDLY: {
            if (qpValue + 2 < QPDELTA_MAX_THRESHOLD)
                qpValue = qpValue + 2;
            return qpValue;
        }
        case DECREASE_STEADILY: {
            if (qpValue - 1 > QPDELTA_MIN_THRESHOLD)
                qpValue = qpValue - 1;
            return qpValue - 1;
        }
        case DECREASE_RAPIDLY: {
            if (qpValue - 2 > QPDELTA_MIN_THRESHOLD)
                qpValue = qpValue - 2;
            return qpValue;
        default:
            HDLOGE(":::: %s jayden test invalid qpDeltaOperation setting!!!!", __FUNCTION__);
            goto err;
        }
    }
 err:
	return qpValue;
}

static void RegionOfInterestOpt(extraEncodeMessage* enc, int mainRegionValue, int otherRegionValue, bool& centralOptimization) {
    if (centralOptimization) { // central region optimization
        for (uint32_t i = 0; i < enc->heightInMBs; i++) {
            for (uint32_t j = 0; j < enc->widthInMBs; j++) {
                if (( i > enc->heightInMBs / 4 && i < enc->heightInMBs * 3 / 4)  && (j > enc->widthInMBs / 4 && j < enc->widthInMBs * 3 / 4)) {
                    enc->qpDeltaMapArray[i* enc->widthInMBs + j] = mainRegionValue;
                } else {
                    enc->qpDeltaMapArray[i* enc->widthInMBs + j] = otherRegionValue;
                }
            }
        }
        centralOptimization = false;
    } else { // surrounding region optimization
        for (uint32_t i = 0; i < enc->heightInMBs; i++) {
            for (uint32_t j = 0; j < enc->widthInMBs; j++) {
				if ( (i < enc->heightInMBs / 4 || i > enc->heightInMBs * 3 / 4 ) || (j < enc->widthInMBs / 4 || j > enc->widthInMBs * 3 / 4)) {
                    enc->qpDeltaMapArray[i* enc->widthInMBs + j] = mainRegionValue;
                } else {
                    enc->qpDeltaMapArray[i* enc->widthInMBs + j] = otherRegionValue;
                }
            }
        }
        centralOptimization = true;
    }
    return;
}
/*
static void useQpdeltaStrategy(NvEncBufferInfo* nvencBufInfo, uint32_t bitrate) {
    if (!nvencBufInfo) {
        HDLOGE(":::: %s invalid, nvencBufInfo ptr: %p", __FUNCTION__, nvencBufInfo);
        return;
    }
    static bool centralOptimization = true;
    bitrateCondition bc;
	bc = (bitrate > 2000000) ? ( bitrate > 2500000 ? HIGH_BITRATE : MEDIUM_BITRATE) : LOW_BITRATE;

    nvencBufInfo->picParams.qpDeltaMapSize = extraEncMsg->qpDeltaMapArraySize;
    switch (bc) {
        case HIGH_BITRATE: {
            RegionOfInterestOpt(extraEncMsg, extraEncMsg->high_bit_qpValue, extraEncMsg->high_bit_qpValue*1.2, centralOptimization);
            nvencBufInfo->picParams.qpDeltaMap = extraEncMsg->qpDeltaMapArray;
            HDLOGE("jayden test qpdela value mode in high bitrate case, bitrate: %d, mode: %d, qpdelta: %d", bitrate, centralOptimization, nvencBufInfo->picParams.qpDeltaMap[0]);
        }
            break;
        case MEDIUM_BITRATE: {
            //memset(extraEncMsg->qpDeltaMapArray, -15, extraEncMsg->qpDeltaMapArraySize);
            RegionOfInterestOpt(extraEncMsg, extraEncMsg->medium_bit_qpValue - extraEncMsg->qpValueOffset , extraEncMsg->medium_bit_qpValue + extraEncMsg->qpValueOffset, centralOptimization);
            //RegionOfInterestOpt(extraEncMsg, -12, -16, centralOptimization);
            nvencBufInfo->picParams.qpDeltaMap  = extraEncMsg->qpDeltaMapArray;
            HDLOGE("jayden test qpdela value mode in medium bitrate case, bitrate: %d, mode: %d, qpdelta: %d", bitrate, centralOptimization, nvencBufInfo->picParams.qpDeltaMap[0]);
        }
            break;
        case LOW_BITRATE: {
            //memset(extraEncMsg->qpDeltaMapArray, -17, extraEncMsg->qpDeltaMapArraySize);
            //RegionOfInterestOpt(extraEncMsg, -14, -18, centralOptimization);
            RegionOfInterestOpt(extraEncMsg, extraEncMsg->low_bit_qpValue - extraEncMsg->qpValueOffset, extraEncMsg->low_bit_qpValue + extraEncMsg->qpValueOffset, centralOptimization);
            nvencBufInfo->picParams.qpDeltaMap  = extraEncMsg->qpDeltaMapArray;
            HDLOGE("jayden test qpdela value mode in low bitrate case, bitrate: %d, mode: %d, qpdelta: %d", bitrate, centralOptimization, nvencBufInfo->picParams.qpDeltaMap[0]);
        }
            break;
        default:
            HDLOGE(":::: %s jayden test invalid qpdelta mode setting!!!!", __FUNCTION__);
            break;
    }
    return;

}
*/
static void useQpDeltaStrategy(NvEncBufferInfo* nvencBufInfo, uint32_t bitrate, qpDeltaMode operation) {
    if (!nvencBufInfo) {
        HDLOGE(":::: %s invalid, nvencBufInfo ptr: %p", __FUNCTION__, nvencBufInfo);
        return;
    }
    static bool centralOptimization          = true;
    bitrateCondition bc                      = (bitrate > MediumBitrateInBits) ? ( bitrate > RatedBitrateInBits ? HIGH_BITRATE : MEDIUM_BITRATE) : LOW_BITRATE;
    static bitrateCondition lastBitCondition = bc;
    static bool bitrateNotJump               = true;
    nvencBufInfo->picParams.qpDeltaMapSize   = extraEncMsg->qpDeltaMapArraySize;
    if (lastBitCondition != bc)
        bitrateNotJump = false;
    switch (bc) {
        case HIGH_BITRATE: {
            static int curQpValue = extraEncMsg->high_bit_qpValue;
            if (dynamicQpAdjustReady) {
                qpDeltaValueOperation(bitrateNotJump, operation, curQpValue);
                dynamicQpAdjustReady = false;
                bitrateNotJump      = true;
            }
            RegionOfInterestOpt(extraEncMsg, curQpValue, curQpValue * 1.2, centralOptimization);
            //RegionOfInterestOpt(extraEncMsg, extraEncMsg->high_bit_qpValue, extraEncMsg->high_bit_qpValue*1.2, centralOptimization);
            nvencBufInfo->picParams.qpDeltaMap = extraEncMsg->qpDeltaMapArray;
            HDLOGE("jayden test qpdela value mode in high bitrate case, bitrate: %d, mode: %d, qpdelta: %d, ope: %d, dynamicqpadjust: %d", bitrate, centralOptimization, nvencBufInfo->picParams.qpDeltaMap[0], (int)operation, dynamicQpAdjustReady);
        }
            break;
        case MEDIUM_BITRATE: {
            //memset(extraEncMsg->qpDeltaMapArray, -15, extraEncMsg->qpDeltaMapArraySize);
            static int curQpValue = extraEncMsg->medium_bit_qpValue;
            if (dynamicQpAdjustReady) {
                qpDeltaValueOperation(bc == lastBitCondition, operation, curQpValue);
                dynamicQpAdjustReady = false;
                bitrateNotJump       = true;
            }
            RegionOfInterestOpt(extraEncMsg, curQpValue - extraEncMsg->qpValueOffset , curQpValue + extraEncMsg->qpValueOffset, centralOptimization);			
            //RegionOfInterestOpt(extraEncMsg, extraEncMsg->medium_bit_qpValue - extraEncMsg->qpValueOffset , extraEncMsg->medium_bit_qpValue + extraEncMsg->qpValueOffset, centralOptimization);
            nvencBufInfo->picParams.qpDeltaMap  = extraEncMsg->qpDeltaMapArray;
            HDLOGE("jayden test qpdela value mode in medium bitrate case, bitrate: %d, mode: %d, qpdelta: %d, ope: %d, dynamicqpadjust: %d", bitrate, centralOptimization, nvencBufInfo->picParams.qpDeltaMap[0], (int)operation, dynamicQpAdjustReady);
        }
            break;
        case LOW_BITRATE: {
            //memset(extraEncMsg->qpDeltaMapArray, -17, extraEncMsg->qpDeltaMapArraySize);
            static int curQpValue = extraEncMsg->low_bit_qpValue;
            if (dynamicQpAdjustReady) {
                qpDeltaValueOperation(bc == lastBitCondition, operation, curQpValue);
                dynamicQpAdjustReady = false;
                bitrateNotJump       = true;
            }
            RegionOfInterestOpt(extraEncMsg, curQpValue - extraEncMsg->qpValueOffset, curQpValue + extraEncMsg->qpValueOffset, centralOptimization);
           // RegionOfInterestOpt(extraEncMsg, extraEncMsg->low_bit_qpValue - extraEncMsg->qpValueOffset, extraEncMsg->low_bit_qpValue + extraEncMsg->qpValueOffset, centralOptimization);
            nvencBufInfo->picParams.qpDeltaMap  = extraEncMsg->qpDeltaMapArray;
            HDLOGE("jayden test qpdela value mode in low bitrate case, bitrate: %d, mode: %d, qpdelta: %d, ope: %d, dynamicqpadjust: %d", bitrate, centralOptimization, nvencBufInfo->picParams.qpDeltaMap[0], (int)operation, dynamicQpAdjustReady);
        }
            break;
        default:
            HDLOGE(":::: %s jayden test invalid qpdelta mode setting!!!!", __FUNCTION__);
            break;
    }
    lastBitCondition = bc;
    return;
}

static bool AVCPrepareIOBuffers(AVCEncoderContext* ctx, NvEncBufferInfo* nvencBufInfo, GLuint tex)
{
    nvencBufInfo->resource.texture = tex;
    nvencBufInfo->resource.target = GL_TEXTURE_2D;

    // register input resource
    NV_ENC_REGISTER_RESOURCE registerResource = { NV_ENC_REGISTER_RESOURCE_VER };
    registerResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX;
    registerResource.width = ctx->width;
    registerResource.height = ctx->height;
    registerResource.pitch = ctx->width * 4;
    registerResource.subResourceIndex = 0;
    registerResource.resourceToRegister = &(nvencBufInfo->resource);
    registerResource.bufferFormat = ctx->format;
    registerResource.bufferUsage = NV_ENC_INPUT_IMAGE;
    NVENC_API_CALL_RET(ctx->nvenc.nvEncRegisterResource(ctx->encoder, &registerResource), false);

    nvencBufInfo->mapInputResource = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    nvencBufInfo->mapInputResource.registeredResource = registerResource.registeredResource;

    nvencBufInfo->picParams = { NV_ENC_PIC_PARAMS_VER };
    nvencBufInfo->picParams.inputWidth = ctx->width;
    nvencBufInfo->picParams.inputHeight = ctx->height;
    nvencBufInfo->picParams.inputPitch = ctx->width * 4;
    nvencBufInfo->picParams.inputBuffer = nvencBufInfo->mapInputResource.mappedResource;
    nvencBufInfo->picParams.bufferFmt = ctx->format;
    nvencBufInfo->picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

    // create bitstream buffer for output
    NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamBuffer = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    NVENC_API_CALL_RET(ctx->nvenc.nvEncCreateBitstreamBuffer(ctx->encoder, &createBitstreamBuffer), false);
    nvencBufInfo->picParams.outputBitstream = createBitstreamBuffer.bitstreamBuffer;

    nvencBufInfo->lockBitstreamData = { NV_ENC_LOCK_BITSTREAM_VER };
    nvencBufInfo->lockBitstreamData.outputBitstream = createBitstreamBuffer.bitstreamBuffer;
    nvencBufInfo->lockBitstreamData.doNotWait = 0;

    return true;
}


void AVCEncodeBuffer(AVCEncCtx context, uint32_t colorBuffer, uint64_t inTimestamp, int reqIDRFrame, IOStream *stream, uint32_t bitrate)
{
    AVCEncoderContext* ctx = (AVCEncoderContext*) context;
    NvEncBufferInfo* nvencBufInfo = NULL;
    bool inputMapped = false;
    int resIDRFrame = 0;
    BufferMap_t::iterator it;
    bool isDumpEnable = gDumpInfo.isDumpEnable;
    // don't drop bitrate below minBitrate
    if (bitrate < ctx->minBitrate)
        bitrate = ctx->minBitrate;

    if (ctx->bitrate != bitrate) {
        ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.averageBitRate = bitrate;
        NVENC_API_CALL(ctx->nvenc.nvEncReconfigureEncoder(ctx->encoder, &(ctx->reconfigParams)));
        ctx->bitrate = bitrate;
    }

    ColorBufferPtr cb = FrameBuffer::getFB()->getColorBuffer_locked(colorBuffer);
    if (!cb) {
        HDLOGE(":::: %s invalid colorBuffer(0x%x)\n", __FUNCTION__, colorBuffer);
        goto err;
    }

    it = ctx->bufferMap.find(cb);
    if (it == ctx->bufferMap.end()) {
        nvencBufInfo = new NvEncBufferInfo;

        // input is stored in texture backing buffer, register it
        if (!AVCPrepareIOBuffers(ctx, nvencBufInfo, cb->getEGLTexture()))
            goto err;

        ctx->bufferMap.insert(std::pair<ColorBufferPtr, NvEncBufferInfo*> (cb, nvencBufInfo));
    }
    else
        nvencBufInfo = it->second;
#if 0
    if (!inputFile) {
        inputFile = fopen("/data/hw_avcenc_in.bin", "wb");
        HDLOGE(":::: %s jayden test open file now!, inputFile: %p\n", __FUNCTION__, inputFile);
        if (!inputFile){
            HDLOGE(":::: %s jayden test open Dump data file, but failed!!!\n", __FUNCTION__);
        }
    }
{
    GLuint texObj    = nvencBufInfo->resource.texture;
    GLubyte* pixels  = (GLubyte*)malloc(ctx->width * ctx->height * 4);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texObj, 0);
    glReadPixels(0, 0, ctx->width, ctx->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    GLenum glErr = glGetError();
    if (glErr != GL_NO_ERROR) {
        HDLOGE(":::: %s jayden test dump RGBA data error!!! \n", __FUNCTION__);
    }
    if (inputFile) {
        fwrite(pixels, sizeof(pixels[0]), ctx->width * ctx->height * 4, inputFile);
        fflush(inputFile);
    }
    if (pixels) {
        free(pixels);
        pixels = NULL;
    }
 }
#endif

#if !TIMERDEBUG
    static qpDeltaMode ope = REMAIN; 
 //   if (!dynamicQpAdjustReady) {
 //       gettimeofday(&startTime, NULL);
 //       dynamicQpAdjustReady = true;
 //       ope = REMAIN;
 //   }
#endif

    if (extraEncMsg)
        useQpDeltaStrategy(nvencBufInfo, bitrate, ope);

    if (isDumpEnable) {
        if (gDumpInfo.currentFrameCount == 0) {
            gDumpInfo.width = ctx->width;
            gDumpInfo.height = ctx->height;
            gDumpInfo.pixels = (GLubyte*)malloc(gDumpInfo.width * gDumpInfo.height * 4);
            reqIDRFrame = 1;
            HDLOGI("Forcing IDR frame for dump");
        }
        GLuint textureObj = nvencBufInfo->resource.texture; // the texture object - glGenTextures+
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureObj, 0);
        glReadPixels(0, 0, gDumpInfo.width, gDumpInfo.height, GL_RGBA, GL_UNSIGNED_BYTE, gDumpInfo.pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        GLenum glErr = glGetError();
        if(glErr != GL_NO_ERROR)
            HDLOGE("GL Dump error: %d\n", glErr);
        fwrite(gDumpInfo.pixels, sizeof(gDumpInfo.pixels[0]), gDumpInfo.width * gDumpInfo.height * 4, gDumpInfo.fpRaw);
    }

    // map input resource
    NVENC_API_CALL_GOTO(ctx->nvenc.nvEncMapInputResource(ctx->encoder, &(nvencBufInfo->mapInputResource)), err);
    nvencBufInfo->picParams.inputBuffer = nvencBufInfo->mapInputResource.mappedResource;
    inputMapped = true;

    // encode buffer
    if (reqIDRFrame) {
        HDLOGI("Request IDR frame\n");
        nvencBufInfo->picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        //picParams.codecPicParams.h264PicParams.constrainedFrame = 1;
    }
    else
        nvencBufInfo->picParams.encodePicFlags = 0;
    nvencBufInfo->picParams.inputTimeStamp = inTimestamp;

    NVENC_API_CALL_GOTO(ctx->nvenc.nvEncEncodePicture(ctx->encoder, &(nvencBufInfo->picParams)), err);

    // get encoded output
    NVENC_API_CALL_GOTO(ctx->nvenc.nvEncLockBitstream(ctx->encoder, &(nvencBufInfo->lockBitstreamData)), err);
    //HDLOGI("frame encoded size=%d type=%x\n", nvencBufInfo->lockBitstreamData.bitstreamSizeInBytes, nvencBufInfo->lockBitstreamData.pictureType);
    if (nvencBufInfo->lockBitstreamData.pictureType == NV_ENC_PIC_TYPE_IDR)
        resIDRFrame = 1;

    stream->writeFully(&(nvencBufInfo->lockBitstreamData.bitstreamSizeInBytes), 4);
    stream->writeFully(&resIDRFrame, 4);
    stream->writeFully(nvencBufInfo->lockBitstreamData.bitstreamBufferPtr, nvencBufInfo->lockBitstreamData.bitstreamSizeInBytes);
#if 0
    if (!outPutFile) {
        outPutFile = fopen("/data/hw_avcenc_out.bin", "wb");
        HDLOGE(":::: %s jayden test open file now!, outPutFile: %p\n", __FUNCTION__, outPutFile);
        if (!outPutFile){
            HDLOGE(":::: %s jayden test open Dump data file, but failed!!!\n", __FUNCTION__);
        }
    }

    if (outPutFile) {
        fwrite(nvencBufInfo->lockBitstreamData.bitstreamBufferPtr, 1, nvencBufInfo->lockBitstreamData.bitstreamSizeInBytes, outPutFile);
        fflush(outPutFile);
    }
#endif   
    if (isDumpEnable) {
        fwrite(nvencBufInfo->lockBitstreamData.bitstreamBufferPtr, 1, nvencBufInfo->lockBitstreamData.bitstreamSizeInBytes, gDumpInfo.fpEnc);
        calculateAVCStats(resIDRFrame, bitrate);
    }
    
    totalEncodedSizeInBytes += nvencBufInfo->lockBitstreamData.bitstreamSizeInBytes;
{
#if TIMERDEBUG
		its.it_value.tv_sec     = 1;
		its.it_value.tv_nsec    = 0;
		its.it_interval.tv_sec  = 1;
		its.it_interval.tv_nsec = 0;
		timer_settime(timerid, 0, &its, NULL);
#else

    static int suitableBrtNumInSec = 0;
	//timeval nowTimeMs             = {0};

    if (bitrate > LowBitrateInBits)
        suitableBrtNumInSec++;
	
    if (!dynamicQpAdjustAllowed) {
        if (checkDynamicQpAdjustAllowed(suitableBrtNumInSec, ctx->reconfigParams.reInitEncodeParams.frameRateNum / 2)) {
            dynamicQpAdjustAllowed = true;
            gettimeofday(&startTime, NULL);
        }
    }

    if (dynamicQpAdjustAllowed && suitableBrtNumInSec > ctx->reconfigParams.reInitEncodeParams.frameRateNum / 2)
        qpDeltaModeSelect(bitrate, suitableBrtNumInSec, ope);
/*
    if (dynamicQpAdjustAllowed && elapsedTimeMs(nowTimeMs, startTime) >= STATISTIC_TIME_MS) {
        uint32_t EncodedSizeInBits = totalEncodedSizeInBytes * 8;
        dynamicQpAdjustReady       = true;
        if (EncodedSizeInBits > HighBitrateInBits) {
            ope = (EncodedSizeInBits > ExHighBitrateInBits ? INCREASE_RAPIDLY : INCREASE_STEADILY);
        } else if ((EncodedSizeInBits >= RatedBitrateInBits && EncodedSizeInBits <= HighBitrateInBits) || EncodedSizeInBits <= LowBitrateInBits) {
            ope = REMAIN;
        } else {
            ope = EncodedSizeInBits >= MediumBitrateInBits ? DECREASE_STEADILY: DECREASE_RAPIDLY;
        }
        HDLOGE("jayden test dynamic qp, bitrate: %d, and totalEncoded size: %f M, last encoded size: %d Byte, brtAdjustThreshold: %d, ope: %d", bitrate, (float)EncodedSizeInBits/(1000*1000), nvencBufInfo->lockBitstreamData.bitstreamSizeInBytes, brtAdjustThreshold, (int)ope);
        totalEncodedSizeInBytes = 0;
        brtAdjustThreshold      = 0;
        gettimeofday(&startTime, NULL);
    } else {
        dynamicQpAdjustReady = false;
    }
 */
#endif
}
    // free resources
    NVENC_API_CALL(ctx->nvenc.nvEncUnlockBitstream(ctx->encoder, nvencBufInfo->lockBitstreamData.outputBitstream));
    NVENC_API_CALL(ctx->nvenc.nvEncUnmapInputResource(ctx->encoder, nvencBufInfo->mapInputResource.mappedResource));
    inputMapped = false;
    return;

err:
    if (inputMapped)
        NVENC_API_CALL(ctx->nvenc.nvEncUnmapInputResource(ctx->encoder, nvencBufInfo->mapInputResource.mappedResource));
    uint32_t outBufferSize = 0;
    stream->writeFully(&outBufferSize, 4);
}

void AVCDestroyEncoder(AVCEncCtx context)
{
    AVCEncoderContext* ctx = (AVCEncoderContext*) context;
    AVCDisableDump();
    // send EOS
    NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
    picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    NVENC_API_CALL(ctx->nvenc.nvEncEncodePicture(ctx->encoder, &picParams));

    for (BufferMap_t::iterator it = ctx->bufferMap.begin(); it != ctx->bufferMap.end(); ++it) {
        // unregister input resources
        NVENC_API_CALL(ctx->nvenc.nvEncUnregisterResource(ctx->encoder, it->second->mapInputResource.registeredResource));

        // destroy bitstream buffer
        NVENC_API_CALL(ctx->nvenc.nvEncDestroyBitstreamBuffer(ctx->encoder, it->second->picParams.outputBitstream));

        delete it->second;
        it->second = NULL;
    }

    if (extraEncMsg) {
        free(extraEncMsg->qpDeltaMapArray);
        extraEncMsg->qpDeltaMapArray = NULL;
        free(extraEncMsg);
        extraEncMsg = NULL;
    }

#if TIMERDEBUG
    if (timerid !=0) {
        timer_delete(&timerid);
        timerid = 0;
    }
#endif

    // destroy encoder
    NVENC_API_CALL(ctx->nvenc.nvEncDestroyEncoder(ctx->encoder));

    // untrack encoder
    RenderThreadInfo* const tinfo = RenderThreadInfo::get();
    tinfo->m_avcEncSet.erase(context);

    destroyEGLResources(ctx);
    HDLOGI("AVC encoder destroyed=0x%" PRIx64 "\n", context);
    if (inputFile) {
        fclose(inputFile);
        inputFile = NULL;
    }
    if (outPutFile) {
        fclose(outPutFile);
        outPutFile = NULL;
    }
    
    delete ctx->reconfigParams.reInitEncodeParams.encodeConfig;
    delete ctx;
    ctx = NULL;
}
