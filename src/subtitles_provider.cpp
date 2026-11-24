// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "include/aegisub/subtitles_provider.h"

#include "ass_dialogue.h"
#include "ass_attachment.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "factory_manager.h"
#include "options.h"
#include "subtitles_provider_csri.h"
#include "subtitles_provider_libass.h"

#include <regex>

namespace {
	struct factory {
		std::string name;
		std::string subtype;
		std::unique_ptr<SubtitlesProvider> (*create)(std::string const& subtype, agi::BackgroundRunner *br);
		bool hidden;
	};

	std::vector<factory> const& factories() {
		static std::vector<factory> factories;
		if (factories.size()) return factories;
#ifdef WITH_CSRI
		for (auto const& subtype : csri::List())
			factories.push_back(factory{"CSRI/" + subtype, subtype, csri::Create, false});
#endif
		factories.push_back(factory{"libass", "", libass::Create, false});
		return factories;
	}

	// 將帶有 \pos(x,y) 的簡體預覽行 Y 座標往下偏移，避免與繁體重疊
	std::string offset_pos_y(std::string const& text, int dy) {
		static const std::regex pos_rx(R"(\\pos\(\s*([+-]?\d+(?:\.\d+)?)\s*,\s*([+-]?\d+(?:\.\d+)?)\s*\))");
		std::smatch m;
		if (std::regex_search(text, m, pos_rx)) {
			double y = std::stod(m[2].str()) + dy;
			std::string repl = "\\pos(" + m[1].str() + "," + std::to_string(static_cast<int>(y)) + ")";
			return std::regex_replace(text, pos_rx, repl, std::regex_constants::format_first_only);
		}
		return text;
	}
}

std::vector<std::string> SubtitlesProviderFactory::GetClasses() {
	return ::GetClasses(factories());
}

std::unique_ptr<SubtitlesProvider> SubtitlesProviderFactory::GetProvider(agi::BackgroundRunner *br) {
	auto preferred = OPT_GET("Subtitle/Provider")->GetString();
	auto sorted = GetSorted(factories(), preferred);

	std::string error;
	for (auto factory : sorted) {
		try {
			auto provider = factory->create(factory->subtype, br);
			if (provider) return provider;
		}
		catch (agi::UserCancelException const&) { throw; }
		catch (agi::Exception const& err) { error += factory->name + ": " + err.GetMessage() + "\n"; }
		catch (...) { error += factory->name + ": Unknown error\n"; }
	}

	throw error;
}

void SubtitlesProvider::LoadSubtitles(AssFile *subs, int time) {
	buffer.clear();

	auto push_header = [&](const char *str) {
		buffer.insert(buffer.end(), str, str + strlen(str));
	};
	auto push_line = [&](std::string const& str) {
		buffer.insert(buffer.end(), &str[0], &str[0] + str.size());
		buffer.push_back('\n');
	};

	push_header("\xEF\xBB\xBF[Script Info]\n");
	for (auto const& line : subs->Info)
		push_line(line.GetEntryData());

	push_header("[V4+ Styles]\n");
	for (auto const& line : subs->Styles)
		push_line(line.GetEntryData());

	if (!subs->Attachments.empty()) {
		push_header("[Fonts]\n");
		for (auto const& attachment : subs->Attachments)
			if (attachment.Group() == AssEntryGroup::FONT)
				push_line(attachment.GetEntryData());
	}

	push_header("[Events]\n");
	for (auto const& line : subs->Events) {
		if (!line.Comment && (time < 0 || !(line.Start > time || line.End <= time))) {
			std::string entry = line.GetEntryData();
			size_t sep = entry.find("|||");
			if (sep == std::string::npos) {
				push_line(entry);
			}
			else {
				// 找到 Dialogue 前 9 個逗號（Text 欄位起點）
				size_t pos = 0;
				for (int i = 0; i < 9 && pos != std::string::npos; ++i) {
					pos = entry.find(',', pos);
					if (pos != std::string::npos) ++pos;
				}

				if (pos != std::string::npos && sep >= pos) {
					std::string prefix = entry.substr(0, pos);
					std::string primary = entry.substr(pos, sep - pos);
					std::string secondary = entry.substr(sep + 3);

					// 若包含卡拉OK標籤 (\k / \K / \kf / \ko) 或固定座標 (\pos)，拆成兩條獨立事件讓時間軸與位置各自獨立計算
					bool has_karaoke = (primary.find("\\k") != std::string::npos || primary.find("\\K") != std::string::npos);
					bool has_pos = (primary.find("\\pos(") != std::string::npos);

					if (has_karaoke || has_pos) {
						push_line(prefix + primary);
						std::string sec_line = has_pos ? offset_pos_y(secondary, 55) : ("{\\an8}" + secondary);
						push_line(prefix + sec_line);
					}
					else {
						// 一般對白行直接以 \N 換行呈現上下雙語
						push_line(prefix + primary + "\\N" + secondary);
					}
				}
				else {
					push_line(entry);
				}
			}
		}
	}

	LoadSubtitles(&buffer[0], buffer.size());
}
