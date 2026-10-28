#pragma once

/* Linux 窗口命令扩展：width=最小化状态（0/1），其余负载字段为零。
 * 与通用 Linux 系统调用号头文件分开存放。 */
#define LINUX_WINDOW_OPERATION_MINIMIZE 6U
/* 独占指针捕获：width=1 表示获取，width=0 表示释放；其余命令字段为零。
 * 放在此处，避免重建通用系统调用 ABI。 */
#define LINUX_WINDOW_OPERATION_CAPTURE 7U
/* address 指向固定 32 字节、以 NUL 结尾的标题；length=32。 */
#define LINUX_WINDOW_OPERATION_TITLE 8U
/* width=1 表示合成器边框，width=0 表示客户端边框；其余负载字段为零。 */
#define LINUX_WINDOW_OPERATION_DECORATIONS 9U
/* 输出：x/y 为客户端屏幕原点，width/height 为客户端尺寸；
 * stride 状态位：bit 0 表示最大化，bit 1 表示全屏。
 * 所有输入负载字段必须为零。 */
#define LINUX_WINDOW_OPERATION_GEOMETRY 10U
/* width=1 表示最大化，width=0 表示还原；其余负载字段为零。 */
#define LINUX_WINDOW_OPERATION_MAXIMIZE 11U
/* width=1 表示进入全屏，width=0 表示还原；其余负载字段为零。 */
#define LINUX_WINDOW_OPERATION_FULLSCREEN 12U
/* 开始交互式移动（width=0）或调整大小（width 为边缘位：
 * left=1、right=2、top=4、bottom=8）。要求 EXPLICIT_MOVE_RESIZE 窗口已收到
 * 左键按下事件且按键仍处于按下状态。其余负载字段为零。 */
#define LINUX_WINDOW_OPERATION_INTERACTIVE 13U
