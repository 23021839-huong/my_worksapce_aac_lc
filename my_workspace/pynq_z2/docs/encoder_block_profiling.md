# Profiling từng khối AAC encoder trên PYNQ-Z2

Profiler này không phụ thuộc `perf` hay gói `linux-tools` của kernel Xilinx.
Nó chỉ được biên dịch khi CMake nhận
`-DFDK_ENABLE_ENCODER_PROFILING=ON`. Bản Release bình thường giữ profiler tắt.

## Chạy nhanh

Đồng bộ working tree có profiler sang Z2, rồi chạy từ root repository:

```bash
bash my_workspace/pynq_z2/sw/scripts/run_encoder_profile.sh
```

Có thể chọn input và thư mục artifact:

```bash
bash my_workspace/pynq_z2/sw/scripts/run_encoder_profile.sh \
  /path/to/mono_48k_pcm16.wav \
  /path/to/profile_artifacts
```

Script build `build-pynq-profile/aac-enc`, khóa tiến trình vào CPU 1 và chạy cấu
hình AAC-LC, 64 kb/s, ADTS, afterburner bật. Mỗi lượt chạy tạo:

```text
encoder_profile.csv
process_resources.txt
profile_output.aac
manifest.txt
SHA256SUMS
```

## Ý nghĩa CSV

| Cột | Ý nghĩa |
|---|---|
| `clock_source` | `thread_cpu_time` trên Linux; không tính thời gian thread bị scheduler dừng |
| `scope` | `root`, `parent` hoặc `detail` |
| `total_us` | Tổng CPU time của khối trên toàn file |
| `percent_of_frame` | Tỷ lệ so với tổng CPU time trong lõi AAC frame |
| `calls` | Tổng số lần gọi |
| `calls_per_frame` | Số lần gọi trung bình trên mỗi frame |
| `average_us` | CPU time trung bình mỗi lần gọi |
| `minimum_us`, `maximum_us` | Biên thời gian mỗi lần gọi |

Các dòng `parent` chứa các dòng `detail` bên dưới, vì vậy không cộng tất cả phần
trăm trong CSV. Ví dụ `psy_total` chứa `window_mdct`, `tns` và các khối
psychoacoustic khác; `qc_total` chứa scalefactor, quantization và Huffman count.

`frame_total` đo lõi `FDKaacEnc_EncodeFrame`; nó không bao gồm đọc WAV và ghi file
tại chương trình `aac-enc`. `process_resources.txt` bổ sung maximum RSS, page fault,
context switch và thời gian toàn tiến trình.

## Điều kiện đo

- Giữ đúng một cấu hình để các lượt chạy so sánh được.
- Đóng các notebook/process nặng khác trên Z2.
- Giữ CPU governor cố định; ưu tiên `performance` nếu image cho phép.
- Chạy ít nhất ba lượt và giữ từng thư mục artifact.
- Dùng thêm corpus có transient và âm nhạc ổn định để kích hoạt cả LONG và SHORT
  window.

Profiler dùng trạng thái toàn cục không khóa để giảm overhead. Chỉ dùng một encoder
handle trên một thread trong build profiling.
