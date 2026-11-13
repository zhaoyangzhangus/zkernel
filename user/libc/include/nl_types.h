#pragma once

/* System 只有一个内置 C/POSIX 区域设置，也没有消息目录。保留 POSIX 接口供 libc++ 的 messages
 * facet 编译；运行时会返回调用方提供的默认消息。 */
typedef int nl_catd;

#define NL_CAT_LOCALE 1

#ifdef __cplusplus
extern "C" {
#endif
nl_catd catopen(const char *name, int oflag);
char *catgets(nl_catd catalog, int set_id, int message_id,
              const char *default_message);
int catclose(nl_catd catalog);
#ifdef __cplusplus
}
#endif
