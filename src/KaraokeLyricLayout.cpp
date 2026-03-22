#include "KaraokeLyricCommon.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>

using namespace Gdiplus;

namespace {

// 上位サロゲートかどうかを判定する。
bool IsHighSurrogate(wchar_t ch) {
	return ch >= 0xD800 && ch <= 0xDBFF;
}

// 下位サロゲートかどうかを判定する。
bool IsLowSurrogate(wchar_t ch) {
	return ch >= 0xDC00 && ch <= 0xDFFF;
}

// 文字列の指定位置から1コードポイントを取り出す。
uint32_t DecodeCodePoint(const std::wstring& text, size_t index, size_t* length) {
	if (index >= text.size()) {
		if (length) *length = 0;
		return 0;
	}

	const auto first = text[index];
	if (IsHighSurrogate(first) && index + 1 < text.size() && IsLowSurrogate(text[index + 1])) {
		if (length) *length = 2;
		return 0x10000 + (((static_cast<uint32_t>(first) - 0xD800) << 10) | (static_cast<uint32_t>(text[index + 1]) - 0xDC00));
	}

	if (length) *length = 1;
	return static_cast<uint32_t>(first);
}

// 結合文字として後続文字に重なる文字かを判定する。
bool IsCombiningMark(uint32_t codepoint) {
	return (codepoint >= 0x0300 && codepoint <= 0x036F) ||
		(codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||
		(codepoint >= 0x1DC0 && codepoint <= 0x1DFF) ||
		(codepoint >= 0x20D0 && codepoint <= 0x20FF) ||
		(codepoint >= 0xFE20 && codepoint <= 0xFE2F) ||
		codepoint == 0x3099 ||
		codepoint == 0x309A;
}

// 異体字セレクタかどうかを判定する。
bool IsVariationSelector(uint32_t codepoint) {
	return (codepoint >= 0xFE00 && codepoint <= 0xFE0F) ||
		(codepoint >= 0xE0100 && codepoint <= 0xE01EF);
}

// フォント設定をGDI+のスタイルフラグへ変換する。
INT ToFontStyle(const FontStyleSettings& font) {
	INT style = FontStyleRegular;
	if (font.bold) style |= FontStyleBold;
	if (font.italic) style |= FontStyleItalic;
	return style;
}

// 指定フォントが使えない場合は代替フォントを返す。
std::unique_ptr<FontFamily> CreateFontFamily(const wchar_t* preferred) {
	auto family = std::make_unique<FontFamily>(preferred);
	if (family->GetLastStatus() == Ok) {
		return family;
	}

	return std::make_unique<FontFamily>(L"MS UI Gothic");
}

// GDIの文字計測で文字列の描画幅を取得する。
int MeasureStringAdvance(const FontStyleSettings& font, const std::wstring& text) {
	if (text.empty()) {
		return 0;
	}

	const auto screen_dc = GetDC(nullptr);
	if (!screen_dc) {
		return 0;
	}

	const auto hdc = CreateCompatibleDC(screen_dc);
	ReleaseDC(nullptr, screen_dc);
	if (!hdc) {
		return 0;
	}

	LOGFONTW logfont{};
	logfont.lfHeight = -static_cast<LONG>(std::lround(font.size));
	logfont.lfWeight = font.bold ? FW_BOLD : FW_NORMAL;
	logfont.lfItalic = font.italic ? TRUE : FALSE;
	logfont.lfCharSet = DEFAULT_CHARSET;
	wcsncpy_s(logfont.lfFaceName, font.name.c_str(), _TRUNCATE);

	const auto hfont = CreateFontIndirectW(&logfont);
	if (!hfont) {
		DeleteDC(hdc);
		return 0;
	}

	const auto old_font = static_cast<HFONT>(SelectObject(hdc, hfont));
	SIZE size{};
	const auto ok = GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
	SelectObject(hdc, old_font);
	DeleteObject(hfont);
	DeleteDC(hdc);
	if (!ok) {
		return 0;
	}

	return size.cx;
}

// ミリ秒をフレーム数へ変換する。
int MillisecondsToFrameCount(int milliseconds, const ProjectSettings& settings) {
	return static_cast<int>(std::llround(milliseconds * settings.FramesPerSecond() / 1000.0));
}

// 歌詞開始時刻をプロジェクト基準の開始フレームへ変換する。
int StartFrameFromMilliseconds(int milliseconds, const ProjectSettings& settings) {
	return MillisecondsToFrameCount(milliseconds, settings) - settings.start_frame;
}

// 歌詞終了時刻をプロジェクト基準の終了フレームへ変換する。
int EndFrameFromMilliseconds(int milliseconds, const ProjectSettings& settings) {
	return MillisecondsToFrameCount(milliseconds, settings) - settings.start_frame;
}

struct TimingLine {
	int index = 0;
	LyricLine source;
	int display_start_frame = 0;
	int display_end_frame = 0;
	int lyric_start_frame = 0;
	int lyric_end_frame = 0;
	int text_width = 0;
	std::vector<TextElementBounds> text_element_bounds;
	std::vector<LayoutSyllable> syllables;
};

struct ProvisionalLayoutLine {
	int index = 0;
	LyricLine source;
	int row_index = 0;
	int display_start_frame = 0;
	int display_end_frame = 0;
	int lyric_start_frame = 0;
	int lyric_end_frame = 0;
	int text_width = 0;
	std::vector<TextElementBounds> text_element_bounds;
	std::vector<LayoutSyllable> syllables;
	int page_id = 0;
};

struct RubyRect {
	float left = 0.0f;
	float right = 0.0f;
};

struct RowPlacement {
	int display_start_frame = 0;
	int display_end_frame = 0;
	int occupied_until_frame = 0;
};

struct RowState {
	explicit RowState(int row_index) : index(row_index) {}
	int index = 0;
	std::vector<RowPlacement> placements;
};

struct ScheduledPlacement {
	RowState* row = nullptr;
	int display_start_frame = 0;
};

struct ActiveDisplayLine {
	int page_id = 0;
	int display_end_frame = 0;
	int occupied_until_frame = 0;
	int visual_row_index = 0;
};

struct PendingPageState {
	// ページ内の未配置行を管理する状態を初期化する。
	PendingPageState(int id, std::vector<ProvisionalLayoutLine> value_lines, int next_frame)
		: page_id(id), lines(std::move(value_lines)), next_start_frame(next_frame) {}

	int page_id = 0;
	std::vector<ProvisionalLayoutLine> lines;
	size_t next_line_index = 0;
	int next_start_frame = 0;

	// 未配置行が残っているかを返す。
	bool HasRemaining() const { return next_line_index < lines.size(); }
	// ページ全体の行数を返す。
	int TotalLineCount() const { return static_cast<int>(lines.size()); }
	// まだ配置していない行数を返す。
	int RemainingCount() const { return static_cast<int>(lines.size() - next_line_index); }

	// 次に配置する行を1つ取り出す。
	ProvisionalLayoutLine TakeNextLine() { return lines[next_line_index++]; }
};

// 2つの表示区間 [start, end) が重なっているかを判定する。
bool IntervalsOverlap(int start_a, int end_a, int start_b, int end_b) {
	return start_a < end_b && start_b < end_a;
}

// 各表示文字要素の描画幅を個別に計測する。
std::vector<int> MeasureTextElementWidths(const std::vector<std::wstring>& elements, const FontStyleSettings& font, TextMeasurer* text_measurer) {
	std::vector<int> widths;
	widths.reserve(elements.size());
	for (const auto& element : elements) {
		widths.push_back(text_measurer->MeasureWidth(font, element));
	}

	return widths;
}

// 文字幅と文字間隔から各文字要素の左右端を組み立てる。
std::vector<TextElementBounds> BuildTextElementBoundsFromMetrics(const std::vector<int>& element_widths, const std::vector<float>& gap_widths) {
	std::vector<TextElementBounds> bounds;
	bounds.reserve(element_widths.size());

	float current = 0.0f;
	for (size_t index = 0; index < element_widths.size(); ++index) {
		const auto start = current;
		current += static_cast<float>(element_widths[index]);
		bounds.push_back(TextElementBounds{ start, current });

		if (index < gap_widths.size()) {
			current += gap_widths[index];
		}
	}

	return bounds;
}

// 1つのルビspanの実描画矩形を返す。
std::optional<RubyRect> BuildRubyRectForSpan(const RubySpan& ruby_span, const std::vector<TextElementBounds>& element_bounds, TextMeasurer* text_measurer, const FontStyleSettings& ruby_font) {
	size_t safe_start = 0;
	size_t safe_end = 0;
	if (!TryGetClampedTextElementRange(ruby_span.start_index, ruby_span.base_length, element_bounds.size(), &safe_start, &safe_end)) {
		return std::nullopt;
	}

	const auto base_start = element_bounds[safe_start].start;
	const auto base_end = element_bounds[safe_end].end;
	const auto ruby_width = static_cast<float>(text_measurer->MeasureWidth(ruby_font, ruby_span.ruby_text));
	const auto center = (base_start + base_end) / 2.0f;
	return RubyRect{ center - (ruby_width / 2.0f), center + (ruby_width / 2.0f) };
}

// ルビ衝突に応じて局所字間を調整した本文配置を構築する。
std::vector<TextElementBounds> BuildAdjustedTextElementBounds(const LyricLine& line, const ProjectSettings& settings, TextMeasurer* text_measurer) {
	const auto elements = SplitTextElements(line.text);
	if (elements.empty()) {
		return {};
	}

	const auto element_widths = MeasureTextElementWidths(elements, settings.font, text_measurer);
	std::vector<float> gap_widths(elements.size() > 0 ? elements.size() - 1 : 0, settings.font.letter_spacing);
	std::vector<const RubySpan*> sorted_spans;
	sorted_spans.reserve(line.ruby_spans.size());
	for (const auto& ruby_span : line.ruby_spans) {
		if (ruby_span.base_length <= 0 || ruby_span.start_index < 0 || ruby_span.start_index >= static_cast<int>(elements.size())) {
			continue;
		}

		sorted_spans.push_back(&ruby_span);
	}

	std::sort(sorted_spans.begin(), sorted_spans.end(), [](const RubySpan* a, const RubySpan* b) {
		if (a->start_index != b->start_index) {
			return a->start_index < b->start_index;
		}
		return a->base_length < b->base_length;
	});

	for (size_t span_index = 1; span_index < sorted_spans.size(); ++span_index) {
		const auto* left_span = sorted_spans[span_index - 1];
		const auto* current_span = sorted_spans[span_index];
		size_t left_start = 0;
		size_t left_end = 0;
		size_t current_start = 0;
		size_t current_end = 0;
		if (!TryGetClampedTextElementRange(left_span->start_index, left_span->base_length, elements.size(), &left_start, &left_end) ||
			!TryGetClampedTextElementRange(current_span->start_index, current_span->base_length, elements.size(), &current_start, &current_end)) {
			continue;
		}

		const auto left_end_index = static_cast<int>(left_end);
		const auto current_start_index = static_cast<int>(current_start);
		if (left_end_index != current_start_index - 1) {
			continue;
		}

		const auto current_end_index = static_cast<int>(current_end);
		if (current_start_index <= 0 || current_end_index < current_start_index) {
			continue;
		}

		const auto element_bounds = BuildTextElementBoundsFromMetrics(element_widths, gap_widths);
		const auto left_rect = BuildRubyRectForSpan(*left_span, element_bounds, text_measurer, settings.ruby_font);
		const auto current_rect = BuildRubyRectForSpan(*current_span, element_bounds, text_measurer, settings.ruby_font);
		if (!left_rect.has_value() || !current_rect.has_value()) {
			continue;
		}

		const auto overlap = left_rect->right - current_rect->left;
		if (overlap <= 0.0f) {
			continue;
		}

		const auto left_gap_index = current_span->start_index - 1;
		const auto internal_start_gap = current_span->start_index;
		const auto internal_end_gap = current_end_index - 1;
		const auto right_gap_index = current_end_index;

		float weighted_sum = gap_widths[left_gap_index];
		float influence = 1.0f;
		float target_floor = gap_widths[left_gap_index];

		for (auto gap_index = internal_start_gap; gap_index <= internal_end_gap; ++gap_index) {
			weighted_sum += gap_widths[gap_index] * 0.5f;
			influence += 0.5f;
			target_floor = std::max(target_floor, gap_widths[gap_index]);
		}

		if (right_gap_index < static_cast<int>(gap_widths.size())) {
			target_floor = std::max(target_floor, gap_widths[right_gap_index]);
		}

		const auto required_target = (overlap + weighted_sum) / influence;
		const auto target_gap = std::ceil(std::max(target_floor, required_target));

		gap_widths[left_gap_index] = target_gap;
		for (auto gap_index = internal_start_gap; gap_index <= internal_end_gap; ++gap_index) {
			gap_widths[gap_index] = target_gap;
		}
		if (right_gap_index < static_cast<int>(gap_widths.size())) {
			gap_widths[right_gap_index] = target_gap;
		}
	}

	return BuildTextElementBoundsFromMetrics(element_widths, gap_widths);
}

// 音節ごとのタイミングをワイプ幅付きレイアウトへ変換する。
std::vector<LayoutSyllable> BuildSyllableLayouts(const LyricLine& line, const ProjectSettings& settings, const std::vector<TextElementBounds>& element_bounds) {
	std::vector<LayoutSyllable> result;
	result.reserve(line.syllables.size());

	int clip_start = 0;
	int element_cursor = 0;
	for (size_t i = 0; i < line.syllables.size(); ++i) {
		const auto& syllable = line.syllables[i];
		const auto syllable_length = CountTextElements(syllable.text);
		if (syllable_length <= 0 || element_cursor >= static_cast<int>(element_bounds.size())) {
			continue;
		}

		const auto end_index = std::min(static_cast<int>(element_bounds.size()) - 1, element_cursor + syllable_length - 1);
		const auto clip_end = static_cast<int>(std::lround(element_bounds[end_index].end));
		result.push_back(LayoutSyllable{
			static_cast<int>(i),
			syllable.text,
			StartFrameFromMilliseconds(syllable.start_time_ms, settings),
			EndFrameFromMilliseconds(syllable.end_time_ms, settings),
			clip_start,
			clip_end });
		clip_start = clip_end;
		element_cursor = end_index + 1;
	}

	return result;
}

// 行ごとの論理ページ番号を割り振る。
std::vector<int> AssignLogicalPageIds(const std::vector<TimingLine>& lines, int max_visible_lines, int section_reset_gap_frames) {
	std::vector<int> page_ids(lines.size(), 0);
	int page_id = 0;
	int line_count_in_page = 0;

	// 表示可能行数を超えたときや長い無音区間でページを切り替える。
	for (size_t index = 0; index < lines.size(); ++index) {
		if (index > 0) {
			const auto idle_gap_frames = lines[index].display_start_frame - lines[index - 1].display_end_frame;
			if (idle_gap_frames > section_reset_gap_frames) {
				page_id++;
				line_count_in_page = 0;
			}
		}

		if (line_count_in_page == max_visible_lines) {
			page_id++;
			line_count_in_page = 0;
		}

		page_ids[index] = page_id;
		line_count_in_page++;
	}

	return page_ids;
}

// 指定行にその表示区間を追加できるか判定する。
bool CanPlaceAt(const RowState& row, int display_start_frame, int display_end_frame, int min_gap_frames) {
	const auto occupied_until_frame = display_end_frame + min_gap_frames;
	for (const auto& placement : row.placements) {
		if (occupied_until_frame <= placement.display_start_frame || placement.occupied_until_frame <= display_start_frame) {
			continue;
		}

		return false;
	}

	return true;
}

// 指定行で衝突しない最も早い開始フレームを探す。
int FindEarliestStart(const RowState& row, int minimum_start_frame, int display_end_frame, int min_gap_frames) {
	int candidate_start_frame = minimum_start_frame;
	auto placements = row.placements;
	std::sort(placements.begin(), placements.end(), [](const RowPlacement& a, const RowPlacement& b) {
		return a.display_start_frame < b.display_start_frame;
	});

	const auto occupied_until_frame = display_end_frame + min_gap_frames;
	for (const auto& placement : placements) {
		if (placement.occupied_until_frame <= candidate_start_frame) {
			continue;
		}

		if (occupied_until_frame <= placement.display_start_frame) {
			break;
		}

		candidate_start_frame = placement.occupied_until_frame;
	}

	return candidate_start_frame;
}

// 行に表示区間を追加して占有情報を更新する。
void AddPlacement(RowState* row, int display_start_frame, int display_end_frame, int min_gap_frames) {
	row->placements.push_back(RowPlacement{
		display_start_frame,
		display_end_frame,
		display_end_frame + min_gap_frames });
}

// 行数制約を守りながら1行分の仮配置を決める。
ScheduledPlacement SchedulePlacement(std::vector<RowState>* rows, int base_display_start_frame, int display_end_frame, int min_gap_frames, int max_visible_lines) {
	std::vector<int> page_start_candidates;
	for (const auto& row : *rows) {
		for (const auto& placement : row.placements) {
			if (placement.display_end_frame > base_display_start_frame) {
				page_start_candidates.push_back(placement.display_start_frame);
			}
		}
	}

	std::sort(page_start_candidates.begin(), page_start_candidates.end());
	page_start_candidates.erase(std::unique(page_start_candidates.begin(), page_start_candidates.end()), page_start_candidates.end());

	// 同じページの行がまとまるよう、既存の開始タイミングを優先して使う。
	for (const auto candidate_start_frame : page_start_candidates) {
		for (auto& row : *rows) {
			if (!CanPlaceAt(row, candidate_start_frame, display_end_frame, min_gap_frames)) {
				continue;
			}

			AddPlacement(&row, candidate_start_frame, display_end_frame, min_gap_frames);
			return ScheduledPlacement{ &row, candidate_start_frame };
		}

		if (static_cast<int>(rows->size()) < max_visible_lines) {
			rows->emplace_back(static_cast<int>(rows->size()));
			auto& row = rows->back();
			AddPlacement(&row, candidate_start_frame, display_end_frame, min_gap_frames);
			return ScheduledPlacement{ &row, candidate_start_frame };
		}
	}

	RowState* best_row = nullptr;
	int best_start_frame = std::numeric_limits<int>::max();
	for (auto& row : *rows) {
		const auto candidate_start_frame = FindEarliestStart(row, base_display_start_frame, display_end_frame, min_gap_frames);
		if (candidate_start_frame < best_start_frame) {
			best_start_frame = candidate_start_frame;
			best_row = &row;
		}
	}

	if (static_cast<int>(rows->size()) < max_visible_lines && base_display_start_frame <= best_start_frame) {
		rows->emplace_back(static_cast<int>(rows->size()));
		auto& row = rows->back();
		AddPlacement(&row, base_display_start_frame, display_end_frame, min_gap_frames);
		return ScheduledPlacement{ &row, base_display_start_frame };
	}

	if (best_row && best_start_frame <= display_end_frame) {
		AddPlacement(best_row, best_start_frame, display_end_frame, min_gap_frames);
		return ScheduledPlacement{ best_row, best_start_frame };
	}

	auto fallback = std::min_element(rows->begin(), rows->end(), [](const RowState& a, const RowState& b) {
		const auto a_end = std::max_element(a.placements.begin(), a.placements.end(), [](const RowPlacement& x, const RowPlacement& y) {
			return x.occupied_until_frame < y.occupied_until_frame;
		});
		const auto b_end = std::max_element(b.placements.begin(), b.placements.end(), [](const RowPlacement& x, const RowPlacement& y) {
			return x.occupied_until_frame < y.occupied_until_frame;
		});
		const auto a_value = a_end == a.placements.end() ? 0 : a_end->occupied_until_frame;
		const auto b_value = b_end == b.placements.end() ? 0 : b_end->occupied_until_frame;
		return a_value < b_value;
	});

	AddPlacement(&(*fallback), base_display_start_frame, display_end_frame, min_gap_frames);
	return ScheduledPlacement{ &(*fallback), base_display_start_frame };
}

// ある時刻で表示可能なページ行を下段から順に確定する。
void ResolvePageAtFrame(
	PendingPageState* page,
	int current_start_frame,
	int max_visible_lines,
	int min_gap_frames,
	std::vector<ActiveDisplayLine>* active,
	std::unordered_map<int, int>* resolved_starts,
	std::unordered_map<int, int>* resolved_rows) {
	// 同一ページは下段から詰めて配置し、別ページが割り込む位置で止める。
	std::unordered_map<int, ActiveDisplayLine> active_by_row;
	for (const auto& line : *active) {
		active_by_row[line.visual_row_index] = line;
	}

	std::vector<int> free_rows;
	std::optional<int> blocker_end_frame;
	for (int row = max_visible_lines - 1; row >= 0; --row) {
		const auto found = active_by_row.find(row);
		if (found != active_by_row.end()) {
			if (found->second.page_id != page->page_id) {
				blocker_end_frame = found->second.occupied_until_frame;
				break;
			}

			continue;
		}

		free_rows.push_back(row);
	}

	if (active->empty() && page->TotalLineCount() == 1) {
		free_rows.clear();
		free_rows.push_back(0);
	}

	const auto assign_count = std::min(static_cast<int>(free_rows.size()), page->RemainingCount());
	for (int i = 0; i < assign_count; ++i) {
		const auto line = page->TakeNextLine();
		(*resolved_starts)[line.index] = current_start_frame;
		(*resolved_rows)[line.index] = free_rows[i];
		active->push_back(ActiveDisplayLine{
			page->page_id,
			line.display_end_frame,
			line.display_end_frame + min_gap_frames,
			free_rows[i] });
	}

	if (!page->HasRemaining()) {
		return;
	}

	if (blocker_end_frame.has_value()) {
		page->next_start_frame = blocker_end_frame.value();
		return;
	}

	auto same_page_end_frame = std::numeric_limits<int>::max();
	for (const auto& line : *active) {
		if (line.page_id == page->page_id) {
			same_page_end_frame = std::min(same_page_end_frame, line.occupied_until_frame);
		}
	}
	page->next_start_frame = same_page_end_frame;
}

// ページ内の継続表示が自然になるよう仮配置を解決する。
std::vector<ProvisionalLayoutLine> ResolvePageContinuity(const std::vector<ProvisionalLayoutLine>& lines, int max_visible_lines, int min_gap_frames) {
	std::unordered_map<int, int> resolved_starts;
	std::unordered_map<int, int> resolved_rows;
	std::map<int, std::vector<ProvisionalLayoutLine>> grouped;
	for (const auto& line : lines) {
		grouped[line.page_id].push_back(line);
	}

	std::vector<PendingPageState> pages;
	for (auto& pair : grouped) {
		auto& page_lines = pair.second;
		std::sort(page_lines.begin(), page_lines.end(), [](const ProvisionalLayoutLine& a, const ProvisionalLayoutLine& b) {
			if (a.lyric_start_frame != b.lyric_start_frame) {
				return a.lyric_start_frame < b.lyric_start_frame;
			}
			return a.index < b.index;
		});

		auto min_start = std::numeric_limits<int>::max();
		for (const auto& line : page_lines) {
			min_start = std::min(min_start, line.display_start_frame);
		}

		pages.emplace_back(pair.first, page_lines, min_start);
	}

	std::vector<ActiveDisplayLine> active;
	// 時系列順に解決して、同じページの表示継続性を保つ。
	while (true) {
		int current_start_frame = std::numeric_limits<int>::max();
		bool has_remaining = false;
		for (const auto& page : pages) {
			if (page.HasRemaining()) {
				has_remaining = true;
				current_start_frame = std::min(current_start_frame, page.next_start_frame);
			}
		}
		if (!has_remaining) {
			break;
		}

		active.erase(
			std::remove_if(active.begin(), active.end(), [current_start_frame](const ActiveDisplayLine& line) {
				return line.occupied_until_frame <= current_start_frame;
			}),
			active.end());

		for (auto& page : pages) {
			if (page.HasRemaining() && page.next_start_frame == current_start_frame) {
				ResolvePageAtFrame(&page, current_start_frame, max_visible_lines, min_gap_frames, &active, &resolved_starts, &resolved_rows);
			}
		}
	}

	std::vector<ProvisionalLayoutLine> result;
	result.reserve(lines.size());
	for (const auto& line : lines) {
		auto resolved = line;
		resolved.display_start_frame = resolved_starts[line.index];
		resolved.row_index = resolved_rows[line.index];
		result.push_back(resolved);
	}

	std::sort(result.begin(), result.end(), [](const ProvisionalLayoutLine& a, const ProvisionalLayoutLine& b) {
		return a.index < b.index;
	});
	return result;
}

// 同時に消したい行群の終了フレームを揃える。
void ExtendGroupedDisappearances(
	const std::vector<ProvisionalLayoutLine>& page_lines,
	size_t start_index,
	size_t count,
	int next_page_start_frame,
	std::unordered_map<int, ProvisionalLayoutLine>* resolved) {
	if (start_index >= page_lines.size()) {
		return;
	}

	const auto end_index = std::min(page_lines.size(), start_index + count);
	if (end_index - start_index < 2) {
		return;
	}

	int target_end_frame = 0;
	for (size_t i = start_index; i < end_index; ++i) {
		target_end_frame = std::max(target_end_frame, std::min(page_lines[i].display_end_frame, next_page_start_frame));
	}

	for (size_t i = start_index; i < end_index; ++i) {
		auto& line = (*resolved)[page_lines[i].index];
		if (line.display_end_frame < target_end_frame) {
			line.display_end_frame = target_end_frame;
		}
	}
}

// 同一ページ内の複数行がなるべく同時に消えるよう終了時刻を調整する。
std::vector<ProvisionalLayoutLine> HarmonizePageDisappearances(const std::vector<ProvisionalLayoutLine>& lines, int min_gap_frames) {
	std::unordered_map<int, ProvisionalLayoutLine> resolved;
	for (const auto& line : lines) {
		resolved[line.index] = line;
	}

	std::map<int, std::vector<ProvisionalLayoutLine>> pages;
	for (const auto& line : lines) {
		pages[line.page_id].push_back(line);
	}

	std::vector<std::vector<ProvisionalLayoutLine>> ordered_pages;
	for (auto& pair : pages) {
		auto& page_lines = pair.second;
		std::sort(page_lines.begin(), page_lines.end(), [](const ProvisionalLayoutLine& a, const ProvisionalLayoutLine& b) {
			if (a.lyric_start_frame != b.lyric_start_frame) {
				return a.lyric_start_frame < b.lyric_start_frame;
			}
			return a.index < b.index;
		});
		ordered_pages.push_back(page_lines);
	}

	// 複数行ページはまとまって切り替わるよう終了時刻を延長する。
	for (size_t page_index = 0; page_index < ordered_pages.size(); ++page_index) {
		const auto& page_lines = ordered_pages[page_index];
		if (page_lines.size() < 3) {
			continue;
		}

		int next_page_start_frame = std::numeric_limits<int>::max();
		if (page_index + 1 < ordered_pages.size()) {
			next_page_start_frame = ordered_pages[page_index + 1].front().display_start_frame - min_gap_frames;
		}
		if (next_page_start_frame <= 0) {
			continue;
		}

		if (page_lines.size() == 3) {
			ExtendGroupedDisappearances(page_lines, 0, 3, next_page_start_frame, &resolved);
			continue;
		}

		ExtendGroupedDisappearances(page_lines, 0, 2, next_page_start_frame, &resolved);
		ExtendGroupedDisappearances(page_lines, 2, 2, next_page_start_frame, &resolved);
	}

	std::vector<ProvisionalLayoutLine> result;
	result.reserve(resolved.size());
	for (const auto& pair : resolved) {
		result.push_back(pair.second);
	}
	std::sort(result.begin(), result.end(), [](const ProvisionalLayoutLine& a, const ProvisionalLayoutLine& b) {
		return a.index < b.index;
	});
	return result;
}

// 他行と重ならない単独表示行かを判定する。
bool IsIsolated(const ProvisionalLayoutLine& line, const std::vector<ProvisionalLayoutLine>& all_lines) {
	for (const auto& other : all_lines) {
		if (other.index == line.index) {
			continue;
		}

		if (IntervalsOverlap(line.display_start_frame, line.display_end_frame, other.display_start_frame, other.display_end_frame)) {
			return false;
		}
	}

	return true;
}

// 本文の文字位置を基準にルビの描画位置を組み立てる。
std::vector<LayoutSyllable> BuildRubyTimingLayouts(
	const RubySpan& ruby_span,
	const std::vector<TextElementTiming>& element_timings,
	size_t safe_start,
	size_t safe_end,
	const std::vector<TextElementBounds>& ruby_bounds,
	const ProjectSettings& settings) {
	std::vector<LayoutSyllable> result;
	if (ruby_span.ruby_timing_segments.empty() || ruby_bounds.empty() || safe_start >= element_timings.size() || safe_end >= element_timings.size()) {
		return result;
	}

	const auto base_start_time_ms = element_timings[safe_start].start_time_ms;
	const auto base_end_time_ms = element_timings[safe_end].end_time_ms;
	if (base_end_time_ms <= base_start_time_ms) {
		return result;
	}

	int clip_start = 0;
	int element_cursor = 0;
	for (size_t index = 0; index < ruby_span.ruby_timing_segments.size(); ++index) {
		const auto& segment = ruby_span.ruby_timing_segments[index];
		const auto element_length = CountTextElements(segment.text);
		if (element_length <= 0 || element_cursor >= static_cast<int>(ruby_bounds.size())) {
			continue;
		}

		const auto start_time_ms = base_start_time_ms + segment.start_time_ms;
		const auto end_time_ms = segment.end_time_ms >= 0
			? base_start_time_ms + segment.end_time_ms
			: base_end_time_ms;
		if (end_time_ms <= start_time_ms || start_time_ms < base_start_time_ms || end_time_ms > base_end_time_ms) {
			result.clear();
			return result;
		}

		const auto end_index = std::min(static_cast<int>(ruby_bounds.size()) - 1, element_cursor + element_length - 1);
		const auto clip_end = static_cast<int>(std::lround(ruby_bounds[end_index].end));
		result.push_back(LayoutSyllable{
			static_cast<int>(index),
			segment.text,
			StartFrameFromMilliseconds(start_time_ms, settings),
			EndFrameFromMilliseconds(end_time_ms, settings),
			clip_start,
			clip_end });
		clip_start = clip_end;
		element_cursor = end_index + 1;
	}

	if (result.empty()) {
		return result;
	}

	const auto last_clip_end = result.back().clip_end;
	const auto expected_clip_end = static_cast<int>(std::lround(ruby_bounds.back().end));
	if (last_clip_end != expected_clip_end) {
		result.clear();
	}

	return result;
}

std::optional<LayoutRuby> BuildRubyLayout(const ProvisionalLayoutLine& line, const ProjectSettings& settings, TextMeasurer* text_measurer) {
	if (line.source.ruby_spans.empty()) {
		return std::nullopt;
	}

	LayoutRuby ruby;
	const auto element_timings = BuildTextElementTimings(line.source.syllables);
	for (const auto& ruby_span : line.source.ruby_spans) {
		size_t safe_start = 0;
		size_t safe_end = 0;
		if (!TryGetClampedTextElementRange(ruby_span.start_index, ruby_span.base_length, line.text_element_bounds.size(), &safe_start, &safe_end)) {
			continue;
		}

		const auto start = line.text_element_bounds[safe_start].start;
		const auto end = line.text_element_bounds[safe_end].end;
		const auto ruby_width = static_cast<float>(text_measurer->MeasureWidth(settings.ruby_font, ruby_span.ruby_text));
		const auto ruby_bounds = BuildTextElementBounds(ruby_span.ruby_text, settings.ruby_font, text_measurer);
		auto timing_segments = BuildRubyTimingLayouts(ruby_span, element_timings, safe_start, safe_end, ruby_bounds, settings);
		const auto base_width = end - start;
		auto offset = start + ((base_width - ruby_width) / 2.0f);
		if (settings.font.italic && settings.ruby_font.italic) {
			offset += settings.font.size * kItalicShear;
		}
		ruby.segments.push_back(LayoutRubySegment{ ruby_span.ruby_text, offset, start, end, ruby_width, std::move(timing_segments) });
	}

	if (ruby.segments.empty()) {
		return std::nullopt;
	}

	return ruby;
}

void ShiftSyllableFrames(std::vector<LayoutSyllable>* syllables, int frame_offset) {
	for (auto& syllable : *syllables) {
		syllable.start_frame += frame_offset;
		syllable.end_frame += frame_offset;
	}
}

} // namespace

// 文字列を表示上の文字要素単位へ分割する。
std::vector<std::wstring> SplitTextElements(const std::wstring& text) {
	std::vector<std::wstring> result;
	size_t index = 0;
	while (index < text.size()) {
		const auto cluster_start = index;
		size_t char_length = 0;
		DecodeCodePoint(text, index, &char_length);
		index += char_length;

		while (index < text.size()) {
			size_t next_length = 0;
			const auto codepoint = DecodeCodePoint(text, index, &next_length);
			if (!IsCombiningMark(codepoint) && !IsVariationSelector(codepoint)) {
				break;
			}

			index += next_length;
		}

		result.emplace_back(text.substr(cluster_start, index - cluster_start));
	}

	return result;
}

// 文字列内の表示文字要素数を数える。
int CountTextElements(const std::wstring& text) {
	return static_cast<int>(SplitTextElements(text).size());
}

// 音節列から各文字要素の時刻情報を展開する。
std::vector<TextElementTiming> BuildTextElementTimings(const std::vector<LyricSyllable>& syllables) {
	std::vector<TextElementTiming> timings;
	for (const auto& syllable : syllables) {
		const auto element_count = CountTextElements(syllable.text);
		for (int index = 0; index < element_count; ++index) {
			timings.push_back(TextElementTiming{ syllable.start_time_ms, syllable.end_time_ms });
		}
	}

	return timings;
}

// 各文字要素の左右端位置を積み上げ計算する。
std::vector<TextElementBounds> BuildTextElementBounds(const std::wstring& text, const FontStyleSettings& font, TextMeasurer* text_measurer) {
	const auto elements = SplitTextElements(text);
	std::vector<TextElementBounds> bounds;
	bounds.reserve(elements.size());

	std::wstring prefix;
	prefix.reserve(text.size());

	float current_start = 0.0f;
	for (size_t index = 0; index < elements.size(); ++index) {
		prefix += elements[index];
		const auto current_end = static_cast<float>(text_measurer->MeasureWidth(font, prefix));
		bounds.push_back(TextElementBounds{ current_start, current_end });

		current_start = current_end;
		if (index + 1 < elements.size()) {
			current_start += font.letter_spacing;
		}
	}

	return bounds;
}

// テキスト幅計測用のGDI+オブジェクトを初期化する。
TextMeasurer::TextMeasurer()
	: bitmap_(1, 1, PixelFormat32bppPARGB),
	  graphics_(&bitmap_) {
	graphics_.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
}

// フォント設定と文字列からキャッシュキーを作る。
std::wstring TextMeasurer::BuildKey(const FontStyleSettings& font, const std::wstring& text) const {
	std::wostringstream stream;
	stream << font.name << L'|' << font.size << L'|' << font.letter_spacing << L'|' << font.bold << L'|' << font.italic << L'|' << text;
	return stream.str();
}

// 文字間隔も含めた描画幅を実測値から計算する。
int TextMeasurer::MeasureWidthUncached(const FontStyleSettings& font, const std::wstring& text) {
	const auto glyph_width = MeasureStringAdvance(font, text);
	const auto letter_spacing = static_cast<float>(std::max(0, CountTextElements(text) - 1)) * font.letter_spacing;
	return static_cast<int>(std::ceil(static_cast<float>(glyph_width) + letter_spacing + 0.0001f));
}

// キャッシュを使って文字列の描画幅を返す。
int TextMeasurer::MeasureWidth(const FontStyleSettings& font, const std::wstring& text) {
	if (text.empty()) {
		return 0;
	}

	const auto key = BuildKey(font, text);
	const auto found = cache_.find(key);
	if (found != cache_.end()) {
		return found->second;
	}

	const auto width = MeasureWidthUncached(font, text);
	cache_.emplace(key, width);
	return width;
}

// 解析済み歌詞から最終的な描画レイアウトを構築する。
LayoutResult BuildLayout(const LyricsDocument& document, const ProjectSettings& settings) {
	if (settings.max_visible_lines < 1 || settings.max_visible_lines > 4) {
		throw std::runtime_error("MaxVisibleLines must be between 1 and 4.");
	}

	auto layout_settings = settings;
	layout_settings.start_frame = 0;
	TextMeasurer text_measurer;
	const auto lead_in_frames = MillisecondsToFrameCount(layout_settings.lead_in_ms, layout_settings);
	const auto hold_frames = MillisecondsToFrameCount(layout_settings.hold_ms, layout_settings);
	const auto min_gap_frames = MillisecondsToFrameCount(layout_settings.min_gap_ms, layout_settings);

	// まず各行の表示区間と文字幅を計算する。
	std::vector<TimingLine> timing_lines;
	timing_lines.reserve(document.lines.size());
	for (size_t index = 0; index < document.lines.size(); ++index) {
		const auto& line = document.lines[index];
		const auto lyric_start_frame = StartFrameFromMilliseconds(line.start_time_ms, layout_settings);
		const auto lyric_end_frame = EndFrameFromMilliseconds(line.end_time_ms, layout_settings);
		const auto display_start_frame = std::max(0, lyric_start_frame - lead_in_frames);
		const auto display_end_frame = std::max(display_start_frame, lyric_end_frame + hold_frames);
		const auto text_element_bounds = BuildAdjustedTextElementBounds(line, layout_settings, &text_measurer);
		const auto text_width = text_element_bounds.empty()
			? 0
			: static_cast<int>(std::lround(text_element_bounds.back().end));
		auto syllables = BuildSyllableLayouts(line, layout_settings, text_element_bounds);

		timing_lines.push_back(TimingLine{
			static_cast<int>(index),
			line,
			display_start_frame,
			display_end_frame,
			lyric_start_frame,
			lyric_end_frame,
			text_width,
			text_element_bounds,
			syllables });
	}

	const auto section_reset_gap_frames = std::max(lead_in_frames, min_gap_frames);
	const auto page_ids = AssignLogicalPageIds(timing_lines, settings.max_visible_lines, section_reset_gap_frames);
	std::vector<ProvisionalLayoutLine> provisional;
	provisional.reserve(timing_lines.size());
	std::vector<RowState> rows;

	// 次に行配置とページ分けを決め、継続表示の整合を取る。
	for (size_t timing_index = 0; timing_index < timing_lines.size(); ++timing_index) {
		const auto& line = timing_lines[timing_index];
		const auto placement = SchedulePlacement(&rows, line.display_start_frame, line.display_end_frame, min_gap_frames, settings.max_visible_lines);
		provisional.push_back(ProvisionalLayoutLine{
			line.index,
			line.source,
			placement.row ? placement.row->index : 0,
			placement.display_start_frame,
			line.display_end_frame,
			line.lyric_start_frame,
			line.lyric_end_frame,
			line.text_width,
			line.text_element_bounds,
			line.syllables,
			page_ids[timing_index] });
	}

	provisional = ResolvePageContinuity(provisional, settings.max_visible_lines, min_gap_frames);
	provisional = HarmonizePageDisappearances(provisional, min_gap_frames);

	LayoutResult result;
	result.lines.reserve(provisional.size());
	// 最後に画面座標とルビ位置を確定して描画用データへ落とし込む。
	for (const auto& line : provisional) {
		const auto isolated = IsIsolated(line, provisional);
		const auto x = isolated
			? -line.text_width / 2.0f
			: (-settings.video_width / 2.0f) + settings.side_margin;
		const auto y = (settings.video_height / 2.0f)
			- settings.bottom_margin
			- settings.font.size
			- (line.row_index * (settings.font.size + settings.line_spacing));
		auto ruby = BuildRubyLayout(line, layout_settings, &text_measurer);
		if (ruby.has_value()) {
			ruby->y = y - settings.ruby_font.size - settings.ruby_gap;
		}

		auto layout_line = LayoutLine{
			line.index,
			line.source,
			line.row_index,
			line.display_start_frame,
			line.display_end_frame,
			line.lyric_start_frame,
			line.lyric_end_frame,
			x,
			y,
			line.text_width,
			line.text_element_bounds,
			line.syllables,
			ruby };
		if (settings.start_frame != 0) {
			const auto frame_offset = -settings.start_frame;
			layout_line.display_start_frame += frame_offset;
			layout_line.display_end_frame += frame_offset;
			layout_line.lyric_start_frame += frame_offset;
			layout_line.lyric_end_frame += frame_offset;
			ShiftSyllableFrames(&layout_line.syllables, frame_offset);
			if (layout_line.ruby.has_value()) {
				for (auto& segment : layout_line.ruby->segments) {
					ShiftSyllableFrames(&segment.timing_segments, frame_offset);
				}
			}
		}
		result.lines.push_back(std::move(layout_line));
	}

	return result;
}
