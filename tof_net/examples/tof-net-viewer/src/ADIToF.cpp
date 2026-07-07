/********************************************************************************/
/*                                                                              */
/* Copyright (c) Microsoft Corporation. All rights reserved.					*/
/*  Portions Copyright (c) 2020 Analog Devices Inc.								*/
/* Licensed under the MIT License.												*/
/*																				*/
/********************************************************************************/

// ADIToFTest.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <aditof/system.h>
#include <aditof/version.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#ifdef USE_GLOG
#include <glog/logging.h>
#else
#include <aditof/log.h>
#endif
#include <iostream>
#include <limits>
#include <string>

#include "ADIMainWindow.h"

#if defined(__APPLE__) && defined(__MACH__)
class GOOGLE_GLOG_DLL_DECL glogLogSink : public google::LogSink {
  public:
    glogLogSink(AppLog *log) : applog(log) {}
    ~glogLogSink() = default;
    virtual void send(google::LogSeverity severity, const char *full_filename,
                      const char *base_filename, int line,
                      const struct ::tm *tm_time, const char *message,
                      size_t message_len) {
        if (applog) {
            std::string msg(message, message_len);
            msg += "\n";
            applog->AddLog(msg.c_str(), nullptr);
        }
    };

  private:
    AppLog *applog = nullptr;
};
#endif

void ProcessArgs(int argc, char **argv, ADIViewerArgs &args);

namespace {

std::string upperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return value;
}

bool splitOption(const std::string &raw, std::string &name,
                 std::string &value) {
    const size_t equal = raw.find('=');
    if (equal == std::string::npos) {
        name = raw;
        value.clear();
        return false;
    }
    name = raw.substr(0, equal);
    value = raw.substr(equal + 1);
    return true;
}

bool readOptionValue(int argc, char **argv, int &index,
                     const std::string &inlineValue, std::string &value) {
    if (!inlineValue.empty()) {
        value = inlineValue;
        return true;
    }
    if (index + 1 >= argc) {
        return false;
    }
    value = argv[++index];
    return true;
}

bool parseU32(const std::string &value, uint32_t &out) {
    if (value.empty()) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

void printUsage() {
    std::cout
        << "tof-net-viewer options:\n"
        << "  --HIGHDPI | --NORMALDPI\n"
        << "  --save-processed\n"
        << "  --save-processed-dir <dir>\n"
        << "  --save-processed-planes depth,ir[,xyz]\n"
        << "  --save-processed-stride <N>\n"
        << "  --save-processed-max-frames <N>\n";
}

} // namespace

void ProcessArgs(int argc, char **argv, ADIViewerArgs &args) {
    // Skip argv[0], which is the path to the executable
    //

    for (int i = 1; i < argc; i++) {
        std::string optionName;
        std::string optionValue;
        splitOption(argv[i], optionName, optionValue);
        const std::string arg = upperCopy(optionName);

        if (arg == std::string("--HIGHDPI")) {
            args.HighDpi = true;
        } else if (arg == std::string("--NORMALDPI")) {
            args.HighDpi = false;
        } else if (arg == std::string("--SAVE-PROCESSED")) {
            args.SaveProcessedSet = true;
            args.SaveProcessed = true;
        } else if (arg == std::string("--NO-SAVE-PROCESSED")) {
            args.SaveProcessedSet = true;
            args.SaveProcessed = false;
        } else if (arg == std::string("--SAVE-PROCESSED-DIR")) {
            std::string value;
            if (readOptionValue(argc, argv, i, optionValue, value)) {
                args.SaveProcessedDir = value;
            } else {
                LOG(WARNING) << "--save-processed-dir requires a directory";
            }
        } else if (arg == std::string("--SAVE-PROCESSED-PLANES")) {
            std::string value;
            if (readOptionValue(argc, argv, i, optionValue, value)) {
                args.SaveProcessedPlanes = value;
            } else {
                LOG(WARNING) << "--save-processed-planes requires a value";
            }
        } else if (arg == std::string("--SAVE-PROCESSED-STRIDE")) {
            std::string value;
            uint32_t parsed = 0;
            if (readOptionValue(argc, argv, i, optionValue, value) &&
                parseU32(value, parsed) && parsed != 0) {
                args.SaveProcessedStrideSet = true;
                args.SaveProcessedStride = parsed;
            } else {
                LOG(WARNING) << "--save-processed-stride requires N > 0";
            }
        } else if (arg == std::string("--SAVE-PROCESSED-MAX-FRAMES")) {
            std::string value;
            uint32_t parsed = 0;
            if (readOptionValue(argc, argv, i, optionValue, value) &&
                parseU32(value, parsed)) {
                args.SaveProcessedMaxFramesSet = true;
                args.SaveProcessedMaxFrames = parsed;
            } else {
                LOG(WARNING)
                    << "--save-processed-max-frames requires N >= 0";
            }
        } else if (arg == std::string("--HELP") ||
                   arg == std::string("-H")) {
            args.HelpRequested = true;
            printUsage();
        }
    }
}

int main(int argc, char **argv) {
    FLAGS_logtostderr = 1;

    ADIViewerArgs args;
    ProcessArgs(argc, argv, args);
    if (args.HelpRequested) {
        return 0;
    }

    auto view = std::make_shared<
        adiMainWindow::ADIMainWindow>(); //Create a new instance

#if defined(__APPLE__) && defined(__MACH__)
    //forward glog messages to GUI log windows
    glogLogSink *sink = new glogLogSink(view->getLog());
    google::AddLogSink(sink);
#endif

    LOG(INFO) << "SDK version: " << aditof::getApiVersion()
              << " | branch: " << aditof::getBranchVersion()
              << " | commit: " << aditof::getCommitVersion();

    if (view->startImGUI(args)) {
        view->render();
    }
    return 0;
}
