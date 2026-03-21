#pragma once

#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// レイアウト前の歌詞1音節分の時刻情報。
struct LyricSyllable {
	int start_time_ms = 0;
	int end_time_ms = 0;
	std::wstring text;
};

// ベース文字列に対応するルビ情報。
struct RubySpan {
	int start_index = 0;
	int base_length = 0;
	std::wstring base_text;
	std::wstring ruby_text;
};

// 1行分の歌詞と、その解析結果をまとめたデータ。
struct LyricLine {
	int start_time_ms = 0;
	int end_time_ms = 0;
	std::wstring text;
	std::vector<LyricSyllable> syllables;
	std::vector<RubySpan> ruby_spans;
	std::wstring raw_line;
};

// 入力全体を保持する歌詞ドキュメント。
struct LyricsDocument {
	std::vector<LyricLine> lines;
};

// 描画に使うフォント設定。
struct FontStyleSettings {
	std::wstring name;
	float size = 64.0f;
	float letter_spacing = 0.0f;
	bool bold = false;
	bool italic = false;
};

// 斜体クリップとルビ補正で共有する shear 量。
inline constexpr float kItalicShear = 0.25f;

// レイアウト計算に必要なプロジェクト設定。
struct ProjectSettings {
	int video_width = 1280;
	int video_height = 720;
	int video_rate = 23976;
	int video_scale = 1000;
	int start_frame = 0;
	int side_margin = 50;
	int bottom_margin = 50;
	int line_spacing = 20;
	int ruby_gap = 0;
	int lead_in_ms = 1000;
	int hold_ms = 250;
	int min_gap_ms = 200;
	int max_visible_lines = 2;
	FontStyleSettings font;
	FontStyleSettings ruby_font;

	// レートとスケールから実フレームレートを求める。
	double FramesPerSecond() const {
		return static_cast<double>(video_rate) / static_cast<double>(video_scale);
	}
};

// text element 範囲を安全にクランプして返す。
inline bool TryGetClampedTextElementRange(int start_index, int base_length, size_t element_count, size_t* start_out, size_t* end_out) {
	if (start_index < 0 || base_length <= 0 || element_count == 0) {
		return false;
	}

	const auto start = static_cast<size_t>(start_index);
	if (start >= element_count) {
		return false;
	}

	const auto available = element_count - start;
	const auto clamped_length = std::min(static_cast<size_t>(base_length), available);
	if (clamped_length == 0) {
		return false;
	}

	if (start_out) {
		*start_out = start;
	}
	if (end_out) {
		*end_out = start + clamped_length - 1;
	}
	return true;
}

// 描画時に使う1音節分の配置情報。
struct LayoutSyllable {
	int index = 0;
	std::wstring text;
	int start_frame = 0;
	int end_frame = 0;
	int clip_start = 0;
	int clip_end = 0;
};

// 文字要素ごとの左右端位置。
struct TextElementBounds {
	float start = 0.0f;
	float end = 0.0f;
};

// 1つのルビ片の配置情報。
struct LayoutRubySegment {
	std::wstring text;
	float offset_x = 0.0f;
	float base_start = 0.0f;
	float base_end = 0.0f;
};

// 1行分のルビ配置情報。
struct LayoutRuby {
	float y = 0.0f;
	std::vector<LayoutRubySegment> segments;
};

// 描画可能な1行分の完成レイアウト。
struct LayoutLine {
	int index = 0;
	LyricLine source;
	int row_index = 0;
	int display_start_frame = 0;
	int display_end_frame = 0;
	int lyric_start_frame = 0;
	int lyric_end_frame = 0;
	float x = 0.0f;
	float y = 0.0f;
	int text_width = 0;
	std::vector<TextElementBounds> text_element_bounds;
	std::vector<LayoutSyllable> syllables;
	std::optional<LayoutRuby> ruby;
};

// 全行分のレイアウト結果。
struct LayoutResult {
	std::vector<LayoutLine> lines;
};

// 文字幅を計測し、同じ問い合わせをキャッシュする。
class TextMeasurer {
public:
	// 計測用のGDI+オブジェクトを初期化する。
	TextMeasurer();

	// 指定フォント・文字列の描画幅を返す。
	int MeasureWidth(const FontStyleSettings& font, const std::wstring& text);

private:
	// キャッシュを使わずに文字幅を計測する。
	int MeasureWidthUncached(const FontStyleSettings& font, const std::wstring& text);
	// フォント設定と文字列からキャッシュキーを組み立てる。
	std::wstring BuildKey(const FontStyleSettings& font, const std::wstring& text) const;

	Gdiplus::Bitmap bitmap_;
	Gdiplus::Graphics graphics_;
	std::unordered_map<std::wstring, int> cache_;
};

// UTF-8テキストファイルを読み込んでワイド文字列へ変換する。
std::optional<std::wstring> ReadLyricsFile(const wchar_t* path);
// タイムタグ付き歌詞を行・音節単位へ解析する。
LyricsDocument ParseTimedLyricsDocument(const std::wstring& text);
// 解析済み歌詞から描画用レイアウトを構築する。
LayoutResult BuildLayout(const LyricsDocument& document, const ProjectSettings& settings);
// 表示上の文字要素数を数える。
int CountTextElements(const std::wstring& text);
// 結合文字を考慮して文字列を表示単位へ分割する。
std::vector<std::wstring> SplitTextElements(const std::wstring& text);
// 各文字要素の左右端位置を配列で返す。
std::vector<TextElementBounds> BuildTextElementBounds(const std::wstring& text, const FontStyleSettings& font, TextMeasurer* text_measurer);
