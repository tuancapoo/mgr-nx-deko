# Thử Metal Gear Rising trên Switch

Đây là bản thử phần cứng đầu tiên. Chưa xác nhận khởi động hoặc FPS trên Switch thật.

1. Chép thư mục `switch/mgr-nx` trong gói SD vào gốc thẻ SD.
2. Đặt dữ liệu Xbox 360 của bạn tại `sdmc:/switch/mgr-nx/game_root/`.
   File `default.xex` nằm ngay trong `game_root`, cùng các file/thư mục dữ liệu gốc.
3. Mở Homebrew Menu bằng title takeover để có đầy đủ RAM, rồi chạy `mgr.nro`.
   Chế độ applet mở qua Album không đủ RAM cho runtime này.
4. Kiểm tra lần lượt màn hình tiêu đề, tay cầm, âm thanh, cutscene, vào chơi,
   một cảnh chiến đấu, checkpoint save, thoát và mở lại để load checkpoint đó.
5. Nếu chạy được, thử cùng cảnh bằng `mgr_fps_limit = 30` trong `mgr.toml`.
   So tốc độ chạy/chém, Blade Mode và đồng bộ âm thanh với giá trị `0`.
   FPS thấp không đủ chứng minh tốc độ logic game vẫn đúng.
6. Gửi lại `logs/`, thông báo lỗi/ảnh nếu có, và mô tả cảnh thử.
   Log CPU/GPU trên PC không dự đoán trực tiếp hiệu năng Switch.

`logs/native.csv` ghi frame time, draw call và thời gian phần GPU work đã được timestamp.
Đây chưa phải toàn bộ GPU busy: khoảng giữa submission còn có thể chứa upload hoặc draw xuất hình.
Kết quả GPU lấy từ truy vấn bất đồng bộ, dùng trung bình theo khoảng thử;
không coi mỗi dòng là GPU time chính xác của cùng frame. Hệ số thời gian NVK
kế thừa từ bản port tham chiếu cần được hiệu chuẩn lại trên máy/driver đang thử.
Trên Switch, cột CPU process trong CSV là `-1` (không có số đo ở API này);
CPU từng luồng được ghi trong `logs/rex/rex_profile.log` mỗi 10 giây.
CSV được flush mỗi giây để tránh ghi SD đồng bộ từng frame.

Gói không ghi đè save đang có trên SD. Save trên PC chưa tự chuyển sang SD.
Giữ lại save và log khi thay NRO cho lần thử sau.
