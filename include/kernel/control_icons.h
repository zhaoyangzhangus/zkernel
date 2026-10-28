#pragma once
#include <stdint.h>
#include <uapi/control_tree.h>
#include <uapi/chrome_caption.h>

/* 公共符号使用归一化矢量轮廓，直接在目标像素上采样，不放大小位图。 */
static inline bool control_icon_round_contains(uint32_t x,uint32_t y,
        uint32_t left,uint32_t top,uint32_t right,uint32_t bottom,uint32_t radius) {
    if (x<left || x>=right || y<top || y>=bottom) return false;
    int32_t dx=x<left+radius ? (int32_t)(left+radius-x) :
        x>right-radius ? (int32_t)(x-(right-radius)) : 0;
    int32_t dy=y<top+radius ? (int32_t)(top+radius-y) :
        y>bottom-radius ? (int32_t)(y-(bottom-radius)) : 0;
    return (uint32_t)(dx*dx+dy*dy)<=radius*radius;
}
/* 圆端线段统一处理直线和斜线，距离计算不依赖输出分辨率。 */
static inline bool control_icon_line(uint32_t x,uint32_t y,
        int32_t ax,int32_t ay,int32_t bx,int32_t by,int32_t radius) {
    int64_t dx=bx-ax,dy=by-ay,px=(int32_t)x-ax,py=(int32_t)y-ay;
    int64_t dot=px*dx+py*dy,length=dx*dx+dy*dy;
    if (dot<=0) return px*px+py*py<=radius*radius;
    if (dot>=length) {
        px=(int32_t)x-bx;py=(int32_t)y-by;
        return px*px+py*py<=radius*radius;
    }
    int64_t cross=px*dy-py*dx;
    return cross*cross<=(int64_t)radius*radius*length;
}
static inline uint32_t control_icon_vector_sample(uint32_t symbol,uint32_t x,uint32_t y) {
    if (symbol==OS_CONTROL_SYMBOL_CLOSE)
        return chrome_caption_contains(3U,(int)(x*1200U/1024U),(int)(y*1200U/1024U)) ? 255U : 0U;
    if (symbol==OS_CONTROL_SYMBOL_FOLDER)
        return control_icon_round_contains(x,y,128,192,512,448,48) ||
            control_icon_round_contains(x,y,64,320,960,896,64) ? 255U : 0U;
    if (symbol!=OS_CONTROL_SYMBOL_FILE) {
        switch (symbol) {
        case OS_CONTROL_SYMBOL_SEARCH: {
            int32_t dx=(int32_t)x-416,dy=(int32_t)y-416,d=dx*dx+dy*dy;
            return (d>=232*232 && d<=296*296) ||
                control_icon_line(x,y,608,608,864,864,36) ? 255U : 0U;
        }
        case OS_CONTROL_SYMBOL_BACK:
        case OS_CONTROL_SYMBOL_FORWARD:
            if (symbol==OS_CONTROL_SYMBOL_FORWARD) x=1023-x;
            return control_icon_line(x,y,608,224,320,512,36) ||
                control_icon_line(x,y,320,512,608,800,36) ? 255U : 0U;
        case OS_CONTROL_SYMBOL_HOME:
            return control_icon_line(x,y,128,448,512,128,32) ||
                control_icon_line(x,y,512,128,896,448,32) ||
                control_icon_line(x,y,256,384,256,864,32) ||
                control_icon_line(x,y,768,384,768,864,32) ||
                control_icon_line(x,y,256,864,768,864,32) ||
                control_icon_line(x,y,448,864,448,608,28) ||
                control_icon_line(x,y,448,608,576,608,28) ||
                control_icon_line(x,y,576,608,576,864,28) ? 255U : 0U;
        case OS_CONTROL_SYMBOL_DRIVE:
            return (control_icon_round_contains(x,y,128,288,896,896,80) &&
                !control_icon_round_contains(x,y,192,352,832,832,24)) ||
                control_icon_line(x,y,192,608,832,608,32) ||
                control_icon_line(x,y,288,736,320,736,24) ||
                control_icon_line(x,y,736,736,736,736,24) ? 255U : 0U;
        case OS_CONTROL_SYMBOL_GRID:
            return control_icon_round_contains(x,y,128,128,448,448,40) ||
                control_icon_round_contains(x,y,576,128,896,448,40) ||
                control_icon_round_contains(x,y,128,576,448,896,40) ||
                control_icon_round_contains(x,y,576,576,896,896,40) ? 255U : 0U;
        case OS_CONTROL_SYMBOL_LIST:
        case OS_CONTROL_SYMBOL_MENU:
            for (uint32_t row=0;row<3;++row) {
                uint32_t cy=256+row*256;
                if (symbol==OS_CONTROL_SYMBOL_LIST &&
                    control_icon_round_contains(x,y,128,cy-48,224,cy+48,20)) return 255;
                if (control_icon_line(x,y,symbol==OS_CONTROL_SYMBOL_LIST?352:160,
                        cy,864,cy,32)) return 255;
            }
            return 0;
        case OS_CONTROL_SYMBOL_SORT_ASCENDING:
        case OS_CONTROL_SYMBOL_SORT_DESCENDING:
            if (symbol==OS_CONTROL_SYMBOL_SORT_DESCENDING) y=1023-y;
            return control_icon_line(x,y,256,160,256,832,32) ||
                control_icon_line(x,y,128,704,256,832,32) ||
                control_icon_line(x,y,256,832,384,704,32) ||
                control_icon_line(x,y,480,288,864,288,32) ||
                control_icon_line(x,y,480,512,736,512,32) ||
                control_icon_line(x,y,480,736,608,736,32) ? 255U : 0U;
        default: return 0;
        }
    }
    if (x<192 || x>=832 || y<64 || y>=960 || (y<256 && x>y+576)) return 0;
    /* 折页与正文线条保留透明度层次，颜色仍由统一主题提供。 */
    if (x>=624 && y<272 && y>=224) return 96;
    if (x>=320 && x<704 && ((y>=448 && y<480) ||
        (y>=608 && y<640) || (y>=768 && y<800))) return 48;
    return 255;
}
static inline uint32_t control_icon_samples_alpha(uint32_t symbol,
        const uint32_t u[4],const uint32_t v[4]) {
    uint32_t coverage=0;
    for (uint32_t sy=0;sy<4;++sy) for (uint32_t sx=0;sx<4;++sx)
        coverage+=control_icon_vector_sample(symbol,u[sx],v[sy]);
    return (coverage+8)/16;
}

/* 扫描一段像素时只在起点做除法，随后通过商和余数精确递推采样坐标。
 * 状态属于当前绘制栈，不缓存颜色、不分配图像，也不引入跨线程共享状态。 */
typedef struct control_icon_axis {
    uint32_t samples[4],remainders[4];
    uint32_t size,step,fraction;
} control_icon_axis_t;

static inline bool control_icon_axis_start(control_icon_axis_t *axis,uint32_t coordinate,uint32_t size) {
    if (!axis || !size || coordinate>=size) return false;
    axis->size=size;axis->step=1024U/size;axis->fraction=1024U%size;
    for (uint32_t i=0;i<4;++i) {
        uint64_t numerator=((uint64_t)coordinate*8+i*2+1)*128;
        axis->samples[i]=(uint32_t)(numerator/size);
        axis->remainders[i]=(uint32_t)(numerator%size);
    }
    return true;
}

static inline void control_icon_axis_next(control_icon_axis_t *axis) {
    for (uint32_t i=0;i<4;++i) {
        axis->samples[i]+=axis->step;
        /* 用减法判断进位，尺寸接近 UINT32_MAX 时也不会让余数相加溢出。 */
        if (axis->remainders[i]>=axis->size-axis->fraction) {
            axis->remainders[i]-=axis->size-axis->fraction;
            ++axis->samples[i];
        } else axis->remainders[i]+=axis->fraction;
    }
}

static inline uint32_t control_icon_vector_alpha(uint32_t symbol,uint32_t x,uint32_t y,uint32_t size) {
    uint32_t u[4],v[4];
    /* 横纵采样坐标各算一次，避免在十六个采样点中重复执行整数除法。 */
    for (uint32_t i=0;i<4;++i) {
        u[i]=(uint32_t)(((uint64_t)x*8+i*2+1)*128/size);
        v[i]=(uint32_t)(((uint64_t)y*8+i*2+1)*128/size);
    }
    /* 四乘四子像素覆盖率只平滑边缘，放大后也不会出现宽达数像素的模糊带。 */
    return control_icon_samples_alpha(symbol,u,v);
}

/* 工具栏、侧边栏及文件图标统一按目标尺寸绘制，继承同一主题颜色。 */
static inline uint32_t control_symbol_alpha(uint32_t symbol,uint32_t x,uint32_t y,uint32_t size) {
    if (!size || x>=size || y>=size) return 0;
    return control_icon_vector_alpha(symbol,x,y,size);
}
