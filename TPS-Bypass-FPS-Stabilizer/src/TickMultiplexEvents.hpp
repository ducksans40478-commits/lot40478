#pragma once
#include <Geode/Geode.hpp>

using namespace geode::prelude;

// Giao thức tối giản để báo hiệu "đang chia 1 frame thật thành nhiều
// substep vật lý" giữa các mod, KHÔNG cần khai dependencies trong
// mod.json và KHÔNG cần link/import bất kỳ API mod cụ thể nào.
//
// LƯU Ý: Geode yêu cầu chữ ký callback của Event phải trả về bool
// (true = tiếp tục lan sang listener khác), nên khai bool(...) thay vì
// void(...) dù chúng ta không thật sự cần giá trị trả về.
//
// File này PHẢI giống hệt (namespace, tên struct, chữ ký) ở cả 2 bên
// thì Event mới match được giữa 2 binary khác nhau.
namespace tickmultiplex {

	// Gửi 1 lần/frame thật, NGAY TRƯỚC vòng lặp substep, kèm dt thật
	// của cả frame (chưa chia nhỏ).
	struct BeginMultiplexEvent final : public Event<BeginMultiplexEvent, bool(double)> {
		using Event::Event;
	};

	// Gửi 1 lần/frame thật, NGAY SAU khi vòng lặp substep kết thúc.
	struct EndMultiplexEvent final : public Event<EndMultiplexEvent, bool()> {
		using Event::Event;
	};

} // namespace tickmultiplex