#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <algorithm>
#include <cmath>

// Không còn include header của SubtickExt-API và không còn cần khai
// "dependencies" trỏ tới mod nào cả. Thay vào đó, mod này chỉ gửi 2
// event nội bộ (định nghĩa trong TickMultiplexEvents.hpp) - mod nào
// muốn "biết" thì tự nghe, mod này không quan tâm ai đang nghe hay
// không, và vẫn compile/chạy bình thường dù không có mod subtick nào
// cài cùng.
#include "TickMultiplexEvents.hpp"

using namespace geode::prelude;

static bool  s_modEnabled       = true;
static bool  s_dtClampEnabled   = true;
static float s_targetTPS        = 240.f;
static float s_maxDtSeconds     = 16.6f / 1000.f;
static constexpr int kMaxStepsPerFrame = 32;

// --- Anti-jitter (corridor squeeze) ---
static bool s_antiJitterEnabled = true;

class $modify(GJBaseGameLayer) {
	// FIX (build error C2338): Geode không cho phép thêm member trực tiếp
	// vào modify class vì sẽ làm lệch layout bộ nhớ so với object gốc của
	// GD. Field mới bắt buộc phải nằm trong struct Fields.
	// https://docs.geode-sdk.org/tutorials/fields
	struct Fields {
		int m_currentFrameSteps = 1; // field mới, KHÔNG có trong GD gốc
	};

	static void onModify(auto& self) {
		(void) self.setHookPriorityPre("GJBaseGameLayer::update", Priority::VeryEarly);
		(void) self.setHookPriorityPre("GJBaseGameLayer::getModifiedDelta", Priority::VeryEarly);
	}

	// FIX (physics bug ở ship khi target-tps cao, vd 360):
	//
	// Bug cũ: computeStepCount() ước lượng số bước bằng ceil(targetTPS/fps),
	// RỒI getModifiedDelta() lại tự chia lại dt/mod và quantize bằng
	// static_cast<int>(extraDelta/stepDelta) (TRUNCATE, không round).
	// Đây là 2 tầng làm tròn khác nhau (ceil ở ngoài, truncate ở trong)
	// không khớp nhau: vì mod bị ceil lên hơi nhiều, mỗi lần chia
	// dt/mod cho ra extraDelta hụt nhẹ so với đúng 1 stepDelta, nên
	// int(extraDelta/stepDelta) rất hay bị ép về 0 -> deltaRet = 0
	// -> substep đó chạy với dt=0 (không tick vật lý gì cả), rồi
	// substep sau cộng dồn bù lại. Cube ít bị lộ vì chuyển động rời
	// rạc, nhưng ship dùng lực đẩy LIÊN TỤC mỗi tick (giữ chuột) nên
	// chuỗi tick kiểu 0,0,1,0,1,1,... làm vận tốc/quỹ đạo giật, sai
	// lệch rõ so với vanilla, càng rõ khi target-tps càng cao (stepDelta
	// càng nhỏ nên sai số làm tròn chiếm tỉ trọng lớn hơn).
	//
	// Fix: bỏ hẳn 2 tầng làm tròn đó, dùng đúng 1 nguồn sự thật duy nhất
	// - một bộ tích lũy fixed-timestep chuẩn kiểu "while (accumulator >=
	// stepDelta)" - để quyết định số bước, và luôn truyền đúng stepDelta
	// (không phải frameDt) làm dt cho mỗi lần gọi update() gốc. Nhờ vậy
	// getModifiedDelta() không cần chia/quantize gì thêm nữa, chỉ relay
	// lại giá trị đã được cắt sẵn.
	double getModifiedDelta(float dt) {
		if (!s_modEnabled || m_isEditor) {
			return GJBaseGameLayer::getModifiedDelta(dt);
		}

		// dt ở đây LUÔN đã là đúng 1 stepDelta (vì update() bên dưới chỉ
		// gọi GJBaseGameLayer::update(stepDelta) cho từng substep), nên
		// không còn gì để chia/quantize nữa - trả thẳng lại, tránh lặp
		// lại bug làm tròn kép.
		return static_cast<double>(dt);
	}

	void update(float dt) {
		if (!s_modEnabled || m_isEditor || !m_started) {
			GJBaseGameLayer::update(dt);
			return;
		}

		float frameDt = dt;
		if (s_dtClampEnabled) {
			frameDt = std::min(frameDt, s_maxDtSeconds);
		}

		// Trong lúc resume (grace period sau khi đóng menu/pause), chạy
		// đúng 1 update vanilla như bình thường, không multiplex, để
		// không phá timing của chính cơ chế resume gốc.
		if (m_resumeTimer >= 1) {
			m_resumeTimer -= 1;
			GJBaseGameLayer::update(frameDt);
			return;
		}

		double stepDelta = (1.0 / static_cast<double>(s_targetTPS))
			* static_cast<double>(std::min<float>(m_gameState.m_timeWarp, 1.0f));

		if (stepDelta <= 0.0) {
			GJBaseGameLayer::update(frameDt);
			return;
		}

		// Dồn thời gian thực của frame này vào ĐÚNG accumulator mà bản
		// thân vanilla engine cũng dùng (m_extraDelta), rồi rút ra số
		// tick nguyên vẹn bằng vòng lặp while thông thường - không còn
		// ước lượng/ceil trước rồi truncate sau nữa.
		m_extraDelta += static_cast<double>(frameDt);

		int steps = 0;
		while (m_extraDelta >= stepDelta && steps < kMaxStepsPerFrame) {
			m_extraDelta -= stepDelta;
			++steps;
		}

		if (steps <= 0) {
			// Chưa đủ 1 tick trọn vẹn (có thể xảy ra nếu target-tps được
			// set thấp hơn cả fps màn hình) - giữ phần dư cho frame sau
			// thay vì ép chạy 1 update với dt=0.
			return;
		}

		m_fields->m_currentFrameSteps = steps;

		// Báo cho mod subtick-input nào đang lắng nghe biết đây là 1
		// frame thật sắp bị chia thành m_fields->m_currentFrameSteps
		// substep, để mod đó (nếu có):
		// 1. dùng frameDt thật làm mẫu số tính ratio, thay vì dt của
		//    từng substep (tránh understate tick duration).
		// 2. chỉ tắt grace-period sau khi TOÀN BỘ frame thật kết thúc,
		//    không phải sau substep đầu tiên.
		// Nếu không có mod nào lắng nghe, send() này chỉ là no-op an toàn.
		tickmultiplex::BeginMultiplexEvent().send(static_cast<double>(frameDt));

		for (int i = 0; i < steps; i++) {
			// FIX: truyền đúng stepDelta (kích thước 1 tick thật) cho mỗi
			// substep, KHÔNG phải frameDt như bản cũ. Bản cũ truyền
			// nguyên frameDt mỗi lần và dựa vào getModifiedDelta() để
			// "chia lại" - chính chỗ chia lại đó là nơi phát sinh bug.
			GJBaseGameLayer::update(static_cast<float>(stepDelta));
		}

		tickmultiplex::EndMultiplexEvent().send();
	}
};

// --- Anti-jitter cho corridor khít hitbox (blue orb + hành lang hẹp) ---
// Đã verify với GeometryDash.bro bản 2.2081 + docs.geode-sdk.org/classes/PlayerObject:
//   postCollision(float dt, bool betweenSteps)  -> dòng 14487, CÓ địa chỉ
//     thật trên Windows (0x38d580) -> hookable.
//   double m_collidedTopMinY / m_collidedBottomMaxY -> dòng 14771-14772
//   double m_yVelocity -> dòng 14794
//
// LƯU Ý QUAN TRỌNG: đã thử hook preCollision()/updateCollideTop()/
// updateCollideBottom() để chặn sớm hơn, nhưng build báo lỗi:
//   "cannot be hooked due to an inline definition existing for the function"
// -> trên binary Windows 2.2081, cả 3 hàm này đã bị compiler gốc INLINE
// thẳng vào hàm gọi, không còn tồn tại địa chỉ hàm độc lập để Geode hook.
// Đây là giới hạn CỨNG của binary, không phải lỗi code. Vì vậy buộc phải
// quay lại đúng cách tiếp cận gốc: chỉ hook postCollision() (hàm duy nhất
// trong chuỗi này thật sự hookable) và đọc trực tiếp 2 field đã được
// engine tự tích lũy sẵn để suy ra trạng thái "bị kẹp cả 2 phía".
class $modify(PlayerObject) {
	void postCollision(float dt, bool betweenSteps) {
		if (!s_antiJitterEnabled) {
			PlayerObject::postCollision(dt, betweenSteps);
			return;
		}

		// Phát hiện trạng thái "bị kẹp" giữa 2 mặt trong cùng 1 (sub)step:
		// cả cạnh trên và cạnh dưới đều có va chạm hợp lệ cùng lúc.
		bool collidedTop    = m_collidedTopMinY    > 0.0;
		bool collidedBottom = m_collidedBottomMaxY > 0.0;

		if (collidedTop && collidedBottom) {
			// FIX: setYVelocity(double, int) có tham số `type` thứ 2 mà
			// bindings không ghi rõ ý nghĩa (không có docstring). Thay vì
			// đoán giá trị đó, gán thẳng field `m_yVelocity`.
			m_yVelocity = 0.0;
		}

		PlayerObject::postCollision(dt, betweenSteps);
	}
};

$on_mod(Loaded) {
	auto mod = Mod::get();

	s_modEnabled = !mod->getSettingValue<bool>("mod-disabled");
	listenForSettingChanges<bool>("mod-disabled", [](bool val) { s_modEnabled = !val; });

	s_targetTPS = static_cast<float>(mod->getSettingValue<double>("target-tps"));
	listenForSettingChanges<double>("target-tps", [](double val) { s_targetTPS = static_cast<float>(val); });

	s_dtClampEnabled = mod->getSettingValue<bool>("enable-dt-clamp");
	listenForSettingChanges<bool>("enable-dt-clamp", [](bool val) { s_dtClampEnabled = val; });

	s_maxDtSeconds = static_cast<float>(mod->getSettingValue<double>("max-dt-ms") / 1000.0);
	listenForSettingChanges<double>("max-dt-ms", [](double val) { s_maxDtSeconds = static_cast<float>(val / 1000.0); });

	s_antiJitterEnabled = mod->getSettingValue<bool>("anti-jitter-enabled");
	listenForSettingChanges<bool>("anti-jitter-enabled", [](bool val) { s_antiJitterEnabled = val; });
}