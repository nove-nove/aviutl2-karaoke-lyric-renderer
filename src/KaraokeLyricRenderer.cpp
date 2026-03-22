#include "KaraokeLyricCommon.h"
#include "filter2.h"
#include "logger2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

#pragma comment(lib, "gdiplus.lib")

#ifndef KLR_PLUGIN_VERSION
#define KLR_PLUGIN_VERSION "dev"
#endif

#define KLR_WIDEN_INNER(x) L##x
#define KLR_WIDEN(x) KLR_WIDEN_INNER(x)

using namespace Gdiplus;

namespace {

inline constexpr uint64_t kMaxLyricsFileSizeBytes = 8ull * 1024ull * 1024ull;

LOG_HANDLE* g_logger = nullptr;
ULONG_PTR g_gdiplus_token = 0;

std::vector<std::wstring> g_font_names;
std::vector<FILTER_ITEM_SELECT::ITEM> g_font_items;

inline constexpr const wchar_t* kPluginInformation =
	L"Timed karaoke lyric renderer for AviUtl2 by novenove ("
	KLR_WIDEN(KLR_PLUGIN_VERSION)
	L")";

FILTER_ITEM_SELECT::ITEM kOutlineModeItems[] = {
	{ L"縁なし", 0 },
	{ L"縁取り(細)", 1 },
	{ L"縁取り(中)", 2 },
	{ L"縁取り(太)", 3 },
	{ nullptr, 0 },
};

// AviUtl2に公開するフィルタUI項目。
FILTER_ITEM_FILE kLyricsFile(
	L"歌詞ファイル",
	L"",
	L"タイムタグ付き歌詞 (*.txt;*.kra;*.lrc)\0*.txt;*.kra;*.lrc\0すべてのファイル (*.*)\0*.*\0");
FILTER_ITEM_SELECT::ITEM kEmptyFontItems[] = {
	{ L"MS UI Gothic", 0 },
	{ nullptr, 0 },
};
FILTER_ITEM_SELECT kFontName(L"フォント", 0, kEmptyFontItems);
FILTER_ITEM_TRACK kFontSize(L"文字サイズ", 64, 8, 256);
FILTER_ITEM_TRACK kRubyScale(L"ルビサイズ (%)", 50, 20, 100);
FILTER_ITEM_TRACK kRubyGap(L"歌詞-ルビ間隔", 0, -64, 128);
FILTER_ITEM_TRACK kLetterSpacing(L"文字間隔", 0, -32, 32);
FILTER_ITEM_TRACK kStartFrame(L"開始フレーム", 0, 0, 500000);
FILTER_ITEM_TRACK kSideMargin(L"左右余白", 50, 0, 512);
FILTER_ITEM_TRACK kBottomMargin(L"下余白", 50, 0, 512);
FILTER_ITEM_TRACK kLineSpacing(L"行間", 50, 0, 256);
FILTER_ITEM_TRACK kLeadInMs(L"先行表示 (ms)", 2000, 0, 10000);
FILTER_ITEM_TRACK kHoldMs(L"残留表示 (ms)", 250, 0, 5000);
FILTER_ITEM_TRACK kMinGapMs(L"最小間隔 (ms)", 200, 0, 5000);
FILTER_ITEM_TRACK kVisibleLines(L"表示行数", 2, 1, 4);
FILTER_ITEM_SELECT kOutlineMode(L"縁取り", 2, kOutlineModeItems);
FILTER_ITEM_COLOR kBeforeTextColor(L"ワイプ前文字色", 0xfcff00);
FILTER_ITEM_COLOR kBeforeEdgeColor(L"ワイプ前縁色", 0x000000);
FILTER_ITEM_COLOR kAfterTextColor(L"ワイプ後文字色", 0x19cc8d);
FILTER_ITEM_COLOR kAfterEdgeColor(L"ワイプ後縁色", 0xffffff);
FILTER_ITEM_CHECK kBold(L"太字", false);
FILTER_ITEM_CHECK kItalic(L"斜体", false);
FILTER_ITEM_CHECK kShowRuby(L"ルビ表示", true);

void* kItems[] = {
	&kLyricsFile,
	&kFontName,
	&kFontSize,
	&kRubyScale,
	&kRubyGap,
	&kLetterSpacing,
	&kStartFrame,
	&kSideMargin,
	&kBottomMargin,
	&kLineSpacing,
	&kLeadInMs,
	&kHoldMs,
	&kMinGapMs,
	&kVisibleLines,
	&kOutlineMode,
	&kBeforeTextColor,
	&kBeforeEdgeColor,
	&kAfterTextColor,
	&kAfterEdgeColor,
	&kBold,
	&kItalic,
	&kShowRuby,
	nullptr,
};

FILTER_PLUGIN_TABLE kFilterPluginTable = {
	FILTER_PLUGIN_TABLE::FLAG_VIDEO | FILTER_PLUGIN_TABLE::FLAG_INPUT,
	L"AviUtl2 Karaoke Lyric Renderer",
	L"カスタムオブジェクト",
	kPluginInformation,
	kItems,
	nullptr,
	nullptr,
};

struct FileMetadata {
	uint64_t write_time = 0;
	uint64_t file_size = 0;
};

struct CachedLayout {
	LayoutResult layout;
};

struct FrameScratchBuffers {
	std::vector<std::uint8_t> bgra;
	std::vector<PIXEL_RGBA> rgba;
	std::vector<PIXEL_RGBA> empty;
};

std::mutex g_cache_mutex;
std::unordered_map<std::wstring, std::shared_ptr<CachedLayout>> g_layout_cache;

// 警告ログを出力する。
void LogWarn(const wchar_t* message) {
	if (g_logger && g_logger->warn) {
		g_logger->warn(g_logger, message);
	}
}

// エラーログを出力する。
void LogError(const wchar_t* message) {
	if (g_logger && g_logger->error) {
		g_logger->error(g_logger, message);
	}
}

// 現在選択中のフォント名を返す。
const wchar_t* GetSelectedFontName() {
	if (!g_font_names.empty()) {
		const auto index = std::clamp(kFontName.value, 0, static_cast<int>(g_font_names.size() - 1));
		return g_font_names[index].c_str();
	}

	return L"MS UI Gothic";
}

// インストール済みフォント一覧から選択肢を初期化する。
void InitializeFontItems() {
	InstalledFontCollection installed_fonts;
	const auto family_count = installed_fonts.GetFamilyCount();
	if (family_count <= 0) {
		return;
	}

	std::vector<FontFamily> families(static_cast<size_t>(family_count));
	int found = 0;
	installed_fonts.GetFamilies(family_count, families.data(), &found);

	g_font_names.clear();
	g_font_names.reserve(found);

	wchar_t family_name[LF_FACESIZE] = {};
	for (int index = 0; index < found; ++index) {
		if (families[index].GetFamilyName(family_name) == Ok) {
			g_font_names.emplace_back(family_name);
		}
	}

	std::sort(g_font_names.begin(), g_font_names.end());
	g_font_names.erase(std::unique(g_font_names.begin(), g_font_names.end()), g_font_names.end());
	if (g_font_names.empty()) {
		g_font_names.push_back(L"MS UI Gothic");
	}

	g_font_items.clear();
	g_font_items.reserve(g_font_names.size() + 1);
	int default_index = 0;
	for (size_t index = 0; index < g_font_names.size(); ++index) {
		if (_wcsicmp(g_font_names[index].c_str(), L"MS UI Gothic") == 0) {
			default_index = static_cast<int>(index);
		}

		g_font_items.push_back(FILTER_ITEM_SELECT::ITEM{ g_font_names[index].c_str(), static_cast<int>(index) });
	}
	g_font_items.push_back(FILTER_ITEM_SELECT::ITEM{ nullptr, 0 });
	kFontName.list = g_font_items.data();
	kFontName.value = default_index;
}

// ファイルの更新時刻とサイズを取得する。
std::optional<FileMetadata> GetFileMetadata(const wchar_t* path) {
	WIN32_FILE_ATTRIBUTE_DATA data{};
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) {
		return std::nullopt;
	}

	ULARGE_INTEGER write_time{};
	write_time.LowPart = data.ftLastWriteTime.dwLowDateTime;
	write_time.HighPart = data.ftLastWriteTime.dwHighDateTime;

	ULARGE_INTEGER size{};
	size.LowPart = data.nFileSizeLow;
	size.HighPart = data.nFileSizeHigh;

	return FileMetadata{ write_time.QuadPart, size.QuadPart };
}

// 現在のシーン設定とUI設定から描画設定を組み立てる。
ProjectSettings BuildProjectSettings(const FILTER_PROC_VIDEO* video) {
	ProjectSettings settings;
	settings.video_width = video->scene->width;
	settings.video_height = video->scene->height;
	settings.video_rate = video->scene->rate;
	settings.video_scale = video->scene->scale;
	settings.start_frame = std::max(0, static_cast<int>(std::lround(kStartFrame.value)));
	settings.side_margin = static_cast<int>(std::lround(kSideMargin.value));
	settings.bottom_margin = static_cast<int>(std::lround(kBottomMargin.value));
	settings.line_spacing = static_cast<int>(std::lround(kLineSpacing.value));
	settings.ruby_gap = static_cast<int>(std::lround(kRubyGap.value));
	settings.lead_in_ms = static_cast<int>(std::lround(kLeadInMs.value));
	settings.hold_ms = static_cast<int>(std::lround(kHoldMs.value));
	settings.min_gap_ms = static_cast<int>(std::lround(kMinGapMs.value));
	settings.max_visible_lines = std::clamp(static_cast<int>(std::lround(kVisibleLines.value)), 1, 4);

	const auto font_name = GetSelectedFontName();
	const auto font_size = static_cast<float>(std::max(8.0, kFontSize.value));
	const auto ruby_scale = static_cast<float>(std::max(0.2, kRubyScale.value / 100.0));
	const auto letter_spacing = static_cast<float>(kLetterSpacing.value);
	settings.font = FontStyleSettings{ font_name, font_size, letter_spacing, kBold.value, kItalic.value };
	settings.ruby_font = FontStyleSettings{ font_name, std::max(8.0f, font_size * ruby_scale), 0.0f, kBold.value, kItalic.value };
	return settings;
}

// キャッシュ判定用のキーを入力条件から組み立てる。
std::wstring BuildCacheKey(const wchar_t* file_path, const FileMetadata& metadata, const ProjectSettings& settings) {
	std::wostringstream stream;
	stream
		<< file_path << L'|'
		<< metadata.write_time << L'|'
		<< metadata.file_size << L'|'
		<< settings.video_width << L'|'
		<< settings.video_height << L'|'
		<< settings.video_rate << L'|'
		<< settings.video_scale << L'|'
		<< settings.start_frame << L'|'
		<< settings.side_margin << L'|'
		<< settings.bottom_margin << L'|'
		<< settings.line_spacing << L'|'
		<< settings.ruby_gap << L'|'
		<< settings.lead_in_ms << L'|'
		<< settings.hold_ms << L'|'
		<< settings.min_gap_ms << L'|'
		<< settings.max_visible_lines << L'|'
		<< settings.font.name << L'|'
		<< settings.font.size << L'|'
		<< settings.font.letter_spacing << L'|'
		<< settings.font.bold << L'|'
		<< settings.font.italic << L'|'
		<< settings.ruby_font.size;
	return stream.str();
}

// 歌詞ファイルを読み込み、必要なら解析済みレイアウトを再構築する。
std::shared_ptr<CachedLayout> GetOrBuildLayout(const FILTER_PROC_VIDEO* video) {
	const auto file_path = kLyricsFile.value;
	if (!file_path || !file_path[0]) {
		return nullptr;
	}

	const auto metadata = GetFileMetadata(file_path);
	if (!metadata.has_value()) {
		LogWarn(L"Failed to read lyrics file metadata.");
		return nullptr;
	}
	if (metadata->file_size > kMaxLyricsFileSizeBytes) {
		LogWarn(L"Lyrics file is too large.");
		return nullptr;
	}

	const auto settings = BuildProjectSettings(video);
	const auto cache_key = BuildCacheKey(file_path, metadata.value(), settings);
	{
		// 入力ファイルと描画条件が同じ間は前回結果を再利用する。
		const std::scoped_lock lock(g_cache_mutex);
		const auto found = g_layout_cache.find(cache_key);
		if (found != g_layout_cache.end()) {
			return found->second;
		}
	}

	auto cache_entry = std::make_shared<CachedLayout>();
	try {
		const auto content = ReadLyricsFile(file_path);
		if (!content.has_value()) {
			LogWarn(L"Failed to read lyrics file.");
			return nullptr;
		}

		const auto document = ParseTimedLyricsDocument(content.value());
		cache_entry->layout = BuildLayout(document, settings);
	}
	catch (const std::exception&) {
		LogError(L"Failed to parse or layout lyrics.");
		return nullptr;
	}

	const std::scoped_lock lock(g_cache_mutex);
	g_layout_cache[cache_key] = cache_entry;
	return cache_entry;
}

// AviUtl2の色設定をGDI+の色へ変換する。
Color ToGdiColor(const FILTER_ITEM_COLOR& item) {
	return Color(255, item.value.r, item.value.g, item.value.b);
}

// フォントサイズと設定から縁取り幅を決める。
float ComputeOutlineWidth(const FontStyleSettings& font) {
	const auto base_width = std::max(2.0f, font.size / 14.0f);
	switch (kOutlineMode.value) {
	case 0:
		return 0.0f;
	case 2:
		return base_width * 1.75f;
	case 3:
		return base_width * 2.5f;
	default:
		return base_width;
	}
}

// フォント設定をGDI+のスタイルフラグへ変換する。
INT ToFontStyle(const FontStyleSettings& font) {
	INT style = FontStyleRegular;
	if (font.bold) style |= FontStyleBold;
	if (font.italic) style |= FontStyleItalic;
	return style;
}

// 指定フォントが使えなければ代替フォントを返す。
std::unique_ptr<FontFamily> CreateFontFamily(const wchar_t* preferred) {
	auto family = std::make_unique<FontFamily>(preferred);
	if (family->GetLastStatus() == Ok) {
		return family;
	}

	return std::make_unique<FontFamily>(L"MS UI Gothic");
}

// Graphicsの状態をスコープ単位で退避・復元する。
struct GraphicsStateScope {
	explicit GraphicsStateScope(Graphics* graphics) : graphics_(graphics), state_(graphics->Save()) {}
	~GraphicsStateScope() { graphics_->Restore(state_); }

	Graphics* graphics_;
	GraphicsState state_;
};

struct TextClipRegion {
	RectF rect{};
	float anchor_y = 0.0f;
	float text_height = 0.0f;
};

// GDI+/GDI の斜体は概ね右上がりの shear とみなし、クリップ境界も同じ向きへ傾ける。
void ApplyTextClip(Graphics* graphics, const TextClipRegion& clip, const FontStyleSettings& font) {
	if (!font.italic) {
		graphics->SetClip(clip.rect, CombineModeIntersect);
		return;
	}

	const auto top = clip.rect.Y;
	const auto bottom = clip.rect.Y + clip.rect.Height;
	const auto left = clip.rect.X;
	const auto right = clip.rect.X + clip.rect.Width;
	const auto anchor_bottom = clip.anchor_y + clip.text_height;
	const auto top_offset = (anchor_bottom - top) * kItalicShear;
	const auto bottom_offset = (anchor_bottom - bottom) * kItalicShear;

	PointF points[4] = {
		PointF(left + top_offset, top),
		PointF(right + top_offset, top),
		PointF(right + bottom_offset, bottom),
		PointF(left + bottom_offset, bottom),
	};

	GraphicsPath clip_path;
	clip_path.AddPolygon(points, 4);
	graphics->SetClip(&clip_path, CombineModeIntersect);
}

// 文字列を文字要素単位でパス化して描画する。
void DrawTextPath(
	Graphics* graphics,
	TextMeasurer* text_measurer,
	const std::wstring& text,
	const FontStyleSettings& font,
	float x,
	float y,
	const Color& fill,
	const Color& edge,
	std::optional<TextClipRegion> clip_region,
	const std::vector<TextElementBounds>* element_bounds = nullptr) {
	if (text.empty()) {
		return;
	}

	GraphicsStateScope scope(graphics);
	if (clip_region.has_value()) {
		ApplyTextClip(graphics, clip_region.value(), font);
	}

	StringFormat format(StringFormat::GenericTypographic());
	format.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip | StringFormatFlagsMeasureTrailingSpaces);

	const auto family = CreateFontFamily(font.name.c_str());
	const auto outline_width = ComputeOutlineWidth(font);
	Pen edge_pen(edge, outline_width);
	edge_pen.SetLineJoin(LineJoinRound);
	SolidBrush fill_brush(fill);

	// 文字間隔とワイプ位置を揃えるため、表示単位ごとに個別描画する。
	const auto elements = SplitTextElements(text);
	const auto fallback_bounds = element_bounds ? std::vector<TextElementBounds>{} : BuildTextElementBounds(text, font, text_measurer);
	for (size_t index = 0; index < elements.size(); ++index) {
		const auto cursor_x = x + (element_bounds && index < element_bounds->size()
			? (*element_bounds)[index].start
			: fallback_bounds[index].start);
		GraphicsPath path;
		path.AddString(
			elements[index].c_str(),
			static_cast<INT>(elements[index].size()),
			family.get(),
			ToFontStyle(font),
			font.size,
			PointF(cursor_x, y),
			&format);

		if (outline_width > 0.0f) {
			graphics->DrawPath(&edge_pen, &path);
		}
		graphics->FillPath(&fill_brush, &path);
	}
}

// 現在フレームに対応する歌詞行のワイプ進行幅を返す。
int ComputeActiveClipWidth(const LayoutLine& line, int current_frame) {
	if (line.syllables.empty()) {
		return 0;
	}

	if (current_frame < line.syllables.front().start_frame) {
		return 0;
	}

	// 現在進行中の音節内を線形補間してワイプ位置を求める。
	for (size_t index = 0; index < line.syllables.size(); ++index) {
		const auto& syllable = line.syllables[index];
		const auto segment_start = syllable.start_frame;
		const auto segment_end = index + 1 < line.syllables.size()
			? line.syllables[index + 1].start_frame
			: line.lyric_end_frame;

		if (current_frame >= segment_end) {
			continue;
		}

		if (segment_end <= segment_start) {
			return syllable.clip_end;
		}

		const auto ratio = static_cast<double>(current_frame - segment_start) / static_cast<double>(segment_end - segment_start);
		return static_cast<int>(std::llround(syllable.clip_start + ((syllable.clip_end - syllable.clip_start) * ratio)));
	}

	return line.syllables.back().clip_end;
}

// 本文の進行幅から対応するルビの進行幅を求める。
float ComputeRubyActiveWidth(const LayoutRubySegment& segment, int current_frame, int line_clip_width) {
	if (!segment.timing_segments.empty()) {
		if (current_frame <= segment.timing_segments.front().start_frame) {
			return 0.0f;
		}

		for (const auto& timing : segment.timing_segments) {
			if (current_frame >= timing.end_frame) {
				continue;
			}

			if (current_frame <= timing.start_frame) {
				return static_cast<float>(timing.clip_start);
			}

			const auto ratio = static_cast<double>(current_frame - timing.start_frame) / static_cast<double>(timing.end_frame - timing.start_frame);
			return static_cast<float>(timing.clip_start + ((timing.clip_end - timing.clip_start) * ratio));
		}

		return static_cast<float>(segment.timing_segments.back().clip_end);
	}

	const auto base_width = std::max(0.0f, segment.base_end - segment.base_start);
	if (base_width <= 0.0f) {
		return 0.0f;
	}

	if (segment.width <= 0.0f) {
		return 0.0f;
	}

	const auto consumed_base = std::clamp(static_cast<float>(line_clip_width) - segment.base_start, 0.0f, base_width);
	const auto ratio = consumed_base / base_width;
	return segment.width * ratio;
}

// GDI+で描いたBGRAバッファをAviUtl2用RGBAへ詰め替える。
void ConvertBgraToRgba(const std::vector<std::uint8_t>& bgra, std::vector<PIXEL_RGBA>* rgba) {
	const auto pixel_count = bgra.size() / 4;
	rgba->resize(pixel_count);
	for (size_t index = 0; index < pixel_count; ++index) {
		(*rgba)[index].r = bgra[(index * 4) + 2];
		(*rgba)[index].g = bgra[(index * 4) + 1];
		(*rgba)[index].b = bgra[index * 4];
		(*rgba)[index].a = bgra[(index * 4) + 3];
	}
}

FrameScratchBuffers& GetFrameScratchBuffers() {
	thread_local FrameScratchBuffers scratch;
	return scratch;
}

// 指定フレームに表示すべき歌詞を1枚の画像として描画する。
bool RenderLyricsFrame(const FILTER_PROC_VIDEO* video, const CachedLayout& cache, FrameScratchBuffers* scratch) {
	const auto width = video->scene->width;
	const auto height = video->scene->height;
	if (width <= 0 || height <= 0) {
		return false;
	}

	const auto pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
	scratch->bgra.assign(pixel_count * 4, 0);
	Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB, scratch->bgra.data());
	Graphics graphics(&bitmap);
	graphics.SetSmoothingMode(SmoothingModeAntiAlias);
	graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	graphics.Clear(Color(0, 0, 0, 0));
	TextMeasurer text_measurer;

	const auto settings = BuildProjectSettings(video);
	const auto current_frame = video->object->frame;
	const auto before_fill = ToGdiColor(kBeforeTextColor);
	const auto before_edge = ToGdiColor(kBeforeEdgeColor);
	const auto after_fill = ToGdiColor(kAfterTextColor);
	const auto after_edge = ToGdiColor(kAfterEdgeColor);

	// 未進行色を先に描き、その上から進行済み部分だけをクリップして重ねる。
	for (const auto& line : cache.layout.lines) {
		if (current_frame < line.display_start_frame || current_frame >= line.display_end_frame) {
			continue;
		}

		const auto draw_x = (width / 2.0f) + line.x;
		const auto draw_y = (height / 2.0f) + line.y;
		const auto clip_width = ComputeActiveClipWidth(line, current_frame);
		const auto padding = ComputeOutlineWidth(settings.font) * 0.5f;
		const auto clip_x = draw_x + clip_width;

		if (clip_x < width + padding) {
			DrawTextPath(
				&graphics,
				&text_measurer,
				line.source.text,
				settings.font,
				draw_x,
				draw_y,
				before_fill,
				before_edge,
				TextClipRegion{
					RectF(clip_x - padding, 0.0f, static_cast<float>(width) - clip_x + padding * 2.0f, static_cast<float>(height)),
					draw_y,
					settings.font.size,
				},
				&line.text_element_bounds);
		}

		if (clip_width > 0) {
			DrawTextPath(
				&graphics,
				&text_measurer,
				line.source.text,
				settings.font,
				draw_x,
				draw_y,
				after_fill,
				after_edge,
				TextClipRegion{
					RectF(0.0f, 0.0f, clip_x + padding, static_cast<float>(height)),
					draw_y,
					settings.font.size,
				},
				&line.text_element_bounds);
		}

		if (kShowRuby.value && line.ruby.has_value()) {
			const auto ruby_y = (height / 2.0f) + line.ruby->y;
			const auto ruby_padding = ComputeOutlineWidth(settings.ruby_font) * 0.5f;
			for (const auto& segment : line.ruby->segments) {
				const auto ruby_x = draw_x + segment.offset_x;
				const auto ruby_clip_width = ComputeRubyActiveWidth(segment, current_frame, clip_width);
				const auto ruby_clip_x = ruby_x + ruby_clip_width;

				if (ruby_clip_x < width + ruby_padding) {
					DrawTextPath(
						&graphics,
						&text_measurer,
						segment.text,
						settings.ruby_font,
						ruby_x,
						ruby_y,
						before_fill,
						before_edge,
						TextClipRegion{
							RectF(ruby_clip_x - ruby_padding, 0.0f, static_cast<float>(width) - ruby_clip_x + ruby_padding * 2.0f, static_cast<float>(height)),
							ruby_y,
							settings.ruby_font.size,
						});
				}

				if (ruby_clip_width > 0.0f) {
					DrawTextPath(
						&graphics,
						&text_measurer,
						segment.text,
						settings.ruby_font,
						ruby_x,
						ruby_y,
						after_fill,
						after_edge,
						TextClipRegion{
							RectF(0.0f, 0.0f, ruby_clip_x + ruby_padding, static_cast<float>(height)),
							ruby_y,
							settings.ruby_font.size,
						});
				}
			}
		}
	}

	ConvertBgraToRgba(scratch->bgra, &scratch->rgba);
	return true;
}

// AviUtl2から毎フレーム呼ばれる描画入口。
bool func_proc_video(FILTER_PROC_VIDEO* video) {
	const auto width = video->scene->width;
	const auto height = video->scene->height;
	auto& scratch = GetFrameScratchBuffers();

	const auto layout = GetOrBuildLayout(video);
	if (!layout) {
		// 読み込み失敗時は透過画像を返して表示だけ止める。
		scratch.empty.assign(static_cast<size_t>(width) * static_cast<size_t>(height), PIXEL_RGBA{});
		video->set_image_data(scratch.empty.data(), width, height);
		return true;
	}

	if (!RenderLyricsFrame(video, *layout, &scratch)) {
		return false;
	}

	video->set_image_data(scratch.rgba.data(), width, height);
	return true;
}

} // namespace

// プラグインが要求するAviUtl2本体バージョンを返す。
EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
	return 2003300;
}

// ロガーを受け取って後続のログ出力に使う。
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* logger) {
	g_logger = logger;
}

// プラグイン初期化時にGDI+とフォント一覧を準備する。
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) {
	(void)version;
	GdiplusStartupInput startup_input;
	if (GdiplusStartup(&g_gdiplus_token, &startup_input, nullptr) != Ok) {
		return false;
	}

	InitializeFontItems();
	return true;
}

// プラグイン終了時にGDI+を解放する。
EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	if (g_gdiplus_token != 0) {
		GdiplusShutdown(g_gdiplus_token);
		g_gdiplus_token = 0;
	}
}

// AviUtl2へフィルタ定義テーブルを返す。
EXTERN_C __declspec(dllexport) FILTER_PLUGIN_TABLE* GetFilterPluginTable(void) {
	kFilterPluginTable.func_proc_video = func_proc_video;
	return &kFilterPluginTable;
}
