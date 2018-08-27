#include <iostream>
#include <fstream>
#include <vector>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

constexpr size_t INPUT_BUFFER_SIZE = 4096;

class FfmpegException : public std::exception {
public:
    FfmpegException(const std::string& _message) : message(_message) {}

    const char* what() const throw () {
        return message.c_str();
    }

private:
    std::string message;
};

constexpr size_t ERROR_BUFFER_SIZE = 1024;
#define STRINGIFY(x) #x
#define TO_STRING(x) STRINGIFY(x)
#define THROW_FFMPEG(message) throw FfmpegException(std::string(__FILE__ ":" TO_STRING(__LINE__) ": ") + message)
#define THROW_ON_AV_ERROR(expr) do { \
    int error_number_##__LINE__ = expr; \
    if (error_number_##__LINE__  < 0) { \
        /* Get the error message */ \
        std::vector<char> buffer_##__LINE__(ERROR_BUFFER_SIZE); \
        av_make_error_string(buffer_##__LINE__.data(), ERROR_BUFFER_SIZE, error_number_##__LINE__); \
        THROW_FFMPEG(buffer_##__LINE__.data()); \
    } \
} while (false)


void fail(const std::string& message) {
    std::cout << message << std::endl;
    std::exit(1);
}
void log(const std::string& message) {
    std::cout << message << std::endl;
}


class FfmpegInput {
public:
    virtual ~FfmpegInput() = default;
    virtual AVIOContext* GetAvioContext() = 0;
};

constexpr size_t BUFFER_SIZE = 4096;
class FfmpegNetworkInput : public FfmpegInput {
public:
    FfmpegNetworkInput(boost::asio::ip::tcp::socket _socket) : socket(std::move(_socket)) {
        buffer = (uint8_t*) av_malloc(BUFFER_SIZE);
        avio_context = avio_alloc_context(
            buffer,
            BUFFER_SIZE,
            0,
            this,
            ReadData,
            nullptr,
            nullptr
        );
    }

    virtual ~FfmpegNetworkInput() {
        avio_context_free(&avio_context);
        av_free(buffer);
        socket.close();
        // TODO: Ensure thread safety
    }

    virtual AVIOContext* GetAvioContext() {
        return avio_context;
    }

private:
    static int ReadData(void* opaque, uint8_t* buffer, int buffer_size) {
        FfmpegNetworkInput& input = *reinterpret_cast<FfmpegNetworkInput*>(opaque);
        std::size_t data_read = boost::asio::read(input.socket, boost::asio::buffer(buffer, buffer_size));
        // TODO: Handle boost error
        return data_read;
    }

    boost::asio::ip::tcp::socket socket;
    uint8_t* buffer;
    AVIOContext* avio_context;
};

class FfmpegDemuxer;

class FfmpegVideoDecoder {
public:

    // FfmpegVideoDecoder(const std::string& codec_name, std::shared_ptr<FfmpegDemuxer> _demuxer) : demuxer(_demuxer) {
    //     codec = avcodec_find_decoder_by_name(codec_name.c_str());
    //     if (!codec) THROW_FFMPEG("Codec " + codec_name + " not found");
        
    //     // AVPacket* packet = av_packet_alloc();

    //     context = avcodec_alloc_context3(codec);
    //     if (!context) THROW_FFMPEG("Context not found");

    //     THROW_ON_AV_ERROR(avcodec_open2(context, codec, nullptr));
    // }

    FfmpegVideoDecoder(AVStream* _stream) : stream(_stream) {
        codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) THROW_FFMPEG("Codec " + std::to_string(stream->codecpar->codec_id) + " not found");
        
        // AVPacket* packet = av_packet_alloc();

        context = avcodec_alloc_context3(codec);
        if (!context) THROW_FFMPEG("Context not found");
        THROW_ON_AV_ERROR(avcodec_parameters_to_context(context, stream->codecpar));
        THROW_ON_AV_ERROR(avcodec_open2(context, codec, nullptr));

        frame = AllocateFrame();
    }

    void SendPacket(AVPacket* packet) {
        int ret = avcodec_send_packet(context, packet);
        if (ret < 0) return;
        // TODO: Correct error handling
        while (ret >= 0) {
            ret = avcodec_receive_frame(context, frame);
            if (ret == AVERROR(EAGAIN)) break;

            fflush(stdout);

            if (on_frame) on_frame(frame);
        }

        av_packet_unref(packet);
    }
    
    void OnFrame(std::function<void(AVFrame*)> _on_frame) {
        on_frame = _on_frame;
    }

    int GetStreamIndex() {
        return stream->index;
    }

private:
    AVFrame* AllocateFrame() {
        AVFrame* tmpframe = av_frame_alloc();
        if (!tmpframe) THROW_FFMPEG("Failed to allocate frame");

        tmpframe->format = context->pix_fmt;
        tmpframe->width = context->width;
        tmpframe->height = context->height;

        THROW_ON_AV_ERROR(av_frame_get_buffer(tmpframe, 32));
        return tmpframe;
    }


    // std::shared_ptr<FfmpegDemuxer> demuxer;
    AVStream* stream;
    AVCodec* codec;
    AVCodecContext* context;
    AVFrame* frame;
    // AVFrame* background_frame;
    // AVPacket* packet;
    std::function<void(AVFrame*)> on_frame;
};

class FfmpegDemuxer {
public:
    FfmpegDemuxer(AVInputFormat* input_format, std::unique_ptr<FfmpegInput> _input) : input(std::move(_input)) {
        format_context = avformat_alloc_context();
        if (!format_context) THROW_FFMPEG("Failed to allocate format context");
        format_context->pb = input->GetAvioContext();

        THROW_ON_AV_ERROR(avformat_open_input(&format_context, nullptr, input_format, nullptr));

        THROW_ON_AV_ERROR(avformat_find_stream_info(format_context, nullptr));
    }

    FfmpegVideoDecoder* FindVideoStream() {
        for (unsigned int i = 0; i < format_context->nb_streams; i++) {
            AVStream* stream = format_context->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                return video_decoder = new FfmpegVideoDecoder(stream);
            }
        }

        return nullptr;
    }

    void StartDemuxing() {
        Runner();
    }

private:
    void Runner() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) THROW_FFMPEG("Failed to allocate packet");
        while (true) {
            int error_number = av_read_frame(format_context, packet);
            if (error_number == AVERROR(EAGAIN) || error_number == AVERROR_EOF) break;
            THROW_ON_AV_ERROR(error_number);

            // TODO: Detect the correct stream
            if (packet->size && packet->stream_index == video_decoder->GetStreamIndex()) {
                video_decoder->SendPacket(packet);
            } else {
                av_packet_unref(packet);
            }
        }
    }

    AVFormatContext* format_context;
    std::unique_ptr<FfmpegInput> input;
    FfmpegVideoDecoder* video_decoder;
    std::thread thread;
};

void reader(boost::asio::ip::tcp::socket socket) {
    std::vector<uint8_t> input_buffer(INPUT_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

    AVPacket* packet = av_packet_alloc();

    AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) fail("Codec not found");

    AVCodecParserContext* parser = av_parser_init(codec->id);
    if (!parser) fail("Parser not found");

    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) fail("Context not found");

    if (avcodec_open2(context, codec, nullptr) < 0) fail("Could not open codec");

    AVFrame* frame = av_frame_alloc();
    if (!frame) fail("Could not allocate video frame");

    //std::basic_ifstream<char> input_file("/home/henrik/Videos/[TSR]_Death_Note_-_01-preview.mkv", std::ios::binary);
    //if (!input_file) fail("File not found");

    while (socket.is_open()) {
        //input_file.read(reinterpret_cast<char*>(input_buffer.data()), INPUT_BUFFER_SIZE);

        //size_t data_size = input_file.gcount();
        size_t data_size = boost::asio::read(socket, boost::asio::buffer(input_buffer, INPUT_BUFFER_SIZE));
        //std::cout << "Read " << data_size << std::endl;
        //if (!data_size) break;

        uint8_t* data = input_buffer.data();
        while (data_size > 0) {
            int handled = av_parser_parse2(parser, context, &packet->data, &packet->size, data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);

            if (handled < 0) break;
            data += handled;
            data_size -= handled;

            if (packet->size) {
                int ret = avcodec_send_packet(context, packet);
                if (ret < 0) break;
                while (ret >= 0) {
                    ret = avcodec_receive_frame(context, frame);

                    fflush(stdout);
                }
            }
        }
    }
}

class FfmpegOutput {
public:
    virtual ~FfmpegOutput() = default;
    virtual AVIOContext* GetAvioContext() = 0;
};


// constexpr size_t BUFFER_SIZE = 4096;
class FfmpegNetworkOutput : public FfmpegOutput {
public:
    FfmpegNetworkOutput(boost::asio::ip::tcp::socket _socket) : socket(std::move(_socket)) {
        buffer = (uint8_t*) av_malloc(BUFFER_SIZE);
        avio_context = avio_alloc_context(
            buffer,
            BUFFER_SIZE,
            1,
            this,
            nullptr,
            WriteData,
            nullptr
        );
    }

    virtual ~FfmpegNetworkOutput() {
        avio_context_free(&avio_context);
        av_free(buffer);
        socket.close();
        // TODO: Ensure thread safety
    }

    virtual AVIOContext* GetAvioContext() {
        return avio_context;
    }

private:
    static int WriteData(void* opaque, uint8_t* buffer, int buffer_size) {
        FfmpegNetworkOutput& output = *reinterpret_cast<FfmpegNetworkOutput*>(opaque);
        boost::asio::write(output.socket, boost::asio::buffer(buffer, buffer_size));
        // TODO: Handle boost error
        return 0;
    }

    boost::asio::ip::tcp::socket socket;
    uint8_t* buffer;
    AVIOContext* avio_context;
};

class FfmpegMuxer {
public:
    FfmpegMuxer(AVOutputFormat* output_format, std::unique_ptr<FfmpegOutput> _output, int _frame_rate) : output(std::move(_output)), time_base{1, _frame_rate}, frame_rate{_frame_rate, 1} {
        THROW_ON_AV_ERROR(avformat_alloc_output_context2(&format_context, output_format, nullptr, nullptr));
        format_context->pb = output->GetAvioContext();
        
        // stream = avformat_new_stream(format_context, nullptr);
        // if (!stream) THROW_FFMPEG("Failed to create new output stream");
        // stream->time_base = AVRational{1, _frame_rate};
    }

    AVRational GetTimeBase() const {
        return time_base;
    }
    AVRational GetFrameRate() const {
        return frame_rate;
    }

    AVStream* NewStream() {
        AVStream* stream = avformat_new_stream(format_context, nullptr);

        if (!stream) THROW_FFMPEG("Failed to create output stream");
        stream->time_base = GetTimeBase();

        return stream;
    }

    void WriteHeaders() {
        AVDictionary* options = nullptr;
        if (avformat_write_header(format_context, &options)) 
            fail("Failed to write format header");

    }

    void WritePacket(AVPacket* packet) {
        av_interleaved_write_frame(format_context, packet);
        av_packet_unref(packet);
    }

    // TODO: Destructor

private:
    AVFormatContext* format_context;
    std::unique_ptr<FfmpegOutput> output;
    AVRational time_base;
    AVRational frame_rate;
};

class FfmpegVideoEncoder {
public:
    static std::unique_ptr<FfmpegVideoEncoder> CreateEncoder(const std::string& codec_name, std::shared_ptr<FfmpegMuxer> muxer) {
        std::unique_ptr<FfmpegVideoEncoder> encoder(new FfmpegVideoEncoder(codec_name, muxer));
        return std::move(encoder);
    }
    // TODO: Destructor

    AVFrame* GetNextFrame() {
        // TODO: Synchronization
        THROW_ON_AV_ERROR(av_frame_make_writable(background_frame));
        return background_frame;
    }

    void SwapFrames() {
        // TODO: Synchronization
        std::swap(background_frame, foreground_frame);
    }

    void WriteFrame() {
        THROW_ON_AV_ERROR(avcodec_send_frame(context, foreground_frame));

        // if (avcodec_send_frame(context, frame) < 0) fail("Failed to send frame");
        while (1) {
            int error_number = avcodec_receive_packet(context, packet);
            if (error_number == AVERROR(EAGAIN) || error_number == AVERROR_EOF) break;
            THROW_ON_AV_ERROR(error_number);

            // Timebase should be correct so no need for rescaling
            // av_packet_rescale_ts(packet, format_context->streams[packet->stream_index]->time_base, output_stream->time_base);
            packet->stream_index = stream->index;

            muxer->WritePacket(packet);
        }
    }

    int GetWidth() {
        return context->width;
    }

    int GetHeight() {
        return context->height;
    }

private:
    FfmpegVideoEncoder(const std::string& codec_name, std::shared_ptr<FfmpegMuxer> _muxer) : muxer(std::move(_muxer)) {
        codec = avcodec_find_encoder_by_name(codec_name.c_str());
        if (!codec) THROW_FFMPEG("Codec " + codec_name + " not found");

        context = avcodec_alloc_context3(codec);
        if (!context) THROW_FFMPEG("Can't create codec context");

        // TODO: Inject bit_rate, width and height from outside
        context->bit_rate = 10000000;
        context->width = 1920;
        context->height = 1080;
        context->time_base = muxer->GetTimeBase();
        context->framerate = muxer->GetFrameRate();
        context->gop_size = 10;
        context->max_b_frames = 1;
        context->pix_fmt = AV_PIX_FMT_YUV420P;

        THROW_ON_AV_ERROR(avcodec_open2(context, codec, nullptr));
        // if (avcodec_open2(context, codec, nullptr) < 0) THROW_FFMPEG("Could not open codec");

        foreground_frame = AllocateFrame();
        background_frame = AllocateFrame();

        packet = av_packet_alloc();
        if (!packet) THROW_FFMPEG("Failed to allocate packet");

        stream = muxer->NewStream();
        THROW_ON_AV_ERROR(avcodec_parameters_from_context(stream->codecpar, context));
    }

    AVFrame* AllocateFrame() {
        AVFrame* frame = av_frame_alloc();
        if (!frame) THROW_FFMPEG("Failed to allocate frame");

        frame->format = context->pix_fmt;
        frame->width = context->width;
        frame->height = context->height;

        THROW_ON_AV_ERROR(av_frame_get_buffer(frame, 32));
        return frame;
    }

    std::shared_ptr<FfmpegMuxer> muxer;
    AVCodec* codec;
    AVCodecContext* context;
    AVStream* stream;
    AVFrame* foreground_frame;
    AVFrame* background_frame;
    AVPacket* packet;
};


int interrupt_callback(void* arg) {
    std::cout << "WTF" << std::endl;
    return 0;
}

int write_packet(void* arg, uint8_t *buffer, int buffer_size) {
    boost::asio::ip::tcp::socket* socket = (boost::asio::ip::tcp::socket*) arg;
    std::cout << "Writing " << buffer_size << " bytes" << std::endl;
    boost::asio::write(*socket, boost::asio::buffer(buffer, buffer_size));
    return 0;
}

void writer(boost::asio::ip::tcp::socket socket) {
    // AVIOContext* io_context = avio_alloc_context(
    //     (uint8_t*) av_malloc(4096),
    //     4096,
    //     1,
    //     &socket,
    //     nullptr,
    //     write_packet,
    //     nullptr
    // );

    // AVFormatContext* format_context;
    // avformat_alloc_output_context2(&format_context, nullptr, "mpegts", nullptr);
    // if (!format_context) fail("Failed to create output context");

    // format_context->interrupt_callback = {.callback = interrupt_callback, .opaque = nullptr};
    // format_context->pb = io_context;

    // AVOutputFormat* output_format = format_context->oformat;

    //AVCodec* codec = avcodec_find_encoder_by_name("h264");
    // AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    // if (!codec) fail("Codec not found");

    // AVStream* output_stream = avformat_new_stream(format_context, nullptr);
    // if (!output_stream) fail("Failed to allocate output stream");
    // output_stream->id = format_context->nb_streams - 1;

    // AVCodecContext* context = avcodec_alloc_context3(codec);
    // if (!context) fail("Cntext not found");

    // AVPacket* packet = av_packet_alloc();
    // if (!packet) fail("Failed to allcoate packet");

    // context->bit_rate = 10000000;
    // context->width = 1920;
    // context->height = 1080;
    // output_stream->time_base = AVRational{1, 25};
    // context->time_base = output_stream->time_base;
    // context->framerate = AVRational{25, 1};
    // context->gop_size = 10;
    // context->max_b_frames = 1;
    // context->pix_fmt = AV_PIX_FMT_YUV420P;

    // if (avcodec_open2(context, codec, nullptr) < 0) fail("Could not open codec");

    // AVPacket* packet = av_packet_alloc();
    // if (!packet) fail("Failed to allcoate packet");


    // if (avcodec_parameters_from_context(output_stream->codecpar, context) < 0) fail("Failed to copy codec parameters");
    // AVDictionary* options = nullptr;
    // if (avformat_write_header(format_context, &options)) 
    //     fail("Failed to write format header");

    auto output = std::unique_ptr<FfmpegNetworkOutput>(new FfmpegNetworkOutput(std::move(socket)));
    auto muxer = std::make_shared<FfmpegMuxer>(av_guess_format("mpegts", nullptr, nullptr), std::move(output), 30);
    // auto muxer = FfmpegMuxer::CreateMuxer(output);
    auto encoder = FfmpegVideoEncoder::CreateEncoder("libx264", muxer);

    muxer->WriteHeaders();
    
    int i = 0;
    while (1) {
        auto frame = encoder->GetNextFrame();

        for (int y = 0; y < encoder->GetHeight(); y++) {
            for (int x = 0; x < encoder->GetWidth(); x++) {
                frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3;
            }
        }
        for (int y = 0; y < encoder->GetHeight(); y++) {
            for (int x = 0; x < encoder->GetWidth(); x++) {
                frame->data[1][y * frame->linesize[0] + x] = 128 + y + i * 2;
                frame->data[12][y * frame->linesize[0] + x] = 64 + x + i * 5;
            }
        }
        frame->pts = i;
        i++;

        encoder->SwapFrames();

        std::cout << "Frame " << i << " created" << std::endl;
        encoder->WriteFrame();
    }

    // int i = 0;
    // while (1) {
    //     if (av_frame_make_writable(frame) < 0) fail("Failed to make frma writable");

    //     for (int y = 0; y < context->height; y++) {
    //         for (int x = 0; x < context->width; x++) {
    //             frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3;
    //         }
    //     }
    //     for (int y = 0; y < context->height; y++) {
    //         for (int x = 0; x < context->width; x++) {
    //             frame->data[1][y * frame->linesize[0] + x] = 128 + y + i * 2;
    //             frame->data[1][y * frame->linesize[0] + x] = 64 + x + i * 5;
    //         }
    //     }
    //     frame->pts = i;
    //     i++;

    //     std::cout << "Frame " << i << " created" << std::endl;

    //     // Encode the frame
    //     if (avcodec_send_frame(context, frame) < 0) fail("Failed to send frame");
    //     while (1) {
    //         int ret = avcodec_receive_packet(context, packet);
    //         if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    //         if (ret < 0) fail("Error during encoding");

    //         std::cout << "Writing : " << packet->size << " @ " << packet->pts << std::endl;

    //         av_packet_rescale_ts(packet, format_context->streams[packet->stream_index]->time_base, output_stream->time_base);
    //         packet->stream_index = output_stream->index;

    //         av_interleaved_write_frame(format_context, packet);

    //         //socket.write_some(boost::asio::buffer(packet->data, packet->size));
    //         // boost::asio::write(socket, boost::asio::buffer(packet->data, packet->size));
    //         av_packet_unref(packet);
    //     }
    // }
}

void handler(boost::asio::ip::tcp::socket target_socket, boost::asio::ip::tcp::socket source_socket) {
    boost::asio::ip::tcp::endpoint target_endpoint(boost::asio::ip::address::from_string("127.0.0.1"), 5001);

    target_socket.connect(target_endpoint);
    // writer(std::move(target_socket));

    auto output = std::unique_ptr<FfmpegNetworkOutput>(new FfmpegNetworkOutput(std::move(target_socket)));
    auto muxer = std::make_shared<FfmpegMuxer>(av_guess_format("mpegts", nullptr, nullptr), std::move(output), 30);
    auto encoder = FfmpegVideoEncoder::CreateEncoder("libx264", muxer);

    muxer->WriteHeaders();

    auto input = std::unique_ptr<FfmpegNetworkInput>(new FfmpegNetworkInput(std::move(source_socket)));
    auto demuxer = std::make_shared<FfmpegDemuxer>(av_find_input_format("mpegts"), std::move(input));
    // auto decoder = std::unique_ptr<FfmpegVideoDecoder>(new FfmpegVideoDecoder("libx264", demuxer));
    
    int i = 0;
    auto decoder = demuxer->FindVideoStream();
    decoder->OnFrame([&](AVFrame* frame) {
        AVFrame* target_frame = encoder->GetNextFrame();
        av_frame_copy(target_frame, frame);
        std::cout << frame->width << "x" << frame->height << " -> " << target_frame->width << "x" << target_frame->height << " " << i << std::endl;
        // int w = std::min(target_frame->width, frame->width);
        // int h = std::min(target_frame->height, frame->height);
        // for (int y = 0; y < h; y++) {
        //     for (int x = 0; x < std::min(target_frame->width, frame->width); x++) {
        //         target_frame->data[0][y * target_frame->linesize[0] + x] = frame->data[0][y * frame->linesize[0] + x];
        //     }
        // }
        // for (int y = 0; y < h; y++) {
        //     for (int x = 0; x < std::min(target_frame->linesize[1], frame->linesize[1]); x++) {
        //         target_frame->data[1][y * target_frame->linesize[1] + x] = frame->data[1][y * frame->linesize[1] + x];
        //     }
        // }
        // for (int y = 0; y < h; y++) {
        //     for (int x = 0; x < std::min(target_frame->linesize[2], frame->linesize[2]); x++) {
        //         target_frame->data[2][y * target_frame->linesize[2] + x] = frame->data[2][y * frame->linesize[2] + x];
        //     }
        // }
        target_frame->pts = i++;
        encoder->SwapFrames();
        encoder->WriteFrame();
    });
    demuxer->StartDemuxing();

}

int main() {

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
