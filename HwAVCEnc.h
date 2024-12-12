/*
 * Copyright (C) 2021 BlueStack Systems, Inc.
 * All Rights Reserved
 *
 * THIS IS UNPUBLISHED PROPRIETARY SOURCE CODE OF BLUESTACK SYSTEMS, INC.
 * The copyright notice above does not evidence any actual or intended
 * publication of such source code.
 */

#ifndef _HW_AVC_ENC_H_
#define _HW_AVC_ENC_H_

#include "avc_common.h"
#include "IOStream.h"

typedef struct {
    bool     isQpEnabled;
    uint32_t widthInMBs;
    uint32_t heightInMBs;
    uint32_t qpDeltaMapArraySize;
    int8_t*  qpDeltaMapArray;
    int      lowBitQpValue;
    int      mediumBitQpValue;
    int      highBitQpValue;
    int      qpValueOffset;
    bool isDynamicMode() const {
        return lowBitQpValue == 0 && mediumBitQpValue == 0 && highBitQpValue== 0 && qpValueOffset== 0;
    }
} QpData;

AVCEncCtx AVCCreateEncoder(int codec, int width, int height, int fps, int bitrate);
void AVCEncodeBuffer(AVCEncCtx context, uint32_t colorBuffer, uint64_t inTimestamp, int reqIDRFrame, IOStream *stream, uint32_t bitrate);
void AVCDestroyEncoder(AVCEncCtx context);

#define MEMBER_REFLECT_ACCESSORS(type, field) \
	 type dynQpDeltaAdjust_get_##field() \
	 { \
         return field; \
	 } \
	 void dynQpDeltaAdjust_set_##field(type v) \
	 { \
         field = v; \
	 }

class dynQpDeltaAdjustMsg final {
  public:
    dynQpDeltaAdjustMsg(void* encMsgPtr);
    ~dynQpDeltaAdjustMsg() = default;
    bool        isGameScreenMotionless();
    bool        checkDynQpAdjustAllowed(uint32_t& suitableBrtNumInSec, uint32_t bitrate);
    void*       qpDeltaModeSelect(uint32_t encodedSizeInBytes, uint32_t &suitableBrtNumInSec);
    int         qpDeltaOperation(bool bitrateNotJump, int qpValue);
    inline int  elapsedTimeMs(timeval startTime);
    MEMBER_REFLECT_ACCESSORS(bool,     mDynQpAdjustReady)
    MEMBER_REFLECT_ACCESSORS(bool,     mDynQpAdjustAllowed)
    MEMBER_REFLECT_ACCESSORS(int,      mMinAdjustThreshold)
    MEMBER_REFLECT_ACCESSORS(int,      mMaxAdjustThreshold)
    MEMBER_REFLECT_ACCESSORS(int,      mMinFpsRequired)
    MEMBER_REFLECT_ACCESSORS(int,      mPrevBrtCondition)
    MEMBER_REFLECT_ACCESSORS(int,      mQpDeltaMode)
    MEMBER_REFLECT_ACCESSORS(uint32_t, kStaticPeriodMs)
    MEMBER_REFLECT_ACCESSORS(uint32_t, kLowWaterMarkBits)
    MEMBER_REFLECT_ACCESSORS(uint32_t, kMediumWaterMarkBits)
    MEMBER_REFLECT_ACCESSORS(uint32_t, kRatedWaterMarkBits)
    MEMBER_REFLECT_ACCESSORS(uint32_t, kHighWaterMarkBits)
    MEMBER_REFLECT_ACCESSORS(uint32_t, kExHighWaterMarkBits)
    MEMBER_REFLECT_ACCESSORS(uint32_t, kTotalEncodedSizeInBytes)
    MEMBER_REFLECT_ACCESSORS(timeval,  kCalStartTime)

 private:
    enum qpDeltaMode {
        REMAIN = 0,
        INCREASE_STEADILY,
        INCREASE_RAPIDLY,
        DECREASE_STEADILY,
        DECREASE_RAPIDLY,
    };
    bool        mDynQpAdjustReady;
    bool        mDynQpAdjustAllowed;
    int         mMinAdjustThreshold;
    int         mMaxAdjustThreshold;
    int         mMinFpsRequired;
    int         mPrevBrtCondition;
    int         mQpDeltaMode;
    uint32_t    kStaticPeriodMs;
    uint32_t    kLowWaterMarkBits;
    uint32_t    kMediumWaterMarkBits;
    uint32_t    kRatedWaterMarkBits;
    uint32_t    kHighWaterMarkBits;
    uint32_t    kExHighWaterMarkBits;
    uint32_t    kTotalEncodedSizeInBytes;
    timeval     kCalStartTime;

    dynQpDeltaAdjustMsg(const dynQpDeltaAdjustMsg& dyn);
    dynQpDeltaAdjustMsg& operator=(const dynQpDeltaAdjustMsg& dyn);
};

#endif  /* #ifndef _HW_AVC_ENC_H_ */
