// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "videodec2_impl.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_utils.h"
#include "videodec_error.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "common/support/avdec.h"

namespace Libraries::Videodec2 {

// The games read the chroma plane after luma rows padded to 64 (1088 for 1080p; 1472 for 1440p).
static constexpr u32 kHeightAlignment = 64;

// Frames fitted into a game display size (BB_GAME_RES) use the display's own row length (3440 for
// 3440x1440), which the games take as the pitch of the movie planes; others the usual 64.
static u32 PitchAlignment() {
    s32 w = 0, h = 0;
    GameResolution(w, h);
    return w ? 16 : 64;
}

namespace {
// bbport: BB_VDEC_STATS=1 prints the host decode cost per movie frame every 5 seconds.
struct VdecStats {
    using Clock = std::chrono::steady_clock;
    bool on = std::getenv("BB_VDEC_STATS") != nullptr;
    double decode = 0, convert = 0, fit = 0, copy = 0, worst = 0;
    u32 frames = 0;
    Clock::time_point window = Clock::now();
    static double Ms(Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
    void Frame(double d, double c, double f, double cp) {
        decode += d;
        convert += c;
        fit += f;
        copy += cp;
        worst = std::max(worst, d + c + f + cp);
        ++frames;
        const auto now = Clock::now();
        if (Ms(window, now) >= 5000.0) {
            std::printf("VdecStats: %u frames in %.1f s, ms per frame: decode %.2f, convert %.2f, fit %.2f, copy %.2f, worst total %.1f\n",
                        frames, Ms(window, now) / 1000.0, decode / frames, convert / frames,
                        fit / frames, copy / frames, worst);
            decode = convert = fit = copy = worst = 0;
            frames = 0;
            window = now;
        }
    }
};
VdecStats g_vdec_stats;
} // namespace

VdecDecoder::VdecDecoder(const OrbisVideodec2DecoderConfigInfo& config_info,
                         const OrbisVideodec2DecoderMemoryInfo& memory_info) {
    // Test knob: BB_VDEC_CPUFLAGS=snb hides AVX2/FMA3/BMI from libavcodec and libswscale (a Sandy Bridge feature set).
    if (const char* f = std::getenv("BB_VDEC_CPUFLAGS"); f && !std::strcmp(f, "snb")) {
        av_force_cpu_flags(AV_CPU_FLAG_MMX | AV_CPU_FLAG_MMXEXT | AV_CPU_FLAG_SSE | AV_CPU_FLAG_SSE2 |
                           AV_CPU_FLAG_SSE3 | AV_CPU_FLAG_SSSE3 | AV_CPU_FLAG_SSE4 |
                           AV_CPU_FLAG_SSE42 | AV_CPU_FLAG_AVX | AV_CPU_FLAG_CMOV);
    }
    const AVCodec* codec = avcodec_find_decoder(
        config_info.codec_type == OrbisVideodec2CodecType::Avc ? AV_CODEC_ID_H264
                                                               : AV_CODEC_ID_HEVC);
    ASSERT(codec);

    m_codec_context = avcodec_alloc_context3(codec);
    ASSERT(m_codec_context);
    m_codec_context->width = config_info.max_frame_width;
    m_codec_context->height = config_info.max_frame_height;
    m_codec_context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    avcodec_open2(m_codec_context, codec, nullptr);
}

VdecDecoder::~VdecDecoder() {
    avcodec_free_context(&m_codec_context);
    sws_freeContext(m_sws_context);
    sws_freeContext(m_fit_context);
    sws_free_context(&m_fit_threaded);
}

s32 VdecDecoder::Decode(const OrbisVideodec2InputData& input_data,
                        OrbisVideodec2FrameBuffer& frame_buffer,
                        OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    if (!input_data.au_data) {
        LOG_ERROR(Lib_Vdec2, "ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER");
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER;
    }
    if (input_data.au_size == 0) {
        LOG_ERROR(Lib_Vdec2, "ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE");
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE;
    }

    const auto t0 = VdecStats::Clock::now();
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR(Lib_Vdec2, "Failed to allocate packet");
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    packet->data = (u8*)input_data.au_data;
    packet->size = input_data.au_size;
    packet->pts = input_data.pts_data;
    packet->dts = input_data.dts_data;
    packet->opaque = reinterpret_cast<void*>(input_data.attached_data);

    int ret = avcodec_send_packet(m_codec_context, packet);
    if (ret == AVERROR_EOF) {
        // Attempt to flush buffers and try again.
        avcodec_flush_buffers(m_codec_context);
        ret = avcodec_send_packet(m_codec_context, packet);
    }
    if (ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Error sending packet to decoder: {}", ret);
        av_packet_free(&packet);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        LOG_ERROR(Lib_Vdec2, "Failed to allocate frame");
        av_packet_free(&packet);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    ret = avcodec_receive_frame(m_codec_context, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        LOG_TRACE(Lib_Vdec2, "AVERROR_EOF or AVERROR(EAGAIN)");
        av_packet_free(&packet);
        av_frame_free(&frame);
        return ORBIS_OK;
    } else if (ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Error receiving frame from decoder: {}", ret);
        av_packet_free(&packet);
        av_frame_free(&frame);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    const auto t1 = VdecStats::Clock::now();
    if (frame->flags & AV_FRAME_FLAG_INTERLACED) {
        LOG_ERROR(Lib_Vdec2, "Interlaced video output is not suported.");
    }

    if (frame->format != AV_PIX_FMT_NV12) {
        AVFrame* nv12_frame = ConvertNV12Frame(*frame);
        ASSERT(nv12_frame);
        av_frame_free(&frame);
        frame = nv12_frame;
    }
    const auto t2 = VdecStats::Clock::now();
    frame = FitToDisplay(frame);
    const auto t3 = VdecStats::Clock::now();

    const bool is_avc = m_codec_context->codec_id == AV_CODEC_ID_H264;
    const u64 info_size =
        is_avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);
    Videodec::CopyNV12Data((u8*)frame_buffer.frame_buffer,
                           frame_buffer.frame_buffer_size - info_size, *frame, kHeightAlignment, PitchAlignment());
    frame_buffer.is_accepted = true;
    if (g_vdec_stats.on) {
        const auto t4 = VdecStats::Clock::now();
        g_vdec_stats.Frame(VdecStats::Ms(t0, t1), VdecStats::Ms(t1, t2), VdecStats::Ms(t2, t3),
                           VdecStats::Ms(t3, t4));
    }

    const auto width = Common::AlignUp<u32>(frame->width, 16);
    const auto pitch = Common::AlignUp<u32>(frame->width, PitchAlignment());
    const auto height = Common::AlignUp<u32>(frame->height, kHeightAlignment);

    output_info.is_valid = true;
    output_info.is_error_frame = false;
    output_info.picture_count = 1; // TODO: 2 pictures for interlaced video
    output_info.codec_type = is_avc ? OrbisVideodec2CodecType::Avc : OrbisVideodec2CodecType::Hevc;
    output_info.frame_width = width;
    output_info.frame_pitch = pitch;
    output_info.frame_height = height;
    output_info.frame_buffer = frame_buffer.frame_buffer;
    output_info.frame_buffer_size = (pitch * height * 3) / 2;

    // Only set frameFormat and framePitchInBytes if the game uses the newer struct version.
    if (output_info.this_size == sizeof(OrbisVideodec2OutputInfo)) {
        output_info.frame_format = 0;
        output_info.frame_pitch_in_bytes = pitch;
    }

    if (is_avc) {
        auto& picture_info = *(OrbisVideodec2AvcPictureInfo*)((u8*)output_info.frame_buffer +
                                                              output_info.frame_buffer_size);

        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - frame->width;
        picture_info.frame_crop_bottom_offset = height - frame->height;
    } else {
        auto& picture_info = *(OrbisVideodec2HevcPictureInfo*)((u8*)output_info.frame_buffer +
                                                               output_info.frame_buffer_size);
        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - frame->width;
        picture_info.frame_crop_bottom_offset = height - frame->height;
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    return ORBIS_OK;
}

s32 VdecDecoder::Flush(OrbisVideodec2FrameBuffer& frame_buffer,
                       OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR(Lib_Vdec2, "Failed to allocate frame");
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    avcodec_send_packet(m_codec_context, nullptr);
    int ret = avcodec_receive_frame(m_codec_context, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_frame_free(&frame);
        return ORBIS_OK;
    } else if (ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Error receiving frame from decoder: {}", ret);
        av_frame_free(&frame);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    if (frame->format != AV_PIX_FMT_NV12) {
        AVFrame* nv12_frame = ConvertNV12Frame(*frame);
        ASSERT(nv12_frame);
        av_frame_free(&frame);
        frame = nv12_frame;
    }
    frame = FitToDisplay(frame);

    const bool is_avc = m_codec_context->codec_id == AV_CODEC_ID_H264;
    const u64 info_size =
        is_avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);
    Videodec::CopyNV12Data((u8*)frame_buffer.frame_buffer,
                           frame_buffer.frame_buffer_size - info_size, *frame, kHeightAlignment, PitchAlignment());
    frame_buffer.is_accepted = true;

    const auto width = Common::AlignUp<u32>(frame->width, 16);
    const auto pitch = Common::AlignUp<u32>(frame->width, PitchAlignment());
    const auto height = Common::AlignUp<u32>(frame->height, kHeightAlignment);

    output_info.is_valid = true;
    output_info.is_error_frame = false;
    output_info.picture_count = 1; // TODO: 2 pictures for interlaced video
    output_info.codec_type = is_avc ? OrbisVideodec2CodecType::Avc : OrbisVideodec2CodecType::Hevc;
    output_info.frame_width = width;
    output_info.frame_pitch = pitch;
    output_info.frame_height = height;
    output_info.frame_buffer = frame_buffer.frame_buffer;
    output_info.frame_buffer_size = (pitch * height * 3) / 2;

    // Only set frameFormat and framePitchInBytes if the game uses the newer struct version.
    if (output_info.this_size == sizeof(OrbisVideodec2OutputInfo)) {
        output_info.frame_format = 0;
        output_info.frame_pitch_in_bytes = pitch;
    }

    if (m_codec_context->codec_id == AV_CODEC_ID_H264) {
        auto& picture_info = *(OrbisVideodec2AvcPictureInfo*)((u8*)output_info.frame_buffer +
                                                              output_info.frame_buffer_size);

        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - frame->width;
        picture_info.frame_crop_bottom_offset = height - frame->height;
    } else {
        auto& picture_info = *(OrbisVideodec2HevcPictureInfo*)((u8*)output_info.frame_buffer +
                                                               output_info.frame_buffer_size);
        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - frame->width;
        picture_info.frame_crop_bottom_offset = height - frame->height;
    }

    av_frame_free(&frame);
    return ORBIS_OK;
}

s32 VdecDecoder::Reset() {
    avcodec_flush_buffers(m_codec_context);
    return ORBIS_OK;
}

AVFrame* VdecDecoder::ConvertNV12Frame(AVFrame& frame) {
    AVFrame* nv12_frame = av_frame_alloc();
    nv12_frame->pts = frame.pts;
    nv12_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    nv12_frame->format = AV_PIX_FMT_NV12;
    nv12_frame->width = frame.width;
    nv12_frame->height = frame.height;
    nv12_frame->sample_aspect_ratio = frame.sample_aspect_ratio;
    nv12_frame->crop_top = frame.crop_top;
    nv12_frame->crop_bottom = frame.crop_bottom;
    nv12_frame->crop_left = frame.crop_left;
    nv12_frame->crop_right = frame.crop_right;
    nv12_frame->opaque = frame.opaque;

    av_frame_get_buffer(nv12_frame, 0);

    if (m_sws_context == nullptr) {
        m_sws_context = sws_getContext(frame.width, frame.height, AVPixelFormat(frame.format),
                                       nv12_frame->width, nv12_frame->height, AV_PIX_FMT_NV12,
                                       SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    }

    const auto res = sws_scale(m_sws_context, frame.data, frame.linesize, 0, frame.height,
                               nv12_frame->data, nv12_frame->linesize);
    if (res < 0) {
        LOG_ERROR(Lib_Vdec2, "Could not convert to NV12: {}", av_err2str(res));
        return nullptr;
    }

    return nv12_frame;
}

void GameResolution(s32& width, s32& height) {
    static s32 w = -1, h = 0;
    if (w < 0) {
        w = 0;
        unsigned pw = 0, ph = 0;
        if (const char* res = std::getenv("BB_GAME_RES");
            res && std::sscanf(res, "%ux%u", &pw, &ph) == 2 && pw >= 640 && ph >= 360 &&
            !(pw == 1920 && ph == 1080)) {
            w = static_cast<s32>(pw & ~1u);
            h = static_cast<s32>(ph & ~1u);
        }
    }
    width = w;
    height = h;
}

// The fit scale is the main CPU cost of a movie at a display size above 1080p (single-threaded bicubic NV12 scale
// of every frame: about 5 ms at 2560x1440 with SSE/AVX only, several times that on an old core). The frame API of
// libswscale splits the picture into bands over worker threads and gives the same pixels as sws_scale.
bool VdecDecoder::ScaleThreaded(const AVFrame* src, AVFrame* canvas, s32 x0, s32 y0, s32 fit_w,
                                s32 fit_h) {
    static const bool disabled = std::getenv("BB_VDEC_FIT_THREADS") &&
                                 !std::strcmp(std::getenv("BB_VDEC_FIT_THREADS"), "0");
    if (disabled) {
        return false;
    }
    if (!m_fit_threaded) {
        m_fit_threaded = sws_alloc_context();
        if (!m_fit_threaded) {
            return false;
        }
        m_fit_threaded->flags = SWS_BICUBIC;
        m_fit_threaded->threads = std::clamp(av_cpu_count() / 2, 1, 8);
    }
    AVFrame* view = av_frame_alloc();   // headers only: points into the canvas, owns nothing
    if (!view) {
        return false;
    }
    view->format = AV_PIX_FMT_NV12;
    view->width = fit_w;
    view->height = fit_h;
    view->data[0] = canvas->data[0] + y0 * canvas->linesize[0] + x0;
    view->linesize[0] = canvas->linesize[0];
    view->data[1] = canvas->data[1] + (y0 / 2) * canvas->linesize[1] + x0;
    view->linesize[1] = canvas->linesize[1];
    view->colorspace = src->colorspace;   // same properties both sides: a plain scale, no conversion
    view->color_range = src->color_range;
    view->color_primaries = src->color_primaries;
    view->color_trc = src->color_trc;
    view->chroma_location = src->chroma_location;
    const int ret = sws_scale_frame(m_fit_threaded, view, src);
    av_frame_free(&view);
    return ret >= 0;
}

AVFrame* VdecDecoder::FitToDisplay(AVFrame* frame) {
    s32 target_w = 0, target_h = 0;
    GameResolution(target_w, target_h);
    if (!target_w || !frame || frame->format != AV_PIX_FMT_NV12 ||
        (frame->width == target_w && frame->height == target_h)) {
        return frame;
    }
    AVFrame* canvas = av_frame_alloc();
    canvas->format = AV_PIX_FMT_NV12;
    canvas->width = target_w;
    canvas->height = target_h;
    canvas->pts = frame->pts;
    canvas->pkt_dts = frame->pkt_dts < 0 ? 0 : frame->pkt_dts;
    canvas->opaque = frame->opaque;
    if (av_frame_get_buffer(canvas, 0) < 0) {
        av_frame_free(&canvas);
        return frame;
    }
    // Fit inside the canvas keeping the movie's shape (even sizes: 4:2:0).
    const double scale = std::min(static_cast<double>(target_w) / frame->width,
                                  static_cast<double>(target_h) / frame->height);
    s32 fit_w = std::min(target_w, static_cast<s32>(frame->width * scale + 0.5) & ~1);
    s32 fit_h = std::min(target_h, static_cast<s32>(frame->height * scale + 0.5) & ~1);
    const s32 x0 = ((target_w - fit_w) / 2) & ~1, y0 = ((target_h - fit_h) / 2) & ~1;
    if (fit_w != target_w || fit_h != target_h) {   // black bars; a movie covering the canvas overwrites it all
        for (s32 y = 0; y < target_h; ++y) {
            std::memset(canvas->data[0] + y * canvas->linesize[0], 16, target_w);
        }
        for (s32 y = 0; y < target_h / 2; ++y) {
            std::memset(canvas->data[1] + y * canvas->linesize[1], 128, target_w);
        }
    }
    if (ScaleThreaded(frame, canvas, x0, y0, fit_w, fit_h)) {
        av_frame_free(&frame);
        return canvas;
    }
    m_fit_context = sws_getCachedContext(m_fit_context, frame->width, frame->height, AV_PIX_FMT_NV12,
                                         fit_w, fit_h, AV_PIX_FMT_NV12, SWS_BICUBIC, nullptr,
                                         nullptr, nullptr);
    if (m_fit_context) {
        u8* dst[4] = {canvas->data[0] + y0 * canvas->linesize[0] + x0,
                      canvas->data[1] + (y0 / 2) * canvas->linesize[1] + x0, nullptr, nullptr};
        const int dst_stride[4] = {canvas->linesize[0], canvas->linesize[1], 0, 0};
        sws_scale(m_fit_context, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
    }
    av_frame_free(&frame);
    return canvas;
}

} // namespace Libraries::Videodec2
