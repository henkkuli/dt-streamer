#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

class FfmpegInput {
public:
    virtual ~FfmpegInput() = default;
    virtual AVIOContext* GetAvioContext() = 0;
};
