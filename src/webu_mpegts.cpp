/*
 *    This file is part of MotionPlus.
 *
 *    MotionPlus is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    MotionPlus is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with MotionPlus.  If not, see <https://www.gnu.org/licenses/>.
 *
*/

#include "motionplus.hpp"
#include "conf.hpp"
#include "logger.hpp"
#include "util.hpp"
#include "picture.hpp"
#include "webu.hpp"
#include "webu_stream.hpp"
#include "webu_mpegts.hpp"
#include "alg_sec.hpp"

/* Version independent uint */
#if (MYFFVER <= 60016)
    typedef uint8_t myuint;
#else
    typedef const uint8_t myuint;
#endif

void webu_mpegts_free_context(ctx_webui *webui)
{
    if (webui->picture != NULL) {
        myframe_free(webui->picture);
        webui->picture = NULL;
    }
    if (webui->ctx_codec != NULL) {
        myavcodec_close(webui->ctx_codec);
        webui->ctx_codec = NULL;
    }
    if (webui->fmtctx != NULL) {
        if (webui->fmtctx->pb != NULL) {
            if (webui->fmtctx->pb->buffer != NULL) {
                av_free(webui->fmtctx->pb->buffer);
                webui->fmtctx->pb->buffer = NULL;
            }
            avio_context_free(&webui->fmtctx->pb);
            webui->fmtctx->pb = NULL;
        }
        avformat_free_context(webui->fmtctx);
        webui->fmtctx = NULL;
    }
    MOTPLS_LOG(DBG, TYPE_STREAM, NO_ERRNO, _("closed"));

}

static int webu_mpegts_pic_send(ctx_webui *webui, unsigned char *img)
{
    int retcd;
    char errstr[128];
    struct timespec curr_ts;
    int64_t pts_interval;

    if (webui->picture == NULL) {
        webui->picture = myframe_alloc();
        webui->picture->linesize[0] = webui->ctx_codec->width;
        webui->picture->linesize[1] = webui->ctx_codec->width / 2;
        webui->picture->linesize[2] = webui->ctx_codec->width / 2;

        webui->picture->format = webui->ctx_codec->pix_fmt;
        webui->picture->width  = webui->ctx_codec->width;
        webui->picture->height = webui->ctx_codec->height;

        webui->picture->pict_type = AV_PICTURE_TYPE_I;
        myframe_key(webui->picture);
        webui->picture->pts = 1;
    }

    webui->picture->data[0] = img;
    webui->picture->data[1] = webui->picture->data[0] +
        (webui->ctx_codec->width * webui->ctx_codec->height);
    webui->picture->data[2] = webui->picture->data[1] +
        ((webui->ctx_codec->width * webui->ctx_codec->height) / 4);

    clock_gettime(CLOCK_REALTIME, &curr_ts);
    pts_interval = ((1000000L * (curr_ts.tv_sec - webui->start_time.tv_sec)) +
        (curr_ts.tv_nsec/1000) - (webui->start_time.tv_nsec/1000));
    webui->picture->pts = av_rescale_q(pts_interval
        ,av_make_q(1,1000000L), webui->ctx_codec->time_base);

    retcd = avcodec_send_frame(webui->ctx_codec, webui->picture);
    if (retcd < 0 ) {
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTPLS_LOG(ERR, TYPE_STREAM, NO_ERRNO
            , _("Error sending frame for encoding:%s"), errstr);
        myframe_free(webui->picture);
        webui->picture = NULL;
        return -1;
    }

    return 0;
}

static int webu_mpegts_pic_get(ctx_webui *webui)
{
    int retcd;
    char errstr[128];
    AVPacket *pkt;

    pkt = NULL;
    pkt = mypacket_alloc(pkt);

    retcd = avcodec_receive_packet(webui->ctx_codec, pkt);
    if (retcd == AVERROR(EAGAIN)) {
        mypacket_free(pkt);
        pkt = NULL;
        return -1;
    }
    if (retcd < 0 ) {
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTPLS_LOG(ERR, TYPE_STREAM, NO_ERRNO
            ,_("Error receiving encoded packet video:%s"), errstr);
        //Packet is freed upon failure of encoding
        return -1;
    }

    pkt->pts = webui->picture->pts;

    retcd =  av_interleaved_write_frame(webui->fmtctx, pkt);
    if (retcd < 0 ) {
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTPLS_LOG(ERR, TYPE_STREAM, NO_ERRNO
            ,_("Error while writing video frame. %s"), errstr);
        return -1;
    }

    mypacket_free(pkt);
    pkt = NULL;

    return 0;
}

static void webu_mpegts_resetpos(ctx_webui *webui)
{
    webui->stream_pos = 0;
    webui->resp_used = 0;
}

static int webu_mpegts_getimg(ctx_webui *webui)
{
    ctx_stream_data *strm;
    struct timespec curr_ts;
    unsigned char *img_data;
    int img_sz;

    if (webu_stream_check_finish(webui)) {
        webu_mpegts_resetpos(webui);
        return 0;
    }

    clock_gettime(CLOCK_REALTIME, &curr_ts);

    memset(webui->resp_image, '\0', webui->resp_size);
    webui->resp_used = 0;

    if (webui->device_id > 0) {
        if ((webui->cam->detecting_motion == false) &&
            (webui->motapp->cam_list[webui->camindx]->conf->stream_motion)) {
            webui->stream_fps = 1;
        } else {
            webui->stream_fps = webui->motapp->cam_list[webui->camindx]->conf->stream_maxrate;
        }
        /* Assign to a local pointer the stream we want */
        if (webui->cnct_type == WEBUI_CNCT_TS_FULL) {
            strm = &webui->cam->stream.norm;
        } else if (webui->cnct_type == WEBUI_CNCT_TS_SUB) {
            strm = &webui->cam->stream.sub;
        } else if (webui->cnct_type == WEBUI_CNCT_TS_MOTION) {
            strm = &webui->cam->stream.motion;
        } else if (webui->cnct_type == WEBUI_CNCT_TS_SOURCE) {
            strm = &webui->cam->stream.source;
        } else if (webui->cnct_type == WEBUI_CNCT_TS_SECONDARY) {
            strm = &webui->cam->stream.secondary;
        } else {
            return 0;
        }
        img_sz = (webui->ctx_codec->width * webui->ctx_codec->height * 3)/2;
        img_data = (unsigned char*) mymalloc(img_sz);
        pthread_mutex_lock(&webui->cam->stream.mutex);
            if (strm->img_data == NULL) {
                memset(img_data, 0x00, img_sz);
            } else {
                memcpy(img_data, strm->img_data, img_sz);
                strm->consumed = true;
            }
        pthread_mutex_unlock(&webui->cam->stream.mutex);
    } else {
        webu_stream_all_getimg(webui);

        img_data = (unsigned char*) mymalloc(webui->motapp->all_sizes->img_sz);

        memcpy(img_data, webui->all_img_data, webui->motapp->all_sizes->img_sz);
    }

    if (webu_mpegts_pic_send(webui, img_data) < 0) {
        myfree(&img_data);
        return -1;
    }
    myfree(&img_data);

    if (webu_mpegts_pic_get(webui) < 0) {
        return -1;
    }

    return 0;
}

static int webu_mpegts_avio_buf(void *opaque, myuint *buf, int buf_size)
{
    ctx_webui *webui =(ctx_webui *)opaque;

    if (webui->resp_size < (size_t)(buf_size + webui->resp_used)) {
        webui->resp_size = (size_t)(buf_size + webui->resp_used);
        webui->resp_image = (unsigned char*)realloc(
            webui->resp_image, webui->resp_size);
        MOTPLS_LOG(ERR, TYPE_STREAM, NO_ERRNO
            ,_("resp_image reallocated %d %d %d")
            ,webui->resp_size
            ,webui->resp_used
            ,buf_size);
    }

    memcpy(webui->resp_image + webui->resp_used, buf, buf_size);
    webui->resp_used += buf_size;

    return buf_size;
}

static ssize_t webu_mpegts_response(void *cls, uint64_t pos, char *buf, size_t max)
{
    ctx_webui *webui =(ctx_webui *)cls;
    size_t sent_bytes;
    (void)pos;

    if (webu_stream_check_finish(webui)) {
        return -1;
    }

    if (webui->stream_pos == 0) {
        webu_stream_delay(webui);
        webu_mpegts_resetpos(webui);
        if (webu_mpegts_getimg(webui) < 0) {
            return 0;
        }
    }

    /* If we don't have anything in the avio buffer at this point bail out */
    if (webui->resp_used == 0) {
        webu_mpegts_resetpos(webui);
        return 0;
    }

    if ((webui->resp_used - webui->stream_pos) > max) {
        sent_bytes = max;
    } else {
        sent_bytes = webui->resp_used - webui->stream_pos;
    }

    memcpy(buf, webui->resp_image + webui->stream_pos, sent_bytes);

    webui->stream_pos = webui->stream_pos + sent_bytes;
    if (webui->stream_pos >= webui->resp_used) {
        webui->stream_pos = 0;
    }

    return sent_bytes;

}

int webu_mpegts_open(ctx_webui *webui)
{
    int retcd, img_w, img_h;
    char errstr[128];
    unsigned char   *buf_image;
    AVStream        *strm;
    const AVCodec   *codec;
    AVDictionary    *opts;

    opts = NULL;
    webui->picture = NULL;
    webui->ctx_codec = NULL;
    webui->fmtctx = NULL;
    webui->stream_fps = 10000;   /* For quick start up*/
    clock_gettime(CLOCK_REALTIME, &webui->start_time);

    webui->fmtctx = avformat_alloc_context();
    webui->fmtctx->oformat = av_guess_format("mpegts", NULL, NULL);
    webui->fmtctx->video_codec_id = MY_CODEC_ID_H264;

    codec = avcodec_find_encoder(MY_CODEC_ID_H264);
    strm = avformat_new_stream(webui->fmtctx, codec);

    if (webui->device_id > 0) {
        if ((webui->cnct_type == WEBUI_CNCT_TS_SUB) &&
            ((webui->cam->imgs.width  % 16) == 0) &&
            ((webui->cam->imgs.height % 16) == 0)) {
            img_w = (webui->cam->imgs.width/2);
            img_h = (webui->cam->imgs.height/2);
        } else {
            img_w = webui->cam->imgs.width;
            img_h = webui->cam->imgs.height;
        }
    } else {
        webu_stream_all_sizes(webui);
        img_w = webui->motapp->all_sizes->width;
        img_h = webui->motapp->all_sizes->height;
    }

    webui->ctx_codec = avcodec_alloc_context3(codec);
    webui->ctx_codec->gop_size      = 15;
    webui->ctx_codec->codec_id      = MY_CODEC_ID_H264;
    webui->ctx_codec->codec_type    = AVMEDIA_TYPE_VIDEO;
    webui->ctx_codec->bit_rate      = 400000;
    webui->ctx_codec->width         = img_w;
    webui->ctx_codec->height        = img_h;
    webui->ctx_codec->time_base.num = 1;
    webui->ctx_codec->time_base.den = 90000;
    webui->ctx_codec->pix_fmt       = MY_PIX_FMT_YUV420P;
    webui->ctx_codec->max_b_frames  = 1;
    webui->ctx_codec->flags         |= MY_CODEC_FLAG_GLOBAL_HEADER;
    webui->ctx_codec->framerate.num  = 1;
    webui->ctx_codec->framerate.den  = 1;
    av_opt_set(webui->ctx_codec->priv_data, "profile", "main", 0);
    av_opt_set(webui->ctx_codec->priv_data, "crf", "22", 0);
    av_opt_set(webui->ctx_codec->priv_data, "tune", "zerolatency", 0);
    av_opt_set(webui->ctx_codec->priv_data, "preset", "superfast",0);
    av_dict_set(&opts, "movflags", "empty_moov", 0);

    retcd = avcodec_open2(webui->ctx_codec, codec, &opts);
    if (retcd < 0) {
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTPLS_LOG(ERR, TYPE_STREAM, NO_ERRNO
            ,_("Failed to copy decoder parameters!: %s"), errstr);
        webu_mpegts_free_context(webui);
        av_dict_free(&opts);
        return -1;
    }

    retcd = avcodec_parameters_from_context(strm->codecpar, webui->ctx_codec);
    if (retcd < 0) {
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTPLS_LOG(ERR, TYPE_STREAM, NO_ERRNO
            ,_("Failed to copy decoder parameters!: %s"), errstr);
        webu_mpegts_free_context(webui);
        av_dict_free(&opts);
        return -1;
    }

    if (webui->device_id > 0) {
        webu_stream_checkbuffers(webui);
    } else {
        webu_stream_all_buffers(webui);
    }

    webui->aviobuf_sz = 4096;
    buf_image = (unsigned char*)av_malloc(webui->aviobuf_sz);
    webui->fmtctx->pb = avio_alloc_context(
        buf_image, (int)webui->aviobuf_sz, 1, webui
        , NULL, &webu_mpegts_avio_buf, NULL);
    webui->fmtctx->flags = AVFMT_FLAG_CUSTOM_IO;

    retcd = avformat_write_header(webui->fmtctx, &opts);
    if (retcd < 0) {
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTPLS_LOG(ERR, TYPE_STREAM, NO_ERRNO
            ,_("Failed to write header!: %s"), errstr);
        webu_mpegts_free_context(webui);
        av_dict_free(&opts);
        return -1;
    }

    webui->stream_pos = 0;
    webui->resp_used = 0;

    av_dict_free(&opts);

    return 0;

}

mhdrslt webu_mpegts_main(ctx_webui *webui)
{
    mhdrslt retcd;
    struct MHD_Response *response;
    p_lst *lst = &webui->motapp->webcontrol_headers->params_array;
    p_it it;

    if (webu_mpegts_open(webui) < 0 ) {
        MOTPLS_LOG(ERR, TYPE_STREAM, NO_ERRNO, _("Unable top open mpegts"));
        return MHD_NO;
    }

    clock_gettime(CLOCK_MONOTONIC, &webui->time_last);

    response = MHD_create_response_from_callback (MHD_SIZE_UNKNOWN, 4096
        ,&webu_mpegts_response, webui, NULL);
    if (!response) {
        MOTPLS_LOG(ERR, TYPE_STREAM, NO_ERRNO, _("Invalid response"));
        return MHD_NO;
    }

    if (webui->motapp->webcontrol_headers->params_count > 0) {
        for (it = lst->begin(); it != lst->end(); it++) {
            MHD_add_response_header (response
                , it->param_name.c_str(), it->param_value.c_str());
        }
    }

    MHD_add_response_header(response, "Content-Transfer-Encoding", "BINARY");
    MHD_add_response_header(response, "Content-Type", "application/octet-stream");

    retcd = MHD_queue_response (webui->connection, MHD_HTTP_OK, response);
    MHD_destroy_response (response);

    return retcd;

}

