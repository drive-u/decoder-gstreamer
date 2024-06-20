//
// Created by hanan on 3/26/24.
//
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <cuda.h>

#include "../interface/FrameProcessApi.h"
#include "GstreamerPipeline.h"


const char* DUframeProcessLibraryVersion() {
    std::cout << "Call version()" << std::endl;
    return "1.0";
}

static GstreamerPipeline gstreamerPipeline;
bool DUframeProcessLibraryInit(DUframeProcessLibraryOnFrameShowCallback /*onFrameShowCallback*/) {
    LOG_INFO(std::cout << "Call DUframeProcessLibraryInit() " << std::endl);
    gstreamerPipeline.start();
    return true;
}

void DUputEncodedFrame(DUencodedFrameData encodedFrameData, DUonFrameDecodedCallback onFrameDecodedCallback) {
    gstreamerPipeline.putEncodedFrame(encodedFrameData, onFrameDecodedCallback);
}

void DUextractFrameData(DUdecodedFrameData decodedData, DUcopyFramePlaneMethod copyData) {
    gstreamerPipeline.extractFrameData(decodedData, copyData);
}


void DUframeProcessLibraryShutdown() {
    LOG_INFO(std::cout << "Call shutdown()" << std::endl);
}