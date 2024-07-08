/*
 * Copyright 2021 BlueStack Systems, Inc.
 * All Rights Reserved
 *
 * THIS IS UNPUBLISHED PROPRIETARY SOURCE CODE OF BLUESTACK SYSTEMS, INC.
 * The copyright notice above does not evidence any actual or intended
 * publication of such source code.
 */

#include <dlfcn.h>
#include <map>
#include <sys/time.h>

#include "HwAVCEnc.h"
#include "nvEncodeAPI.h"
#include "ColorBuffer.h"
#include "FrameBuffer.h"
#include "OpenGLESDispatch/EGLDispatch.h"
#include "RenderThreadInfo.h"
#include "PgaServer.h"

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

QpData qpData;
ColorBufferSet avcCbSet;

typedef enum {
    LOW_BITRATE,
    MEDIUM_BITRATE,
    HIGH_BITRATE,
} bitrateCondition;

extern const GLint* getGlesMaxContextAttribs();

typedef struct
{
    NV_ENC_INPUT_RESOURCE_OPENGL_TEX        resource;           // input resource
    NV_ENC_MAP_INPUT_RESOURCE               mapInputResource;
    NV_ENC_PIC_PARAMS                       picParams;
    NV_ENC_LOCK_BITSTREAM                   lockBitstreamData;  // output data
} NvEncBufferInfo;

typedef NVENCSTATUS NVENCAPI (*NvEncodeAPICreateInstance_t)(NV_ENCODE_API_FUNCTION_LIST *functionList);
typedef std::unordered_map<GLuint, NvEncBufferInfo*> BufferMap_t;

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

dynQpDeltaAdjustMsg* dynQpAdjPtr = NULL;
#define BYTES2BITS(a)    ((a)*8)

inline int dynQpDeltaAdjustMsg::elapsedTimeMs(timeval startTime){
    timeval currentTime;
    gettimeofday(&currentTime, NULL);
    int elapsed_time = (currentTime.tv_sec - startTime.tv_sec)*1000.0f +
                         (currentTime.tv_usec - startTime.tv_usec) / 1000.0f;
    return elapsed_time;
 }

dynQpDeltaAdjustMsg::dynQpDeltaAdjustMsg() {
    // init dynamic qpDelta parameters Resource
    dynQpDeltaAdjust_set_mDynQpAdjustReady(false);
    dynQpDeltaAdjust_set_mDynQpAdjustAllowed(false);
    dynQpDeltaAdjust_set_mMinAdjustThreshold(0);
    dynQpDeltaAdjust_set_mMaxAdjustThreshold(12);
    dynQpDeltaAdjust_set_mPrevBrtCondition((int)LOW_BITRATE);
    dynQpDeltaAdjust_set_mQpDeltaMode(REMAIN);
    dynQpDeltaAdjust_set_kStaticPeriodMs(1000lu);
    dynQpDeltaAdjust_set_kLowWaterMarkBits(1000lu * 1000);
    dynQpDeltaAdjust_set_kMediumWaterMarkBits((2000lu * 1000));
    dynQpDeltaAdjust_set_kRatedWaterMarkBits(2500lu * 1000);
    dynQpDeltaAdjust_set_kHighWaterMarkBits(3500lu* 1000);
    dynQpDeltaAdjust_set_kExHighWaterMarkBits(5000lu * 1000);
    dynQpDeltaAdjust_set_kTotalEncodedSizeInBytes(0lu);
    timeval curTime 		 = {0};
    gettimeofday(&curTime, NULL);
    dynQpDeltaAdjust_set_kCalStartTime(curTime);
    //kCheckStartTime = kCalStartTime;
    HDLOGE(":::: %s jayden test dynQpDeltaAdjustMsg, mMinAdjustThreshold: %d, mDynQpAdjustAllowed: %d, LowWaterMarkBits: %d, kStaticPeriodMs: %d, mQpDeltaMode: %d", __FUNCTION__, dynQpDeltaAdjust_get_mMinAdjustThreshold(),
		mDynQpAdjustAllowed, kLowWaterMarkBits, kStaticPeriodMs, mQpDeltaMode);
}

/***
   need to check the network condition before using it, and in order to skip the the game launch and loading phase,
   we should activate it only after the bitrate and fps are stable
that are:
    1: the bitrate is higher than minBitrate for at least 15 times in 1 sec
    2: condition 1 was meet continouly for at least 30 times
    3: if the instance was created for more than 5 mins, that means the loading phase has been skipped, it's also the right timing.
should activate the dynamic qpdelta adjustment algorithm until meet conditions 1 and 2 or condition 3
***/

bool dynQpDeltaAdjustMsg::checkDynQpAdjustAllowed(uint32_t& suitableBrtNumInSec, uint32_t bitrate) {
    bool allowed                         = false;
    static int continousBrtSuitableTimes = 0;
    static timeval nowTimeMs             = kCalStartTime;

    if (elapsedTimeMs(nowTimeMs) > kStaticPeriodMs * 60 * 5) {
        allowed             = true;
        suitableBrtNumInSec = 0;
        HDLOGE(":::: %s jayden test checkDynQpAdjustAllowed 5min arrived!!!!", __FUNCTION__);
        return allowed;
    }

    if (elapsedTimeMs(nowTimeMs) > kStaticPeriodMs) {
        if (suitableBrtNumInSec > mMinFpsRequired) {
/*            HDLOGE("jayden test checkDynQpAdjustAllowed mMinFpsRequired: %d, suitableBrtNumInSec: %d, continousBrtSuitableTimes: %d, bitrate: %d",dynQpDeltaAdjust_get_mMinFpsRequired(), 
                             suitableBrtNumInSec,
                             continousBrtSuitableTimes, bitrate);*/
            continousBrtSuitableTimes++;
            gettimeofday(&nowTimeMs, NULL);
            if (continousBrtSuitableTimes > 30) {
                allowed             = true;
                suitableBrtNumInSec = 0;
                continousBrtSuitableTimes = 0;
                HDLOGE("jayden test checkDynQpAdjustAllowed suitable times found, continousBrtSuitableTimes: %d!!!!", continousBrtSuitableTimes);
                return allowed;
            }
        } else {
            //HDLOGE("jayden test checkDynQpAdjustAllowed suitableBrtNumInSec not meet condition, reset all to 0, suitableBrtNumInSec: %d, bitrate: %d", suitableBrtNumInSec, bitrate);
            continousBrtSuitableTimes = 0;
            gettimeofday(&nowTimeMs, NULL);
        }
        suitableBrtNumInSec = 0;
    }
    return allowed;
}

// if the time of encodedSizeInBits less than kLowWaterMarkBits last for more than 3ms, judge it as true.
bool dynQpDeltaAdjustMsg::isGameScreenMotionless() {
    //static uint64_t prevTotalEncodedSizeInBytes = 0;
    uint32_t encodedSizeInBits  = BYTES2BITS(kTotalEncodedSizeInBytes);
    bool     ret                = false;
    static timeval presentTime  = {0};
    static bool startMarked     = false;
    if (encodedSizeInBits <= kLowWaterMarkBits) {
        if (!startMarked) {
            gettimeofday(&presentTime, NULL);
            startMarked = true;
            HDLOGE(":::: %s jayden test start mark now, EncodedSizeInBits : %d, kLowWaterMarkBits: %d....", __FUNCTION__, encodedSizeInBits, kLowWaterMarkBits);
        }
        if (startMarked && elapsedTimeMs(presentTime) > 3 * kStaticPeriodMs) {
            HDLOGE(":::: %s jayden test end mark now, kStaticPeriodMs: %d.", __FUNCTION__, kStaticPeriodMs);
            ret = true;
        }
    } else {
        startMarked = false;
        ret = false;
    }
    if (ret)
        HDLOGE(":::: %s jayden test return %d!!!!", __FUNCTION__, ret);
    //ret = (EncodedSizeInBits <= kLowWaterMarkBits / 2 && prevTotalEncodedSizeInBytes <= kLowWaterMarkBits / 2 /*|| ratedBrtNumInSec > minFpsRequired*/) ? true : false;
    //prevTotalEncodedSizeInBytes = EncodedSizeInBits;
    return ret;
}

void* dynQpDeltaAdjustMsg::qpDeltaModeSelect(uint32_t encodedSizeInBytes, uint32_t &suitableBrtNumInSec) {
    static qpDeltaMode ope = REMAIN;
    if (elapsedTimeMs(kCalStartTime) >= kStaticPeriodMs) { // using current dynamic qpdelta adjustment strategy at least one second
        kTotalEncodedSizeInBytes   = encodedSizeInBytes;
        uint32_t encodedSizeInBits = BYTES2BITS(kTotalEncodedSizeInBytes);
        bool isMotionless = isGameScreenMotionless();
        HDLOGE(":::: %s jayden test encodedSize %d byte, EncodedSizeInBits: %f Mbit, qpdelta Mode: %d!!!!", __FUNCTION__, encodedSizeInBytes, (float)encodedSizeInBits/(1024*1024), (int)ope);
        mDynQpAdjustReady = true;
        kTotalEncodedSizeInBytes =  0;
        suitableBrtNumInSec      =  0;
        timeval curTime          = {0};
        gettimeofday(&curTime, NULL);
        kCalStartTime = curTime;
        if (encodedSizeInBits > kHighWaterMarkBits) {
            ope = encodedSizeInBits < kExHighWaterMarkBits ? INCREASE_STEADILY: INCREASE_RAPIDLY;
            return &ope;
        } else if ((encodedSizeInBits >= kRatedWaterMarkBits &&
                   encodedSizeInBits <= kHighWaterMarkBits) || isMotionless) {
            ope = REMAIN;
            return &ope;
        } else  {
            ope = encodedSizeInBits >= kMediumWaterMarkBits ? DECREASE_STEADILY: DECREASE_RAPIDLY;
            return &ope;
        }
    } else {
        kTotalEncodedSizeInBytes   = encodedSizeInBytes;
        mDynQpAdjustReady = false;
        ope = REMAIN;
        return &ope;
    }
}

int dynQpDeltaAdjustMsg::qpDeltaOperation(bool bitrateNotJump, int qpDelValue){
    int val          = qpDelValue;
    qpDeltaMode mode = (qpDeltaMode)mQpDeltaMode;
    HDLOGE(":::: %s jayden test qpDeltaOperation, mode: %d, bitrateNotJump: %d, qpDelValue:%d!!!!", __FUNCTION__, mode, bitrateNotJump, qpDelValue);

    if (bitrateNotJump == false || mDynQpAdjustReady == false)
        goto ret;

    switch (mode) {
        case REMAIN:
            goto ret;
        case INCREASE_STEADILY: {
            if (qpDelValue + 1 <= mMaxAdjustThreshold)
                val = qpDelValue + 1;
            return val;
        }
        case INCREASE_RAPIDLY: {
            if (qpDelValue + 2 <= mMaxAdjustThreshold)
                val = qpDelValue + 2;
            return val;
        }
        case DECREASE_STEADILY: {
            if (qpDelValue - 1 >= mMinAdjustThreshold)
                val = qpDelValue - 1;
            return val;
        }
        case DECREASE_RAPIDLY: {
            if (qpDelValue - 2 >= mMinAdjustThreshold)
                val= qpDelValue - 2;
            return val;
        }
        default:
            HDLOGE(":::: %s jayden test invalid qpDeltaOperation!!!!", __FUNCTION__);
            goto ret;
    }
ret:
    HDLOGW(":::: %s jayden test dynamic qpDeltaOperation not used!!!!", __FUNCTION__);
    return val;
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

AVCEncCtx AVCCreateEncoder(int width, int height, int fps, int bitrate)
{
    AVCEncoderContext* ctx = new AVCEncoderContext;
    ctx->width = width;
    ctx->height = height;
    ctx->bitrate = bitrate;
    ctx->minBitrate = bitrate / 2.5;
    ctx->format = NV_ENC_BUFFER_FORMAT_ABGR;

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

    if (qpData.isQpEnabled) {
        ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        HDLOGI("%s: QP is enabled, lowBitQpValue=%d, mediumBitQpValue=%d, highBitQpValue=%d, qpValueOffset=%d\n", __FUNCTION__, qpData.lowBitQpValue, qpData.mediumBitQpValue, qpData.highBitQpValue, qpData.qpValueOffset);

        ctx->reconfigParams.reInitEncodeParams.encodeConfig->profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
        ctx->reconfigParams.reInitEncodeParams.encodeConfig->encodeCodecConfig.h264Config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
        ctx->reconfigParams.reInitEncodeParams.encodeConfig->rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA;

        qpData.widthInMBs  = ((width + 15) & ~15) >> 4;
        qpData.heightInMBs = ((height + 15) & ~15) >> 4;
        qpData.qpDeltaMapArraySize  = qpData.widthInMBs * qpData.heightInMBs;
        qpData.qpDeltaMapArray      = (int8_t*) malloc(qpData.qpDeltaMapArraySize * sizeof(int8_t));
        memset(qpData.qpDeltaMapArray, 0, qpData.qpDeltaMapArraySize);HDLOGE("jayden test create dynQpAdjPtr start, fps: %d", fps>>1);
        dynQpAdjPtr = new dynQpDeltaAdjustMsg;
        dynQpAdjPtr->dynQpDeltaAdjust_set_mMinFpsRequired(fps>>1);
    }
    else {
        HDLOGI("%s: QP is disabled\n", __FUNCTION__);
        ctx->reconfigParams.reInitEncodeParams.encodeConfig->profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    }

    ctx->reconfigParams.reInitEncodeParams.maxEncodeWidth = width;
    ctx->reconfigParams.reInitEncodeParams.maxEncodeHeight = height;
    ctx->reconfigParams.reInitEncodeParams.tuningInfo = AVC_TURING_INFO;

    NVENC_API_CALL_RET(ctx->nvenc.nvEncInitializeEncoder(ctx->encoder, &(ctx->reconfigParams.reInitEncodeParams)), 0);

    HDLOGI("AVC encoder created=0x%" PRIx64 " width=%d height=%d fps=%d bitrate=%d minBitrate=%d\n", (AVCEncCtx)ctx, width, height, fps, bitrate, ctx->minBitrate);
    return (AVCEncCtx) ctx;
}

static void RegionOfInterestOpt(int mainRegionValue, int otherRegionValue, bool& centralOptimization) {
    if (qpData.qpValueOffset == 0 || mainRegionValue == otherRegionValue) {
        memset(qpData.qpDeltaMapArray, mainRegionValue, qpData.qpDeltaMapArraySize);
        return;
    }

    if (centralOptimization) { // central region optimization
        for (uint32_t i = 0; i < qpData.heightInMBs; i++) {
            for (uint32_t j = 0; j < qpData.widthInMBs; j++) {
                if (( i > qpData.heightInMBs / 4 && i < qpData.heightInMBs * 3 / 4)  && (j > qpData.widthInMBs / 4 && j < qpData.widthInMBs * 3 / 4)) {
                    qpData.qpDeltaMapArray[i* qpData.widthInMBs + j] = mainRegionValue;
                } else {
                    qpData.qpDeltaMapArray[i* qpData.widthInMBs + j] = otherRegionValue;
                }
            }
        }
        centralOptimization = false;
    } else { // surrounding region optimization
        for (uint32_t i = 0; i < qpData.heightInMBs; i++) {
            for (uint32_t j = 0; j < qpData.widthInMBs; j++) {
                if ( (i < qpData.heightInMBs / 4 || i > qpData.heightInMBs * 3 / 4 ) || (j < qpData.widthInMBs / 4 || j > qpData.widthInMBs * 3 / 4)) {
                    qpData.qpDeltaMapArray[i* qpData.widthInMBs + j] = mainRegionValue;
                } else {
                    qpData.qpDeltaMapArray[i* qpData.widthInMBs + j] = otherRegionValue;
                }
            }
        }
        centralOptimization = true;
    }

    return;
}

static void useQpdeltaStrategy(NvEncBufferInfo* nvencBufInfo, uint32_t bitrate) {
    if (!nvencBufInfo) {
        HDLOGE(":::: %s invalid, nvencBufInfo ptr: %p", __FUNCTION__, nvencBufInfo);
        return;
    }

    static bool centralOptimization = true;
    bitrateCondition bc;
    bc = (bitrate > 2000000) ? ( bitrate >= 2500000 ? HIGH_BITRATE : MEDIUM_BITRATE) : LOW_BITRATE;

    nvencBufInfo->picParams.qpDeltaMapSize = qpData.qpDeltaMapArraySize;

    switch (bc) {
        case HIGH_BITRATE:
            if (dynQpAdjPtr != nullptr){
                // adjusting based on the prev state of HIGH_BITRATE
                int curHighQpValue = qpData.highBitQpValue;
                if (dynQpAdjPtr->dynQpDeltaAdjust_get_mDynQpAdjustReady()) {
                    bitrateCondition prevBrtCond = (bitrateCondition)dynQpAdjPtr->dynQpDeltaAdjust_get_mPrevBrtCondition();
                    qpData.highBitQpValue = dynQpAdjPtr->qpDeltaOperation(prevBrtCond == bc, curHighQpValue);
                    HDLOGE(":::: %s jayden test, after qpDeltaOperation in high bit case, highBitQpValue: %d, mediumBitQpValue: %d, lowBitQpValue: %d",
						  __FUNCTION__, qpData.highBitQpValue, qpData.mediumBitQpValue, qpData.lowBitQpValue);
                    dynQpAdjPtr->dynQpDeltaAdjust_set_mDynQpAdjustReady(false);
                }
            }
            RegionOfInterestOpt(qpData.highBitQpValue, qpData.highBitQpValue*1.2, centralOptimization);
            nvencBufInfo->picParams.qpDeltaMap = qpData.qpDeltaMapArray;
            break;
        case MEDIUM_BITRATE:
            if (dynQpAdjPtr != nullptr){
                // adjusting based on the prev state of MEDIUM_BITRATE
                int curMediumQpValue = qpData.mediumBitQpValue;
                if (dynQpAdjPtr->dynQpDeltaAdjust_get_mDynQpAdjustReady()) {
                    bitrateCondition prevBrtCond = (bitrateCondition)dynQpAdjPtr->dynQpDeltaAdjust_get_mPrevBrtCondition();
                    qpData.mediumBitQpValue = dynQpAdjPtr->qpDeltaOperation(prevBrtCond == bc, curMediumQpValue);
                    dynQpAdjPtr->dynQpDeltaAdjust_set_mDynQpAdjustReady(false);
                    HDLOGE(":::: %s jayden test, after qpDeltaOperation in medium bit case, highBitQpValue: %d, mediumBitQpValue: %d, lowBitQpValue: %d",
                                               __FUNCTION__, qpData.highBitQpValue, qpData.mediumBitQpValue, qpData.lowBitQpValue);
                }
            }
            RegionOfInterestOpt(qpData.mediumBitQpValue - qpData.qpValueOffset , qpData.mediumBitQpValue + qpData.qpValueOffset, centralOptimization);
            nvencBufInfo->picParams.qpDeltaMap  = qpData.qpDeltaMapArray;
            break;
        case LOW_BITRATE:
            if (dynQpAdjPtr != nullptr){
                // adjusting based on the prev state of LOW_BITRATE
                int curLowQpValue = qpData.lowBitQpValue;
                if (dynQpAdjPtr->dynQpDeltaAdjust_get_mDynQpAdjustReady()) {
                    bitrateCondition prevBrtCond = (bitrateCondition)dynQpAdjPtr->dynQpDeltaAdjust_get_mPrevBrtCondition();
                    qpData.lowBitQpValue = dynQpAdjPtr->qpDeltaOperation(prevBrtCond == bc, curLowQpValue);
                    dynQpAdjPtr->dynQpDeltaAdjust_set_mDynQpAdjustReady(false);
                    HDLOGE(":::: %s jayden test, after qpDeltaOperation in low bit case, highBitQpValue: %d, mediumBitQpValue: %d, lowBitQpValue: %d",
                                        __FUNCTION__, qpData.highBitQpValue, qpData.mediumBitQpValue, qpData.lowBitQpValue);
                }
            }
            RegionOfInterestOpt(qpData.lowBitQpValue - qpData.qpValueOffset, qpData.lowBitQpValue + qpData.qpValueOffset, centralOptimization);
            nvencBufInfo->picParams.qpDeltaMap  = qpData.qpDeltaMapArray;
            break;
        default:
            HDLOGE(":::: %s invalid qpdelta mode setting!!!", __FUNCTION__);
            break;
    }
	if (dynQpAdjPtr != nullptr) dynQpAdjPtr->dynQpDeltaAdjust_set_mPrevBrtCondition((int)bc);
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
    GLuint tex;

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

    tex = cb->getEGLTexture();
    it = ctx->bufferMap.find(tex);
    if (it == ctx->bufferMap.end()) {
        nvencBufInfo = new NvEncBufferInfo;

        // input is stored in texture backing buffer, register it
        if (!AVCPrepareIOBuffers(ctx, nvencBufInfo, tex))
            goto err;

        ctx->bufferMap.insert(std::pair<GLuint, NvEncBufferInfo*> (tex, nvencBufInfo));
        avcCbSet.insert(colorBuffer);
    }
    else
        nvencBufInfo = it->second;

    if (qpData.isQpEnabled)
        useQpdeltaStrategy(nvencBufInfo, bitrate);

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

    if (qpData.isQpEnabled && dynQpAdjPtr != nullptr) {
        static uint32_t suitableBrtNumInSec = 0;
        if (bitrate > dynQpAdjPtr->dynQpDeltaAdjust_get_kLowWaterMarkBits())
            suitableBrtNumInSec++;
        // step 1: check dynamic qpdelta algorithm allowed, then reset the mCalStartTime;
        if (!dynQpAdjPtr->dynQpDeltaAdjust_get_mDynQpAdjustAllowed()) {
            if (dynQpAdjPtr->checkDynQpAdjustAllowed(suitableBrtNumInSec, bitrate)) {
                dynQpAdjPtr->dynQpDeltaAdjust_set_mDynQpAdjustAllowed(true);
                timeval curTime = {0};
                gettimeofday(&curTime, NULL);
                dynQpAdjPtr->dynQpDeltaAdjust_set_kCalStartTime(curTime);
            }
        }
        // step 2: calculate EncodedSize and select qpDeltaMode
        if(dynQpAdjPtr->dynQpDeltaAdjust_get_mDynQpAdjustAllowed()) {
            uint32_t  encodedSize = dynQpAdjPtr->dynQpDeltaAdjust_get_kTotalEncodedSizeInBytes();
            encodedSize += nvencBufInfo->lockBitstreamData.bitstreamSizeInBytes;
            int* mode = reinterpret_cast<int*>(dynQpAdjPtr->qpDeltaModeSelect(encodedSize, suitableBrtNumInSec));
            if (suitableBrtNumInSec == 0 && bitrate > dynQpAdjPtr->dynQpDeltaAdjust_get_kLowWaterMarkBits())
                HDLOGE("jayden test after 1.0sec qpDeltaModeSelect bitrate: %d, kTotalEncodedSizeInByte: %d, qpmode select: %d", bitrate, encodedSize, *mode);
            dynQpAdjPtr->dynQpDeltaAdjust_set_mQpDeltaMode(*mode);
        }
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

    // Disable the OSD
    osdInfo.OSDEnabled = 0;

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
    avcCbSet.clear();

    if (qpData.isQpEnabled) {
        free(qpData.qpDeltaMapArray);
        qpData.qpDeltaMapArray = NULL;
        qpData.isQpEnabled = false;
        if (dynQpAdjPtr) {
           delete dynQpAdjPtr;
           dynQpAdjPtr = NULL;
        }
    }

    // destroy encoder
    NVENC_API_CALL(ctx->nvenc.nvEncDestroyEncoder(ctx->encoder));

    // untrack encoder
    RenderThreadInfo* const tinfo = RenderThreadInfo::get();
    tinfo->m_avcEncSet.erase(context);

    destroyEGLResources(ctx);
    HDLOGI("AVC encoder destroyed=0x%" PRIx64 "\n", context);

    delete ctx->reconfigParams.reInitEncodeParams.encodeConfig;
    delete ctx;
    ctx = NULL;
}
