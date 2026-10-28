#pragma once
#define SETTINGS_ADAPTER_LIMIT 8U
static os_net_adapter_t g_adapters[SETTINGS_ADAPTER_LIMIT];
static uint32_t g_adapter_count,g_adapter_total;
static bool g_adapters_valid;
static void settings_adapters_refresh(void) {
    g_adapter_count=0;g_adapter_total=0;g_adapters_valid=false;
    for (uint32_t i=0;i<SETTINGS_ADAPTER_LIMIT;++i) {
        os_net_adapter_t item={.hdr={sizeof(item),OS_SYSCALL_ABI_VERSION},.index=i};
        if (settings_syscall_one(OS_SYS_NET_ENUM_ADAPTER,(uintptr_t)&item)!=0) return;
        g_adapter_total=item.count;g_adapters_valid=true;
        if (i>=item.count) break;
        g_adapters[g_adapter_count++]=item;
    }
}
static void settings_network_content(uint32_t parent) {
    uint32_t line=ui_text_line_height(),gap=settings_scale(12U);
    uint32_t total=0;
    for (uint32_t i=0;i<g_adapter_count;++i) {
        bool active=(g_adapters[i].flags&OS_NET_ADAPTER_ACTIVE)!=0;
        total+=(active?8U:3U)*line+settings_scale(32U)+gap;
    }
    if (g_adapter_total>g_adapter_count) total+=line;
    uint32_t reserved=user_caption_height()+settings_scale(36U);
    uint32_t visible=g_window.height>reserved?g_window.height-reserved:0;
    g_scroll_limit=total>visible?total-visible:0;
    if (g_scroll>g_scroll_limit) g_scroll=g_scroll_limit;
    g_nodes[parent].scroll_y=(int32_t)g_scroll;g_nodes[parent].gap=gap;
    if (!g_adapters_valid) {settings_label(parent,"无法读取网卡信息，请再次点击网络",SETTINGS_ERROR_TEXT);return;}
    if (!g_adapter_count) {settings_label(parent,"未检测到 PCI 网络适配器",SETTINGS_HINT_TEXT);return;}
    for (uint32_t i=0;i<g_adapter_count;++i) {
        const os_net_adapter_t *item=&g_adapters[i];
        bool active=(item->flags&OS_NET_ADAPTER_ACTIVE)!=0;
        uint32_t card=settings_card(parent,active?8U:3U);
        char value[96]={0};
        append_text(value,sizeof(value),item->subclass==0?"有线网卡 ":"网络控制器 ");
        append_decimal(value,sizeof(value),i+1);
        if (item->vendor==0x10ECU && item->device==0x8126U) append_text(value,sizeof(value)," · Realtek RTL8126");
        else if (item->vendor==0x8086U) append_text(value,sizeof(value)," · Intel");
        settings_label(card,value,SETTINGS_SECTION_TEXT);
        value[0]=0;append_hex_group(value,sizeof(value),item->vendor);append_text(value,sizeof(value),":");
        append_hex_group(value,sizeof(value),item->device);append_text(value,sizeof(value)," · PCI ");
        append_decimal(value,sizeof(value),item->segment);append_text(value,sizeof(value),":");
        append_decimal(value,sizeof(value),item->bus);append_text(value,sizeof(value),":");
        append_decimal(value,sizeof(value),item->slot);append_text(value,sizeof(value),".");
        append_decimal(value,sizeof(value),item->function);
        settings_label(card,value,SETTINGS_HINT_TEXT);
        if (!active) {settings_label(card,"未作为当前网络接口启用",SETTINGS_HINT_TEXT);continue;}
        bool link=g_status_valid&&(g_status.flags&OS_NET_STATUS_LINK_UP);
        settings_label(card,!g_status_valid?"状态不可用":link?"已连接 · 自动配置（DHCP）":"未连接",
            link?SETTINGS_CONNECTED_TEXT:SETTINGS_DISCONNECTED_TEXT);
        if (!g_status_valid) continue;
        format_mac(value,sizeof(value),g_status.mac);settings_value(card,"MAC  ",value);
        format_ipv4_prefix(value,sizeof(value),g_status.ipv4_address,g_status.ipv4_prefix_length);
        settings_value(card,"IPv4  ",value);
        format_ipv4(value,sizeof(value),g_status.ipv4_gateway);settings_value(card,"网关  ",value);
        format_ipv6(value,sizeof(value),g_status.ipv6_address,(g_status.flags&OS_NET_STATUS_IPV6_CONFIGURED)!=0);
        settings_value(card,"IPv6  ",value);
        value[0]=0;append_decimal(value,sizeof(value),g_status.reset_count);settings_value(card,"重置次数  ",value);
    }
    if (g_adapter_total>g_adapter_count) settings_label(parent,"仅显示前 8 个网络适配器",SETTINGS_HINT_TEXT);
}
