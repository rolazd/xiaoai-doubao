# 在这块 Armbian 板子上运行豆包

## 板子和安装位置

- 板子：晶晨 S905X（S905X-P212），Armbian 24.11 / Ubuntu 24.04 aarch64，4 核，788 MB 内存。
- 声卡：C-Media USB 声卡（card 1）
  - 采集：16 kHz / 2 声道 S16（两路内容相同，单麦复制）
  - 播放：48 kHz / 2 声道 S16（ALSA `plug` 自动重采样）
- 安装目录：`/opt/xiaoai-doubao`
  - `xiaoai_plus_speaker` 主程序
  - `lib/` sherpa-onnx 共享库（rpath `$ORIGIN/lib`）
  - `assets/` 唤醒模型、tokens、CA 证书
  - `keywords/` 唤醒词（豆包豆包）和退出词（退出）
  - `config.ini` 配置（权限 600）

## 还差一步：火山引擎 API Key

`config.ini` 里的 `api_key` 现在是占位符。把真实的火山引擎 Key 填进去（只改这一行，别发到聊天里）：

```bash
nano /opt/xiaoai-doubao/config.ini
chmod 600 /opt/xiaoai-doubao/config.ini
```

然后启用并启动服务：

```bash
systemctl enable --now xiaoai-doubao
systemctl status xiaoai-doubao
```

看日志：

```bash
journalctl -u xiaoai-doubao -f
```

## 只测唤醒（不需要 Key）

```bash
cd /opt/xiaoai-doubao
./xiaoai_plus_speaker --kws-only \
  --keywords-file keywords/keywords_doubao_repeat_end.txt
```

对着 USB 麦克风说"豆包豆包"，日志里出现 `gate armed` 就是识别到了。Ctrl-C 退出。

## 当前配置的关键点

- 单麦、S16、16 kHz：`[audio] channels=1, bits_per_sample=16`
- 半双工：`duplex_mode=half`（USB 声卡没有播放回采，做不了插话打断）
- 音量控件：`volume_card=1, volume_control=PCM`（范围 0–25，程序按百分比调节）
- 用量目录：程序默认写到 `/data/xiaoai-plus/usage`（权限 700），这块板子上已手动创建；删掉或权限不对会导致唤醒后拒绝开放会话
- 唤醒线程：`kws_num_threads=1`（官方 sherpa 库没打关线程空转的补丁，线程越多待机 CPU 越高）
- 唤醒门槛：`kws_threshold=0.10`，按键词文件里"豆包豆包"的阈值是 0.05

## 重新编译

依赖已经放在板子上：

- sherpa-onnx 共享包：`/root/sherpa_shared`（aarch64 shared-cpu v1.12.25）
- 依赖源码：`/root/deps/{json-3.11.3,IXWebSocket-11.4.5,spdlog-1.15.1}`
- 源码：`/root/xiaoai-doubao-src/xiaoai-plus`
- 一键重建：`/root/build-native.sh`，产物 `build-native/xiaoai_plus_speaker`

重新安装：

```bash
install -m 755 /root/xiaoai-doubao-src/xiaoai-plus/build-native/xiaoai_plus_speaker \
  /opt/xiaoai-doubao/xiaoai_plus_speaker
systemctl restart xiaoai-doubao
```

## WiFi

- 芯片：Realtek **RTL8189FS**（SDIO ID `024c:f179`）。
- 驱动：`8189fs.ko` 已安装到 `/lib/modules/6.1.119-ophub/extra/`，开机自动加载
  （`/etc/modules-load.d/8189fs.conf`），节能参数在 `/etc/modprobe.d/8189fs.conf`。
- 接口：`wlan0` 是主接口，`wlan1` 是驱动并发模式带来的副接口，连接用 `wlan0`。
- 省电已关闭：驱动 `rtw_power_mgnt=0`、`rtw_ips_mode=0`，NetworkManager `wifi.powersave=2`。
  空闲不会进入 PS/IPS 休眠（待机功耗略增），避免长时间闲置后连不上。
- 尚未保存 WiFi 凭据。连接（密码交互输入，不会回显）：

```bash
nmcli --ask device wifi connect "你的SSID"
# 或者
nmtui
```

- 有线 `eth0` 保持连接时，NetworkManager 会优先走有线，WiFi 作为备用。
- 内核升级后 vermagic 会变，`8189fs.ko` 需要按新内核重编
  （源码在 `/root/rtl8189ES_linux`，需要对应版本的 `linux-headers`）。

## 已知限制

- 官方 sherpa 共享库没有项目里的"关线程空转"补丁，待机 CPU 偏高（2 线程时约 1.4 核，1 线程约一半）。要彻底解决需要按 `toolchain/build-sherpa-static.sh` 的补丁从源码重编。
- 唤醒分数（score）字段官方库没有导出，程序里按"命中即接受"处理，结束词阈值过滤在这块板子上不生效。
- 合成语音（espeak 等）无法可靠触发唤醒词，请用真人语音测试。
- 旧的小爱联动（mipns 日志、mphelper、tts_play.sh）在这块板子上是空操作，只会在日志里留一条无害警告。
