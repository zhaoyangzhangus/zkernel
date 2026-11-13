/*
 * 直接使用不可变的 NVIDIA 注册表键声明。选择 NV_DEFINE_REGISTRY_KEY_TABLE 后，
 * 厂商头文件会生成完整的参数表；此处不维护 System 专用的数据库副本。
 */
#include <stddef.h>

#include "nv.h"

#define NV_DEFINE_REGISTRY_KEY_TABLE
#include "nv-reg.h"
