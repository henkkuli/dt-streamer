#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

class FfmpegOutput {
public:
    virtual ~FfmpegOutput() = default;
    virtual AVIOContext* GetAvioContext() = 0;
};
