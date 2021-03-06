/*****************************************************************************
*                     C O M P I L E R   F L A G S
******************************************************************************
*/

#define LOG_TAG  "AudioVoiceUIDL"
//#define DUMP_VPW_StreamIn_DATA
//#define forUT
//#define ENABLE_LOG_VPWStreamIn
#ifdef ENABLE_LOG_VPWStreamIn
#undef ALOGV
#define ALOGV(...) ALOGD(__VA_ARGS__)
#endif

/*****************************************************************************
*                E X T E R N A L   R E F E R E N C E S
******************************************************************************
*/
#define MTK_LOG_ENABLE 1
#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <utils/Log.h>
#include <utils/String8.h>
#include <utils/threads.h>


#include <cutils/properties.h>


#include "AudioType.h"
#include "audio_custom_exp.h"
#include "AudioVUnlockDL.h"
#include <log/log.h>
#include <cutils/ashmem.h>

#define RTPM_PRIO_AUDIO_I2S 90

/*****************************************************************************
*                         D A T A   T Y P E S
******************************************************************************
*/

/*****************************************************************************
*                          C O N S T A N T S
******************************************************************************
*/

/*****************************************************************************
*                        C L A S S   D E F I N I T I O N
******************************************************************************
*/
namespace android {

static pthread_mutex_t mVUnlockReadMutex;
#ifdef forUT
static pthread_mutex_t mVUnlockWriteMutex;
#endif
AudioVUnlockDL *UniqueVUnlockDLInstance = NULL;

static long long getTimeMs() {
    struct timeval t1;
    long long ms;

    gettimeofday(&t1, NULL);
    ms = t1.tv_sec * 1000LL + t1.tv_usec / 1000;

    return ms;
}
#if 0
class VUnlockSharedBuffer : public BnMemory {
public:
    VUnlockSharedBuffer() {
        ALOGV("+VUnlockSharedBuffer");
        sp<MemoryHeapBase> heap = new MemoryHeapBase(1024, 0, "VUnlockSharedBuffer");
        if (heap != NULL) {
            mMemory = new MemoryBase(heap, 0, 1024);
            int32 *data = (int32_t *)mMemory->pointer();
            if (data != NULL) {
                *data = 0;
            }
        }
        ALOGV("-VUnlockSharedBuffer");
    }
    virtual ~VUnlockSharedBuffer() {
        mMemory = NULL;
    }
    static void instantiate() {
        ALOGV("+VUnlockSharedBuffer::instantiate");
        defaultServiceManager()->addService(String16("VUnlock_SHARED_BUFFER_SERVICE"), new VUnlockSharedBuffer());
        ProcessState::self()->startThreadPool();
        IPCThreadState::self()->joinThreadPool();
        ALOGV("-VUnlockSharedBuffer::instantiate");
    }
    virtual sp<IMemory> getMemory() {
        return mMemory;
    }
private:
    sp<MemoryBase> mMemory;

};
#endif
AudioVUnlockRingBuf::AudioVUnlockRingBuf() {
    int ret = 0;

    ret = pthread_mutex_init(&mBufMutex, NULL);
    if (ret != 0) {
        ALOGI("Failed to initialize AudioVUnlockRingBuf mBufMutex!");
    }

    ret = pthread_cond_init(&mBuf_Cond, NULL);
    if (ret != 0) {
        ALOGI("Failed to initialize AudioVUnlockRingBuf mBuf_Cond!");
    }

    //mbufAddr = new char[VOICE_UNLOCK_RING_BUFFER_SIZE];
    memset(mbufAddr, 0, VOICE_UNLOCK_RING_BUFFER_SIZE);
    mbuf.pBufBase = mbufAddr;
    mbuf.pRead = mbufAddr;
    mbuf.pWrite = mbufAddr;
    mbuf.pBufEnd = mbufAddr + VOICE_UNLOCK_RING_BUFFER_SIZE;
    mbuf.bufLen = VOICE_UNLOCK_RING_BUFFER_SIZE;
    mbuf.buffull = false;
}
AudioVUnlockRingBuf::~AudioVUnlockRingBuf() {
}
uint32_t AudioVUnlockRingBuf:: WriteAdvance(void *buf, uint32_t datasz) {
    int32_t leftspace;
    uint32_t datawritten = 0;
    pthread_mutex_lock(&mBufMutex);
    //SXLOGD("[WriteAdvance],start mbuf.pWrite %x, mbuf.pRead %x ", mbuf.pWrite,mbuf.pRead);
    if (mbuf.pWrite == mbuf.pRead) {
        if (!mbuf.buffull) {
            leftspace = mbuf.bufLen;
        } else {
            leftspace = 0;
        }
    } else {
        leftspace = (mbuf.pRead - mbuf.pWrite);
        if (leftspace < 0) {
            leftspace = mbuf.bufLen + leftspace;
        }
    }
    datasz = ((uint32_t)leftspace) < datasz ? ((uint32_t)leftspace) : datasz;
    if ((mbuf.pWrite + datasz) < mbuf.pBufEnd) {
        memcpy(mbuf.pWrite, buf, datasz);
        datawritten += datasz;
        mbuf.pWrite += datasz;
    } else {
        memcpy(mbuf.pWrite, buf, (mbuf.pBufEnd - mbuf.pWrite));
        memcpy(mbuf.pBufBase, (char *)buf + (mbuf.pBufEnd - mbuf.pWrite), datasz - (mbuf.pBufEnd - mbuf.pWrite));
        datawritten += datasz;
        mbuf.pWrite = mbuf.pBufBase + (datasz - (mbuf.pBufEnd - mbuf.pWrite)) ;
    }
    if ((mbuf.pWrite == mbuf.pRead) && datawritten != 0) {
        mbuf.buffull = 1;
    }

    //SXLOGD("[WriteAdvance] Write offset  = %d" ,mbuf.pWrite -mbuf.pBufBase );
    //SXLOGD("[WriteAdvance] read offset  = %d" ,mbuf.pRead -mbuf.pBufBase );
    pthread_mutex_unlock(&mBufMutex);
    return datawritten;
}

int32_t AudioVUnlockRingBuf:: Write(void *buf, uint32_t datasz) {
    uint32_t leftsz = datasz;
    char *bufptr = (char *)buf;
    uint32_t loopCount = 0;
    int32_t writeAmount = 0;
    if (buf == NULL) {
        return -1;
    }
    while (leftsz > 0) {
        int32_t datawritten;
        datawritten = WriteAdvance(bufptr, leftsz);
        leftsz -= datawritten;
        bufptr += datawritten;
        writeAmount += datawritten;
        if (leftsz != 0) {
            // sleep 5ms
            usleep(1 * 1000);
        }

        loopCount ++;
        if (loopCount == 5 && leftsz != 0) {
            ALOGD("[AudioVUnlockRingBuf:: Write] Drop Stream out data! data droped %d , write %d", leftsz, writeAmount);
            break;
        }
    }

    //SXLOGD("[AudioVUnlockRingBuf:: Write] loopCount %d, %d", loopCount, writeAmount);
    return writeAmount;
}

uint32_t AudioVUnlockRingBuf:: AdvanceReadPointer(uint32_t datasz) {
    pthread_mutex_lock(&mBufMutex);

    if ((mbuf.pRead + datasz) < mbuf.pBufEnd) {
        mbuf.pRead += datasz;
    } else {
        mbuf.pRead = mbuf.pBufBase + (datasz - (mbuf.pBufEnd - mbuf.pRead)) ;
    }
    if (mbuf.pRead == mbuf.pWrite && datasz != 0) {
        mbuf.buffull = 0;
    }

    //SXLOGD("[AdvanceReadPointer] Write offset  = %d" ,mbuf.pWrite -mbuf.pBufBase );
    //SXLOGD("[AdvanceReadPointer] read offset  = %d" ,mbuf.pRead -mbuf.pBufBase );
    pthread_mutex_unlock(&mBufMutex);
    return 0;
}

uint32_t AudioVUnlockRingBuf:: ReadWithoutAdvance(void *buf, uint32_t datasz) {
    int32_t leftdata;
    uint32_t dataread = 0;
    if (buf == NULL || datasz == 0) {
        return 0;
    }
    pthread_mutex_lock(&mBufMutex);
    if (mbuf.pWrite == mbuf.pRead) {
        if (!mbuf.buffull) {
            leftdata = 0;
        } else {
            leftdata =  mbuf.bufLen;
        }
    } else {
        leftdata = (mbuf.pWrite - mbuf.pRead);
        if (leftdata < 0) {
            leftdata = mbuf.bufLen + leftdata;
        }
    }

    datasz = ((uint32_t)leftdata) < datasz ? ((uint32_t)leftdata) : datasz;

    if ((mbuf.pRead + datasz) < mbuf.pBufEnd) {
        memcpy(buf, mbuf.pRead, datasz);
        dataread += datasz;
    } else {
        memcpy(buf, mbuf.pRead, (mbuf.pBufEnd - mbuf.pRead));
        memcpy((char *)buf + (mbuf.pBufEnd - mbuf.pRead), mbuf.pBufBase, datasz - (mbuf.pBufEnd - mbuf.pRead));
        dataread += datasz;
    }

    //SXLOGD("[ReadWithoutAdvance] write offset  = %d" ,mbuf.pWrite -mbuf.pBufBase );
    //SXLOGD("[ReadWithoutAdvance] read offset  = %d" ,mbuf.pRead -mbuf.pBufBase );
    pthread_mutex_unlock(&mBufMutex);
    return dataread;
}

uint32_t AudioVUnlockRingBuf:: ReadAdvance(void *buf, uint32_t datasz) {
    int32_t leftdata;
    uint32_t dataread = 0;
    pthread_mutex_lock(&mBufMutex);
    if (mbuf.pWrite == mbuf.pRead) {
        if (!mbuf.buffull) {
            leftdata = 0;
        } else {
            leftdata =  mbuf.bufLen;
        }
    } else {
        leftdata = (mbuf.pWrite - mbuf.pRead);
        if (leftdata < 0) {
            leftdata = mbuf.bufLen + leftdata;
        }
    }

    datasz = ((uint32_t)leftdata) < datasz ? ((uint32_t)leftdata) : datasz;
    if ((mbuf.pRead + datasz) < mbuf.pBufEnd) {
        memcpy(buf, mbuf.pRead, datasz);
        dataread += datasz;
        mbuf.pRead += datasz;
    } else {
        memcpy(buf, mbuf.pRead, (mbuf.pBufEnd - mbuf.pRead));
        memcpy((char *)buf + (mbuf.pBufEnd - mbuf.pRead), mbuf.pBufBase, datasz - (mbuf.pBufEnd - mbuf.pRead));
        dataread += datasz;
        mbuf.pRead = mbuf.pBufBase + (datasz - (mbuf.pBufEnd - mbuf.pRead)) ;
    }
    if ((mbuf.pRead == mbuf.pWrite) && dataread != 0) {
        mbuf.buffull = 0;
    }

    //SXLOGD("[ReadAdvance] write offset  = %d" ,mbuf.pWrite -mbuf.pBufBase );
    //SXLOGD("[ReadAdvance] read offset  = %d" ,mbuf.pRead -mbuf.pBufBase );
    pthread_mutex_unlock(&mBufMutex);
    return dataread;
}

int32_t AudioVUnlockRingBuf:: Read(void *buf, uint32_t datasz) {
    uint32_t leftsz =  datasz;
    char *bufptr = (char *) buf;
    uint32_t loopCount = 0;
    int32_t ret = 0;
    int32_t readAmount = 0;
    if (buf == NULL) {
        return -1;
    }

    while (leftsz > 0) {
        uint32_t dataread;
        dataread = ReadAdvance(bufptr, leftsz);
        leftsz -= dataread;
        bufptr += dataread;
        readAmount += dataread;
        if (leftsz) {
            //sleep 5ms
            usleep(5 * 1000);
        }
        loopCount ++;
        if (loopCount == 10 && leftsz != 0) {
            int32_t ret = -1;
            break;
        }
        ret = readAmount;
    }
    return ret;
}
uint32_t AudioVUnlockRingBuf:: GetBuflength(void) {
    return mbuf.bufLen;
}

uint32_t AudioVUnlockRingBuf:: GetBufDataSz(void) {
    int32_t leftdata = 0;
    pthread_mutex_lock(&mBufMutex);

    //SXLOGD("[GetBufDataSz], mbuf.pWrite %x, mbuf.pRead %x ", mbuf.pWrite,mbuf.pRead);
    if (mbuf.pWrite == mbuf.pRead) {
        if (!mbuf.buffull) {
            leftdata = 0;
        } else {
            leftdata =  mbuf.bufLen;
        }
    } else {
        leftdata = (mbuf.pWrite - mbuf.pRead);
        //SXLOGD("[GetBufDataSz], leftdata %d ",leftdata);
        if (leftdata < 0) {
            leftdata += mbuf.bufLen ;
        }
    }
    pthread_mutex_unlock(&mBufMutex);
    return (uint32_t)leftdata;
}
uint32_t AudioVUnlockRingBuf:: WaitBufData(void) {
    int32_t leftdata = 0;
    pthread_mutex_lock(&mBufMutex);
    if (mbuf.pWrite == mbuf.pRead) {
        if (!mbuf.buffull) {
            leftdata = 0;
        } else {
            leftdata =  mbuf.bufLen;
        }
    } else {
        leftdata = (mbuf.pWrite - mbuf.pRead);
        if (leftdata < 0) {
            leftdata = mbuf.bufLen + leftdata;
        }
    }
    /*change conditiont to  leftdata <=2, since SRC won't work if leftdata <=2*/
    //if(leftdata == 0) // wait till buffer filled
    if (leftdata <= 0) { // wait till buffer has 92 ms data.
        ALOGV("[WaitBufData]Buffer empty , wait %d", leftdata);
        pthread_cond_wait(&mBuf_Cond, &mBufMutex);
    }
    pthread_mutex_unlock(&mBufMutex);
    return (uint32_t)leftdata;
}

uint32_t AudioVUnlockRingBuf:: SignalBufData(void) {
    int32_t leftdata = 0;
    //pthread_mutex_lock( &mBufMutex);SignalBufData
    pthread_cond_broadcast(&mBuf_Cond);
    //SXLOGV("[SignalBufData] SignalBufData, mBuf_Cond %d", mBuf_Cond.value);
    //pthread_mutex_unlock( &mBufMutex);
    return (uint32_t)leftdata;
}

uint32_t AudioVUnlockRingBuf:: GetBufSpace(void) {
    uint32_t leftspace = 0;

    pthread_mutex_lock(&mBufMutex);
    if (mbuf.pWrite == mbuf.pRead) {
        if (!mbuf.buffull) {
            leftspace = mbuf.bufLen;
        } else {
            leftspace = 0;
        }
    } else if (mbuf.pWrite > mbuf.pRead) {
        leftspace = mbuf.bufLen - (mbuf.pWrite - mbuf.pRead);
    } else {
        leftspace = mbuf.pRead - mbuf.pWrite;
    }

    pthread_mutex_unlock(&mBufMutex);
    return leftspace;
}
uint32_t AudioVUnlockRingBuf:: ResetBuf() {

    pthread_mutex_lock(&mBufMutex);
    memset(mbufAddr, 0, mbuf.bufLen);
    mbuf.pBufBase = mbufAddr;
    mbuf.pRead = mbufAddr;
    mbuf.pWrite = mbufAddr;
    mbuf.buffull = false;
    pthread_mutex_unlock(&mBufMutex);
    return 0;
}

//#define WRITEOVER
void *ReadRoutine(void *hdl __unused) {
    pthread_detach(pthread_self());

    AudioVUnlockRingBuf *ringBuf_in;
    AudioVUnlockRingBuf *ringBuf_out;
    int32_t readAmount;
    int32_t rwfailcount = 0;
    AudioVUnlockDL *VInstance = AudioVUnlockDL::getInstance();
    char *tempbuffer  = new char[VOICE_UNLOCK_RING_BUFFER_SIZE];
    char *tempSRCbuffer = new char[6000];
    uint32_t dataread = 0;
    int32_t datawritten = 0;
    ALOGV(" [ReadRoutine] start");
    // defaut setting of thread init
    pthread_mutex_lock(&mVUnlockReadMutex);
    VInstance->mReadThreadExit = false;
    VInstance->mReadThreadActive = true;
    pthread_mutex_unlock(&mVUnlockReadMutex);

    ringBuf_in = (AudioVUnlockRingBuf *) & (VInstance->mRingBufIn);
    ringBuf_out = (AudioVUnlockRingBuf *) & (VInstance->mRingBufOut);

    if (VInstance == NULL || ringBuf_in == NULL || ringBuf_out == NULL) {
        ALOGD(" [ReadRoutine] Buffer NULL,  Exit");
        pthread_mutex_lock(&mVUnlockReadMutex);
        VInstance->mReadThreadExit = true;
        VInstance->mReadThreadActive = false;
        pthread_mutex_unlock(&mVUnlockReadMutex);
    }

#ifdef MTK_AUDIO_ADJUST_PRIORITY
    int result = -1;
    // if set priority false , force to set priority
    if (result == -1) {
        struct sched_param sched_p;
        sched_getparam(0, &sched_p);
        sched_p.sched_priority = RTPM_PRIO_AUDIO_I2S;
        if (0 != sched_setscheduler(0, SCHED_RR, &sched_p)) {
            ALOGI("[%s] failed, errno: %d", __func__, errno);
        } else {
            sched_p.sched_priority = RTPM_PRIO_AUDIO_I2S;
            sched_getparam(0, &sched_p);
            ALOGV("sched_setscheduler ok, priority: %d", sched_p.sched_priority);
        }
    }
#endif

    while (!VInstance->mReadThreadExit) {
        // Clear DL time if remaining data is pushed.
        if (VInstance->mDLtime.tv_nsec != 0 && VInstance->mNewDLtime.tv_sec  != 0) {
            if (VInstance->mInRemaining == 0 && VInstance->mOutRemaining == 0) {
                VInstance->mDLtime.tv_sec  = 0;
                VInstance->mDLtime.tv_nsec = 0;
                //SXLOGV("[ReadRoutine] All Data from last playback pushed");
            }
        }
        // switch DL time to new DL time
        if (VInstance->mDLtime.tv_sec == 0 && VInstance->mNewDLtime.tv_sec  != 0) {
            VInstance->mDLtime.tv_sec = VInstance->mNewDLtime.tv_sec ;
            VInstance->mDLtime.tv_nsec = VInstance->mNewDLtime.tv_nsec;
            VInstance->mNewDLtime.tv_sec  = 0;
            VInstance->mNewDLtime.tv_nsec = 0;
            ALOGV("[ReadRoutine] switch to new DL time %ld %ld", VInstance->mDLtime.tv_sec, VInstance->mDLtime.tv_nsec);
        }

        uint32_t readsz = ringBuf_in->GetBufDataSz();
        dataread = ringBuf_in->ReadWithoutAdvance(tempbuffer, readsz);
        if (VInstance->mInRemaining != 0) {
            dataread = dataread > VInstance->mInRemaining ? VInstance->mInRemaining : dataread;
        }
        if (dataread <= 0 || VInstance->mDLtime.tv_sec == 0) { // do not read data if mDLtime is ready.
            if (VInstance->StreamOutStandBy() && ringBuf_out->GetBufDataSz() == 0) {
                //SXLOGV("[ReadRoutine]  Standby, and all data from last playback is pushed");
                VInstance->mDLtime.tv_sec = 0;
                VInstance->mDLtime.tv_nsec = 0;
            }
            //SXLOGV("[ReadRoutine] No data from stream out, sleep");
            usleep(30 * 1000);
        } else {
            //do SRC
            uint32_t dataproduced = ringBuf_out->GetBufSpace();
            dataproduced = dataproduced > 6000 ? 6000 : dataproduced;
            int32_t dataConsumed = VInstance->DoSRC((uint8_t *) tempbuffer, &dataread, (uint8_t *) tempSRCbuffer, &dataproduced);

#ifdef DUMP_VPW_StreamIn_DATA
            //VInstance->DumpData(tempSRCbuffer, dataproduced);
            if (VInstance->mOutFile_1 != NULL) {
                VInstance->mOutFile_1 = fopen("/data/vendor/audiohal/audio_dump/VPW_SRC.pcm", "ab+");
                fwrite(tempSRCbuffer, sizeof(char), dataproduced, VInstance->mOutFile_1);
                fclose(VInstance->mOutFile_1);
            }
#endif
            if (dataConsumed > 0) {
                datawritten = ringBuf_out->Write(tempSRCbuffer, dataproduced);
                ALOGV("[ReadRoutine] write to ring out, datawritten %d", datawritten);
            } else {
                datawritten = 0;
            }
            if (datawritten <= 0) { //  write fail
                rwfailcount++;
                if (rwfailcount > 100) {
                    ALOGV("[ReadRoutine] Fail, write fail");
                    break;
                }
                ALOGV("[ReadRoutine] No space to write, sleep");
                usleep(10 * 1000); //10ms
            } else { //advance read pointer according to data actually write to output buffer
                ringBuf_in->AdvanceReadPointer((uint32_t)dataConsumed);
                if (VInstance->mInRemaining != 0) {
                    ALOGV("[ReadRoutine] last playback in buffer remaining %d", VInstance->mInRemaining);
                    VInstance->mInRemaining -= dataConsumed;
                    VInstance->mOutRemaining +=  datawritten;
                }
                ringBuf_out->SignalBufData();
                ALOGV("[ReadRoutine] advance Read poniter %d ", (uint32_t)dataConsumed);
                rwfailcount = 0;
            }
        }
    }
    delete[] tempbuffer;
    delete[] tempSRCbuffer;
    ALOGV("[ReadRoutine] exit ");

    VInstance->ClearState(VPWStreamIn_READ_START);

    ALOGV("stop and signal ReadRefFromRing to stop");
    ringBuf_out->SignalBufData();

    int cnt_val = 50;
    while ((VInstance->mReadFunctionActive == true) && (cnt_val > 0)) {
        ALOGD("[ReadRoutine]Signal ReadRefFromRing to stop and wait (%d) ", cnt_val);
        cnt_val--;
        ringBuf_out->SignalBufData();
        usleep(1000 * 10);
    }

    pthread_mutex_lock(&mVUnlockReadMutex);
    VInstance->mReadThreadExit = true;
    VInstance->mReadThreadActive = false;
    pthread_mutex_unlock(&mVUnlockReadMutex);

    pthread_exit(NULL);
}

AudioVUnlockDL::AudioVUnlockDL() {
    int32_t ret;
    ret = pthread_mutex_init(&mVUnlockReadMutex, NULL);
    if (ret != 0) {
        ALOGI("Failed to initialize AudioVUnlockDL mMutex!");
    }

    ret = pthread_mutex_init(&mSRCMutex, NULL);
    if (ret != 0) {
        ALOGI("Failed to initialize AudioVUnlockDL mSRCMutex!");
    }

    mWriteThreadExit = false;
    mWriteThreadActive = false;
    mWriteThread = 0;
    mReadThreadExit = true;
    mReadThreadActive = false;
    mReadFunctionActive = false;
    mOutFile = 0;
    mOutFile_1 = NULL;
    mOutFile_2 = NULL;
    mDLtime.tv_sec = 0;
    mDLtime.tv_nsec = 0;
    mNewDLtime.tv_nsec = 0;
    mNewDLtime.tv_sec = 0;
    mStandbyTime.tv_sec = 0;
    mStandbyTime.tv_nsec = 0;
    mGetTime = true;
    mInRemaining = 0;
    mOutRemaining = 0;
    mInChannel = 2;
    mOutputSampleRate = VPW_OUTPUT_SAMPLERATE;
    mInputSampleRate = 44100;
    mpSrcHdl = NULL;
    mSrcBufLen = 0;
    mReadThread = 0;
    mState = VPWStreamIn_CREATED;
    mInputStandby = true;
    mOutChannel = 0;
    mStreamOutLatency = 92;
    mSampleCount_Dump = 0;
    mULtime.tv_sec = 0;
    mULtime.tv_nsec = 0;
    mNeedBlock = false;
    mFormat = 0;
    mTempBuf = NULL;
    mTempBufsz = 0;
}


AudioVUnlockDL::~AudioVUnlockDL() {
    /* if (mRingBufIn!=NULL)
     {
         delete mRingBufIn;
     }
     if (mRingBufOut != NULL)
     {
         delete mRingBufOut;
     } */
    if (mpSrcHdl != NULL) {
        mpSrcHdl->close();
        deleteMtkAudioSrc(mpSrcHdl);
        mpSrcHdl = NULL;
    }
    if (mTempBuf != NULL) {
        delete [] mTempBuf;
    }
}

int32_t AudioVUnlockDL::SetUplinkStartTime(struct timespec uplinkStartTime) {
    mULtime = uplinkStartTime;
    ALOGD("[SetUplinkStartTime] mULtime sec %ld nsec %ld", mULtime.tv_sec, mULtime.tv_nsec);
    return 0;

}
int32_t AudioVUnlockDL::SetUplinkStartTime(struct timespec uplinkStartTime __unused, int clear) {
    if ((mULtime.tv_sec == 0 &&  mULtime.tv_nsec == 0) &&  clear == 0) {
        clock_gettime(CLOCK_MONOTONIC, &mULtime);
#if 1
        mULtime.tv_nsec = mULtime.tv_nsec - (int64_t)50 * 1000 * 1000;
        if (mULtime.tv_nsec >= 1000000000) {
            mULtime.tv_sec += 1;
            mULtime.tv_nsec -= 1000000000;
        }
#endif
        //mULtime = uplinkStartTime;
        ALOGD("[SetUplinkStartTime] mULtime sec %ld nsec %ld", mULtime.tv_sec, mULtime.tv_nsec);
    } else if (clear == 1) {
        mULtime.tv_sec = 0;
        mULtime.tv_nsec = 0;
        ALOGD("[SetUplinkStartTime] mULtime sec %ld nsec %ld", mULtime.tv_sec, mULtime.tv_nsec);
    }
    return 0;
}
int32_t AudioVUnlockDL:: GetSRCInputParameter(
    uint32_t inSR,                  /* Input, input sampling rate of the conversion */
    uint32_t inChannel,             /* Input, input channel number of the conversion */
    audio_format_t format
) {

    ALOGD("[GetSRCInputParameter] mOutputSampleRate %d,  mInputSampleRate %d, mInChannel %d, mOutChannel, %d format %d", (int)mOutputSampleRate, (int)inSR, (int)inChannel, (int)mOutChannel, format);
    if (format != AUDIO_FORMAT_PCM_16_BIT &&
        format != AUDIO_FORMAT_PCM_32_BIT &&
        format != AUDIO_FORMAT_PCM_8_24_BIT) {
        return -1;
    }
    mFormat = format;
    pthread_mutex_lock(&mSRCMutex);
    if (mInputSampleRate != inSR || mInChannel != inChannel) {
        mInputSampleRate = inSR;
        mInChannel = inChannel;
        if (mpSrcHdl != NULL) {
            mpSrcHdl->close();
            deleteMtkAudioSrc(mpSrcHdl);
            mpSrcHdl = NULL;
        }
    }
    if (mpSrcHdl == NULL && mOutputSampleRate != 0 && mOutChannel != 0 && mInputSampleRate != 0 && mInChannel != 0) {
        mpSrcHdl = newMtkAudioSrc(mInputSampleRate, mInChannel, mOutputSampleRate, mOutChannel, SRC_IN_Q1P15_OUT_Q1P15);
        mpSrcHdl->open();

    }
    if (!mpSrcHdl) {
        ALOGD("[GetSRCInputParameter] create SRC handle fail");
        pthread_mutex_unlock(&mSRCMutex);
        return -1;
    }
    pthread_mutex_unlock(&mSRCMutex);
    return 0;
}

int32_t AudioVUnlockDL:: SetSRC(
    uint32_t outSR,                 /* Input, output sampling rate of the conversion */
    uint32_t outChannel            /* Input, output channel number of the conversion */
) {
    if (outSR == 0 || ((outChannel != 1) && (outChannel != 2))) {
        return -1;
    }
    ALOGD("[SetSRC] mOutputSampleRate %d,  mInputSampleRate %d, mInChannel %d, mOutChannel, %d ", (int)outSR, (int)mInputSampleRate, (int)mInChannel, (int)outChannel);
    pthread_mutex_lock(&mSRCMutex);
    // check if setting change.
    if (mOutputSampleRate != outSR || mOutChannel != outChannel) {
        mOutputSampleRate = outSR;
        mOutChannel = outChannel;
        if (mpSrcHdl != NULL) {
            mpSrcHdl->close();
            deleteMtkAudioSrc(mpSrcHdl);
            mpSrcHdl = NULL;
        }
    }

    //set blisrc

    if (mpSrcHdl == NULL && mOutputSampleRate != 0 && mOutChannel != 0 && mInputSampleRate != 0 && mInChannel != 0) {
        mpSrcHdl = newMtkAudioSrc(mInputSampleRate, mInChannel, outSR, outChannel, SRC_IN_Q1P15_OUT_Q1P15);
        mpSrcHdl->open();

    }

    if (!mpSrcHdl) {
        ALOGD("[SetSRC] create SRC handle fail");
        pthread_mutex_unlock(&mSRCMutex);
        return -1;
    }

    pthread_mutex_unlock(&mSRCMutex);
    return 0;
}
int32_t AudioVUnlockDL:: DoSRC(uint8_t *inbuf, uint32_t *datasz, uint8_t *outbuf, uint32_t *outlength) {
    uint8_t *inPtr = inbuf ;
    uint8_t *outPtr = outbuf;
    uint32_t outbuflen = *outlength;
    uint32_t dataProduce = 0;
    uint32_t count = 40;
    uint32_t dataconsumed = 0;
    pthread_mutex_lock(&mSRCMutex);
    if (mpSrcHdl == NULL) {
        ALOGD("[DoSRC] SRC not created");
        pthread_mutex_unlock(&mSRCMutex);
        return -1;
    }

    while (count) {
        ALOGV("count %d ,inPtr %p, inbuf %p, datasz %d, outPtr %p, outbuf %p, outlength %d ", count, inPtr, inbuf, *datasz, outPtr, outbuf, *outlength);
        int consumed_this_time = *datasz;
        mpSrcHdl->process((int16_t *)inPtr, datasz, (int16_t *)outPtr, outlength);
        consumed_this_time -= *datasz;

        dataconsumed += consumed_this_time;
        ALOGV("after count %d , datasz %d,  outlength%d ", count,  *datasz, *outlength);
        dataProduce += *outlength;
        if (*datasz == 0 || *outlength == 0) {
            break;
        } else {
            inPtr = inbuf + dataconsumed;
            outPtr = outbuf + dataProduce;
        }
        // left space in output buffer
        *outlength = outbuflen - dataProduce;
        if (*outlength == 0) {
            break;
        }
        count --;
    }

    pthread_mutex_unlock(&mSRCMutex);
    /*if(count == 0 && *datasz !=0 && *outlength != 0)
    {
        ALOGD("[DoSRC] do not finish SRC *datasz %d, &outlength %d", *datasz,*outlength );
        //return -1;
    }*/
    *outlength = dataProduce;
    return dataconsumed;
}
int32_t AudioVUnlockDL::GetFirstDLTime() {
    if (!StateInputStart()) {
        //SXLOGV("[WriteStreamOutToRing] AudioVUnlockDL is not initialized");
        return -1;
    }

    if (mGetTime == true) {
        clock_gettime(CLOCK_MONOTONIC, &mNewDLtime);

        ALOGD("[GetFirstDLTime] DL time  %ld,  %ld", mNewDLtime.tv_sec, mNewDLtime.tv_nsec);
        mGetTime = false;
        if (mDLtime.tv_sec != 0) {
            mInRemaining = mRingBufIn.GetBufDataSz();
            mOutRemaining = mRingBufOut.GetBufDataSz();
            ALOGD("[GetFirstDLTime] input buf never cleared IN remaining %d, Out remaining %d", mInRemaining, mOutRemaining);
        }
    }
    return 0;
}

int32_t AudioVUnlockDL::SetDownlinkStartTime(int remainMs) {
    int timeOffset = remainMs < 15 ? 0 : (remainMs - 15);

    if (!StateInputStart()) {
        return -1;
    }

    if (mGetTime == true && mInputStandby == false) {
        clock_gettime(CLOCK_MONOTONIC, &mNewDLtime);
        //mNewDLtime.tv_nsec = mNewDLtime.tv_nsec + (int64_t)mStreamOutLatency * 1000 * 1000 / 2 - 20000000;
        mNewDLtime.tv_nsec = mNewDLtime.tv_nsec + (int64_t)timeOffset * 1000 * 1000;
        if (mNewDLtime.tv_nsec >= 1000000000) {
            mNewDLtime.tv_sec += 1;
            mNewDLtime.tv_nsec -= 1000000000;
        }

        ALOGD("[SetDownlinkStartTime] get DL time: mNewDLtime.tv_sec %ld, mNewDLtime.tv_nsec %ld, remainMs %d", mNewDLtime.tv_sec, mNewDLtime.tv_nsec, remainMs);
        mGetTime = false;
        if (mDLtime.tv_sec != 0) {
            mInRemaining = mRingBufIn.GetBufDataSz();
            mOutRemaining = mRingBufOut.GetBufDataSz();
            ALOGD("[SetDownlinkStartTime] input buf never cleared IN remaining %d, Out remaining %d", mInRemaining, mOutRemaining);
        }
    }

    return 0;
}

int32_t AudioVUnlockDL::WriteStreamOutToRing(const void *buf, uint32_t datasz) {
    int32_t datawritten = 0;
    //SXLOGD("[WriteStreamOutToRing] start datasz %d", datasz);
    if (/*mRingBufIn == NULL ||*/ !StateInputStart()) {
        //SXLOGV("[WriteStreamOutToRing] AudioVUnlockDL is not initialized");
        return -1;
    }

    if (buf == NULL || datasz == 0) {
        ALOGD("[WriteStreamOutToRing] input buf and datasz null");
        return -1;
    }

    if (mFormat != AUDIO_FORMAT_PCM_16_BIT) {
        int32_t shift = 8;
        if (mFormat == AUDIO_FORMAT_PCM_8_24_BIT) {
            shift = 8;
        } else if (mFormat == AUDIO_FORMAT_PCM_32_BIT) {
            shift = 16;
        }

        int32_t *pInPtr = (int32_t *)buf;
        int16_t *pOutPtr = (int16_t *)buf;
        int32_t framecount = datasz >> 2;
        int32_t temp;

        while (framecount) {
            temp  = (*pInPtr) >> shift;
            //ALOGD("temp 0x%x",(((unsigned short*) &temp)[0])  );
            *pOutPtr = (((unsigned short *) &temp)[0])  ;

            pOutPtr ++;
            pInPtr ++;
            framecount--;
        };

        datasz = datasz >> 1;
        datawritten = mRingBufIn.Write((void *)buf, datasz);
    } else {

        datawritten = mRingBufIn.Write((void *)buf, datasz);
    }

#ifdef DUMP_VPW_StreamIn_DATA
    if (mOutFile != NULL) {
        //SXLOGD("[WriteStreamOutToRing]  Dump file");
        fwrite(buf, sizeof(char), datawritten, mOutFile);
        fclose(mOutFile);
        //VInstance->mSampleCount_Dump += datawritten>>1;
    }
#endif
    ALOGV("[WriteStreamOutToRing] datawritten %d", datawritten);
    return datawritten;
}
void AudioVUnlockDL::SetInputStandBy(bool val) {
    mInputStandby = val;
    if (val) {
        ALOGV("[SetInputStandBy] val %d", val);
        int cnt_val = 30;
        mRingBufOut.SignalBufData();
        while ((mReadFunctionActive == true) && (cnt_val > 0) && (mNeedBlock == false)) {
            mRingBufOut.SignalBufData();
            ALOGD("[SetInputStandBy] wait ReadRefFromRing to exit (%d) ", cnt_val);
            cnt_val--;
            usleep(1000 * 3);
        }
        ALOGD("[SetInputStandBy] ReadRefFromRing to exit? (%d) ", mReadFunctionActive);
        mGetTime = true;
    } else {
        ALOGV("[SetInputStandBy] val %d", val);
    }
}
bool AudioVUnlockDL::StreamOutStandBy() {
    return mInputStandby ;
}

int32_t AudioVUnlockDL::DumpData(void *buf __unused, uint32_t datasz __unused) {
#ifdef DUMP_VPW_StreamIn_DATA
    if (mOutFile != NULL) {
        //SXLOGD("[DumpData]  Dump file");
        mOutFile = fopen("/data/vendor/audiohal/audio_dump/VPW_In.pcm", "ab+");
        fwrite(buf, sizeof(char), datasz, mOutFile);
        fclose(mOutFile);
        //VInstance->mSampleCount_Dump += datawritten>>1;
    }
#endif
    return 0;
}

int32_t AudioVUnlockDL::ReadRefFromRing(void *buf, uint32_t datasz, void *DLtime) {
    uint32_t dataread = 0;
    uint32_t dataAvailable;
    //uint8_t* tempbuf ;
    uint32_t finaldataout = 0;
    uint32_t dataproduced = datasz;
    uint32_t leftInRing = 0;
    int32_t dataConsumed = 0;
    uint8_t *bufptr = (uint8_t *)buf;
    int count = 10;
    struct timespec *timeptr;
    //SXLOGV("[ReadRefFromRing] start ");
    if (/*mRingBufIn == NULL ||*/ !StateInputStart()) {
        ALOGD("[ReadRefFromRing] AudioVUnlockDL is not initialized");
        return -1;
    }

    if (buf == NULL || datasz == 0 || DLtime == NULL) {
        ALOGD("[ReadRefFromRing] buf and datasz null");
        return -1;
    }
    //timeptr = (struct timespec*) DLtime
    //*timeptr = mDLtime;

    mReadFunctionActive = true;
    struct timespec *ptr;
    if (mNeedBlock == true) {
        ALOGV("[ReadRefFromRing] mNeedBlock %d ", mNeedBlock);
        mRingBufOut.WaitBufData();
        mNeedBlock = false;
    }

    if (mOutRemaining != 0) {
        datasz = mOutRemaining > datasz ? datasz : mOutRemaining;
        dataproduced = datasz ;
    }
    while (count) {
        //check how much data to be read, if no data then wait
        if (!mInputStandby && StateInputStart()) {
            ALOGV("[ReadRefFromRing] mNeedBlock %d ", mNeedBlock);
            mRingBufOut.WaitBufData();
        }
        ptr = (struct timespec *) static_cast< void *>(DLtime);
        *ptr =  mDLtime;
        //SXLOGV("[ReadRefFromRing] mDLtime.tv_sec %d, mDLtime.tv_nsec %d",mDLtime.tv_sec,mDLtime.tv_nsec );
        dataread = mRingBufOut.ReadAdvance(bufptr, dataproduced);
        if (mOutRemaining != 0) {
            mOutRemaining -= dataread;
            ALOGV("[ReadRefFromRing] last playback remaining %d", mOutRemaining);
        }
        bufptr += dataread;
        finaldataout += dataread;
        dataproduced = datasz - finaldataout;
        if (dataproduced == 0) {
            ALOGV("[ReadRefFromRing] output buffer filled. break");
            break;
        }
        if (mInputStandby) {
            if ((mRingBufOut.GetBufDataSz() != 0) && count == 1) {
                count ++;
                ALOGI("[ReadRefFromRing] Playback in standby mode,  but ringbuf still has data, mRingBufOut.GetBufDataSz() %d, mRingBufIn.GetBufDataSz() %d", mRingBufOut.GetBufDataSz(), mRingBufIn.GetBufDataSz());
            }
        }
        count --;
    }
#ifdef DUMP_VPW_StreamIn_DATA
    if (mOutFile_2 != NULL) {

        mOutFile_2 = fopen("/data/vendor/audiohal/audio_dump/VPW_Out.pcm", "ab+");
        fwrite(buf, sizeof(char), finaldataout, mOutFile_2);
        fclose(mOutFile_2);
    }
#endif
    if (finaldataout != datasz) {
        mNeedBlock = true;
    }

    ALOGV("[ReadRefFromRing] end finaldataout %d time %ld", finaldataout, mDLtime.tv_sec);

    mReadFunctionActive = false;
    return finaldataout;
}
bool AudioVUnlockDL::startInput() {
    ALOGV("...[startInput]...");

    int ret = 0;
    if (StateInputStart()) {
        ALOGV("[startInput] already start thread");
        return true;
    }

    ALOGV("[startInput] +create AudioVUnlockDL ReadRoutine thread");
    ret = pthread_create(&mReadThread, NULL, ReadRoutine, this);
    ALOGV("[startInput] -create AudioVUnlockDL ReadRoutine thread");
    int cnt_val = 10;
    while ((mReadThreadActive != true) && (cnt_val > 0)) {
        ALOGV("[startInput] wait thread to exit (%d) ", cnt_val);
        cnt_val--;
        usleep(1000 * 10);
    }

    mOutRemaining = 0;
    mInRemaining = 0;
    mRingBufIn.ResetBuf();
    mRingBufOut.ResetBuf();
    if (ret == 0)
    {SetState(VPWStreamIn_READ_START);}
    //clock_gettime(CLOCK_MONOTONIC, &mDLtime);
    //SXLOGD("[GetDownlinkSystemTime] mDLtime sec %ld nsec %ld", mDLtime.tv_sec, mDLtime.tv_nsec);

#ifdef DUMP_VPW_StreamIn_DATA
    if (mOutFile == NULL) {
        mOutFile = fopen("/data/vendor/audiohal/audio_dump/VPW_In.pcm", "ab+");
    }
    if (mOutFile == NULL) {
        ALOGV("Fail to Open File %x ", mOutFile);
    }

    if (mOutFile_1 == NULL) {
        mOutFile_1 = fopen("/data/vendor/audiohal/audio_dump/VPW_SRC.pcm", "ab+");
    }
    if (mOutFile_1 == NULL) {
        ALOGV("Fail to Open File %x ", mOutFile_1);
    }

    if (mOutFile_2 == NULL) {
        mOutFile_2 = fopen("/data/vendor/audiohal/audio_dump/VPW_Out.pcm", "ab+");
    }
    if (mOutFile_2 == NULL) {
        ALOGV("Fail to Open File %x ", mOutFile_2);
    }
    mSampleCount_Dump = 0;
#endif
    return true;
}

bool AudioVUnlockDL::stopInput() {
    ALOGD("...[stopInput]...");
    int ret = 0;
    int index = 0;

    if (!StateInputStart()) {
        ALOGD("[stopInput] mState != VPWStreamIn_READ_START mState = %d", mState);
        return false;
    }

    pthread_mutex_lock(&mVUnlockReadMutex);
    mReadThreadExit = true;
    pthread_mutex_unlock(&mVUnlockReadMutex);
    int cnt_val = 50;

    while ((mReadThreadActive == true) && (cnt_val > 0)) {
        ALOGD("[stopInput] wait thread to exit (%d) ", cnt_val);
        cnt_val--;
        usleep(1000 * 50);
    }
    mULtime.tv_sec = 0;
    mULtime.tv_nsec = 0;
    mDLtime.tv_sec = 0;
    mDLtime.tv_nsec = 0;

    if (cnt_val <= 0) {
        ALOGD("[stopInput] mReadThreadActive:%d, cnt_val:%d ", mReadThreadActive, cnt_val);
    }
#ifdef DUMP_VPW_StreamIn_DATA
    if (mOutFile != NULL) {
        ALOGD("[stopInput]  Close dump file");
        fclose(mOutFile);
        mOutFile = NULL;
    }

    if (mOutFile_1 != NULL) {
        ALOGD("[stopInput]  Close dump file");
        fclose(mOutFile_1);
        mOutFile_1 = NULL;
    }

    if (mOutFile_2 != NULL) {
        ALOGD("[stopInput]  Close dump file");
        fclose(mOutFile_2);
        mOutFile_2 = NULL;
    }
#endif


    if (mReadThreadActive) {
        ALOGD("[stopInput]  mReadThreadActive is false,  stop fail");
        return false;
    }


    mOutRemaining = 0;
    mInRemaining = 0;

    // stop read in from Stream out
    //delete ring buffer
    ClearState(VPWStreamIn_READ_START);
    mGetTime = true;

    return true;
}
void AudioVUnlockDL::GetStreamOutLatency(int32_t latency) {
    mStreamOutLatency = latency;
}
int32_t  AudioVUnlockDL::GetVoiceUnlockULTime(void *ULtime) {
    struct timespec *ptr;
    if (ULtime == NULL) {
        return -1;
    }
    ptr = (struct timespec *) static_cast<void *>(ULtime);
    *ptr = mULtime ;
    ALOGD("ULtime->tv_sec %ld, ULtime->tv_nsec %ld", ptr->tv_sec, ptr->tv_nsec);
    return 0;
}

int32_t  AudioVUnlockDL::GetLatency() {
    int32_t latency = 0;
#if 0
    uint32_t Inlatency = 0;
    uint32_t Outlatency = 0;

    if (/*mRingBufOut == NULL || mRingBufIn == NULL ||*/ !StateInputStart()) {
        ALOGD("[WriteStreamOutToRing] AudioVUnlockDL is not initialized");
        return -1;
    }
    Inlatency = mRingBufIn.GetBufDataSz();
    ALOGD("[GetLatency]  mRingBufIn buffer data %d bytes", Inlatency);
    Outlatency = mRingBufOut.GetBufDataSz();

    ALOGD("[GetLatency]  mRingBufOut data %d bytes", Outlatency);
    if (mInputSampleRate != 0 && mInChannel != 0) {
        Inlatency = Inlatency * 1000 / (mInputSampleRate * mInChannel) / 2;
    }

    if (mOutputSampleRate != 0 && mOutChannel != 0) {
        Outlatency = Outlatency * 1000 / (mOutputSampleRate * mOutChannel) / 2;
    }
#endif
    return latency;
}
void AudioVUnlockDL::ClearState(uint32_t state) {
    ALOGD("clear AudioVUnlockDL state = %d", state);
    mState &= (~state);
    ALOGD("clear AudioVUnlockDL mState = %d", mState);
}

void AudioVUnlockDL::SetState(uint32_t state) {
    ALOGD("Set AudioVUnlockDL state = %d", state);
    mState |= state;
    ALOGD("Set AudioVUnlockDL mState = %d", mState);
}
int32_t AudioVUnlockDL::GetState(void) {
    return mState;
}
bool AudioVUnlockDL::StateInputStart(void) {
    return mState & VPWStreamIn_READ_START;
}

void AudioVUnlockDL:: freeInstance() {
    ALOGV("+delete UniqueVUnlockDLInstance");
    if (UniqueVUnlockDLInstance != NULL) {
        delete UniqueVUnlockDLInstance;
    }

    UniqueVUnlockDLInstance = NULL;
    return;
}

AudioVUnlockDL *AudioVUnlockDL::getInstance() {
    //SXLOGV("+new Voice unlock DL instance fd, size, flag");
    if (UniqueVUnlockDLInstance == NULL) {

        UniqueVUnlockDLInstance = new AudioVUnlockDL();

    }
    return UniqueVUnlockDLInstance;
}
#if 0//def forUT
#ifdef MTK_BASIC_PACKAGE
bool AudioVUnlockDL::StateStartwrite(void) {
    return false;
}

bool AudioVUnlockDL::startWrite(void) {
    return false;
}

bool AudioVUnlockDL::stopWrite() {
    return false;
}
#else
void *WriteRoutine(void *hdl) {
    AudioVUnlockDL *VInstance = AudioVUnlockDL::getInstance();
    int result = -1;
    int32_t buflen = 640;
    char *buf = new char[640];
    pthread_mutex_lock(&mVUnlockWriteMutex);
    VInstance->mWriteThreadExit = false;
    VInstance->mWriteThreadActive = true;
    pthread_mutex_unlock(&mVUnlockWriteMutex);
    FILE *OutFile = NULL;
    OutFile = fopen("/system/VPW.pcm", "wb");
    if (OutFile == NULL) {
        ALOGD("Fail to Open File %x ", OutFile);
    }

#ifdef MTK_AUDIO_ADJUST_PRIORITY
    // if set prority false , force to set priority
    if (result == -1) {
        struct sched_param sched_p;
        sched_getparam(0, &sched_p);
        sched_p.sched_priority = RTPM_PRIO_AUDIO_I2S;
        if (0 != sched_setscheduler(0, SCHED_RR, &sched_p)) {
            ALOGI("[%s] failed, errno: %d", __func__, errno);
        } else {
            sched_p.sched_priority = RTPM_PRIO_AUDIO_I2S;
            sched_getparam(0, &sched_p);
            ALOGD("sched_setscheduler ok, priority: %d", sched_p.sched_priority);
        }
    }
#endif

    //VInstance ->SetSRC(16000,1);
    //VInstance -> startInput();

    if (AudioSystem::getVoiceUnlockDLInstance()) {
        ALOGD(" get Voice Unlock Instance ok");
    }
    int latency = AudioSystem::GetVoiceUnlockDLLatency();
    ALOGD(" get Voice Unlock latency  %d", latency);
    AudioSystem::SetVoiceUnlockSRC(16000, 1);
    AudioSystem::startVoiceUnlockDL();
    while (!VInstance->mWriteThreadExit) {
        int32_t dataSRC;
        int32_t latency;
        ALOGD(" [write routine] ReadRefFromRing ");
        struct timespec DLtime;
        struct timespec ULtime;
        dataSRC = AudioSystem::ReadRefFromRing(buf, buflen, (void *)&DLtime);
        ALOGD(" [write routine] DL time %d %d dataSRC %d", DLtime.tv_sec, DLtime.tv_nsec, dataSRC);

        AudioSystem::GetVoiceUnlockULTime((void *)&ULtime);
        ALOGD("[write routine] UL time %d %d ", ULtime.tv_sec, ULtime.tv_nsec);

        latency = AudioSystem::GetVoiceUnlockDLLatency();
        ALOGD(" get Voice Unlock latency  %d", latency);
        if (dataSRC > 0) {
#ifdef DUMP_VPW_StreamIn_DATA
            if (OutFile != NULL) {
                ALOGV("[DumpData]  Dump file");
                fwrite(buf, sizeof(char), dataSRC, OutFile);
            }
#endif
        } else if (dataSRC == -1) {
            ALOGD("[write routine] dataSRC %d, exit", dataSRC);
            break;
        }
        //latency = VInstance->GetLatency();
        //SXLOGD("[write routine] latency %d ms", latency);
        ALOGD("[write routine] dataSRC %d, sleep", dataSRC);
        usleep(10 * 1000);
    }

    pthread_mutex_lock(&mVUnlockWriteMutex);
    VInstance->mWriteThreadExit = true;
    VInstance->mWriteThreadActive = false;
    pthread_mutex_unlock(&mVUnlockWriteMutex);
    ALOGD("[WriteRoutine] exit ");
    VInstance->ClearState(VPWStreamIn_WRITE_START);

    AudioSystem::freeVoiceUnlockDLInstance();
    if (OutFile != NULL) {
        ALOGD("[stopInput]  Close dump file");
        fclose(OutFile);
        OutFile = NULL;
    }
    return 0;
}
bool AudioVUnlockDL::StateStartwrite(void) {
    return mState & VPWStreamIn_WRITE_START;
}

bool AudioVUnlockDL::startWrite() {
    ALOGD("...[startwrite]...");

    int ret = 0;
    if (StateStartwrite()) {
        return true;
    }

    ALOGD("[startwrite] +create AudioVUnlockDL WriteRoutine thread");
    ret = pthread_create(&mWriteThread, NULL, WriteRoutine, this);
    ALOGD("[startwrite] -create AudioVUnlockDL WriteRoutine thread");
    if (ret == 0)
    {SetState(VPWStreamIn_WRITE_START);}

    return true;
}

bool AudioVUnlockDL::stopWrite() {
    ALOGD("...[stopWrite]...");
    int ret = 0;
    int index = 0;

    if (!StateStartwrite()) {
        ALOGD("[stopWrite] mState != VPWStreamIn_READ_START mState = %d", mState);
        return false;
    }
    AudioSystem::stopVoiceUnlockDL();

    pthread_mutex_lock(&mVUnlockWriteMutex);
    mWriteThreadExit = true;
    pthread_mutex_unlock(&mVUnlockWriteMutex);

    int cnt_val = 50;

    while ((mWriteThreadActive == true) && (cnt_val > 0)) {
        ALOGD("[stopWrite] wait thread to exit (%d) ", cnt_val);
        cnt_val--;
        usleep(1000 * 50);
    }

    if (cnt_val <= 0) {
        ALOGD("[stopWrite] mWriteThreadActive:%d, cnt_val:%d ", mWriteThreadActive, cnt_val);
    }


    if (mReadThreadActive) {
        ALOGD("[stopWrite]  mWriteThreadActive is false,  stop fail");
        return false;
    }
    // stop read in from Stream out
    //delete ring buffer
    ClearState(VPWStreamIn_WRITE_START);

    return true;
}
#endif
#endif

}

