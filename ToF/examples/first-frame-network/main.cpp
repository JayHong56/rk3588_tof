/*
 * BSD 3-Clause License
 *
 * Copyright (c) 2019, Analog Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <aditof/camera.h>
#include <aditof/frame.h>
#include <aditof/system.h>
#include <aditof/version.h>
#ifdef USE_GLOG
#include <glog/logging.h>
#else
#include <aditof/log.h>
#endif
#include <algorithm>
#include <cstdint>
#include <iostream>

using namespace aditof;

int main(int argc, char *argv[]) {

    google::InitGoogleLogging(argv[0]);
    FLAGS_alsologtostderr = 1;

    LOG(INFO) << "SDK version: " << aditof::getApiVersion()
              << " | branch: " << aditof::getBranchVersion()
              << " | commit: " << aditof::getCommitVersion();

    Status status = Status::OK;

    if (argc < 3) {
        LOG(ERROR) << "No ip or config file provided! ./first-frame-network <ip> <config_file> [frame_type]";
        return 0;
    }

    std::string ip = argv[1];
    std::string configFile = argv[2];
    System system;

    std::vector<std::shared_ptr<Camera>> cameras;
    system.getCameraListAtIp(cameras, ip);
    if (cameras.empty()) {
        LOG(WARNING) << "No cameras found";
        return 0;
    }

    auto camera = cameras.front();

    status = camera->setControl("initialization_config", configFile);
    if(status != Status::OK){
        LOG(ERROR) << "Failed to set control!";
        return 0;
    }

    status = camera->initialize();
    if (status != Status::OK) {
        LOG(ERROR) << "Could not initialize camera!";
        return 0;
    }

    aditof::CameraDetails cameraDetails;
	camera->getDetails(cameraDetails);

	LOG(INFO) << "SD card image version: " << cameraDetails.sdCardImageVersion;
	LOG(INFO) << "Kernel version: " << cameraDetails.kernelVersion;
	LOG(INFO) << "U-Boot version: " << cameraDetails.uBootVersion;

    std::vector<std::string> frameTypes;
    camera->getAvailableFrameTypes(frameTypes);
    if (frameTypes.empty()) {
        std::cout << "no frame type available!";
        return 0;
    }
    const std::string frameType = argc >= 4 ? argv[3] : "lr-mixed";
    status = camera->setFrameType(frameType);
    if (status != Status::OK) {
        LOG(ERROR) << "Could not set camera frame type!";
        return 0;
    }

    status = camera->start();
    if (status != Status::OK) {
        LOG(ERROR) << "Could not start the camera!";
        return 0;
    }
    aditof::Frame frame;

    status = camera->requestFrame(&frame);
    if (status != Status::OK) {
        LOG(ERROR) << "Could not request frame!";
        return 0;
    } else {
        LOG(INFO) << "succesfully requested frame!";
    }

    uint16_t *data1;
    status = frame.getData("ir", &data1);

    if (status != Status::OK) {
        LOG(ERROR) << "Could not get frame data!";
        return 0;
    }

    if (!data1) {
        LOG(ERROR) << "no memory allocated in frame";
        return 0;
    }

    FrameDataDetails fDetails;
    frame.getDataDetails("ir", fDetails);
    const size_t pixelCount =
        static_cast<size_t>(fDetails.width) * fDetails.height;
    uint16_t minValue = UINT16_MAX;
    uint16_t maxValue = 0;
    uint64_t sum = 0;
    size_t nonZero = 0;
    for (size_t i = 0; i < pixelCount; ++i) {
        minValue = std::min(minValue, data1[i]);
        maxValue = std::max(maxValue, data1[i]);
        sum += data1[i];
        nonZero += data1[i] != 0;
    }
    LOG(INFO) << "IR frame " << fDetails.width << "x" << fDetails.height
              << ": min=" << minValue << " max=" << maxValue
              << " nonzero=" << nonZero << "/" << pixelCount
              << " mean=" << (pixelCount ? sum / pixelCount : 0);
    std::cout << "First pixels:";
    for (size_t i = 0; i < std::min<size_t>(pixelCount, 16); ++i) {
        std::cout << ' ' << data1[i];
    }
    std::cout << std::endl;

    camera->stop();

    return 0;
}
