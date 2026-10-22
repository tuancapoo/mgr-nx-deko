# MGR Wwise: bằng chứng từ log Switch 2026-10-16

Chưa thể xác định hàm nóng bên trong `AK::EventManager` hoặc phân chia guest mix/event, decode, đồng bộ và wait. Log hiện tại **tắt lấy mẫu stack**; không có PC để tra `addr2line`.

Nguồn: `out/nx/switch_logs/rex_profile.log` (209064 byte), cùng `mgr_001.log`. Chỉ có một `rex_profile.log` trong `out/` khi kiểm tra. Bỏ báo cáo startup 0 giây vì các giá trị rate chưa có khoảng đo hợp lệ.

Đây là log stock-clock cũ, không phải lượt 360p/1785 MHz mà người dùng báo EventManager khoảng 35% một core. Các số bên dưới không thay thế kết quả mới đó; hiện chưa có stack/caller của lượt mới để phân tích.

32 khoảng normal, mỗi khoảng khoảng 10 giây, đều `samples 0`; dòng 3 ghi `Stack sampling: disabled`. CPU dưới đây là phần trăm **một core**, theo `svcGetInfo(InfoType_ThreadTickCount)` trong `sdk/src/ui/switch_perf.cpp:422`.

| Luồng | 17 khoảng EventManager ít hoạt động | 15 khoảng EventManager hoạt động nhiều |
|---|---:|---:|
| AK::EventManage | 0.6% | 9.3–11.5%, trung bình 11.01% |
| XMA Decoder | 0.0% | 0.0–1.0%, trung bình 0.21% |
| Audio Worker | 1.4–1.5% | 2.8–3.6%, trung bình 3.44% |
| Main XThread | 98.8–100.5% | 69.5–94.1%, trung bình 73.65% |
| GPU ring native | 3.6–4.3% | 15.6–40.1%, trung bình 18.43% |

Nhóm thứ hai chạy 5.8–7.6 FPS. Tên hai nhóm chỉ dựa vào CPU EventManager, không xác nhận chúng là menu hay gameplay. Có thể kết luận EventManager tiêu thụ khoảng 0.11 core trong nhóm này; chưa có bằng chứng nó là nút thắt chính hoặc đang decode.

Trong cả 32 khoảng: không có request audio thiếu dữ liệu. Cuối log, 471/471 buffer audout có PCM khác zero; nhóm đầu là 0/470–471. Đây là bộ đếm đầu ra trong `SwitchAudioSystem::FillAndAppend`, không đo CPU của Wwise, không xác nhận âm thanh đúng hoặc không có jitter.

## Những đường gọi cần phân biệt

- Guest Wwise chạy qua các hàm `sub_XXXXXXXX`; hiện không có địa chỉ lấy mẫu để xếp hạng chúng.
- `XmaDecoder::WorkerThreadMain` gọi `XmaContext::Work` (`sdk/src/audio/xma_decoder.cpp`). `XmaContext::DecodePacket` dùng FFmpeg `avcodec_send_packet`/`avcodec_receive_frame` (`xma_context.cpp`). Đây mới là bằng chứng source về decoder, không dựa vào tên luồng.
- `XmaDecoder::WriteRegister` cũng có thể gọi `context.Work()` ngay trên luồng yêu cầu kick. `audio_xma_in_worker=0` làm vậy; mode 2 có fallback. Switch mặc định mode 1, nhưng log được gửi không ghi giá trị thực tế hay các dòng `[xma]` timing. Vì thế CPU thấp ở luồng decoder không đủ loại trừ decode trên luồng khác.
- `AudioSystem::WorkerThreadMain` chờ semaphore rồi dispatch **guest callback** (`audio_system.cpp:116`); CPU luồng này không đồng nghĩa toàn bộ là host mixer.
- Thiếu stack/wait callers nên chưa thể xác định EventManager chờ event, tranh lock hay xử lý DSP/event.

FFmpeg Switch đã có các object NEON `float_dsp_neon.S`, `fft_neon.S`, `mdct_neon.S` trong build Ninja. `config_switch_aarch64.h` kế thừa config ARM64 bật `HAVE_NEON=1`, `HAVE_ARMV8=1`; `ff_get_cpu_flags_aarch64()` trả các flag này, và `audio_ffmpeg_simd` mặc định true. Vì thế chưa có bằng chứng về một tùy chọn NEON đang bị bỏ quên có thể bật để tăng FPS ngay. Đây là kiểm tra cấu hình/source, chưa phải lấy mẫu xác nhận callee NEON nào đang chạy. Codegen chỉ đổi mã guest Wwise; FFmpeg host có đường build riêng.

## Phép đo tiếp theo

1. Giữ nguyên âm thanh và cảnh thử; bật `switch_profile_report=true`, `switch_profile_stacks=true` cho một lượt 30–60 giây. Giữ lượt hiện tại làm baseline không lấy mẫu.
2. Lưu đúng ELF `sa2` và NRO của **cùng lượt** cùng TOML. ELF hiện tại `out/nx/sa2` đã được rebuild sau log 05:13; không dùng nó để suy địa chỉ của bản cũ. Log hiện cũng không có PC nào để tra.
3. Chạy `tools/switch_profile_report.py <log> --elf <ELF-cùng-bản> --mhz 0 --thread AK::Event --top 15`; xem cả nhóm stack/wait trong log gốc. `--mhz 0` giữ mọi xung CPU; mặc định công cụ chỉ giữ 1020 MHz. Lấy top guest `sub_`, self host FFmpeg/XMA, và tỷ lệ waiting trước khi chọn hướng tối ưu.
4. Ghi giá trị thực tế `audio_xma_in_worker` và các dòng `[xma] kick/split/lock` sẵn có. Chúng cung cấp số context, thời gian decode, kick-to-ready và thời gian block; ghép với stack mới để phân biệt decode với đồng bộ.

Đã thêm đo lường opt-in `audio_xma_profile=false` trong hai source audio trên. Bật trước khởi động để ghi `[xma-profile]` mỗi khoảng 10 giây có hoạt động, theo tên luồng, host ID (khớp ID profiler Switch), guest handle và mode XMA. Có số kick, số lần gọi FFmpeg DecodePacket, số frame nhận được, tổng/max wall time và tổng thread CPU time; CPU là `-1` nếu API không hỗ trợ. Kick bao gồm decode đồng bộ nên không cộng hai tổng với nhau. Bộ đếm nằm riêng từng luồng, không log mỗi packet; `Work()` trả true không bị tính nhầm là decode. Không có thay đổi về codec, PCM, scheduling hoặc chất lượng âm thanh.

## Kiểm tra instrumentation trên PC

Root đã compile SDK cho PC/ARM64 và chạy probe 75 giây, tắt tiếng ở đầu ra, save bản sao; title hiển thị đúng, không có error/WAIT_REG_MEM timeout. Log: `out/pc/captures/native_20261016_094212/runtime.log`. Ba báo cáo ghi mode 0, cùng host_tid 13008 / guest_handle F8000104: 864 frame giải mã trong 25.820 ms wall/13.11 s, rồi 378 frame trong 11.926 ms/12.07 s và 378 trong 12.301 ms/12.09 s. Bộ đếm đo được DecodePacket thật, không nhầm Work thành decode. Tên host thread rỗng trong probe PC này; vẫn có ID để ghép. CPU -1 đúng như thiết kế vì chỉ triển khai thread CPU trên Switch.

Đây là title trên PC, không phải gameplay Switch: không dùng các timing này để suy CPU Wwise 35% trên máy thật. Cần runtime log của NRO mới với `audio_xma_profile=true` để biết ai thực sự giải mã và phần CPU tương ứng.
