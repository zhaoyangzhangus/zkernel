#pragma once

/* 设置中的网络栏目统一提交控件树，不保存或修改窗口像素。 */
static void update_window(void) {
    uint32_t caption=user_caption_height();
    if (caption==g_caption_height) return;
    os_window_update_t request={.hdr={sizeof(request),OS_SYSCALL_ABI_VERSION},
        .identifier=g_window.identifier,.flags=OS_WINDOW_CAPTION_HEIGHT(caption)};
    if (!settings_syscall_one(OS_SYS_WINDOW_UPDATE,(uintptr_t)&request)) g_caption_height=caption;
}
static void settings_publish(void) {
    if (g_tree_overflow) return;
    /* 定时读取状态不等于画面变化；相同快照不再跨系统调用复制和重新布局。 */
    if (g_tree.revision && g_tree.count==g_node_count && g_tree.text_size==g_text_used &&
        !memcmp(g_nodes,g_published_nodes,g_node_count*sizeof(*g_nodes)) &&
        !memcmp(g_tree_text,g_published_text,g_text_used)) {
        update_window();return;
    }
    os_window_control_tree_t tree={.hdr={sizeof(tree),OS_SYSCALL_ABI_VERSION},
        .identifier=g_window.identifier,.count=g_node_count,.nodes=(uintptr_t)g_nodes,
        .text=(uintptr_t)g_tree_text,.text_size=g_text_used};
    /* 失败时保留屏幕上实际存在的快照，不能用未发布节点校验排队事件。 */
    if (settings_syscall_one(OS_SYS_WINDOW_CONTROL_TREE,(uintptr_t)&tree)) return;
    memcpy(g_published_nodes,g_nodes,g_node_count*sizeof(*g_nodes));
    memcpy(g_published_text,g_tree_text,g_text_used);
    g_tree=tree;g_tree.nodes=(uintptr_t)g_published_nodes;g_tree.text=(uintptr_t)g_published_text;
    update_window();
}
/* 刷新不会改变根键盘目标，不能丢弃旧版本里已排队的按键或修饰键释放。 */
static bool settings_control_valid(const os_control_event_t *event) {
    if (os_control_event_node(&g_tree,g_published_nodes,event)) return true;
    return g_tree.count && event->action==OS_CONTROL_KEY && event->target==1U &&
        event->node_index==0U && event->node_kind==OS_CONTROL_CONTAINER && event->user_data==0U &&
        event->tree_revision && event->tree_revision<=g_tree.revision;
}
