#include "KaraokeLyricCommon.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>

namespace {

inline constexpr uint64_t kMaxLyricsFileSizeBytes = 8ull * 1024ull * 1024ull;

struct ParsedLineTransform {
	std::wstring visible_line;
	std::vector<RubySpan> ruby_spans;
};

// 指定コードページのバイト列をワイド文字列へ変換する。
std::optional<std::wstring> DecodeToWide(const std::string& text, UINT code_page, DWORD flags) {
	if (text.empty()) {
		return std::wstring{};
	}

	const auto length = MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (length <= 0) {
		return std::nullopt;
	}

	std::wstring result(length, L'\0');
	if (MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), result.data(), length) <= 0) {
		return std::nullopt;
	}
	return result;
}

// ASCII 数字かどうかを判定する。
bool IsDigit(wchar_t ch) {
	return ch >= L'0' && ch <= L'9';
}

// [mm:ss:cc] 形式のタイムタグを読む。
bool TryReadTimeTag(const std::wstring& text, size_t index, int* milliseconds, size_t* next_index) {
	if (index + 10 > text.size()) {
		return false;
	}

	if (text[index] != L'[' || text[index + 3] != L':' || text[index + 6] != L':' || text[index + 9] != L']') {
		return false;
	}

	for (size_t i = 1; i < 9; ++i) {
		if (i == 3 || i == 6) {
			continue;
		}

		if (!IsDigit(text[index + i])) {
			return false;
		}
	}

	// 固定フォーマットの [mm:ss:cc] をミリ秒へ変換する。
	const int minutes = ((text[index + 1] - L'0') * 10) + (text[index + 2] - L'0');
	const int seconds = ((text[index + 4] - L'0') * 10) + (text[index + 5] - L'0');
	const int centiseconds = ((text[index + 7] - L'0') * 10) + (text[index + 8] - L'0');

	if (milliseconds) {
		*milliseconds = (((minutes * 60) + seconds) * 1000) + (centiseconds * 10);
	}
	if (next_index) {
		*next_index = index + 10;
	}
	return true;
}

// 指定位置以降で最初に見つかるタイムタグ位置を返す。
size_t FindNextTimeTag(const std::wstring& text, size_t start_index) {
	for (size_t index = start_index; index + 10 <= text.size(); ++index) {
		if (TryReadTimeTag(text, index, nullptr, nullptr)) {
			return index;
		}
	}

	return std::wstring::npos;
}

// 文字列からタイムタグを除去して表示用テキストを作る。
std::wstring StripTimeTags(const std::wstring& text) {
	std::wstring result;
	result.reserve(text.size());

	size_t index = 0;
	while (index < text.size()) {
		size_t next_index = 0;
		if (TryReadTimeTag(text, index, nullptr, &next_index)) {
			index = next_index;
			continue;
		}

		result.push_back(text[index]);
		index++;
	}

	return result;
}

// text element 単位の範囲から親文字列を取り出す。
std::wstring GetBaseTextByTextElementRange(const std::wstring& visible_text, int start_index, int base_length) {
	const auto elements = SplitTextElements(visible_text);
	size_t safe_start = 0;
	size_t safe_end = 0;
	if (!TryGetClampedTextElementRange(start_index, base_length, elements.size(), &safe_start, &safe_end)) {
		return {};
	}

	std::wstring result;
	for (size_t index = safe_start; index <= safe_end; ++index) {
		result += elements[index];
	}

	return result;
}

// 指定したリテラルを消費し、カーソルを進める。
bool ConsumeLiteral(const std::wstring& text, size_t* cursor, const wchar_t* literal) {
	const auto literal_length = wcslen(literal);
	if (*cursor + literal_length > text.size()) {
		return false;
	}

	if (text.compare(*cursor, literal_length, literal) != 0) {
		return false;
	}

	*cursor += literal_length;
	return true;
}

// 空白を読み飛ばす。
void SkipWhitespace(const std::wstring& text, size_t* cursor) {
	while (*cursor < text.size() && iswspace(text[*cursor])) {
		(*cursor)++;
	}
}

// 10 進整数を読む。
bool TryParseInteger(const std::wstring& text, size_t* cursor, int* value) {
	const auto start = *cursor;
	int parsed_value = 0;
	while (*cursor < text.size() && IsDigit(text[*cursor])) {
		const auto digit = text[*cursor] - L'0';
		if (parsed_value > ((std::numeric_limits<int>::max() - digit) / 10)) {
			return false;
		}

		parsed_value = (parsed_value * 10) + digit;
		(*cursor)++;
	}

	if (start == *cursor) {
		return false;
	}

	*value = parsed_value;
	return true;
}

// 改行コードを 1 パスで LF に正規化する。
std::wstring NormalizeLineEndings(const std::wstring& text) {
	std::wstring normalized;
	normalized.reserve(text.size());

	for (size_t index = 0; index < text.size(); ++index) {
		if (text[index] == L'\r') {
			normalized.push_back(L'\n');
			if (index + 1 < text.size() && text[index + 1] == L'\n') {
				index++;
			}
			continue;
		}

		normalized.push_back(text[index]);
	}

	return normalized;
}

// エスケープ付きの引用文字列を読む。
bool TryParseQuotedString(const std::wstring& text, size_t* cursor, std::wstring* value) {
	if (*cursor >= text.size() || text[*cursor] != L'"') {
		return false;
	}

	(*cursor)++;
	std::wstring builder;
	while (*cursor < text.size()) {
		const auto current = text[(*cursor)++];
		if (current == L'\\') {
			if (*cursor >= text.size()) {
				return false;
			}

			const auto escaped = text[(*cursor)++];
			switch (escaped) {
			case L'\\': builder.push_back(L'\\'); break;
			case L'"': builder.push_back(L'"'); break;
			case L'n': builder.push_back(L'\n'); break;
			case L'r': builder.push_back(L'\r'); break;
			case L't': builder.push_back(L'\t'); break;
			default: builder.push_back(escaped); break;
			}
			continue;
		}

		if (current == L'"') {
			*value = builder;
			return true;
		}

		builder.push_back(current);
	}

	return false;
}

// \ruby(...) 形式のルビ指定を 1 件読む。
bool TryParseRubyTag(const std::wstring& block, size_t* cursor, RubySpan* span) {
	if (*cursor >= block.size() || block[*cursor] != L'\\') {
		return false;
	}

	(*cursor)++;
	int start_index = 0;
	int base_length = 0;
	std::wstring ruby_text;

	if (!ConsumeLiteral(block, cursor, L"ruby(")) return false;
	if (!TryParseInteger(block, cursor, &start_index)) return false;
	if (!ConsumeLiteral(block, cursor, L",")) return false;
	if (!TryParseInteger(block, cursor, &base_length)) return false;
	if (!ConsumeLiteral(block, cursor, L",")) return false;
	if (!TryParseQuotedString(block, cursor, &ruby_text)) return false;
	if (!ConsumeLiteral(block, cursor, L")")) return false;

	*span = RubySpan{ start_index, base_length, L"", ruby_text };
	return true;
}

// オーバーライドブロック全体からルビ指定を抽出する。
bool TryParseOverrideBlock(const std::wstring& block, std::vector<RubySpan>* spans) {
	size_t cursor = 0;
	bool parsed_any = false;
	while (cursor < block.size()) {
		SkipWhitespace(block, &cursor);
		if (cursor >= block.size()) {
			break;
		}

		RubySpan span;
		if (!TryParseRubyTag(block, &cursor, &span)) {
			return false;
		}

		parsed_any = true;
		spans->push_back(span);
	}

	return parsed_any;
}

// ASS のオーバーライドブロックを除去しつつルビ指定を抽出する。
std::vector<RubySpan> ExtractOverrideRubySpans(const std::wstring& line, std::wstring* stripped_line) {
	std::vector<RubySpan> spans;
	std::wstring builder;
	size_t index = 0;

	// 表示には不要なブロックを除き、ルビ指定だけを拾う。
	while (index < line.size()) {
		if (line[index] == L'{') {
			const auto close_index = line.find(L'}', index + 1);
			if (close_index != std::wstring::npos && close_index > index + 1) {
				const auto block = line.substr(index + 1, close_index - index - 1);
				if (TryParseOverrideBlock(block, &spans)) {
					index = close_index + 1;
					continue;
				}
			}
		}

		builder.push_back(line[index]);
		index++;
	}

	*stripped_line = builder;
	return spans;
}

// ASS ルビカラオケの重複タイムタグ構文を 1 箇所だけ解釈する。
bool TryMatchRuby(
	const std::wstring& line,
	size_t index,
	size_t* consumed_length,
	std::wstring* base_text,
	std::wstring* ruby_text,
	std::wstring* replacement) {
	// 重複する開始タグから、本体 1 回・ルビ 1 回のペアを読む。
	int start_time = 0;
	size_t cursor = 0;
	if (!TryReadTimeTag(line, index, &start_time, &cursor)) {
		return false;
	}

	const auto start_tag = line.substr(index, cursor - index);
	const auto base_start = cursor;
	const auto base_end = FindNextTimeTag(line, cursor);
	if (base_end == std::wstring::npos || base_end <= base_start) {
		return false;
	}

	*base_text = line.substr(base_start, base_end - base_start);
	if (base_text->empty()) {
		return false;
	}

	int repeated_start = 0;
	size_t repeated_cursor = 0;
	if (!TryReadTimeTag(line, base_end, &repeated_start, &repeated_cursor) || repeated_start != start_time) {
		return false;
	}

	if (repeated_cursor >= line.size() || line[repeated_cursor] != L'(') {
		return false;
	}

	cursor = repeated_cursor + 1;
	int ruby_start_time = 0;
	size_t ruby_cursor = 0;
	if (!TryReadTimeTag(line, cursor, &ruby_start_time, &ruby_cursor) || ruby_start_time != start_time) {
		return false;
	}

	const auto ruby_start = ruby_cursor;
	const auto ruby_end = FindNextTimeTag(line, ruby_cursor);
	if (ruby_end == std::wstring::npos || ruby_end <= ruby_start) {
		return false;
	}

	*ruby_text = line.substr(ruby_start, ruby_end - ruby_start);
	if (ruby_text->empty()) {
		return false;
	}

	int end_time = 0;
	size_t end_cursor = 0;
	if (!TryReadTimeTag(line, ruby_end, &end_time, &end_cursor)) {
		return false;
	}

	if (end_cursor >= line.size() || line[end_cursor] != L')') {
		return false;
	}

	cursor = end_cursor + 1;
	int trailing_end_time = 0;
	if (TryReadTimeTag(line, cursor, &trailing_end_time, nullptr) && trailing_end_time != end_time) {
		return false;
	}

	if (FindNextTimeTag(*base_text, 0) != std::wstring::npos) {
		return false;
	}

	wchar_t end_tag[16] = {};
	_snwprintf_s(end_tag, _countof(end_tag), _TRUNCATE, L"[%02d:%02d:%02d]",
		(end_time / 60000) % 100,
		(end_time / 1000) % 60,
		(end_time / 10) % 100);

	*replacement = start_tag + *base_text + end_tag;
	*consumed_length = cursor - index;
	return true;
}

// RhythmicaLyrics 行はそのまま扱う。
ParsedLineTransform TransformRhythmicaLine(const std::wstring& line) {
	return ParsedLineTransform{ line, {} };
}

// ASS ルビカラオケ行を表示テキストとルビ情報へ変換する。
ParsedLineTransform TransformAssRubyLine(const std::wstring& line) {
	std::wstring source_line;
	auto override_spans = ExtractOverrideRubySpans(line, &source_line);
	std::wstring visible;
	std::vector<RubySpan> spans;
	size_t index = 0;

	// まず本文埋め込みルビを拾い、その後で明示 override を優先適用する。
	while (index < source_line.size()) {
		size_t consumed_length = 0;
		std::wstring base_text;
		std::wstring ruby_text;
		std::wstring replacement;
		if (TryMatchRuby(source_line, index, &consumed_length, &base_text, &ruby_text, &replacement)) {
			spans.push_back(RubySpan{
				CountTextElements(StripTimeTags(visible)),
				CountTextElements(base_text),
				base_text,
				ruby_text });
			visible += replacement;
			index += consumed_length;
			continue;
		}

		visible.push_back(source_line[index]);
		index++;
	}

	const auto final_visible_text = StripTimeTags(visible);
	if (!override_spans.empty()) {
		std::map<std::pair<int, int>, RubySpan> exact_overrides;
		for (auto span : override_spans) {
			span.base_text = GetBaseTextByTextElementRange(final_visible_text, span.start_index, span.base_length);
			exact_overrides[{ span.start_index, span.base_length }] = span;
		}

		std::vector<RubySpan> filtered;
		for (const auto& span : spans) {
			if (exact_overrides.find({ span.start_index, span.base_length }) == exact_overrides.end()) {
				filtered.push_back(span);
			}
		}
		for (const auto& pair : exact_overrides) {
			filtered.push_back(pair.second);
		}

		std::sort(filtered.begin(), filtered.end(), [](const RubySpan& a, const RubySpan& b) {
			return a.start_index < b.start_index;
		});
		spans = std::move(filtered);
	}

	return ParsedLineTransform{ visible, spans };
}

} // namespace

// 歌詞ファイルを読み込み、UTF-8 / CP932 を自動判定してワイド文字列へ変換する。
std::optional<std::wstring> ReadLyricsFile(const wchar_t* path) {
	if (!path || !path[0]) {
		return std::nullopt;
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		return std::nullopt;
	}

	stream.seekg(0, std::ios::end);
	const auto file_size = stream.tellg();
	if (file_size < 0 || static_cast<uint64_t>(file_size) > kMaxLyricsFileSizeBytes) {
		return std::nullopt;
	}
	stream.seekg(0, std::ios::beg);

	std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
	if (bytes.size() >= 3 &&
		static_cast<unsigned char>(bytes[0]) == 0xEF &&
		static_cast<unsigned char>(bytes[1]) == 0xBB &&
		static_cast<unsigned char>(bytes[2]) == 0xBF) {
		bytes.erase(0, 3);
	}

	if (const auto utf8_text = DecodeToWide(bytes, CP_UTF8, MB_ERR_INVALID_CHARS)) {
		return utf8_text;
	}

	return DecodeToWide(bytes, 932, 0);
}

// タイムタグ付き歌詞全体を行単位・音節単位に分解する。
LyricsDocument ParseTimedLyricsDocument(const std::wstring& text) {
	LyricsDocument document;
	const auto normalized = NormalizeLineEndings(text);

	size_t line_start = 0;
	while (line_start <= normalized.size()) {
		const auto line_end = normalized.find(L'\n', line_start);
		const auto raw_line = normalized.substr(line_start, line_end == std::wstring::npos ? std::wstring::npos : line_end - line_start);
		line_start = (line_end == std::wstring::npos) ? normalized.size() + 1 : line_end + 1;

		bool has_non_space = false;
		for (const auto ch : raw_line) {
			if (!iswspace(ch)) {
				has_non_space = true;
				break;
			}
		}
		if (!has_non_space) {
			continue;
		}

		// ASSルビカラオケ形式の変換で RhythmicaLyrics も処理する。
		const auto transformed = TransformAssRubyLine(raw_line);
		std::vector<size_t> tag_indices;
		std::vector<int> tag_times;
		for (size_t index = 0; index + 10 <= transformed.visible_line.size(); ++index) {
			int milliseconds = 0;
			if (TryReadTimeTag(transformed.visible_line, index, &milliseconds, nullptr)) {
				tag_indices.push_back(index);
				tag_times.push_back(milliseconds);
				index += 9;
			}
		}

		if (tag_indices.size() < 2) {
			continue;
		}

		// 隣接するタイムタグ間を 1 音節として扱い、ワイプ区間を作る。
		std::vector<LyricSyllable> syllables;
		std::wstring pending_text;
		for (size_t i = 0; i + 1 < tag_indices.size(); ++i) {
			const auto text_start = tag_indices[i] + 10;
			const auto text_length = tag_indices[i + 1] > text_start ? (tag_indices[i + 1] - text_start) : 0;
			if (text_length == 0) {
				continue;
			}

			auto segment_text = transformed.visible_line.substr(text_start, text_length);
			const auto start_time_ms = tag_times[i];
			const auto end_time_ms = tag_times[i + 1];
			if (end_time_ms <= start_time_ms) {
				pending_text += segment_text;
				continue;
			}

			segment_text = pending_text + segment_text;
			pending_text.clear();
			syllables.push_back(LyricSyllable{ start_time_ms, end_time_ms, segment_text });
		}

		const auto visible_text = StripTimeTags(transformed.visible_line);
		if (syllables.empty() || visible_text.empty()) {
			continue;
		}

		document.lines.push_back(LyricLine{
			syllables.front().start_time_ms,
			syllables.back().end_time_ms,
			visible_text,
			syllables,
			transformed.ruby_spans,
			raw_line });
	}

	return document;
}
