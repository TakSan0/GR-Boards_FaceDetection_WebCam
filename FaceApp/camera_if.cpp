/* Copyright (c) 2017 Gnomons Vietnam Co., Ltd., Gnomons Co., Ltd.
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
* this list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
* this list of conditions and the following disclaimer in the documentation
* and/or other materials provided with the distribution.
*
* 3. Neither the name of the copyright holder nor the names of its contributors
* may be used to endorse or promote products derived from this software without
* specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*/

#include "camera_if.hpp"
#include "JPEG_Converter.h"

using namespace cv;

static uint8_t FrameBuffer_Video[FRAME_BUFFER_STRIDE * FRAME_BUFFER_HEIGHT]__attribute((section("NC_BSS"),aligned(32)));
static DisplayBase Display;

/* jpeg convert */
#define JPEG_BUFF_NUM          (3)

static uint8_t JpegBuffer[JPEG_BUFF_NUM][1024 * 64]__attribute((section("NC_BSS"),aligned(32)));
static int    buff_lending_cnt[JPEG_BUFF_NUM] = {0};
static size_t jcu_encode_size[JPEG_BUFF_NUM];
static int image_idx = 0;
static int jcu_buf_index_write = 0;
static int jcu_buf_index_write_done = 0;
static int jcu_encoding = 0;
static int Vfield_Int_Skip_Cnt = 0;
static int Vfield_Int_Cnt = 0;
static JPEG_Converter Jcu;
static int jpeg_quality = 75;
static bool set_quality_req = false;

#if MBED_CONF_APP_LCD
#define RESULT_BUFFER_BYTE_PER_PIXEL  (2u)
#define RESULT_BUFFER_STRIDE          (((VIDEO_PIXEL_HW * RESULT_BUFFER_BYTE_PER_PIXEL) + 31u) & ~31u)
static uint8_t user_frame_buffer_result[RESULT_BUFFER_STRIDE * FRAME_BUFFER_HEIGHT]__attribute((section("NC_BSS"),aligned(32)));
static bool draw_square = false;

void ClearSquare(void) {
    if (draw_square) {
        memset(user_frame_buffer_result, 0, sizeof(user_frame_buffer_result));
        draw_square = false;
    }
}

void DrawSquare(int x, int y, int w, int h, uint32_t const colour) {
    int idx_base;
    int wk_idx;
    int i;
    uint8_t coller_pix[RESULT_BUFFER_BYTE_PER_PIXEL];  /* ARGB4444 */

    idx_base = (x + (VIDEO_PIXEL_HW * y)) * RESULT_BUFFER_BYTE_PER_PIXEL;

    /* Select color */
    coller_pix[0] = (colour >> 8) & 0xff;  /* 4:Green 4:Blue */
    coller_pix[1] = colour & 0xff;         /* 4:Alpha 4:Red  */

    /* top */
    wk_idx = idx_base;
    for (i = 0; i < w; i++) {
        user_frame_buffer_result[wk_idx++] = coller_pix[0];
        user_frame_buffer_result[wk_idx++] = coller_pix[1];
    }

    /* middle */
    for (i = 1; i < (h - 1); i++) {
        wk_idx = idx_base + (VIDEO_PIXEL_HW * RESULT_BUFFER_BYTE_PER_PIXEL * i);
        user_frame_buffer_result[wk_idx + 0] = coller_pix[0];
        user_frame_buffer_result[wk_idx + 1] = coller_pix[1];
        wk_idx += (w - 1) * RESULT_BUFFER_BYTE_PER_PIXEL;
        user_frame_buffer_result[wk_idx + 0] = coller_pix[0];
        user_frame_buffer_result[wk_idx + 1] = coller_pix[1];
    }

    /* bottom */
    wk_idx = idx_base + (VIDEO_PIXEL_HW * RESULT_BUFFER_BYTE_PER_PIXEL * (h - 1));
    for (i = 0; i < w; i++) {
        user_frame_buffer_result[wk_idx++] = coller_pix[0];
        user_frame_buffer_result[wk_idx++] = coller_pix[1];
    }
    draw_square = true;
}
#endif

static void JcuEncodeCallBackFunc(JPEG_Converter::jpeg_conv_error_t err_code) {
    if (err_code == JPEG_Converter::JPEG_CONV_OK) {
        jcu_buf_index_write_done = jcu_buf_index_write;
        image_idx++;
    }
    jcu_encoding = 0;
}

static bool check_available_buffer(void) {
    if (buff_lending_cnt[jcu_buf_index_write] == 0) {
        return true;
    }

    for (int i = 0; i < JPEG_BUFF_NUM; i++) {
        if (buff_lending_cnt[i] == 0) {
            jcu_buf_index_write = i;
            return true;
        }
    }

    return false;
}

int SetJpegQuality(int quality)
{
    if ((quality != jpeg_quality) && (quality > 0) && (quality <= 75)) {
        jpeg_quality = quality;
        set_quality_req = true;
    }
    return jpeg_quality;
}

void SetVfieldIntSkipCnt(int skip_cnt) {
    Vfield_Int_Skip_Cnt = skip_cnt;
}

size_t get_jpeg_buff(int * p_image_idx, uint8_t** pp_buf) {
    while ((jcu_encoding == 1) || (*p_image_idx == image_idx)) {
        Thread::wait(1);
    }
    int wk_idx = jcu_buf_index_write_done;
    buff_lending_cnt[wk_idx]++;
    *pp_buf = JpegBuffer[wk_idx];
    *p_image_idx = image_idx;

    return (size_t)jcu_encode_size[wk_idx];
}

void free_jpeg_buff(uint8_t* buff) {
    for (int i = 0; i < JPEG_BUFF_NUM; i++) {
        if (buff == JpegBuffer[i]) {
            if (buff_lending_cnt[i] > 0) {
                buff_lending_cnt[i]--;
            }
            break;
        }
    }
}

static void IntCallbackFunc_Vfield(DisplayBase::int_type_t int_type) {
    if (Vfield_Int_Cnt < Vfield_Int_Skip_Cnt) {
        Vfield_Int_Cnt++;
        return;
    }
    Vfield_Int_Cnt = 0;

    //Interrupt callback function
    if ((jcu_encoding == 0) && (check_available_buffer())) {
        if (set_quality_req) {
            Jcu.SetQuality(jpeg_quality);
            set_quality_req = false;
        }

        JPEG_Converter::bitmap_buff_info_t bitmap_buff_info;
        JPEG_Converter::encode_options_t   encode_options;

        bitmap_buff_info.width              = VIDEO_PIXEL_HW;
        bitmap_buff_info.height             = VIDEO_PIXEL_VW;
        bitmap_buff_info.format             = JPEG_Converter::WR_RD_YCbCr422;
        bitmap_buff_info.buffer_address     = (void *)FrameBuffer_Video;

        encode_options.encode_buff_size     = sizeof(JpegBuffer[0]);
        encode_options.p_EncodeCallBackFunc = &JcuEncodeCallBackFunc;
        encode_options.input_swapsetting    = JPEG_Converter::WR_RD_WRSWA_32_16_8BIT;
        jcu_encoding = 1;
        jcu_encode_size[jcu_buf_index_write] = 0;
        if (Jcu.encode(&bitmap_buff_info, JpegBuffer[jcu_buf_index_write],
            &jcu_encode_size[jcu_buf_index_write], &encode_options) != JPEG_Converter::JPEG_CONV_OK) {
            jcu_encode_size[jcu_buf_index_write] = 0;
            jcu_encoding = 0;
        }
    }
}

/* Starts the camera */
void camera_start(void)
{
    // Initialize the background to black
    for (uint32_t i = 0; i < sizeof(FrameBuffer_Video); i += 2) {
        FrameBuffer_Video[i + 0] = 0x10;
        FrameBuffer_Video[i + 1] = 0x80;
    }

    // Camera
#if ASPECT_RATIO_16_9
    EasyAttach_Init(Display, 640, 360);  //aspect ratio 16:9
#else
    EasyAttach_Init(Display);            //aspect ratio 4:3
#endif

    Display.Graphics_Irq_Handler_Set(DisplayBase::INT_TYPE_S0_VFIELD, 0, IntCallbackFunc_Vfield);

    // Video capture setting (progressive form fixed)
    Display.Video_Write_Setting(
        DisplayBase::VIDEO_INPUT_CHANNEL_0,
        DisplayBase::COL_SYS_NTSC_358,
        (void *)FrameBuffer_Video,
        FRAME_BUFFER_STRIDE,
        VIDEO_FORMAT,
        WR_RD_WRSWA,
        VIDEO_PIXEL_VW,
        VIDEO_PIXEL_HW
    );
    EasyAttach_CameraStart(Display, DisplayBase::VIDEO_INPUT_CHANNEL_0);

#if MBED_CONF_APP_LCD
    DisplayBase::rect_t rect;

    // GRAPHICS_LAYER_0
    rect.vs = 0;
    rect.vw = VIDEO_PIXEL_VW;
    rect.hs = 0;
    rect.hw = VIDEO_PIXEL_HW;
    Display.Graphics_Read_Setting(
        DisplayBase::GRAPHICS_LAYER_0,
        (void *)FrameBuffer_Video,
        FRAME_BUFFER_STRIDE,
        GRAPHICS_FORMAT,
        WR_RD_WRSWA,
        &rect
    );
    Display.Graphics_Start(DisplayBase::GRAPHICS_LAYER_0);

    // GRAPHICS_LAYER_2
    memset(user_frame_buffer_result, 0, sizeof(user_frame_buffer_result));

    rect.vs = 0;
    rect.vw = VIDEO_PIXEL_VW;
    rect.hs = 0;
    rect.hw = VIDEO_PIXEL_HW;
    Display.Graphics_Read_Setting(
        DisplayBase::GRAPHICS_LAYER_2,
        (void *)user_frame_buffer_result,
        RESULT_BUFFER_STRIDE,
        DisplayBase::GRAPHICS_FORMAT_ARGB4444,
        DisplayBase::WR_RD_WRSWA_32_16BIT,
        &rect
    );
    Display.Graphics_Start(DisplayBase::GRAPHICS_LAYER_2);

    Thread::wait(50);
    EasyAttach_LcdBacklight(true);
#endif
}

/* Takes a video frame */
void create_gray(Mat &img_gray)
{
    // Transform buffer into OpenCV matrix
    Mat img_yuv(VIDEO_PIXEL_VW, VIDEO_PIXEL_HW, CV_8UC2, FrameBuffer_Video);

    // Convert from YUV422 to grayscale
    // [Note] Although the camera spec says the color space is YUV422,
    // using the color conversion code COLOR_YUV2GRAY_YUY2 gives
    // better result than using COLOR_YUV2GRAY_Y422
    // (Confirm by saving an image to SD card and then viewing it on PC.)
    cvtColor(img_yuv, img_gray, COLOR_YUV2GRAY_YUY2);
}
