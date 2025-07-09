/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2023-5-10      ShiHao       first version
 * 2024-xx-xx     YourName     Modify to TCP server for OpenMV communication
 * 2025-07-07     YourName     Replace green LED(PF13) with blue LED(PF11)
 * 2025-07-09     YourName     Add PC notification for fall detection
 */

#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include <board.h>
#include <msh.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>  // 用于获取错误码
#include <arpa/inet.h>  // 添加inet_addr函数声明

#include <wlan_mgnt.h>
#include <wlan_prot.h>
#include <wlan_cfg.h>
#include <stdio.h>
#include <stdlib.h>

#define DBG_TAG "main"
#define DBG_LVL DBG_LOG  // 确保调试日志能输出
#include <rtdbg.h>

/* WiFi配置参数 */
#define WLAN_SSID       "533"
#define WLAN_PASSWORD   "RGZN5533@"
#define NET_READY_TIME_OUT (rt_tick_from_millisecond(15 * 1000))

/* 服务器配置（监听端口） */
#define SERVER_PORT     8081  // 与OpenMV连接的端口

/* 电脑服务器配置 */
#define PC_SERVER_IP    "192.168.1.109"  // 修改为电脑的实际IP
#define PC_SERVER_PORT  8888             // 电脑TCP服务器端口

/* 报警消息格式 */
#define ALARM_MSG_FALL      "ALARM: Fall detected! Immediate attention needed!"
#define ALARM_MSG_NORMAL    "ALARM CLEARED: Posture returned to normal"

/* 报警硬件配置 */
#define ALARM_BUZZER_PIN    GET_PIN(B, 0)  // 蜂鸣器引脚(PB10)
#define ALARM_LED_PIN       GET_PIN(F, 12)  // 报警LED引脚(红色,PF12)
#define BLUE_LED_PIN        GET_PIN(F, 11)  // 正常状态LED引脚(蓝色,PF11)

/* 全局变量声明 */
static rt_thread_t recv_thread = RT_NULL;  // 数据接收线程
static rt_thread_t pc_client_thread = RT_NULL;  // 电脑客户端线程
static rt_bool_t net_available = RT_FALSE;
static struct rt_semaphore net_ready;       // 网络就绪信号量
static struct rt_semaphore scan_done;       // 扫描完成信号量
static int server_fd = -1;                  // 服务器Socket
static int client_fd = -1;                  // 客户端(OpenMV)连接Socket
static int pc_server_fd = -1;               // 电脑服务器Socket

/* 函数原型声明 */
void wlan_scan_report_hander(int event, struct rt_wlan_buff *buff, void *parameter);
void wlan_scan_done_hander(int event, struct rt_wlan_buff *buff, void *parameter);
static void accept_thread(void *parameter);  // 接受连接线程

/* 初始化与电脑服务器的连接 */
static void init_pc_server_connection(void) {
    struct sockaddr_in pc_addr;

    LOG_D("=== Initializing PC server connection ===");

    // 创建Socket
    pc_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (pc_server_fd < 0) {
        LOG_E("Create PC server socket failed! error: %d", errno);
        return;
    }

    // 配置服务器地址
    rt_memset(&pc_addr, 0, sizeof(pc_addr));
    pc_addr.sin_family = AF_INET;
    pc_addr.sin_port = htons(PC_SERVER_PORT);
    pc_addr.sin_addr.s_addr = inet_addr(PC_SERVER_IP);

    LOG_D("Attempting to connect to PC server at %s:%d", PC_SERVER_IP, PC_SERVER_PORT);

    // 连接电脑服务器
    if (connect(pc_server_fd, (struct sockaddr*)&pc_addr, sizeof(pc_addr)) <0) {
        LOG_E("Connect to PC server failed! error: %d", errno);
        closesocket(pc_server_fd);
        pc_server_fd = -1;
    } else {
        LOG_I("✅ Successfully connected to PC server");
    }
}

/* 向电脑发送报警消息 */
static void send_alarm_to_pc(rt_bool_t is_fall) {
    const char *msg = is_fall ? ALARM_MSG_FALL : ALARM_MSG_NORMAL;
    int ret;

    // 检查连接状态
    if (pc_server_fd < 0) {
        LOG_W("PC server connection not available, attempting to reconnect...");
        init_pc_server_connection();
    }

    // 发送消息
    if (pc_server_fd >= 0) {
        ret = send(pc_server_fd, msg, strlen(msg), 0);
        if (ret < 0) {
            LOG_E("Failed to send alarm to PC! error: %d", errno);
            closesocket(pc_server_fd);
            pc_server_fd = -1;
        } else {
            LOG_I("📨 Alarm sent to PC: %s", msg);
        }
    } else {
        LOG_E("❌ Cannot send alarm: no connection to PC server");
    }
}

/* 电脑连接维护线程 */
static void pc_client_maintain_thread(void *parameter) {
    LOG_D("PC client maintain thread started");

    while (1) {
        // 每分钟检查一次连接
        rt_thread_mdelay(60000);

        if (pc_server_fd < 0) {
            LOG_D("Attempting to reconnect to PC server...");
            init_pc_server_connection();
        } else {
            // 发送心跳包保持连接
            if (send(pc_server_fd, "PING", 4, 0) < 0) {
                LOG_W("PC server connection lost, will reconnect");
                closesocket(pc_server_fd);
                pc_server_fd = -1;
            } else {
                LOG_D("Heartbeat sent to PC server");
            }
        }
    }
}

/* 报警控制函数 */
static void alarm_control(rt_bool_t enable) {
    if (enable) {
        // 报警状态：红色LED亮、蜂鸣器响、蓝色LED灭
        rt_pin_write(ALARM_BUZZER_PIN, PIN_HIGH);  // 蜂鸣器高电平工作
        rt_pin_write(ALARM_LED_PIN, PIN_LOW);      // 红色LED低电平点亮
        rt_pin_write(BLUE_LED_PIN, PIN_HIGH);      // 蓝色LED高电平熄灭
        LOG_I("Alarm triggered: Abnormal posture detected!");

        /* 向电脑发送跌倒报警 */
        send_alarm_to_pc(RT_TRUE);

    } else {
        // 正常状态：红色LED灭、蜂鸣器停、蓝色LED亮
        rt_pin_write(ALARM_BUZZER_PIN, PIN_LOW);   // 蜂鸣器低电平关闭
        rt_pin_write(ALARM_LED_PIN, PIN_HIGH);     // 红色LED高电平熄灭
        rt_pin_write(BLUE_LED_PIN, PIN_LOW);       // 蓝色LED低电平点亮
        LOG_I("Alarm released: Posture returned to normal");

        /* 向电脑发送报警解除 */
        send_alarm_to_pc(RT_FALSE);
    }
}

/* 硬件初始化 */
static void hardware_init(void) {
    /* 初始化报警引脚 */
    rt_pin_mode(ALARM_BUZZER_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(ALARM_LED_PIN, PIN_MODE_OUTPUT);
    /* 初始化蓝色LED引脚 */
    rt_pin_mode(BLUE_LED_PIN, PIN_MODE_OUTPUT);

    /* 初始状态：关闭报警，点亮蓝色LED */
    alarm_control(RT_FALSE);
    LOG_D("Hardware initialization completed");

    /* 临时测试：验证蓝色LED是否正常工作 */
    LOG_D("Testing blue LED...");
    rt_pin_write(BLUE_LED_PIN, PIN_HIGH);  // 熄灭
    rt_thread_mdelay(500);
    rt_pin_write(BLUE_LED_PIN, PIN_LOW);   // 点亮
    rt_thread_mdelay(500);
    rt_pin_write(BLUE_LED_PIN, PIN_HIGH);  // 熄灭
    rt_thread_mdelay(500);
    rt_pin_write(BLUE_LED_PIN, PIN_LOW);   // 恢复正常状态
    LOG_D("Blue LED test completed");
}

/* 数据接收线程：从OpenMV读取数据并控制报警 */
static void vision_recv_thread(void *parameter) {
    uint8_t recv_buf[1];  // 接收1字节数据(0/1)
    int ret;

    LOG_D("Vision receive thread started");

    while (1) {
        // 等待客户端连接
        while (client_fd < 0) {
            rt_thread_mdelay(100);
            continue;
        }

        // 读取OpenMV发送的数据
        ret = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (ret > 0) {
            LOG_D("Received data: %d", recv_buf[0]);
            // 解析数据：1=跌倒(报警)，0=正常(解除报警)
            if (recv_buf[0] == 1) {
                alarm_control(RT_TRUE);
            } else if (recv_buf[0] == 0) {
                alarm_control(RT_FALSE);
            } else {
                LOG_W("Unknown data: %d", recv_buf[0]);
            }
        } else if (ret == 0) {
            // 客户端断开连接
            LOG_W("OpenMV disconnected");
            closesocket(client_fd);
            client_fd = -1;
            alarm_control(RT_FALSE);  // 连接断开时恢复正常状态
        } else {
            // 接收失败
            LOG_E("Failed to receive data, error code: %d", errno);
            closesocket(client_fd);
            client_fd = -1;
            alarm_control(RT_FALSE);  // 接收错误时恢复正常状态
        }
        rt_thread_mdelay(100);  // 控制接收频率
    }
}

/* 初始化TCP服务器并监听端口 */
static void tcp_server_init(void) {
    struct sockaddr_in server_addr;

    LOG_D("=== Entering tcp_server_init() ===");

    // 创建服务器Socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_E("Step 1/3: Create server socket failed! error code: %d", errno);
        return;
    }
    LOG_D("Step 1/3: Server socket created (fd=%d)", server_fd);

    // 配置服务器地址
    rt_memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    LOG_D("Step 2/3: Server address configured (port=%d)", SERVER_PORT);

    // 绑定端口
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_E("Step 2/3: Bind port %d failed! error code: %d", SERVER_PORT, errno);
        closesocket(server_fd);
        server_fd = -1;
        return;
    }
    LOG_D("Step 2/3: Port %d bound successfully", SERVER_PORT);

    // 开始监听(最大1个连接)
    if (listen(server_fd, 1) < 0) {
        LOG_E("Step 3/3: Listen port %d failed! error code: %d", SERVER_PORT, errno);
        closesocket(server_fd);
        server_fd = -1;
        return;
    }
    LOG_D("Step 3/3: Port %d is listening", SERVER_PORT);

    LOG_I("TCP server started, listening on port %d...", SERVER_PORT);
}

/* 接受连接线程：等待OpenMV连接 */
static void accept_thread(void *parameter) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    LOG_D("Accept thread started, waiting for connection...");

    while (1) {
        // 等待服务器Socket初始化完成
        while (server_fd < 0) {
            rt_thread_mdelay(100);
            continue;
        }

        // 接受客户端连接(阻塞)
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            LOG_E("Accept connection failed! error code: %d", errno);
            rt_thread_mdelay(1000);
            continue;
        }

        LOG_I("✅ OpenMV connected successfully! Client FD: %d", client_fd);
    }
}

/* 网络就绪回调函数 */
static void wlan_ready_handler(int event, struct rt_wlan_buff *buff, void *parameter) {
    net_available = RT_TRUE;
    rt_sem_release(&net_ready);
    LOG_D("Network ready callback triggered");
}

/* 断开连接回调函数 */
static void wlan_station_disconnect_handler(int event, struct rt_wlan_buff *buff, void *parameter) {
    LOG_I("Network disconnected!");
    net_available = RT_FALSE;
    alarm_control(RT_FALSE);  // 网络断开时恢复正常状态

    // 关闭连接
    if (client_fd >= 0) {
        closesocket(client_fd);
        client_fd = -1;
    }

    // 关闭电脑服务器连接
    if (pc_server_fd >= 0) {
        closesocket(pc_server_fd);
        pc_server_fd = -1;
    }
}

static void print_wlan_information(struct rt_wlan_info *info, int index);
static int wifi_autoconnect(void);

int main(void) {
    int result = RT_EOK;
    struct rt_wlan_info info;
    static int i = 0;

    LOG_D("=== System startup ===");

    /* 硬件初始化 */
    hardware_init();

    /* 等待500ms以便WiFi模块初始化 */
    rt_thread_mdelay(500);

    /* 扫描热点 */
    LOG_D("Starting to scan hotspots...");
    rt_sem_init(&scan_done, "scan_done", 0, RT_IPC_FLAG_FIFO);
    rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_REPORT, wlan_scan_report_hander, &i);
    rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_DONE, wlan_scan_done_hander, RT_NULL);

    if (rt_wlan_scan() == RT_EOK) {
        LOG_D("Scanning started...");
    } else {
        LOG_E("Scanning failed");
    }
    rt_sem_take(&scan_done, RT_WAITING_FOREVER);

    /* 连接热点 */
    LOG_D("Starting to connect to hotspot...");
    rt_sem_init(&net_ready, "net_ready", 0, RT_IPC_FLAG_FIFO);

    /* 注册WiFi事件回调 */
    rt_wlan_register_event_handler(RT_WLAN_EVT_READY, wlan_ready_handler, RT_NULL);
    rt_wlan_register_event_handler(RT_WLAN_EVT_STA_DISCONNECTED, wlan_station_disconnect_handler, RT_NULL);

    /* 连接WiFi热点 */
    result = rt_wlan_connect(WLAN_SSID, WLAN_PASSWORD);
    if (result == RT_EOK) {
        rt_memset(&info, 0, sizeof(struct rt_wlan_info));
        rt_wlan_get_info(&info);
        LOG_D("Device information:");
        print_wlan_information(&info, 0);

        /* 等待获取IP */
        result = rt_sem_take(&net_ready, NET_READY_TIME_OUT);
        if (result == RT_EOK) {
            LOG_D("Network is ready! IP address obtained");

            /* 初始化TCP服务器 */
            LOG_D("=== Starting TCP server initialization ===");
            tcp_server_init();

            /* 检查TCP服务器是否初始化成功 */
            if (server_fd < 0) {
                LOG_E("=== TCP server initialization failed! ===");
            } else {
                LOG_D("=== TCP server initialization completed ===");

                /* 创建接受连接线程 */
                rt_thread_t accept_tid = rt_thread_create(
                    "accept",
                    accept_thread,
                    RT_NULL,
                    1024,  // 栈大小
                    24,    // 优先级
                    10     // 时间片
                );
                if (accept_tid) {
                    rt_thread_startup(accept_tid);
                    LOG_D("Accept thread created and started");
                } else {
                    LOG_E("Failed to create accept thread! error code: %d", errno);
                }

                /* 创建数据接收线程 */
                recv_thread = rt_thread_create(
                    "vision_recv",
                    vision_recv_thread,
                    RT_NULL,
                    2048,
                    25,
                    10
                );
                if (recv_thread) {
                    rt_thread_startup(recv_thread);
                    LOG_D("Data receiving thread created and started");
                } else {
                    LOG_E("Failed to create receiving thread! error code: %d", errno);
                }

                /* 初始化电脑服务器连接 */
                init_pc_server_connection();

                /* 创建电脑连接维护线程 */
                pc_client_thread = rt_thread_create(
                    "pc_client",
                    pc_client_maintain_thread,
                    RT_NULL,
                    1024,
                    26,
                    10
                );
                if (pc_client_thread) {
                    rt_thread_startup(pc_client_thread);
                    LOG_D("PC client thread started");
                } else {
                    LOG_E("Failed to create PC client thread!");
                }
            }
        } else {
            LOG_D("Waiting for IP timed out!");
        }

        rt_wlan_unregister_event_handler(RT_WLAN_EVT_READY);
        rt_sem_detach(&net_ready);
    } else {
        LOG_E("Failed to connect to hotspot(%s)!", WLAN_SSID);
    }

    /* 配置自动重连 */
    LOG_D("Starting automatic reconnection...");
    wifi_autoconnect();

    LOG_D("Main function initialization completed");
    return 0;
}

/* 扫描报告处理函数 */
void wlan_scan_report_hander(int event, struct rt_wlan_buff *buff, void *parameter) {
    struct rt_wlan_info *info = RT_NULL;
    int index = 0;
    RT_ASSERT(event == RT_WLAN_EVT_SCAN_REPORT);
    RT_ASSERT(buff != RT_NULL);
    RT_ASSERT(parameter != RT_NULL);

    info = (struct rt_wlan_info *)buff->data;
    index = *((int *)parameter);
    print_wlan_information(info, index);
    ++*((int *)parameter);
}

/* 扫描完成处理函数 */
void wlan_scan_done_hander(int event, struct rt_wlan_buff *buff, void *parameter) {
    RT_ASSERT(event == RT_WLAN_EVT_SCAN_DONE);
    rt_sem_release(&scan_done);
}

static void wlan_connect_handler(int event, struct rt_wlan_buff *buff, void *parameter) {
    rt_kprintf("%s\n", __FUNCTION__);
    if (buff && buff->len == sizeof(struct rt_wlan_info)) {
        rt_kprintf("ssid : %s \n", ((struct rt_wlan_info *)buff->data)->ssid.val);
    }
}

static void wlan_connect_fail_handler(int event, struct rt_wlan_buff *buff, void *parameter) {
    rt_kprintf("%s\n", __FUNCTION__);
    if (buff && buff->len == sizeof(struct rt_wlan_info)) {
        rt_kprintf("ssid : %s \n", ((struct rt_wlan_info *)buff->data)->ssid.val);
    }
}

static void print_wlan_information(struct rt_wlan_info *info, int index) {
    char *security;
    if (index == 0) {
        rt_kprintf("             SSID                      MAC            security    rssi chn Mbps\n");
        rt_kprintf("------------------------------- -----------------  -------------- ---- --- ----\n");
    }

    rt_kprintf("%-32.32s", &(info->ssid.val[0]));
    rt_kprintf("%02x:%02x:%02x:%02x:%02x:%02x  ",
               info->bssid[0], info->bssid[1], info->bssid[2],
               info->bssid[3], info->bssid[4], info->bssid[5]);
    switch (info->security) {
        case SECURITY_OPEN: security = "OPEN"; break;
        case SECURITY_WEP_PSK: security = "WEP_PSK"; break;
        case SECURITY_WEP_SHARED: security = "WEP_SHARED"; break;
        case SECURITY_WPA_TKIP_PSK: security = "WPA_TKIP_PSK"; break;
        case SECURITY_WPA_AES_PSK: security = "WPA_AES_PSK"; break;
        case SECURITY_WPA2_AES_PSK: security = "WPA2_AES_PSK"; break;
        case SECURITY_WPA2_TKIP_PSK: security = "WPA2_TKIP_PSK"; break;
        case SECURITY_WPA2_MIXED_PSK: security = "WPA2_MIXED_PSK"; break;
        case SECURITY_WPS_OPEN: security = "WPS_OPEN"; break;
        case SECURITY_WPS_SECURE: security = "WPS_SECURE"; break;
        default: security = "UNKNOWN"; break;
    }
    rt_kprintf("%-14.14s ", security);
    rt_kprintf("%-4d ", info->rssi);
    rt_kprintf("%3d ", info->channel);
    rt_kprintf("%4d\n", info->datarate / 1000000);
}

static int wifi_autoconnect(void) {
    rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
    rt_wlan_config_autoreconnect(RT_TRUE);
    rt_wlan_register_event_handler(RT_WLAN_EVT_STA_CONNECTED, wlan_connect_handler, RT_NULL);
    rt_wlan_register_event_handler(RT_WLAN_EVT_STA_CONNECTED_FAIL, wlan_connect_fail_handler, RT_NULL);
    return 0;
}
