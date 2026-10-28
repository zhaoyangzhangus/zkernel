#pragma once

#include <kernel/base.h>

#define POWER_MAX_DEVICES 32U

typedef enum power_system_state {
    POWER_SYSTEM_RUNNING = 0,
    POWER_SYSTEM_SUSPENDING,
    POWER_SYSTEM_SUSPENDED,
    POWER_SYSTEM_RESUMING,
    POWER_SYSTEM_FAILED,
} power_system_state_t;

typedef enum power_device_state {
    POWER_DEVICE_RUNNING = 0,
    POWER_DEVICE_SUSPENDING,
    POWER_DEVICE_SUSPENDED,
    POWER_DEVICE_RESUMING,
    POWER_DEVICE_FAILED,
} power_device_state_t;

typedef kstatus_t (*power_suspend_fn)(void *context);
typedef kstatus_t (*power_resume_fn)(void *context);

typedef struct power_device {
    const char *name;
    power_suspend_fn suspend;
    power_resume_fn resume;
    void *context;
    atomic_uint state;
    bool registered;
} power_device_t;

struct device;

bool power_manager_init(void);
/* action：0 为重启，1 为关机；同步文件并停止网卡后才交给固件。 */
kstatus_t power_system_request(uint32_t action);
kstatus_t power_register_device(const char *name, power_suspend_fn suspend,
                                power_resume_fn resume, void *context,
                                power_device_t **out);
kstatus_t power_unregister_device(power_device_t *device);
kstatus_t power_register_device_object(struct device *device,
                                       power_device_t **out);
kstatus_t power_unregister_device_object(struct device *device);
kstatus_t power_system_suspend(void);
kstatus_t power_system_resume(void);
/* 挂起已注册设备后请求 ACPI S3/S4；调用者必须处理返回后的恢复路径。 */
kstatus_t power_system_enter_acpi_sleep(uint8_t sleep_state);
power_system_state_t power_get_system_state(void);
power_device_state_t power_get_device_state(const power_device_t *device);
bool power_self_test(void);
