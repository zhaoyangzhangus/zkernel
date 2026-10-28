#pragma once

/* libc++ 会先探测此 glibc 风格路径，再回退到 wchar.h。 */
#include <libc/mbstate.h>
