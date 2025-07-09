import sensor
import image
import time
import tf
import os
import socket
import network
import ustruct

# 1. 摄像头初始化
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=2000)
clock = time.clock()

# 2. 连接WiFi热点
SSID = "test"
PASSWORD = "2912871692"

wlan = network.WLAN(network.STA_IF)
wlan.active(True)
wlan.connect(SSID, PASSWORD)

# 等待WiFi连接
wifi_timeout = 0
while not wlan.isconnected():
    time.sleep(1)
    wifi_timeout += 1
    print(f"等待WiFi连接...（{wifi_timeout}秒）")
    if wifi_timeout > 30:
        print("WiFi连接超时，请检查热点信息！")
        while True:
            time.sleep(1)

# 打印网络信息
ip_info = wlan.ifconfig()
print(f"OpenMV网络信息：IP={ip_info[0]}, 子网掩码={ip_info[1]}, 网关={ip_info[2]}")

# 3. 配置STM32服务器信息
STM32_IP = "192.168.137.211"  # 确保这是STM32的实际IP
STM32_PORT = 8081

# 4. 创建TCP客户端并连接STM32
client_socket = None

def connect_to_stm32():
    global client_socket
    if client_socket:
        try:
            client_socket.close()
        except:
            pass
        client_socket = None

    try:
        client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client_socket.settimeout(5)
        client_socket.connect((STM32_IP, STM32_PORT))
        print(f"✅ 成功连接到STM32：{STM32_IP}:{STM32_PORT}")
        return True
    except OSError as e:
        err_code = e.args[0] if e.args else "未知"
        err_msg = {
            111: "连接被拒绝（STM32未启动服务器或端口错误）",
            112: "主机不可达（IP错误或网络隔离）",
            110: "连接超时（STM32未响应）"
        }.get(err_code, f"错误码{err_code}")
        print(f"❌ 连接失败：{err_msg}")
        client_socket = None
        return False

# 初始连接重试
retry_count = 0
while retry_count < 20 and not connect_to_stm32():
    retry_count += 1
    print(f"重试连接...（{retry_count}/20）")
    time.sleep(2)
if not client_socket:
    print("❌ 超过最大重试次数，无法连接STM32")
    while True:
        time.sleep(1)

# 模型初始化
def load_model():
    model_path = 'trained.tflite'
    if model_path not in os.listdir():
        raise Exception(f"{model_path} 未找到")

    try:
        net = tf.load(model_path, load_to_fb=True)
        print("模型加载成功!")
        return net
    except Exception as e:
        raise Exception("模型加载失败: " + str(e))

# 加载模型
net = load_model()

# 类别标签
labels = ["normal", "fall"]  # 假设0=正常，1=跌倒
confidence_threshold = 0.8  # 设置置信度阈值为0.8

# 类别标签与颜色（绿色表示正常，红色表示跌倒）
colors = [(0, 255, 0), (255, 0, 0)]

# 姿态识别函数（修改：检测到fall且置信度≥0.8时返回1）
def detect_posture(img):
    try:
        # 执行目标检测（使用0.0阈值获取所有结果，后续手动过滤）
        detections = net.detect(img, threshold=0.0)

        # 初始化结果为正常
        detected_posture = 0
        fall_detected = False
        highest_fall_confidence = 0.0

        # 解析检测结果
        for class_idx in range(len(labels)):
            class_name = labels[class_idx]
            class_detections = detections[class_idx] if class_idx < len(detections) else []

            for obj in class_detections:
                try:
                    confidence = obj.output() if callable(getattr(obj, 'output', None)) else 0.0
                    confidence = float(confidence)

                    # 绘制检测框（无论置信度如何）
                    x, y, w, h = obj.rect()
                    color = colors[class_idx]

                    # 绘制矩形框
                    img.draw_rectangle(x, y, w, h, color=color, thickness=2)

                    # 在矩形框上方绘制标签和置信度
                    label_text = f"{class_name}: {confidence:.2f}"
                    img.draw_string(x, y - 12, label_text, color=color, scale=1.5)

                    # 记录最高的fall置信度，并检查是否超过阈值
                    if class_name == "fall":
                        if confidence > highest_fall_confidence:
                            highest_fall_confidence = confidence

                        if confidence >= confidence_threshold:
                            fall_detected = True
                            print(f"⚠️ 检测到跌倒！置信度: {confidence:.2f}（阈值: {confidence_threshold}）")

                except Exception as e:
                    print("解析检测对象失败:", str(e))

        # 打印最终判断依据
        if fall_detected:
            print(f"✅ 确认跌倒：最高fall置信度 {highest_fall_confidence:.2f} ≥ {confidence_threshold}")
        else:
            print(f"❌ 未确认跌倒：最高fall置信度 {highest_fall_confidence:.2f} < {confidence_threshold}")

        # 根据是否检测到fall且置信度达标返回结果
        return 1 if fall_detected else 0

    except Exception as e:
        print(f"姿态检测异常: {e}")
        return 0  # 异常时返回正常状态

# 主循环（增强错误处理）
try:
    while True:
        try:
            # 计算帧率
            clock.tick()

            # 捕获图像
            img = sensor.snapshot()

            # 检测姿态
            posture = detect_posture(img)

            # 检查连接状态
            if not client_socket:
                print("连接丢失，尝试重连...")
                if not connect_to_stm32():
                    time.sleep(2)
                    continue

            # 发送姿态数据
            client_socket.send(ustruct.pack("B", posture))

            # 打印更详细的状态信息
            status_text = "⚠️ 报警: 检测到跌倒" if posture == 1 else "✅ 正常状态"
            print(f"发送姿态：{posture} ({status_text})，帧率：{clock.fps():.1f}")

        except OSError as e:
            print(f"网络错误: {e}, 尝试重连...")
            connect_to_stm32()

        except Exception as e:
            print(f"处理异常: {e}")

        # 控制处理频率
        time.sleep(0.1)

except KeyboardInterrupt:
    print("用户中断，关闭连接...")
    if client_socket:
        client_socket.close()
    print("程序终止")
