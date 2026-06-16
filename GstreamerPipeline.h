#pragma once

#include <gst/app/app.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>
#include <atomic>
#include <queue>
#include <vector>

#define TRACE_VAL(val) #val << ": " << val << "; "
//#define LOG_DEBUG(stream) stream
#define LOG_DEBUG(stream)
#define LOG_INFO(stream) stream
#define LOG_ERROR(stream) stream
class GstreamerPipeline {
public:
    static constexpr size_t INITIAL_FRAME_BUFFER_SIZE = 4 * 1024 * 1024; // 4MB per slot

    GstreamerPipeline():
        _isRunning( false )
    {
        LOG_INFO(std::cout << "init the GstreamerPipeline class " << std::endl);
        for (auto& buf : encodedFrames) {
            buf.reserve(INITIAL_FRAME_BUFFER_SIZE);
        }
    }
    GstreamerPipeline(const GstreamerPipeline &) = delete;
    GstreamerPipeline& operator=(const GstreamerPipeline &) = delete;

    ~GstreamerPipeline()
    {
        stop();
    }

    void start() {
        LOG_INFO(std::cout << "start GstreamerPipeline class " << std::endl);
        playGetVideoPackets();
    }

    void playGetVideoPackets() {
        if ( !_isRunning ) {
            createPipeline();
            addBusWatch();
            GstElement *appsink = gst_bin_get_by_name( GST_BIN(_currentPipelineElement), "appsink" );
            g_signal_connect( appsink, "new_sample", G_CALLBACK( GstreamerPipeline::onNewSample ), this );
            gst_object_unref(appsink);
            appsrc = gst_bin_get_by_name( GST_BIN(_currentPipelineElement), "appsrc" );

            startPipeline();
        }
        else {
            LOG_ERROR(std::cout << "ERROR " << "decoder pipeline playGetVideoPackets called, but checkPipelineState thread is already running." << std::endl);
        }
    }
    void startPipeline() {
        if ( !_isRunning && _currentPipelineElement != NULL ) {
            LOG_INFO(std::cout <<  "Start Gstremer Decoder pipeline" << std::endl);
            gst_element_set_state( _currentPipelineElement, GST_STATE_PLAYING );
            _isRunning = true;
        }
    }


    void createPipeline() {
        GError *e = NULL;
        const std::string pipe = _gstreamPipeline;
        LOG_INFO(std::cout << "Running pipeline: " << std::endl << "~~~ " + pipe << std::endl);
        gst_init(NULL, NULL);
        // gst_debug_set_active(TRUE);
        // gst_debug_set_default_threshold(GST_LEVEL_LOG);
        _currentPipelineElement = gst_parse_launch( pipe.c_str(), &e );
        if ( e != NULL || _currentPipelineElement == NULL ) {
            LOG_ERROR(std::cout << "ERROR Failed to run pipeline: " << std::endl);
            const char* errMsg = (e != NULL) ? e->message : "unknown pipeline error";
            LOG_ERROR(std::cout << " ~~~ " << pipe << std::endl << "[Error]: " << errMsg << std::endl);
            throw std::runtime_error(errMsg);
        }
        assert(_currentPipelineElement != nullptr);
    }
    void setOnFrameDecodedCallback(DUonFrameDecodedCallback onFrameDecodedCallback) {
        _onFrameDecodedCallback = onFrameDecodedCallback;
    }
    void putEncodedFrame(DUencodedFrameData encodedFrameData, DUonFrameDecodedCallback onFrameDecodedCallback) {
        if (!_onFrameDecodedCallback) {
            LOG_INFO(std::cout << " putEncodedFrame: ========= set callback ===========" << std::endl);
            _onFrameDecodedCallback = onFrameDecodedCallback;
        }
        auto frameBuffer = encodedFrameData._buffer;
        auto frameSize = encodedFrameData._size;
        LOG_DEBUG(std::cout << " putEncodedFrame" << std::endl);
        auto slot = _pushBufferIndex % RING;
        // Always release the previous GstBuffer for this slot before reusing its backing memory.
        if (_pushbuffer[slot] != NULL) {
            // If frameSize exceeds capacity, resize will reallocate the vector — moving the pointer.
            // If GStreamer still holds a reference to this buffer, that becomes a use-after-free.
            if (frameSize > encodedFrames[slot].capacity() && !gst_buffer_is_writable(_pushbuffer[slot])) {
                LOG_INFO(std::cout << "WARNING: GstBuffer slot " << slot << " still held by pipeline and reallocation is needed; consider increasing RING or INITIAL_FRAME_BUFFER_SIZE" << std::endl);
            }
            gst_buffer_unref(_pushbuffer[slot]);
            _pushbuffer[slot] = NULL;
        }
        if (frameSize > encodedFrames[slot].size()) {
            encodedFrames[slot].resize(frameSize);
        }
        memcpy(encodedFrames[slot].data(), frameBuffer, frameSize);
        _lastBufferDataSize = frameSize;

        auto pushBufferIndex = slot;
        _pushbuffer[pushBufferIndex] = gst_buffer_new_wrapped_full(
                                        GST_MEMORY_FLAG_READONLY,
                                        encodedFrames[pushBufferIndex].data(),
                                        _lastBufferDataSize,
                                        0,
                                        _lastBufferDataSize,
                                        NULL,
                                        NULL  // No free function - we manage the memory
                                    );
        // _pushbuffer[pushBufferIndex] = gst_buffer_new_wrapped (encodedFrames[pushBufferIndex], _lastBufferDataSize);
        GST_BUFFER_DURATION (_pushbuffer[pushBufferIndex] ) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_TIMESTAMP (_pushbuffer[pushBufferIndex] ) = gst_util_uint64_scale (encodedFrameData._timestamp, GST_USECOND, 1);
        GST_BUFFER_OFFSET(_pushbuffer[pushBufferIndex]) = encodedFrameData._frameIndex;
        GST_BUFFER_DTS(_pushbuffer[pushBufferIndex]) = GST_BUFFER_TIMESTAMP(_pushbuffer[pushBufferIndex]);
        auto buffer = _pushbuffer[pushBufferIndex];

        LOG_DEBUG(std::cout << "XXX Frame push info: " << TRACE_VAL(GST_BUFFER_TIMESTAMP (buffer)) << std::endl);
        g_signal_emit_by_name (appsrc, "push-buffer",  buffer, &ret);
        if (ret != 0) {
            LOG_INFO(std::cout << "WARNING " <<"g_signal_emit_by_name returned error: " << ret << std::endl);
        } else{
            LOG_DEBUG(std::cout << "g_signal_emit_by_name succeed" << std::endl);
        }
        _pushBufferIndex++;
    }


    void stop() {
        if ( _isRunning && _currentPipelineElement != NULL ) {
            LOG_INFO(std::cout << "Stop decoder GstreamerPipeline" << std::endl);
            gst_element_set_state( _currentPipelineElement, GST_STATE_NULL );
            gst_object_unref(_currentPipelineElement);
            removeBusWatch();
            _currentPipelineElement = NULL;
            _isRunning = false;
        }
    }

    void removeBusWatch() {
        assert( _busWatchId != INVALID_BUS_WATCH_ID );
        g_source_remove(_busWatchId);
        _busWatchId = INVALID_BUS_WATCH_ID;
    }

    void addBusWatch() {
        assert(_currentPipelineElement != nullptr);
        assert(_busWatchId == INVALID_BUS_WATCH_ID);

        GstBus *bus = gst_element_get_bus( _currentPipelineElement );
        constexpr auto UNUSED_USER_DATA = nullptr;
        _busWatchId = gst_bus_add_watch(bus, busCallback, UNUSED_USER_DATA);
        assert(_busWatchId != INVALID_BUS_WATCH_ID);
        gst_object_unref( bus );
    }

    static gboolean busCallback( GstBus * /* bus */, GstMessage *msg, gpointer /*unusedUserData*/) {
        GError *err = NULL;
        gchar *debug = NULL;

        LOG_ERROR(std::cout << "ERROR " <<"Bus callback called with message of type: " << GST_MESSAGE_TYPE_NAME(msg) << std::endl);

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_EOS:
                g_print("got EOS\n");
                break;
            case GST_MESSAGE_WARNING:
                gst_message_parse_warning(msg, &err, &debug);
                g_print("[WARNING] %s\n%s\n", err->message, debug);
                break;
            case GST_MESSAGE_ERROR:
                gst_message_parse_error(msg, &err, &debug);
                g_print("[ERROR] %s\n%s\n", err->message, debug);
                break;
            default:
                LOG_ERROR(std::cout << "ERROR " <<"Bus callback called with unhandled message type: " << GST_MESSAGE_TYPE_NAME(msg) << std::endl);
                break;
        }
        return TRUE;
    }

    static GstFlowReturn onNewSample( GstElement *element, gpointer user_data )
    {
        LOG_DEBUG(std::cout << " onNewSample" << std::endl);
        GstAppSink *appsink = (GstAppSink *)element;
        GstreamerPipeline *self = (GstreamerPipeline *)user_data;
        self->onVideoFrame(appsink);
        return GST_FLOW_OK;
    }

    void onVideoFrame( GstAppSink *appsink )
    {
        auto sample = gst_app_sink_pull_sample(appsink);
        auto caps = gst_sample_get_caps(sample);
        auto buffer = gst_sample_get_buffer(sample);
        if (caps == NULL || buffer == NULL) {
            LOG_INFO(std::cout << "WARNING  onVideoFrame: caps or buffer is null" << std::endl);
            gst_sample_unref(sample);
            return;
        }
        GstVideoInfo info;
        if (!gst_video_info_from_caps(&info, caps)) {
            LOG_INFO(std::cout << "WARNING  onVideoFrame: failed get info" << std::endl);
            gst_sample_unref(sample);
            return;
        }
        auto timestamp = GST_BUFFER_TIMESTAMP(buffer);
        LOG_DEBUG(std::cout << "onVideoFrame: " << TRACE_VAL(timestamp) << std::endl);
        DUdecodedFrameData decodedFrameData = {._internalData = sample, ._format = DUframeFormat::duYUV420,
                                               ._timestamp = timestamp, ._width = (unsigned) info.width,
                                               ._height = (unsigned) info.height};
        if (_onFrameDecodedCallback) {
            _onFrameDecodedCallback(decodedFrameData);
        }
        LOG_DEBUG(std::cout << "onVideoFrame done: " << TRACE_VAL(timestamp) << std::endl);
    }

    bool extractFrameData(DUdecodedFrameData decodedFrameData, DUcopyFramePlaneMethod copyMethod) {

        GstSample *sample = (GstSample*) decodedFrameData._internalData;

        if (sample == NULL) {
            LOG_INFO(std::cout << "WARNING  Empty sample" << std::endl);
            return false;
        }
        GstCaps *caps = gst_sample_get_caps(sample);
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        if (caps == NULL || buffer == NULL) {
            LOG_INFO(std::cout << "WARNING processGframe: caps or buffer is null" << std::endl);
            return false;
        }
        LOG_DEBUG(std::cout << "Frame pop info: " << TRACE_VAL(GST_BUFFER_TIMESTAMP(buffer))
                                          << TRACE_VAL(decodedFrameData._timestamp) << std::endl);
        GstVideoInfo info;
        if (!gst_video_info_from_caps(&info, caps))
            return false;
        GstVideoFrame frame;
        if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
            return false;

        static std::atomic<bool> loggedStrides{false};
        if (!loggedStrides.exchange(true)) {
            LOG_INFO(std::cout << "extractFrameData strides: Y=" << GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0)
                               << " U=" << GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1)
                               << " V=" << GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2)
                               << " size=" << info.width << "x" << info.height << std::endl);
        }
        const unsigned width  = (unsigned)info.width;
        const unsigned height = (unsigned)info.height;
        const unsigned ySize  = width * height;
        const unsigned uvSize = width * height / 4;
        const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        const int strideU = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
        const int strideV = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2);
        const uint8_t* planeY = (const uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
        const uint8_t* planeU = (const uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&frame, 1);
        const uint8_t* planeV = (const uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&frame, 2);
        for (unsigned r = 0; r < height; r++) {
            copyMethod(width,     r * width,                     (void*)(planeY + r * strideY));
        }
        for (unsigned r = 0; r < height / 2; r++) {
            copyMethod(width / 2, ySize + r * (width / 2),        (void*)(planeU + r * strideU));
            copyMethod(width / 2, ySize + uvSize + r * (width / 2), (void*)(planeV + r * strideV));
        }
        gst_video_frame_unmap (&frame);
        gst_sample_unref(sample);
        return true;
    }

private:
//    Engine::EncodedFrameMetadata _metadataLatch;
//    VideoIntegrity::VideoIntegrityData _integrityDataLatch;
    DUonFrameDecodedCallback _onFrameDecodedCallback = nullptr;
    GstElement *_currentPipelineElement = NULL;
    static constexpr guint INVALID_BUS_WATCH_ID = 0;
    bool                    _noSignal               = false;
    bool                    _noStartTimestamp       = false;
    pid_t                   _threadCheckPipelineStateId     = -1;
    std::atomic<bool>       _restartPipeline        { false };
    bool                    _loop                   = false;
    bool                    _trigger                = false;
    uint32_t                _appSinkFrameIndex      = 0;
    guint                   _busWatchId             = INVALID_BUS_WATCH_ID;
    unsigned long long      _startTimestampFrameIndex = 0;
    // const std::string       _gstreamPipeline = "appsrc name=appsrc ! video/x-ivf, width=1920, height=1080, framerate=30/1 ! ivfparse ! dav1ddec ! autovideosink";
    //const std::string       _gstreamPipeline = "appsrc name=appsrc ! decodebin ! autovideosink sync=false async=false ";
    // TODO we should not use the decodebin and we should use directly with the dav1ddec -> ! ivfparse ! dav1ddec
    //const std::string       _gstreamPipeline = "appsrc name=appsrc ! filesink location=/tmp/AV1Video.ivf";
    const std::string _gstreamPipeline = std::getenv("GSTREAMER_PIPELINE") ?
        std::getenv("GSTREAMER_PIPELINE") :
        "appsrc name=appsrc is-live=true max-bytes=0 ! decodebin ! videoconvert ! video/x-raw,format=I420 ! appsink name=appsink emit-signals=true sync=false";
    std::atomic<bool>       _isRunning;
    GstElement *appsrc = NULL;
    GstFlowReturn ret;
    static constexpr unsigned int RING = 5;
    GstBuffer* _pushbuffer[RING] = {NULL, NULL, NULL, NULL, NULL};
    std::vector<unsigned char> encodedFrames[RING];
    unsigned int _pushBufferIndex = 0;
    unsigned int _lastBufferDataSize = 0;

};

