#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Chromium window_control_*_old.icon (12px variant), Linux MD captions.
 * Copyright 2016 The Chromium Authors
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * Neither the name of Google LLC nor the names of its contributors may be
 * used to endorse or promote products derived from this software without
 * specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * 路径坐标以百分之一 DIP 为单位保存；使用 8x8 覆盖采样，不依赖 Skia。
 */
#define CHROME_CAPTION_WIDTH 32U
/* 整数覆盖率在圆周保留一个像素的过渡，任意字号都保持居中圆形。 */
static inline uint32_t os_window_caption_circle_alpha(uint32_t diameter, uint32_t x, uint32_t y) {
    if (!diameter || x>=diameter || y>=diameter) return 0U;
    int64_t dx=2*(int64_t)x+1-diameter,dy=2*(int64_t)y+1-diameter;
    int64_t distance=dx*dx+dy*dy;
    int64_t outer=(int64_t)diameter*diameter;
    int64_t inner=diameter>2U ? (int64_t)(diameter-2U)*(diameter-2U) : 0;
    if (distance>=outer) return 0U;
    return distance<=inner ? 255U : (uint32_t)((outer-distance)*255/(outer-inner));
}
#define CHROME_CAPTION_HEIGHT 40U
#define CHROME_CAPTION_ICON_SIZE 12U
#define CHROME_FRAME_RADIUS 8U
#define CHROME_FRAME_BORDER_ALPHA 0x26U
#define CHROME_FRAME_BORDER_WIDTH 4U
static inline uint32_t chrome_frame_inset(uint32_t row, uint32_t width) {
    static const uint8_t inset[8] = {4, 2, 1, 1, 0, 0, 0, 0};
    return width >= 16U && row < 8U ? inset[row] : 0U;
}

/* 8 DIP 圆形的固定 8x8 子像素覆盖率，镜像应用于四个角。
 * 合成器热路径中不使用浮点运算或超采样。 */
static const uint8_t chrome_corner_coverage[8][8] = {
    {0,0,0,0,48,155,215,255},
    {0,0,12,155,255,255,255,255},
    {0,12,195,255,255,255,255,255},
    {0,155,255,255,255,255,255,255},
    {48,255,255,255,255,255,255,255},
    {155,255,255,255,255,255,255,255},
    {215,255,255,255,255,255,255,255},
    {255,255,255,255,255,255,255,255},
};
/* 系统 1px 窗框的内圈覆盖率，与外圈同心，避免在圆角处产生方形色块。 */
static const uint8_t chrome_corner_inner_coverage[8][8] = {
    {0,0,0,0,0,0,0,0},
    {0,0,0,0,24,135,211,251},
    {0,0,0,92,243,255,255,255},
    {0,0,92,255,255,255,255,255},
    {0,24,243,255,255,255,255,255},
    {0,135,255,255,255,255,255,255},
    {0,211,255,255,255,255,255,255},
    {0,251,255,255,255,255,255,255},
};
static inline uint8_t chrome_frame_alpha(uint32_t x, uint32_t y,
                                         uint32_t width, uint32_t height) {
    if (x >= width || y >= height) return 0U;
    if (width < 16U || height < 16U) return 255U;
    if (x >= width / 2U) x = width - 1U - x;
    if (y >= height / 2U) y = height - 1U - y;
    return x < 8U && y < 8U ? chrome_corner_coverage[y][x] : 255U;
}
static inline uint32_t chrome_frame_row_inset(uint32_t row, uint32_t width,
                                             uint32_t height) {
    if (height < 16U || row >= height) return 0U;
    if (row >= height / 2U) row = height - 1U - row;
    return chrome_frame_inset(row, width);
}

static inline bool chrome_caption_contains(uint32_t icon, int x, int y) {
    if (icon == 1U) return x >= 100 && x < 1100 && y >= 900 && y < 1100;
    if (icon == 2U) return x >= 100 && x < 1100 && y >= 100 && y < 1100 &&
        !(x >= 300 && x < 900 && y >= 300 && y < 900);
    if (icon == 4U) {
        bool upper = x >= 400 && x < 1100 && y >= 100 && y < 800 &&
            !(x >= 600 && x < 900 && y >= 300 && y < 600);
        bool lower = (x >= 100 && x < 300 && y >= 300 && y < 1100) ||
            (x >= 300 && x < 900 && y >= 900 && y < 1100);
        return upper || lower;
    }
    if (icon != 3U) return false;
    static const int16_t points[][2] = {
        {600,459}, {246,105}, {105,246}, {459,600},
        {105,954}, {246,1095}, {600,741}, {954,1095},
        {1095,954}, {741,600}, {1095,246}, {954,105}
    };
    bool inside = false;
    for (unsigned i = 0, j = 11; i < 12; j = i++) {
        int ax = points[i][0], ay = points[i][1];
        int bx = points[j][0], by = points[j][1];
        if ((ay > y) != (by > y)) {
            int64_t left = (int64_t)(x - ax) * (by - ay);
            int64_t right = (int64_t)(bx - ax) * (y - ay);
            if ((by > ay && left < right) || (by < ay && left > right))
                inside = !inside;
        }
    }
    return inside;
}

static inline uint8_t chrome_caption_alpha(uint32_t icon, unsigned x, unsigned y) {
    if (x >= 12U || y >= 12U) return 0;
    unsigned covered = 0;
    for (unsigned sy = 0; sy < 8; ++sy)
        for (unsigned sx = 0; sx < 8; ++sx)
            covered += chrome_caption_contains(icon, (int)x * 100 + (int)(sx * 100 + 50) / 8,
                                               (int)y * 100 + (int)(sy * 100 + 50) / 8);
    return (uint8_t)((covered * 255U + 32U) / 64U);
}
