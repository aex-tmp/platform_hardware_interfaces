/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "StreamInHAL"
//#define LOG_NDEBUG 0

#include <android/log.h>
#include <hardware/audio.h>
#include <mediautils/SchedulingPolicyService.h>

#include "StreamIn.h"

using ::android::hardware::audio::V2_0::MessageQueueFlagBits;

namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {
namespace implementation {

namespace {

class ReadThread : public Thread {
  public:
    // ReadThread's lifespan never exceeds StreamIn's lifespan.
    ReadThread(std::atomic<bool>* stop,
            audio_stream_in_t* stream,
            StreamIn::DataMQ* dataMQ,
            StreamIn::StatusMQ* statusMQ,
            EventFlag* efGroup,
            ThreadPriority threadPriority)
            : Thread(false /*canCallJava*/),
              mStop(stop),
              mStream(stream),
              mDataMQ(dataMQ),
              mStatusMQ(statusMQ),
              mEfGroup(efGroup),
              mThreadPriority(threadPriority),
              mBuffer(new uint8_t[dataMQ->getQuantumCount()]) {
    }
    virtual ~ReadThread() {}

    status_t readyToRun() override;

  private:
    std::atomic<bool>* mStop;
    audio_stream_in_t* mStream;
    StreamIn::DataMQ* mDataMQ;
    StreamIn::StatusMQ* mStatusMQ;
    EventFlag* mEfGroup;
    ThreadPriority mThreadPriority;
    std::unique_ptr<uint8_t[]> mBuffer;

    bool threadLoop() override;
};

status_t ReadThread::readyToRun() {
    if (mThreadPriority != ThreadPriority::NORMAL) {
        int err = requestPriority(
                getpid(), getTid(), static_cast<int>(mThreadPriority), true /*asynchronous*/);
        ALOGW_IF(err, "failed to set priority %d for pid %d tid %d; error %d",
                static_cast<int>(mThreadPriority), getpid(), getTid(), err);
    }
    return OK;
}

bool ReadThread::threadLoop() {
    // This implementation doesn't return control back to the Thread until it decides to stop,
    // as the Thread uses mutexes, and this can lead to priority inversion.
    while(!std::atomic_load_explicit(mStop, std::memory_order_acquire)) {
        // TODO: Remove manual event flag handling once blocking MQ is implemented. b/33815422
        uint32_t efState = 0;
        mEfGroup->wait(static_cast<uint32_t>(MessageQueueFlagBits::NOT_FULL), &efState, NS_PER_SEC);
        if (!(efState & static_cast<uint32_t>(MessageQueueFlagBits::NOT_FULL))) {
            continue;  // Nothing to do.
        }

        const size_t availToWrite = mDataMQ->availableToWrite();
        ssize_t readResult = mStream->read(mStream, &mBuffer[0], availToWrite);
        Result retval = Result::OK;
        uint64_t read = 0;
        if (readResult >= 0) {
            read = readResult;
            if (!mDataMQ->write(&mBuffer[0], readResult)) {
                ALOGW("data message queue write failed");
            }
        } else {
            retval = Stream::analyzeStatus("read", readResult);
        }
        IStreamIn::ReadStatus status = { retval, read };
        if (!mStatusMQ->write(&status)) {
            ALOGW("status message queue write failed");
        }
        mEfGroup->wake(static_cast<uint32_t>(MessageQueueFlagBits::NOT_EMPTY));
    }

    return false;
}

}  // namespace

StreamIn::StreamIn(audio_hw_device_t* device, audio_stream_in_t* stream)
        : mIsClosed(false), mDevice(device), mStream(stream),
          mStreamCommon(new Stream(&stream->common)),
          mStreamMmap(new StreamMmap<audio_stream_in_t>(stream)),
          mEfGroup(nullptr), mStopReadThread(false) {
}

StreamIn::~StreamIn() {
    close();
    mStream = nullptr;
    mDevice = nullptr;
}

// Methods from ::android::hardware::audio::V2_0::IStream follow.
Return<uint64_t> StreamIn::getFrameSize()  {
    return audio_stream_in_frame_size(mStream);
}

Return<uint64_t> StreamIn::getFrameCount()  {
    return mStreamCommon->getFrameCount();
}

Return<uint64_t> StreamIn::getBufferSize()  {
    return mStreamCommon->getBufferSize();
}

Return<uint32_t> StreamIn::getSampleRate()  {
    return mStreamCommon->getSampleRate();
}

Return<void> StreamIn::getSupportedSampleRates(getSupportedSampleRates_cb _hidl_cb)  {
    return mStreamCommon->getSupportedSampleRates(_hidl_cb);
}

Return<Result> StreamIn::setSampleRate(uint32_t sampleRateHz)  {
    return mStreamCommon->setSampleRate(sampleRateHz);
}

Return<AudioChannelMask> StreamIn::getChannelMask()  {
    return mStreamCommon->getChannelMask();
}

Return<void> StreamIn::getSupportedChannelMasks(getSupportedChannelMasks_cb _hidl_cb)  {
    return mStreamCommon->getSupportedChannelMasks(_hidl_cb);
}

Return<Result> StreamIn::setChannelMask(AudioChannelMask mask)  {
    return mStreamCommon->setChannelMask(mask);
}

Return<AudioFormat> StreamIn::getFormat()  {
    return mStreamCommon->getFormat();
}

Return<void> StreamIn::getSupportedFormats(getSupportedFormats_cb _hidl_cb)  {
    return mStreamCommon->getSupportedFormats(_hidl_cb);
}

Return<Result> StreamIn::setFormat(AudioFormat format)  {
    return mStreamCommon->setFormat(format);
}

Return<void> StreamIn::getAudioProperties(getAudioProperties_cb _hidl_cb)  {
    return mStreamCommon->getAudioProperties(_hidl_cb);
}

Return<Result> StreamIn::addEffect(uint64_t effectId)  {
    return mStreamCommon->addEffect(effectId);
}

Return<Result> StreamIn::removeEffect(uint64_t effectId)  {
    return mStreamCommon->removeEffect(effectId);
}

Return<Result> StreamIn::standby()  {
    return mStreamCommon->standby();
}

Return<AudioDevice> StreamIn::getDevice()  {
    return mStreamCommon->getDevice();
}

Return<Result> StreamIn::setDevice(const DeviceAddress& address)  {
    return mStreamCommon->setDevice(address);
}

Return<Result> StreamIn::setConnectedState(const DeviceAddress& address, bool connected)  {
    return mStreamCommon->setConnectedState(address, connected);
}

Return<Result> StreamIn::setHwAvSync(uint32_t hwAvSync)  {
    return mStreamCommon->setHwAvSync(hwAvSync);
}

Return<void> StreamIn::getParameters(const hidl_vec<hidl_string>& keys, getParameters_cb _hidl_cb) {
    return mStreamCommon->getParameters(keys, _hidl_cb);
}

Return<Result> StreamIn::setParameters(const hidl_vec<ParameterValue>& parameters)  {
    return mStreamCommon->setParameters(parameters);
}

Return<void> StreamIn::debugDump(const hidl_handle& fd)  {
    return mStreamCommon->debugDump(fd);
}

Return<Result> StreamIn::start() {
    return mStreamMmap->start();
}

Return<Result> StreamIn::stop() {
    return mStreamMmap->stop();
}

Return<void> StreamIn::createMmapBuffer(int32_t minSizeFrames, createMmapBuffer_cb _hidl_cb) {
    return mStreamMmap->createMmapBuffer(
            minSizeFrames, audio_stream_in_frame_size(mStream), _hidl_cb);
}

Return<void> StreamIn::getMmapPosition(getMmapPosition_cb _hidl_cb) {
    return mStreamMmap->getMmapPosition(_hidl_cb);
}

Return<Result> StreamIn::close()  {
    if (mIsClosed) return Result::INVALID_STATE;
    mIsClosed = true;
    if (mReadThread.get()) {
        mStopReadThread.store(true, std::memory_order_release);
        status_t status = mReadThread->requestExitAndWait();
        ALOGE_IF(status, "read thread exit error: %s", strerror(-status));
    }
    if (mEfGroup) {
        status_t status = EventFlag::deleteEventFlag(&mEfGroup);
        ALOGE_IF(status, "read MQ event flag deletion error: %s", strerror(-status));
    }
    mDevice->close_input_stream(mDevice, mStream);
    return Result::OK;
}

// Methods from ::android::hardware::audio::V2_0::IStreamIn follow.
Return<void> StreamIn::getAudioSource(getAudioSource_cb _hidl_cb)  {
    int halSource;
    Result retval = mStreamCommon->getParam(AudioParameter::keyInputSource, &halSource);
    AudioSource source(AudioSource::DEFAULT);
    if (retval == Result::OK) {
        source = AudioSource(halSource);
    }
    _hidl_cb(retval, source);
    return Void();
}

Return<Result> StreamIn::setGain(float gain)  {
    return Stream::analyzeStatus("set_gain", mStream->set_gain(mStream, gain));
}

Return<void> StreamIn::prepareForReading(
        uint32_t frameSize, uint32_t framesCount, ThreadPriority threadPriority,
        prepareForReading_cb _hidl_cb)  {
    status_t status;
    // Create message queues.
    if (mDataMQ) {
        ALOGE("the client attempts to call prepareForReading twice");
        _hidl_cb(Result::INVALID_STATE,
                DataMQ::Descriptor(), StatusMQ::Descriptor());
        return Void();
    }
    std::unique_ptr<DataMQ> tempDataMQ(
            new DataMQ(frameSize * framesCount, true /* EventFlag */));
    std::unique_ptr<StatusMQ> tempStatusMQ(new StatusMQ(1));
    if (!tempDataMQ->isValid() || !tempStatusMQ->isValid()) {
        ALOGE_IF(!tempDataMQ->isValid(), "data MQ is invalid");
        ALOGE_IF(!tempStatusMQ->isValid(), "status MQ is invalid");
        _hidl_cb(Result::INVALID_ARGUMENTS,
                DataMQ::Descriptor(), StatusMQ::Descriptor());
        return Void();
    }
    // TODO: Remove event flag management once blocking MQ is implemented. b/33815422
    status = EventFlag::createEventFlag(tempDataMQ->getEventFlagWord(), &mEfGroup);
    if (status != OK || !mEfGroup) {
        ALOGE("failed creating event flag for data MQ: %s", strerror(-status));
        _hidl_cb(Result::INVALID_ARGUMENTS,
                DataMQ::Descriptor(), StatusMQ::Descriptor());
        return Void();
    }

    // Create and launch the thread.
    mReadThread = new ReadThread(
            &mStopReadThread,
            mStream,
            tempDataMQ.get(),
            tempStatusMQ.get(),
            mEfGroup,
            threadPriority);
    status = mReadThread->run("reader", PRIORITY_URGENT_AUDIO);
    if (status != OK) {
        ALOGW("failed to start reader thread: %s", strerror(-status));
        _hidl_cb(Result::INVALID_ARGUMENTS,
                DataMQ::Descriptor(), StatusMQ::Descriptor());
        return Void();
    }

    mDataMQ = std::move(tempDataMQ);
    mStatusMQ = std::move(tempStatusMQ);
    _hidl_cb(Result::OK, *mDataMQ->getDesc(), *mStatusMQ->getDesc());
    return Void();
}

Return<uint32_t> StreamIn::getInputFramesLost()  {
    return mStream->get_input_frames_lost(mStream);
}

Return<void> StreamIn::getCapturePosition(getCapturePosition_cb _hidl_cb)  {
    Result retval(Result::NOT_SUPPORTED);
    uint64_t frames = 0, time = 0;
    if (mStream->get_capture_position != NULL) {
        int64_t halFrames, halTime;
        retval = Stream::analyzeStatus(
                "get_capture_position",
                mStream->get_capture_position(mStream, &halFrames, &halTime));
        if (retval == Result::OK) {
            frames = halFrames;
            time = halTime;
        }
    }
    _hidl_cb(retval, frames, time);
    return Void();
}

} // namespace implementation
}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android
