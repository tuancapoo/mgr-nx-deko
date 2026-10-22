# MGR: codegen theo từng bước (2026-10-16)

Baseline được giữ tại `out/backups/before_partial_codegen_20261016_090828/`.
Có NRO đang dùng (92,702,917 byte), ELF cùng bản, shader/config, PC runtime,
source archive và manifest SHA-256. NRO và source archive đã kiểm tra hash.
Dữ liệu game không được nhân bản; các thử nghiệm này không sửa dữ liệu game.

## Các bước

| Flavor | Generator options bật | Post transforms của bản optimised |
|---|---|---|
| classic | Không thêm | Không |
| s1-fp-moves | fp_moves_modeless | Không |
| s2-control-locals | s1 + cr_as_local, ctr_as_local, xer_as_local, reserved_as_local | Không |
| optimised | Toàn bộ perf_overrides.toml cũ | Có; không dùng cho lượt thử này |

`tools/codegen.ps1 -Flavor s1-fp-moves` tạo manifest có marker stage.
`post_codegen.py` kiểm tra đúng danh sách cờ rồi bỏ qua toàn bộ pipeline
optimised, kể cả khi CMake tự chạy lại generator. Localized natives sẵn có
của baseline vẫn được giữ. Nhãn stage được đọc vào dòng `[enh]` của runtime.
Không thêm fast-math, bỏ đồng bộ bộ nhớ, constant folding hay địa chỉ SA2.

S1 tránh đổi flush mode quanh các thao tác chuyển bit và dùng helper chuyển
đổi float/double khi đang ở VMX mode. Các phép cộng/trừ/nhân/chia giữ đường cũ.
S2 đã có trong công cụ nhưng chưa dùng làm bản đề nghị trên Switch: đo S1
riêng trước để biết ảnh hưởng và dễ khoanh vùng lỗi.

Số vị trí xuất hiện trong source sinh ra (không phải số lần chạy):

| Lệnh/helper | Classic | S1 |
|---|---:|---:|
| disableFlushMode | 79,441 | 61,513 |
| enableFlushMode | 11,838 | 10,744 |
| SingleToDoubleAnyMode | 0 | 8,200 |
| DoubleToSingleAnyMode | 0 | 2,661 |

Tổng vị trí đổi flush mode giảm 20.84%. Đây không phải dự báo mức tăng FPS.

## Kiểm tra

- Pipeline selector: classic/full/partial được phân biệt; partial có thêm cờ
  ngoài danh sách bị từ chối.
- `tools/win/run_sdk_tests.ps1 -Only fp_moves_modeless`: 4,000,196 phép kiểm
  tra khớp từng bit, gồm NaN và denormal, hai flush mode, giữ flush mask.
  Negative control bắt được 4,022 sai khác load và 11,811 store.
- Existing `fcti_copy` và `fp_any_mode` tests cũng đạt, gồm negative controls.
- Hai source XMA mới đã compile cho PC và ARM64. Bộ đo mặc định tắt.
- PC probe 75 giây đạt: title 1280x720 hiển thị đúng, đủ 15 frame chụp,
  0 error, 0 draw rejected, 0 WAIT_REG_MEM timeout; XMA ghi được ba báo cáo.
  Capture: `out/pc/captures/native_20261016_094212/last_frame.png`.
  Lượt trước khóa 30 FPS chụp quá sớm (khung đen), không được tính là pass
  và chưa được dùng kết luận tốc độ game ở 30 FPS.
- Kết quả chạy PC và artifact NRO được ghi trong thư mục thí nghiệm
  `out/experiments/partial_codegen_20261016/` sau khi hoàn tất build.

## Artifact S1 đã hoàn tất

`out/experiments/partial_codegen_20261016/s1-fp-moves/mgr.nro`:
92,596,421 byte, SHA-256
`9FB36F73C717E87DC21AD8E305DD08212B34762C3CC78BE0B21F1039A3310920`.
ELF cùng bản là `mgr.elf`; có manifest hash và README chép SD.
Đã kiểm tra ELF64/AArch64, NRO0 và nhãn codegen S1 trong binary.
NRO ở `out/nx/sdcard/switch/mgr-nx/mgr.nro` vẫn khớp hash baseline.
Chưa chạy NRO mới trên Switch; chưa có FPS gain được đo.

## Wwise và phép đo trên máy thật

Xem `docs/mgr-wwise-analysis.md`. Log Switch trong workspace là log cũ tắt
stack sampling, không đủ xác định callee làm EventManager tốn CPU trong lượt
14–16 FPS mới hơn. Không suy decoder từ tên luồng.

Giữ cùng save, cảnh, camera, độ phân giải và xung cho A/B; đo nhiều khoảng
10 giây sau khi warm-up. Để so FPS, tắt `audio_xma_profile` và stack sampling.
Giữ config đang dùng, chỉ thay NRO; không cần chép lại game hay shader.

Lượt phân tích riêng: thêm `audio_xma_profile = true` vào `mgr.toml` trước
khởi động, chơi 60 giây rồi giữ runtime log. `[xma-profile]` cho biết luồng
thực sự gọi FFmpeg, CPU/wall time và mode worker. Kick bao gồm decode inline,
không cộng hai cột như hai chi phí độc lập. CPU `-1` là không đo được.

Nếu cần top guest functions, bật `switch_profile_stacks = true` cho lượt
phân tích riêng rồi dùng ELF đúng bản với `tools/switch_profile_report.py`,
thêm `--mhz 0` để không lọc mất lượt ép xung. Lấy mẫu có overhead; không dùng
FPS của lượt đó để kết luận lợi ích codegen.
