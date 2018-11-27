#include <iostream>
#include <fstream>
#include <vector>
#include <boost/asio.hpp>
extern "C" {
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

#include "FfmpegNetworkOutput.h"
#include "FfmpegNetworkInput.h"
#include "FfmpegDemuxer.h"
#include "FfmpegMuxer.h"
#include "FfmpegVideoEncoder.h"

void handler(boost::asio::ip::tcp::socket target_socket, boost::asio::ip::tcp::socket source_socket) {
    boost::asio::ip::tcp::endpoint target_endpoint(boost::asio::ip::address::from_string("127.0.0.1"), 5001);

    target_socket.connect(target_endpoint);
    // writer(std::move(target_socket));

    auto output = std::unique_ptr<FfmpegNetworkOutput>(new FfmpegNetworkOutput(std::move(target_socket)));
    auto muxer = std::make_shared<FfmpegMuxer>(av_guess_format("mpegts", nullptr, nullptr), std::move(output), 30);
    auto encoder = FfmpegVideoEncoder::CreateEncoder("libx264", muxer, 1920, 1080);

    muxer->WriteHeaders();

    auto input = std::unique_ptr<FfmpegNetworkInput>(new FfmpegNetworkInput(std::move(source_socket)));
    // auto demuxer = std::make_shared<FfmpegDemuxer>(av_find_input_format("mpegts"), std::move(input));
    auto demuxer = std::make_shared<FfmpegDemuxer>(av_find_input_format("x11grab"), ":0.0");
    // auto decoder = std::unique_ptr<FfmpegVideoDecoder>(new FfmpegVideoDecoder("libx264", demuxer));

    SwsContext* scaling_context = sws_getContext(1920, 1080, AV_PIX_FMT_BGR0, 1920, 1080, AV_PIX_FMT_YUV420P, 0, nullptr, nullptr, nullptr);
    if (!scaling_context) THROW_FFMPEG("Failed to create scaling context");

    int i = 0;
    auto decoder = demuxer->FindVideoStream();
    decoder->OnFrame([&](AVFrame* frame) {
        AVFrame* target_frame = encoder->GetNextFrame();
        sws_scale(scaling_context, frame->data, frame->linesize, 0, frame->height, target_frame->data, target_frame->linesize);
        std::cout << frame->width << "x" << frame->height << " -> " << target_frame->width << "x" << target_frame->height << " " << i << std::endl;
        target_frame->pts = i++;
        encoder->SwapFrames();
        encoder->WriteFrame();
    });
    demuxer->StartDemuxing();

}

int main() {
    avdevice_register_all();

    boost::asio::io_service io_service;
    boost::asio::ip::tcp::acceptor acceptor(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 5000));

    std::vector<std::thread> threads;

    for (;;) {
        boost::asio::ip::tcp::socket source_socket(io_service);
        boost::asio::ip::tcp::socket target_socket(io_service);

        acceptor.accept(source_socket);
        std::cout << "Client accepted" << std::endl;

        threads.emplace_back(handler, std::move(target_socket), std::move(source_socket));
    }

}
