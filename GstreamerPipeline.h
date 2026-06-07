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
            LOG_ERROR(std::cout << " ~~~ " << pipe << std::endl << "[Error]: " << e->message << std::endl);
            throw std::runtime_error(e->message);
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
        // If frameSize exceeds capacity, the vector would reallocate — invalidating any GStreamer buffer
        // still holding a pointer to the old memory. Unref it first to avoid use-after-free.
        if (frameSize > encodedFrames[slot].capacity()) {
            if (_pushbuffer[slot] != NULL) {
                gst_buffer_unref(_pushbuffer[slot]);
                _pushbuffer[slot] = NULL;
            }
        } else if (_pushbuffer[slot] != NULL && !gst_buffer_is_writable(_pushbuffer[slot])) {
            gst_buffer_unref(_pushbuffer[slot]);
        }

        encodedFrames[slot].resize(frameSize);
        memcpy(encodedFrames[slot].data(), frameBuffer, frameSize);
        _lastBufferDataSize = frameSize;

        if (!_capsSet && frameSize >= 5) {
            // Detect codec from first NAL byte after Annex-B start code (00 00 00 01).
            // In H264, SPS/PPS/IDR always have nal_ref_idc=3 (bits 5-6 both set → byte & 0x60 == 0x60).
            // In H265, the first NAL byte (VPS=0x40, SPS=0x42, PPS=0x44, IDR=0x26/0x28)
            // never has both bits 5 and 6 set simultaneously.
            uint8_t nalByte = encodedFrames[slot].data()[4];
            bool isH265 = (nalByte & 0x60) != 0x60;
            const char* mimeType = isH265 ? "video/x-h265" : "video/x-h264";
            GstCaps* caps = gst_caps_new_simple(mimeType,
                                                "stream-format", G_TYPE_STRING, "byte-stream",
                                                "alignment",     G_TYPE_STRING, "au",
                                                NULL);
            g_object_set(appsrc, "caps", caps, NULL);
            gst_caps_unref(caps);
            _capsSet = true;
            LOG_INFO(std::cout << "putEncodedFrame: set appsrc caps to " << mimeType
                               << " (NAL byte=0x" << std::hex << (int)nalByte << std::dec << ")" << std::endl);
        }

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
            return;
        }
        GstVideoInfo info;
        if (!gst_video_info_from_caps(&info, caps)) {
            LOG_INFO(std::cout << "WARNING  onVideoFrame: failed get info" << std::endl);
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

        static bool loggedStrides = false;
        if (!loggedStrides) {
            loggedStrides = true;
            LOG_INFO(std::cout << "extractFrameData strides: Y=" << GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0)
                               << " U=" << GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1)
                               << " V=" << GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2)
                               << " size=" << info.width << "x" << info.height << std::endl);
        }
        unsigned ySize = info.width * info.height;
        unsigned uvSize = info.width * info.height / 4;
        copyMethod(ySize,          0,             GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        copyMethod(uvSize,         ySize,         GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
        copyMethod(uvSize,         ySize + uvSize, GST_VIDEO_FRAME_PLANE_DATA(&frame, 2));
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
    GstBuffer *pushbuffer = NULL;
    GstFlowReturn ret;
    bool                    _firstIDRArived         = false;
    bool                    _capsSet                = false;
    static constexpr unsigned int RING=5;
    GstBuffer* _pushbuffer[RING] = {NULL,NULL,NULL,NULL,NULL};
    std::vector<unsigned char> encodedFrames[RING];
    unsigned int _pushBufferIndex = 0;
    unsigned int _popBufferIndex = 0;
    unsigned int _lastBufferDataSize = 0;

};

