#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <sys/types.h>
#include <linux/ioctl.h>

#define MONITOR_MAGIC 'm'

struct monitor_request {
    pid_t pid;
    char container_id[64];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

#endif
