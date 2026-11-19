# Member 2 — paging 64-bit

## Chạy test độc lập

Chạy từ gốc repo với GCC, GNU linker và pthreads (Ubuntu hoặc MinGW).

    gcc -std=gnu11 -O1 -g -pthread -Iinclude tests/mm64_test.c src/mm64.c -Wl,--wrap=calloc -o mm64_test
    ./mm64_test

Trên PowerShell, đặt output vào thư mục tạm và chạy file .exe.
Test tự cung cấp MEMPHY giả. Không link thêm src/mm-memphy.c.
--wrap=calloc mô phỏng lỗi cấp bộ nhớ để kiểm tra rollback.

## Thiết kế và số liệu

Mỗi mm có PGD riêng gồm 512 entry. Entry thư mục chứa con trỏ bảng con.
Entry PT cuối chứa PTE theo bit layout của bài. Mỗi bảng dài
512 × 8 = 4096 byte và được cấp khi cần.
Các trường p4d/pud/pmd/pt cũ trong mm không phải gốc độc lập.
Tra cứu đi từ pgd theo đủ năm chỉ số.

Địa chỉ gồm PGD[56:48], P4D[47:39], PUD[38:30], PMD[29:21],
PT[20:12] và offset[11:0]. Bit 63:57 mở rộng dấu của bit 56.
pgn là địa chỉ ảo dịch phải 12 bit, kể cả địa chỉ kernel nửa trên.

0x1fffff thuộc PT index 511 của PMD index 0. 0x200000 chuyển sang
PT index 0 của PMD index 1. Frame 0 hợp lệ.
PTE zero là chưa map. Trang RAM bật PRESENT và tắt SWAPPED.
Trang swap bật SWAPPED và tắt PRESENT.

Mapping đầu tiên cần 5 bảng, tổng 20480 byte. Trang liền kề trong
cùng PT không cần bảng mới. Qua biên 2 MiB cần thêm một PT,
tổng 24576 byte khi dùng chung các cấp trên.
Test hai thread trên một mm xác nhận 129 lookup đầy đủ tạo 645 entry reads.

pte_lookup_reads đếm entry đọc bởi pte_get_entry, kể cả lookup dừng sớm.
Lookup đủ năm cấp cộng 5. Chưa tính truy cập byte dữ liệu, ghi PTE hoặc
đi bảng lúc map. page_table_bytes đếm các bảng đang tồn tại, chưa tính
metadata VMA hay overhead malloc. In dump không tăng bộ đếm lookup.

## Giao diện tích hợp

- alloc_pages_range trả 0 và danh sách node caller sở hữu khi thành công.
  Hết frame trả (addr_t)-3000. Lỗi đối số/cấp node trả (addr_t)-1.
  Khi lỗi đầu ra rỗng, frame đã lấy được trả về free_fp_list.
- vm_map_ram chuyển quyền sở hữu frame cho mapping và free node tạm khi
  thành công. Khi lỗi trả lại frame. Dải map nằm trong [astart, aend),
  bắt đầu ở biên trang.
- vmap_page_range nhận frame đã giữ sẵn. Khi lỗi caller vẫn sở hữu frame.
- vmap_pgd_memset chuẩn bị entry zero cho dải chưa map. Gặp mapping có sẵn
  báo lỗi và giữ nguyên mapping. Đây là quyết định triển khai cần xác nhận
  với Member 3 nếu SYSMEM_MAP_OP cần thao tác chẩn đoán khác.
- destroy_mm free bảng trang, VMA, free-region list, FIFO và metadata cache
  pool. Caller phải thu hồi frame RAM/swap trước và đợi CPU/syscall/DMA hết
  sử dụng mm. Hàm không free chính struct mm và có thể gọi lại.
- Member 3 hoàn thiện MEMPHY_get_freefp và thống nhất khóa free_fp_list.
  Rollback dùng lại node dự trữ để trả frame không cần malloc.
  page_table_lock chỉ bảo vệ lời gọi trong module này. Bên sửa MEMPHY trực
  tiếp cần cơ chế khóa chung khi tích hợp.
- Member 4 thay duyệt pgd phẳng trong free_pcb_memph bằng duyệt entry lá
  năm cấp trước khi gọi destroy_mm.
- Member 5 nối cleanup vào vòng đời tiến trình và thống nhất bảng kernel.

## Q5 — Lợi ích của paging N cấp

Bảng nhiều cấp chia không gian địa chỉ thành các nhánh. Chỉ cấp bảng con
cho nhánh sử dụng giúp tiết kiệm RAM khi không gian địa chỉ lớn và thưa.
Tăng số cấp cho phép mở rộng địa chỉ trong khi mỗi bảng vẫn nhỏ, dễ cấp.
Đổi lại, tra cứu cần thêm lần đọc entry theo số cấp. TLB giảm số lần
đi lại bảng trang. Cấp theo nhu cầu tạo thêm chi phí quản lý và xử lý lỗi.
Cần so sánh dung lượng bảng thực cấp và chi phí tra cứu cùng workload.

## Xác minh

Test riêng kiểm tra cô lập mm, đổi tầng, địa chỉ canonical, offset 12 bit,
PTE RAM/swap, frame 0, cấp frame thành công/hết RAM, lỗi calloc,
rollback map, số liệu, truy cập đồng thời và cleanup gọi lại.
Chưa có kết quả chạy toàn OS hoặc DMA trên Verilator.
