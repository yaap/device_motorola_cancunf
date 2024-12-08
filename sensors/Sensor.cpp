/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "Sensor.h"

#include <hardware/sensors.h>
#include <log/log.h>
#include <utils/SystemClock.h>

#include <cmath>

static bool readBool(int fd, bool seek) {
    char c;
    int rc;

    ALOGE("readBool is called with fd of %d and seek of %d", fd, seek);

    if (seek) {
        rc = lseek(fd, 0, SEEK_SET);
        if (rc) {
            ALOGE("failed to seek: %d", rc);
            return false;
        }
    }

    rc = read(fd, &c, sizeof(c));
    if (rc != 1) {
        ALOGE("failed to read bool: %d", rc);
        return false;
    }

    return c != '0';
}

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace subhal {
namespace implementation {

using ::android::hardware::sensors::V1_0::MetaDataEventType;
using ::android::hardware::sensors::V1_0::OperationMode;
using ::android::hardware::sensors::V1_0::Result;
using ::android::hardware::sensors::V1_0::SensorFlagBits;
using ::android::hardware::sensors::V1_0::SensorStatus;
using ::android::hardware::sensors::V2_1::Event;
using ::android::hardware::sensors::V2_1::SensorInfo;
using ::android::hardware::sensors::V2_1::SensorType;

Sensor::Sensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : mIsEnabled(false),
      mSamplingPeriodNs(0),
      mLastSampleTimeNs(0),
      mCallback(callback),
      mMode(OperationMode::NORMAL) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.vendor = "The LineageOS Project";
    mSensorInfo.version = 1;
    constexpr float kDefaultMaxDelayUs = 1000 * 1000;
    mSensorInfo.maxDelay = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = 0;
    mRunThread = std::thread(startThread, this);
}

Sensor::~Sensor() {
    // Ensure that lock is unlocked before calling mRunThread.join() or a
    // deadlock will occur.
    {
        std::unique_lock<std::mutex> lock(mRunMutex);
        mStopThread = true;
        mIsEnabled = false;
        mWaitCV.notify_all();
    }
    mRunThread.join();
}

const SensorInfo& Sensor::getSensorInfo() const {
    return mSensorInfo;
}

void Sensor::batch(int32_t samplingPeriodNs) {
    samplingPeriodNs =
        std::clamp(samplingPeriodNs, mSensorInfo.minDelay * 1000, mSensorInfo.maxDelay * 1000);

    if (mSamplingPeriodNs != samplingPeriodNs) {
        mSamplingPeriodNs = samplingPeriodNs;
        // Wake up the 'run' thread to check if a new event should be generated now
        mWaitCV.notify_all();
    }
}

void Sensor::activate(bool enable) {
    std::lock_guard<std::mutex> lock(mRunMutex);
    if (mIsEnabled != enable) {
        mIsEnabled = enable;
        mWaitCV.notify_all();
    }
}

Result Sensor::flush() {
    // Only generate a flush complete event if the sensor is enabled and if the sensor is not a
    // one-shot sensor.
    if (!mIsEnabled) {
        return Result::BAD_VALUE;
    }

    // Note: If a sensor supports batching, write all of the currently batched events for the sensor
    // to the Event FMQ prior to writing the flush complete event.
    Event ev;
    ev.sensorHandle = mSensorInfo.sensorHandle;
    ev.sensorType = SensorType::META_DATA;
    ev.u.meta.what = MetaDataEventType::META_DATA_FLUSH_COMPLETE;
    std::vector<Event> evs{ev};
    mCallback->postEvents(evs, isWakeUpSensor());

    return Result::OK;
}

void Sensor::startThread(Sensor* sensor) {
    sensor->run();
}

void Sensor::run() {
    std::unique_lock<std::mutex> runLock(mRunMutex);
    constexpr int64_t kNanosecondsInSeconds = 1000 * 1000 * 1000;

    ALOGE("Sensor::run() is called");

    while (!mStopThread) {
        if (!mIsEnabled || mMode == OperationMode::DATA_INJECTION) {
            mWaitCV.wait(runLock, [&] {
                return ((mIsEnabled && mMode == OperationMode::NORMAL) || mStopThread);
            });
        } else {
            timespec curTime;
            clock_gettime(CLOCK_REALTIME, &curTime);
            int64_t now = (curTime.tv_sec * kNanosecondsInSeconds) + curTime.tv_nsec;
            int64_t nextSampleTime = mLastSampleTimeNs + mSamplingPeriodNs;

            if (now >= nextSampleTime) {
                mLastSampleTimeNs = now;
                nextSampleTime = mLastSampleTimeNs + mSamplingPeriodNs;
                mCallback->postEvents(readEvents(), isWakeUpSensor());
            }

            mWaitCV.wait_for(runLock, std::chrono::nanoseconds(nextSampleTime - now));
        }
    }
}

bool Sensor::isWakeUpSensor() {
    ALOGE("isWakeUpSensor called with mSensorInfo.flags of %d", mSensorInfo.flags);
    return mSensorInfo.flags & static_cast<uint32_t>(SensorFlagBits::WAKE_UP);
}

std::vector<Event> Sensor::readEvents() {
    std::vector<Event> events;
    Event event;
    event.sensorHandle = mSensorInfo.sensorHandle;
    event.sensorType = mSensorInfo.type;
    event.timestamp = ::android::elapsedRealtimeNano();
    event.u.vec3.x = 0;
    event.u.vec3.y = 0;
    event.u.vec3.z = 0;
    event.u.vec3.status = SensorStatus::ACCURACY_HIGH;
    events.push_back(event);
    return events;
}

void Sensor::setOperationMode(OperationMode mode) {
    std::lock_guard<std::mutex> lock(mRunMutex);
    if (mMode != mode) {
        mMode = mode;
        mWaitCV.notify_all();
    }
}

bool Sensor::supportsDataInjection() const {
    ALOGE("supportsDataInjection called with mSensorInfo.flags of %d", mSensorInfo.flags);
    return mSensorInfo.flags & static_cast<uint32_t>(SensorFlagBits::DATA_INJECTION);
}

Result Sensor::injectEvent(const Event& event) {
    Result result = Result::OK;
    if (event.sensorType == SensorType::ADDITIONAL_INFO) {
        // When in OperationMode::NORMAL, SensorType::ADDITIONAL_INFO is used to push operation
        // environment data into the device.
    ALOGE("injectEvent says ADDITIONAL_INFO");
    } else if (!supportsDataInjection()) {
        ALOGE("injectEvent says supportsDataInjection is false so invalid operation");
        result = Result::INVALID_OPERATION;
    } else if (mMode == OperationMode::DATA_INJECTION) {
        ALOGE("injectEvent says mode is DATA_INJECION so we postEvents");
        mCallback->postEvents(std::vector<Event>{event}, isWakeUpSensor());
    } else {
	ALOGE("injectEvent says BAD_VALUE");
        result = Result::BAD_VALUE;
    }
    return result;
}

OneShotSensor::OneShotSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : Sensor(sensorHandle, callback) {
    mSensorInfo.minDelay = -1;
    mSensorInfo.maxDelay = 0;
    mSensorInfo.flags |= SensorFlagBits::ONE_SHOT_MODE;
}

SysfsPollingOneShotSensor::SysfsPollingOneShotSensor(
    int32_t sensorHandle, ISensorsEventCallback* callback, const std::string& pollPath,
    const std::string& name, const std::string& typeAsString,
    SensorType type)
    : OneShotSensor(sensorHandle, callback) {
    mSensorInfo.name = name;
    mSensorInfo.type = type;
    mSensorInfo.typeAsString = typeAsString;
    mSensorInfo.maxRange = 2048.0f;
    mSensorInfo.resolution = 1.0f;
    mSensorInfo.power = 0;
    mSensorInfo.flags |= SensorFlagBits::WAKE_UP;

    ALOGE("SysfsPollingOneShotSensor::SysfsPollingOneShotSensor says mSensorInfo.flags is %d", mSensorInfo.flags);

    int rc;

    rc = pipe(mWaitPipeFd);
    if (rc < 0) {
        mWaitPipeFd[0] = -1;
        mWaitPipeFd[1] = -1;
        ALOGE("failed to open wait pipe: %d", rc);
    }

    mPollFd = open(pollPath.c_str(), O_RDONLY);
    if (mPollFd < 0) {
        ALOGE("failed to open poll fd: %d", mPollFd);
    }

    ALOGE("mPollFd succeeded so yes it read the node: %s", pollPath.c_str());
    if (mWaitPipeFd[0] < 0 || mWaitPipeFd[1] < 0 || mPollFd < 0) {
        mStopThread = true;
        return;
    }

    mPolls[0] = {
        .fd = mWaitPipeFd[0],
        .events = POLLIN,
    };

    mPolls[1] = {
        .fd = mPollFd,
        .events = POLLERR | POLLPRI,
    };
}

SysfsPollingOneShotSensor::~SysfsPollingOneShotSensor() {
    interruptPoll();
}

void SysfsPollingOneShotSensor::activate(bool enable, bool notify, bool lock) {
    std::unique_lock<std::mutex> runLock(mRunMutex, std::defer_lock);

    if (lock) {
        runLock.lock();
    }

    if (mIsEnabled != enable) {

        mIsEnabled = enable;

        if (notify) {
            interruptPoll();
            mWaitCV.notify_all();
        }
    }

    if (lock) {
        runLock.unlock();
    }
}

void SysfsPollingOneShotSensor::activate(bool enable) {
    activate(enable, true, true);
}

void SysfsPollingOneShotSensor::setOperationMode(OperationMode mode) {
    Sensor::setOperationMode(mode);
    interruptPoll();
}

void SysfsPollingOneShotSensor::run() {
    std::unique_lock<std::mutex> runLock(mRunMutex);

    ALOGE("SysfsPollingOneShotSensor::run starts");
    while (!mStopThread) {
        ALOGE("SysfsPollingOneShotSensor::run is running while loop until mStopThread is false");
        if (!mIsEnabled || mMode == OperationMode::DATA_INJECTION) {
            ALOGE("SysfsPollingOneShotSensor::run is a if function when mIsEnabled is false or mMode is OperationMode::DATA_INJECTION");
            mWaitCV.wait(runLock, [&] {
                return ((mIsEnabled && mMode == OperationMode::NORMAL) || mStopThread);
            });
        } else {
	    ALOGE("SysfsPollingOneShotSensor::run mIsEnabled is true or mMode is not OperationMode::DATA_INJECTION");
            // Cannot hold lock while polling.
            runLock.unlock();
            int rc = poll(mPolls, 2, -1);
            runLock.lock();

            if (rc < 0) {
                ALOGE("failed to poll: %d", rc);
                mStopThread = true;
                continue;
            }

            ALOGE("SysfsPollingOneShotSensor::run runs poll(mPolls, 2, -1); but it ain't 0 so rc is %d", rc);

            if (mPolls[1].revents == mPolls[1].events && readBool(mPollFd, true /* seek */)) {
		ALOGE("SysfsPollingOneShotSensor::run mPolls[1].revents is equal to mPolls[1].events and readBool returns a true so it does work");
                activate(false, false, false);
                mCallback->postEvents(readEvents(), isWakeUpSensor());
            } else if (mPolls[0].revents == mPolls[0].events) {
		ALOGE("SysfsPollingOneShotSensor::run mPolls[1].revents is not equal to mPolls[1].events or readBool isn't true or both isn't true so it checks if mPolls[0].revents is equal to mPolls[0].events and calls readBool");
                readBool(mWaitPipeFd[0], false /* seek */);
            }
        }
    }
}

void SysfsPollingOneShotSensor::interruptPoll() {
    if (mWaitPipeFd[1] < 0) return;

    char c = '1';
    write(mWaitPipeFd[1], &c, sizeof(c));
}

std::vector<Event> SysfsPollingOneShotSensor::readEvents() {
    std::vector<Event> events;
    Event event;
    event.sensorHandle = mSensorInfo.sensorHandle;
    event.sensorType = mSensorInfo.type;
    event.timestamp = ::android::elapsedRealtimeNano();
    fillEventData(event);
    events.push_back(event);
    return events;
}

void SysfsPollingOneShotSensor::fillEventData(Event& event) {
    event.u.data[0] = 0;
    event.u.data[1] = 0;
}

}  // namespace implementation
}  // namespace subhal
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
