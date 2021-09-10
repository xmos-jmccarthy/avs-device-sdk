// Copyright (c) 2021 XMOS LIMITED. This Software is subject to the terms of the
// XMOS Public License: Version 1
/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <memory>

#include <AVSCommon/Utils/Logger/Logger.h>
#include <wiringPi.h>

#include "GPIO/GPIOKeywordDetector.h"

namespace alexaClientSDK {
namespace kwd {

using namespace avsCommon::utils::logger;

/// String to identify log entries originating from this file.
static const std::string TAG("GPIOKeywordDetector");

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsCommon::utils::logger::LogEntry(TAG, event)

/// GPIO pin to monitor
static const int GPIO_PIN = 0;

/// Number of m_maxSamplesPerPush * WW_REWIND_SAMPLES to rewind when WW is detected on GPIO
static const size_t WW_REWIND_SAMPLES = 10;

/// Wakeword string
static const std::string WAKEWORD_STRING = "alexa";

/// The number of hertz per kilohertz.
static const size_t HERTZ_PER_KILOHERTZ = 1000;

/// The timeout to use for read calls to the SharedDataStream.
const std::chrono::milliseconds TIMEOUT_FOR_READ_CALLS = std::chrono::milliseconds(1000);

/// The GPIO WW compatible AVS sample rate of 16 kHz.
static const unsigned int GPIO_COMPATIBLE_SAMPLE_RATE = 16000;

/// The GPIO WW compatible bits per sample of 16.
static const unsigned int GPIO_COMPATIBLE_SAMPLE_SIZE_IN_BITS = 16;

/// The GPIO WW compatible number of channels, which is 1.
static const unsigned int GPIO_COMPATIBLE_NUM_CHANNELS = 1;

/// The GPIO WW compatible audio encoding of LPCM.
static const avsCommon::utils::AudioFormat::Encoding GPIO_COMPATIBLE_ENCODING =
    avsCommon::utils::AudioFormat::Encoding::LPCM;

/// The GPIO WW compatible endianness which is little endian.
static const avsCommon::utils::AudioFormat::Endianness GPIO_COMPATIBLE_ENDIANNESS =
    avsCommon::utils::AudioFormat::Endianness::LITTLE;

/**
 * Checks to see if an @c avsCommon::utils::AudioFormat is compatible with GPIO WW.
 *
 * @param audioFormat The audio format to check.
 * @return @c true if the audio format is compatible with GPIO WW and @c false otherwise.
 */
static bool isAudioFormatCompatibleWithGPIOWW(avsCommon::utils::AudioFormat audioFormat) {
    if (GPIO_COMPATIBLE_ENCODING != audioFormat.encoding) {
        ACSDK_ERROR(LX("isAudioFormatCompatibleWithGPIOWWFailed")
                        .d("reason", "incompatibleEncoding")
                        .d("gpiowwEncoding", GPIO_COMPATIBLE_ENCODING)
                        .d("encoding", audioFormat.encoding));
        return false;
    }
    if (GPIO_COMPATIBLE_ENDIANNESS != audioFormat.endianness) {
        ACSDK_ERROR(LX("isAudioFormatCompatibleWithGPIOWWFailed")
                        .d("reason", "incompatibleEndianess")
                        .d("gpiowwEndianness", GPIO_COMPATIBLE_ENDIANNESS)
                        .d("endianness", audioFormat.endianness));
        return false;
    }
    if (GPIO_COMPATIBLE_SAMPLE_RATE != audioFormat.sampleRateHz) {
        ACSDK_ERROR(LX("isAudioFormatCompatibleWithGPIOWWFailed")
                        .d("reason", "incompatibleSampleRate")
                        .d("gpiowwSampleRate", GPIO_COMPATIBLE_SAMPLE_RATE)
                        .d("sampleRate", audioFormat.sampleRateHz));
        return false;
    }
    if (GPIO_COMPATIBLE_SAMPLE_SIZE_IN_BITS != audioFormat.sampleSizeInBits) {
        ACSDK_ERROR(LX("isAudioFormatCompatibleWithGPIOWWFailed")
                        .d("reason", "incompatibleSampleSizeInBits")
                        .d("gpiowwSampleSizeInBits", GPIO_COMPATIBLE_SAMPLE_SIZE_IN_BITS)
                        .d("sampleSizeInBits", audioFormat.sampleSizeInBits));
        return false;
    }
    if (GPIO_COMPATIBLE_NUM_CHANNELS != audioFormat.numChannels) {
        ACSDK_ERROR(LX("isAudioFormatCompatibleWithGPIOWWFailed")
                        .d("reason", "incompatibleNumChannels")
                        .d("gpiowwNumChannels", GPIO_COMPATIBLE_NUM_CHANNELS)
                        .d("numChannels", audioFormat.numChannels));
        return false;
    }
    return true;
}

std::unique_ptr<GPIOKeywordDetector> GPIOKeywordDetector::create(
        std::shared_ptr<AudioInputStream> stream,
        avsCommon::utils::AudioFormat audioFormat,
        std::unordered_set<std::shared_ptr<KeyWordObserverInterface>> keyWordObservers,
        std::unordered_set<std::shared_ptr<KeyWordDetectorStateObserverInterface>> keyWordDetectorStateObservers,
        std::chrono::milliseconds msToPushPerIteration)  {

    if (!stream) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullStream"));
        return nullptr;
    }

    // TODO: ACSDK-249 - Investigate cpu usage of converting bytes between endianness and if it's not too much, do it.
    if (isByteswappingRequired(audioFormat)) {
        ACSDK_ERROR(LX("createFailed").d("reason", "endianMismatch"));
        return nullptr;
    }

    if (!isAudioFormatCompatibleWithGPIOWW(audioFormat)) {
        return nullptr;
    }

    std::unique_ptr<GPIOKeywordDetector> detector(new GPIOKeywordDetector(
        stream, keyWordObservers, keyWordDetectorStateObservers, audioFormat));

    if (!detector->init()) {
        ACSDK_ERROR(LX("createFailed").d("reason", "initDetectorFailed"));
        return nullptr;
    }

    return detector;
}

GPIOKeywordDetector::GPIOKeywordDetector(
    std::shared_ptr<AudioInputStream> stream,
    std::unordered_set<std::shared_ptr<KeyWordObserverInterface>> keyWordObservers,
    std::unordered_set<std::shared_ptr<KeyWordDetectorStateObserverInterface>> keyWordDetectorStateObservers,
    avsCommon::utils::AudioFormat audioFormat,
    std::chrono::milliseconds msToPushPerIteration) :
        AbstractKeywordDetector(keyWordObservers, keyWordDetectorStateObservers),
        m_stream{stream},
        m_maxSamplesPerPush((audioFormat.sampleRateHz / HERTZ_PER_KILOHERTZ) * msToPushPerIteration.count()) {
}

GPIOKeywordDetector::~GPIOKeywordDetector() {
    m_isShuttingDown = true;
    if (m_detectionThread.joinable())
        m_detectionThread.join();
}

bool GPIOKeywordDetector::init() {
    setenv("WIRINGPI_GPIOMEM", "1", 1);
    if (wiringPiSetup() < 0) {
        ACSDK_ERROR(LX("initFailed").d("reason", "wiringPiSetup failed"));
        return false;
    }

    pinMode(GPIO_PIN, INPUT);

    m_streamReader = m_stream->createReader(AudioInputStream::Reader::Policy::BLOCKING);
    if (!m_streamReader) {
        ACSDK_ERROR(LX("initFailed").d("reason", "createStreamReaderFailed"));
        return false;
    }

    m_isShuttingDown = false;
    m_detectionThread = std::thread(&GPIOKeywordDetector::detectionLoop, this);
    return true;
}

void GPIOKeywordDetector::detectionLoop() {
    m_beginIndexOfStreamReader = m_streamReader->tell();
    notifyKeyWordDetectorStateObservers(KeyWordDetectorStateObserverInterface::KeyWordDetectorState::ACTIVE);
    std::vector<int16_t> audioDataToPush(m_maxSamplesPerPush);

    while (!m_isShuttingDown) {
        bool didErrorOccur = false;
        auto wordsRead = readFromStream(
            m_streamReader,
            m_stream,
            audioDataToPush.data(),
            audioDataToPush.size(),
            TIMEOUT_FOR_READ_CALLS,
            &didErrorOccur);
        if (didErrorOccur) {
            /*
             * Note that this does not include the overrun condition, which the base class handles by instructing the
             * reader to seek to BEFORE_WRITER.
             */
            break;
        } else if (wordsRead == AudioInputStream::Reader::Error::OVERRUN) {
            /*
             * Updating reference point of Reader so that new indices that get emitted to keyWordObservers can be
             * relative to it.
             */
            m_beginIndexOfStreamReader = m_streamReader->tell();
        } else if (wordsRead > 0) {
            // Words were successfully read.
            // check gpio value
            int gpioValue = digitalRead(GPIO_PIN);

            if (gpioValue == HIGH)
            {
                ACSDK_INFO(LX("WW detected"));
                notifyKeyWordObservers(
                    m_stream,
                    WAKEWORD_STRING,
                    // avsCommon::sdkInterfaces::KeyWordObserverInterface::UNSPECIFIED_INDEX,
                    (m_streamReader->tell() < (m_maxSamplesPerPush*WW_REWIND_SAMPLES) ? 0 : m_streamReader->tell() - (m_maxSamplesPerPush*WW_REWIND_SAMPLES)),
                    m_streamReader->tell());
            }
        }
    }
    m_streamReader->close();
}

}  // namespace kwd
}  // namespace alexaClientSDK
