# MGR: thu shader và pipeline theo chương

Chơi mỗi chương một lượt bình thường trước. Dữ liệu phản ánh những gì đã xuất
hiện trong lượt chơi; không có bộ đếm nào chứng minh đã bao phủ 100% một chương.
Mẫu mới được giữ qua các lần mở game. Dữ liệu đã có không bị xóa khi qua chương.

## Cài gói thu trên Switch

Thoát game. Giải nén ZIP trên PC, chép thư mục `switch` vào gốc thẻ SD, thay
`mgr.nro`, `mgr.toml`, cặp `sa2_shaders.mesp`/`.idx` và `cache/mgr_pipeline_seed.bin`.
Không xóa thư mục `cache` hay dữ liệu save. Gói shader mới có 1119 mục,
bao gồm những sửa trước đây cho cầu, Mistral và cổng.

Gói chơi thử mới mặc định **tắt thu**, giữ 360p và các tối ưu hiện tại. Muốn
thu tiếp, chép `options/mgr-capture.toml` vào `switch/mgr-nx/` và đổi tên thành
`mgr.toml`. Gói chapter-cache ban đầu vẫn là cấu hình bật thu. Không bật stack
sampling, đo GPU sâu, hash chẩn đoán hay ghi CSV từng frame. Chỉ shader mới được
ghi vào `collect`; ghi lên SD vẫn có thể gây khựng ngắn khi gặp mẫu mới.

NRO mới có `mgr_startup_prewarm = true`: màn hình **PREPARING GRAPHICS** hiển thị
số pipeline đã xử lý/tổng số, rồi tự vào game sau khi chuẩn bị xong. Số này
gồm seed và danh sách cache cục bộ, không chứng minh đã bao phủ toàn bộ game.
Lần sau vẫn duyệt danh sách nhưng tái sử dụng cache GPU đã lưu. Nếu không có
seed hoặc danh sách trống, game vẫn khởi chạy. Mục bị thiếu shader/lỗi được
báo riêng; không giả báo đã chuẩn bị thành công. Đặt `mgr_startup_prewarm = false`
để dùng prewarm nền như bản trước. `sa2_native_pipelines_prewarm = false` tắt
prewarm hoàn toàn. Cặp shader/seed mới dùng được với NRO cũ nhưng NRO cũ chưa
có màn hình chờ này. Thời gian Switch phải đo trên máy thật.

## Gửi một chương

1. Chơi và lưu game. Hết chương hoặc muốn gửi giữa chừng thì thoát game hẳn.
2. Gắn thẻ SD vào PC. Không xuất khi game còn đang ghi dữ liệu.
3. Trong PowerShell, tại thư mục `pc-tools` đi kèm gói, chạy ví dụ:

```powershell
powershell -ExecutionPolicy Bypass -File .\export_mgr_chapter.ps1 -GameFolder "E:\switch\mgr-nx" -Chapter R-00
```

Thay `E:` bằng ổ thẻ SD của bạn. Chương tiếp dùng `R-01`, `R-02`... Gửi thêm
giữa chương có thể dùng cùng tên chương; mỗi ZIP có thời gian riêng.
ZIP nằm trong `pc-tools/exports`. Gửi ZIP đó để phân tích và gộp.
Không cần xóa `collect` hoặc `cache` sau khi xuất; lần sau công cụ tự loại mẫu trùng.

Nếu không chạy công cụ, chép/nén ba đường dẫn dưới đây, giữ cấu trúc thư mục,
rồi cho biết đang ở chương nào. Không cần gửi game assets hay save:

- `switch/mgr-nx/collect/`
- `switch/mgr-nx/cache/sa2_native_pipelines.bin`
- `switch/mgr-nx/logs/`

Xuất ZIP bằng PowerShell không cần cài Python. Công cụ gộp phía phát triển
dùng Python 3.10+, không cần cài gì thêm trên Switch.
File `mgr-play.toml` trong thư mục `options` có thể chép đè thành `mgr.toml`
khi muốn chơi mà không thu shader nữa. Các mẫu đã thu vẫn được giữ.

## Gộp và tạo gói dùng chung (phía phát triển)

Các lệnh sau chạy tại gốc repo, dùng Python chuẩn, không cần thư viện ngoài:

```powershell
python tools/mgr_chapter_cache.py merge --input "MGR-R-00.zip" --chapter R-00 --store out/chapter-cache
python tools/mgr_chapter_cache.py review --store out/chapter-cache --output out/chapter-review-R00
python tools/mgr_chapter_cache.py seed --store out/chapter-cache --output out/shared/switch/mgr-nx/cache/mgr_pipeline_seed.bin
```

Quét trước các container XDK nguyên vẹn ở dạng không nén trong `default.xex`
và `media/*.cpk`, nếu muốn bổ sung đầu vào trước khi chơi:

```powershell
python tools/mgr_scan_shader_containers.py --game-root assets/game_root --known out/experiments/chapter_release_20261019/shader-package/in_ok --output out/static-shader-scan
python tools/mgr_chapter_cache.py merge --input out/static-shader-scan --chapter STATIC --store out/chapter-cache
```

Đây không phải công cụ giải nén toàn bộ CPK/XEX: phần bị nén/mã hóa không được
giải mã. Kết quả không chứng minh đã tìm hết shader. Runtime vẫn thu biến thể
và tổ hợp pipeline khi chơi; mọi mẫu quét được vẫn phải kiểm tra trước khi dịch.

`merge` lưu đối tượng theo SHA-256 và báo số tài nguyên/pipeline mới, file bị
loại, cảnh báo chạm giới hạn. Một ZIP nhập lại không tạo phiên trùng. Báo cáo
từng phiên nằm trong `sessions/`; kho tích lũy giữ cả các phiên trước.
`--chapters R-00 R-01` trên `seed`/`review` chọn các phiên mang nhãn tương ứng;
mỗi phiên có thể chứa dữ liệu tích lũy từ các chương trước đó.

`review/containers` là container thu lúc tạo shader hoặc quét file; `review/variants` chỉ chứa biến thể VS;
`review/rebuilt-ps` là PS được dựng lại và `review/raw` là microcode chưa nhận
diện. Kiểm tra tính tương thích trước khi dịch. File `.pending` chỉ được đưa
vào hàng đợi kiểm tra nếu cấu trúc đầy đủ; không đồng nghĩa tương thích.

Đối với biến thể vertex declaration, chạy bằng bộ template hiện tại:

```powershell
python tools/repair_mgr_shader_variants.py --base "out/experiments/chapter_release_20261019/shader-package/in_ok" --variants out/chapter-review-R00/variants --output out/chapter-proof-R00
```

Chỉ đầu ra `NEW_PROVEN` được gộp vào đầu vào build sau khi kiểm tra báo cáo.
Microcode thô/PS dựng lại cần phân tích riêng, không đưa thẳng vào thư viện.
Build gói mới bằng `build_mgr_shader_library.py` với `--base` trỏ gói hiện tại,
kiểm tra ảnh các save liên quan, rồi phân phối cả cặp `.mesp`/`.idx` cùng seed.
Không cần rebuild NRO chỉ để thêm shader đã được xác minh.

Seed là danh sách NFPL v2, không chứa mã shader hoặc dữ liệu Vulkan driver.
Game gộp seed vào danh sách cục bộ, ánh xạ lại shader bằng fingerprint, giữ
nguyên cache do driver trên máy tạo. Không chia sẻ/ghép nối các blob driver PC
và Switch. Seed không tự khắc phục shader chưa có trong cặp `.mesp`/`.idx`.

Giới hạn mới: 65.536 pipeline tích lũy, 8.192 biến thể VS được xét mỗi phiên
(có thể cấu hình tối đa 65.536). Chạm giới hạn sẽ có cảnh báo trong log; không
báo đã thu đầy đủ. Nếu cần bổ sung, ưu tiên đúng đoạn còn thiếu, không mặc định
yêu cầu chơi lại toàn bộ chương. Cache giúp giảm khựng do biên dịch/đọc shader,
không bảo đảm đạt 30 FPS ở cảnh bị giới hạn bởi công việc CPU/GPU mỗi frame.
