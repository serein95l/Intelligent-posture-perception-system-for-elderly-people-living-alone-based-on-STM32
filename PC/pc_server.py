import socket
import threading
import time
import winsound  # Windows 系统声音警报

# 服务器配置
SERVER_IP = "0.0.0.0"  # 监听所有接口
SERVER_PORT = 8888

# 报警消息类型定义
FALL_ALARM = "Fall detected"
NORMAL_ALARM = "Posture returned to normal"

# 状态跟踪
current_state = "normal"  # 初始状态为正常
last_alarm_time = time.time()  # 记录上次报警时间


def handle_client(client_socket, address):
    global current_state, last_alarm_time
    print(f"📡 客户端连接: {address[0]}:{address[1]}")

    try:
        while True:
            data = client_socket.recv(1024)
            if not data:
                break

            message = data.decode('utf-8')

            # 处理可能合并的多个报警消息
            alarms = split_alarms(message)

            for alarm in alarms:
                alarm_time = time.strftime('%H:%M:%S')

                # 状态变化时记录日志
                if FALL_ALARM in alarm and current_state != "fall":
                    current_state = "fall"
                    print_fall_alarm(alarm_time, alarm)
                    last_alarm_time = time.time()
                elif NORMAL_ALARM in alarm and current_state != "normal":
                    current_state = "normal"
                    print_normal_alarm(alarm_time, alarm)
                    last_alarm_time = time.time()
                else:
                    # 相同状态的重复报警，只在超过一定时间间隔时记录
                    if time.time() - last_alarm_time > 5:  # 5秒间隔
                        if FALL_ALARM in alarm:
                            print_fall_alarm(alarm_time, alarm)
                        else:
                            print_normal_alarm(alarm_time, alarm)
                        last_alarm_time = time.time()

    except ConnectionResetError:
        print(f"客户端 {address[0]} 断开连接")
    finally:
        client_socket.close()


def split_alarms(message):
    """分割可能合并在一起的多个报警消息"""
    # 处理连续的NORMAL_ALARM合并情况
    alarms = []

    # 先尝试按"ALARM:"分割
    parts = message.split("ALARM:")

    for part in parts:
        part = part.strip()
        if not part:
            continue

        if FALL_ALARM in part:
            alarms.append("ALARM: " + part)
        else:
            # 处理NORMAL_ALARM可能的合并
            normal_parts = part.split("ALARM CLEARED:")
            for np in normal_parts:
                np = np.strip()
                if np:
                    alarms.append("ALARM CLEARED: " + np)

    return alarms


def print_fall_alarm(timestamp, message):
    """格式化打印跌倒报警信息并播放警报声"""
    print(f"🚨 [{timestamp}] 收到报警: {message}")

    # 播放三次警报声
    for _ in range(3):
        winsound.Beep(1000, 500)  # 频率1000Hz，持续时间500ms
        time.sleep(0.2)


def print_normal_alarm(timestamp, message):
    """格式化打印姿态恢复正常的报警信息"""
    print(f"✅ [{timestamp}] 收到报警: {message}")


def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind((SERVER_IP, SERVER_PORT))
    server.listen(5)
    server.settimeout(1.0)  # 设置超时以便优雅退出

    print(f"🖥️ 监护服务器启动，监听端口 {SERVER_PORT}...")
    print("等待STM32设备连接...")
    print(f"服务器IP: {socket.gethostbyname(socket.gethostname())}")

    try:
        while True:
            try:
                client_sock, addr = server.accept()
                client_handler = threading.Thread(
                    target=handle_client,
                    args=(client_sock, addr)
                )
                client_handler.start()
            except socket.timeout:
                continue
    except KeyboardInterrupt:
        print("\n服务器关闭中...")
    finally:
        server.close()


if __name__ == "__main__":
    main()