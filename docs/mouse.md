# Điều khiển bằng chuột

Ghi lại toàn bộ phần chuột của bản port PC (PR #8, #9, #11).

## Nguyên tắc

- **Chỉ có một con trỏ.** Chuột điều khiển trực tiếp con trỏ của chính game, không vẽ thêm con trỏ thứ hai và không giả lập bấm phím.
- **Rê chuột là chọn ngay, click là xác nhận.** Game vẫn tự kiểm tra mọi lựa chọn, ví dụ ô đi không hợp lệ thì game tự từ chối.
- **Khi game tự di con trỏ** (bàn phím, tay cầm, menu đóng…), chuột tiếp tục từ vị trí mới đó, nên chuột và con trỏ không bao giờ lệch nhau.

## Cài đặt (F2 → Controls)

| Mục | Ý nghĩa |
|---|---|
| Mouse | Bật / tắt chuột |
| Game pointer (captures the mouse) | **Bật (mặc định):** chuột bị khóa trong cửa sổ, con trỏ hệ điều hành bị ẩn, con trỏ game là con trỏ duy nhất. **Tắt:** dùng con trỏ hệ điều hành trỏ thẳng vào màn hình. |
| Pointer speed | Độ nhạy chuột khi bật Game pointer (5–100%; file cài đặt cho tới 200%) |

- Chuột tự nhả ra khi mở F2 hoặc khi cửa sổ game mất focus.
- **Cursor Speed** trong OPTION của game không ảnh hưởng tốc độ con trỏ chuột. Nó chỉ quyết định tốc độ cuộn bản đồ khi chuột ép vào mép (xem phần Bản đồ).

## Nút chuột chung

| Nút | Tác dụng |
|---|---|
| Trái | ○ (chọn / xác nhận) |
| Giữ trái | □: tua nhanh. Chỉ khi đang có sự kiện hoặc lượt địch, xem phần Tua nhanh. |
| Phải | × (quay lại). Trên bản đồ có con trỏ tự do thì là **Start**. |
| Giữa | △. Ở danh sách đội của MOVE / QUEST thì là □ (đánh dấu). |
| Nút bên (Back / Forward) | **Start**, ở mọi màn |
| Lăn bánh xe | Lên / Xuống (cuộn danh sách, Yes/No, đổi quân chủ…) |
| Nghiêng bánh xe | Không dùng (đã bỏ) |

## Theo từng màn

### Bản đồ

| Màn | Cách dùng |
|---|---|
| Bản đồ trận đánh | Rê tới ô nào là con trỏ đặt lên ô đó; click để chọn. Chọn ô đi (MOVE) và mục tiêu tấn công / phép làm giống vậy. |
| Bản đồ chiến lược | Cây bút đi theo chuột; click vào lâu đài để mở menu lâu đài. |
| Cuộn bản đồ (cả hai loại) | Đẩy chuột ra mép thì camera đi theo. Đẩy dứt khoát tới vùng camera bám thì coi như đang giữ phím hướng: dừng tay bản đồ vẫn cuộn tiếp theo Cursor Speed; kéo ngược lại một chút (hoặc click) là dừng. Đẩy vào góc thì cuộn chéo. |
| Mở Start menu | Chuột phải trên bản đồ (khi con trỏ tự do), hoặc nút bên của chuột. |

### Menu và danh sách

| Màn | Cách dùng |
|---|---|
| Menu biểu tượng (lệnh trong trận, menu lâu đài, menu Organize, Start menu) | Trỏ vào mục, click. |
| Menu danh sách (menu lệnh / hệ thống trong trận, "Your Path?", "What is your skill?"…) | Rê lên xuống, click. Chuột phải để đóng. |
| Danh sách cuộn (đòn đánh, phép, kỹ năng, vật phẩm…) | Rê để chọn; đẩy quá hàng đầu / hàng cuối hoặc lăn bánh xe để cuộn; click để chọn. |
| Danh sách đội (MOVE, QUEST, LOCATION) | Như trên. Game không lưu tổng số đội, nên khi đẩy quá mép chuột gửi phím Lên / Xuống để game tự cuộn. Ctrl+click, Shift+click hoặc chuột giữa = □ để đánh dấu đội. |
| Hộp Yes / No | Vị trí ngang của chuột chọn Yes hay No; lăn lên = Yes, xuống = No; click để trả lời. |
| SUMMON | Rê chọn quái, đẩy quá mép hoặc lăn để cuộn, click để triệu hồi. |
| Bảng trạng thái unit, trang 2 | Trỏ vào đòn đánh / phép / kỹ năng để đọc mô tả; click để sang trang. |

### Start menu

| Màn | Cách dùng |
|---|---|
| UNITS: danh sách hiệp sĩ | Rê, cuộn, click. **Kéo chuột hẳn sang ngang** (khoảng 64 px, ngang nhiều hơn dọc) để lật trang cột (Lv/Exp/Class ↔ HP/MP/R.Pow/Align…). Khi tắt Game pointer thì click vào mũi tên đỏ ở góc trên cũng lật trang. |
| UNITS: hàng tiêu đề (sắp xếp) | Đang ở hiệp sĩ đầu tiên thì đẩy chuột lên thêm để vào hàng tiêu đề. Di ngang để chọn cột; click để sắp xếp; đẩy xuống hoặc lăn xuống để quay lại danh sách. |
| DOMAIN | Ô vuông đi theo chuột; click để nhảy bản đồ tới đó. |
| LOCATION | Như danh sách đội (đẩy quá mép hoặc lăn để cuộn). |
| SAVE | Trỏ vào ô file, click. |
| OPTION | Rê chọn dòng; click để đổi giá trị (ví dụ Unit Number Normal → Large). |

### Organize (menu lâu đài)

| Màn | Cách dùng |
|---|---|
| Lưới đội hình / kho quái | Trỏ vào ô, click. |
| EQUIP / ITEM | Lưới vật phẩm 2 cột: rê chọn ô (ô trống của hàng lẻ bị bỏ qua); đẩy quá lưới hoặc lăn để cuộn; click để chọn. ITEM sẽ hỏi lại bằng Yes / No. |
| NAME | Bảng chữ (Katakana / Eng.): trỏ vào chữ hoặc các nút Kana / Eng. / Space / Cancel / Finish, click. Trỏ vào ô trống thì tự chọn chữ gần nhất trong hàng. |
| DELETE (thả quái) | Chọn quái trong lưới, rồi trả lời hộp "Release …?" Yes / No. |

### Màn tiêu đề và vào game mới

| Màn | Cách dùng |
|---|---|
| PUSH START BUTTON | Click trái (chuột gửi Start, vì màn này game chỉ nhận Start). |
| NEWGAME / CONTINUE / TUTORIAL / RECOLLECTION | Rê và click. |
| CONTINUE → LOAD | Chọn SLOT, rồi chọn file như SAVE. |
| SELECT RULER | Kéo chuột ngang hoặc lăn bánh xe để đổi quân chủ; click để chọn; sau đó hộp Yes / No. |

### Tua nhanh

| Lúc | Cách dùng |
|---|---|
| Hội thoại sự kiện | Click = qua câu. **Giữ chuột trái** khoảng 1/3 giây trở lên thì chuột giữ □: tua nhanh; thả ra là về tốc độ thường. |
| Lượt địch trong trận | Giữ chuột trái = giữ □: tua nhanh. Hết lượt địch là tự nhả, không tua sang lượt của mình. |

## Ghi chú kỹ thuật (cho lần sửa sau)

Code nằm ở `src/pc/mouse.c`. Mỗi màn được nhận ra qua một **hook** ở đầu hàm game: hàm nào đã chạy trong 2 khung hình gần nhất thì coi màn đó đang mở. Danh sách hook nằm trong `src/pc/hook_list.txt`; recompiler tự chèn `hook_f_<seg>_<addr>()`.

| Context | Màn | Hàm / RAM chính |
|---|---|---|
| 1 | Bản đồ trận | Cờ 0x800A6AB0 bit 0 (con trỏ tự do nhận input); vị trí con trỏ 0x800A6AB4 / AB8; mode 0x800A6994 |
| 2 | Menu biểu tượng | main 8002B940 (hàm input); bảng vị trí mục 0x800ADCE0 |
| 4 | Danh sách | 80021EB8 (danh sách chung); UNITS 80049AD4; file thẻ nhớ 80045A74; menu nhỏ 800257F8; SUMMON ov1 800D09A0; tiêu đề ov3 (trạng thái 0x800C6B38) |
| 5 | Bản đồ chiến lược | Cờ 0x800A6AB0 bit 3; camera 0x800A6794 / 98 |
| 6 | Lưới Organize | ov1 800CDB1C |
| 7 | Yes / No | main 800219B4; chỉ số 0x800A685C |
| 8 | Trạng thái unit trang 2 | main 8002F840 |
| 9 | DOMAIN | ov1 800C57D8 |
| 10 | SELECT RULER | ov3 800B81EC (trạng thái 0x800C6B44 = 10; chỉ số 0x800C6B70) |
| 11 | Lưới vật phẩm | ov1 800CA034 (số món 0x800DC680; cột 0x800DC894) |
| 12 | NAME | ov1 800CF8A8 (cột 0x800DC658, hàng 0x800DC65C, bảng chữ 0x800DA4F0) |

Các địa chỉ khác:

| Địa chỉ | Ý nghĩa |
|---|---|
| 0x800A69F4 = 1 | Đang có sự kiện (main 80032004 bật) |
| 0x800A6BE0 | Phe đang đi trong trận (0 = mình) |
| 0x800A67BC bit 3 | Đang chuyển cảnh (fade): không nhận input |
| 0x800A63E8 | Cursor Speed 1–5. Chuột không dùng; chỉ là tốc độ game tự cuộn khi ép mép. |
| 0x800A69DC | **Không phải** biến màn hình. Đó là một bộ đếm, sau khi bấm Next ở bảng info vẫn giữ 3. Đã từng gây lỗi chuột chết. |
| Kho đồ | Xem memory `item-cheat` / file `docs/cheats/*.cht` |

## Công cụ thử (`build/mtest`, không commit)

- `run.sh <tên> "<phím>" <từ> <đến> <mỗi> <giây> <state>`: chạy game theo kịch bản và chụp màn hình.
  - `BI="khung:mask,..."` để giữ nút tay cầm.
  - `NOCAP=1` để dùng con trỏ hệ điều hành.
- Lệnh chuột trong kịch bản:

  | Lệnh | Ý nghĩa |
  |---|---|
  | `Rel=x;y` | Di tương đối |
  | `Move=x;y` | Di tới điểm (toạ độ = 3 × pixel màn hình) |
  | `Click=x;y` | Click tại điểm |
  | `LClick` | Click trái (bấm rồi thả) |
  | `LDown`, `LUp` | Bấm / thả trái riêng |
  | `RBtn`, `MBtn`, `XBtn` | Chuột phải / giữa / nút bên |

- Save state mẫu trong `orig/`:

  | File | Nội dung |
  |---|---|
  | `auto3` | Bản đồ chiến lược |
  | `slot1`, `slot2` | Trận đánh |
  | `event` | Giữa hội thoại mở đầu |
  | `enemy` | Lượt địch |

- `BRIG_POKE="khung:addr=value"` ghi RAM. Ví dụ `poke_items.txt` nạp đủ vật phẩm để thử.
- Biến môi trường để xem log: `BRIG_MOUSE_TRACE=1` (đổi context), `BRIG_RAMDUMP=khung,...` (chụp RAM).
