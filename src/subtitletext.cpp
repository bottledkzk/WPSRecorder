#include "subtitletext.h"

#include "editproject.h"
#include "uilanguage.h"

#include <QStringList>
#include <algorithm>

namespace
{
bool AsciiWord(QChar ch)
{
	const auto c = ch.unicode();
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
}

bool ParsePronunciations(const QString& text, KRRPronunciations& result, QString& error)
{
	KRRPronunciations parsed;
	if (text.size() > 6000) { error = Ui("读音词表最多 6000 个字符。"); return false; }
	QStringList terms;
	const auto lines = text.split('\n');
	for (int index = 0; index < lines.size(); ++index)
	{
		const auto line = lines[index].trimmed();
		if (line.isEmpty()) continue;
		const auto separator = line.indexOf("=>");
		const auto term = separator < 0 ? QString() : line.left(separator).trimmed();
		const auto reading = separator < 0 ? QString() : line.mid(separator + 2).trimmed();
		const bool control = std::any_of(line.begin(), line.end(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; });
		if (term.isEmpty() || term.size() > 80 || reading.isEmpty() || reading.size() > 120 ||
			reading.contains("=>") || control || parsed.readings.contains(term.toCaseFolded()))
		{
			error = Ui("第 %1 行需要唯一的「术语 => 读音」。").arg(index + 1);
			return false;
		}
		terms.append(term);
		parsed.readings.insert(term.toCaseFolded(), reading);
	}
	if (terms.size() > 40) { error = Ui("一个工程最多设 40 条读音规则。"); return false; }
	std::stable_sort(terms.begin(), terms.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
	for (auto& term : terms)
	{
		const bool left = AsciiWord(term.front()), right = AsciiWord(term.back());
		term = (left ? QString("(?<![A-Za-z0-9_])") : QString()) + QRegularExpression::escape(term) +
			(right ? QString("(?![A-Za-z0-9_])") : QString());
	}
	parsed.pattern = QRegularExpression(terms.join('|'), QRegularExpression::CaseInsensitiveOption);
	result = parsed;
	return true;
}

QString KRRPronunciations::apply(const QString& text) const
{
	if (readings.isEmpty() || text.isEmpty()) return text;
	auto matches = pattern.globalMatch(text);
	QString result;
	int copied = 0;
	while (matches.hasNext())
	{
		const auto match = matches.next();
		result += text.mid(copied, match.capturedStart() - copied);
		result += readings.value(match.captured().toCaseFolded(), match.captured());
		copied = match.capturedEnd();
	}
	return copied ? result + text.mid(copied) : text;
}

QString SubtitleSpokenText(const KRREditCaption& line, bool translation, const KRRPronunciations& pronunciations)
{
	const auto& text = !line.narrationText.isEmpty() ? line.narrationText : translation ? line.translation : line.text;
	if (!line.pronunciation.isEmpty())
	{
		KRRPronunciations local;
		QString error;
		if (ParsePronunciations(line.pronunciation, local, error))
			return pronunciations.apply(local.apply(text));
	}
	return pronunciations.apply(text);
}

bool SubtitleTakeMatches(const KRREditCaption& line, const QString& take, const KRRPronunciations& pronunciations)
{
	return !take.isEmpty() && (take == SubtitleSpokenText(line, true, pronunciations) ||
		take == SubtitleSpokenText(line, false, pronunciations));
}

void InvalidateStoredSubtitleTake(KRREditProject& project, KRREditCaption& line, const KRRPronunciations& pronunciations)
{
	if (line.dubbingFile.isEmpty() || SubtitleTakeMatches(line, line.dubbingText, pronunciations)) return;
	line.dubbingFile.clear();
	line.dubbingText.clear();
	line.dubbingNeedsRefresh = true;
	project.markDubbingTrackStale(line.chapterAt >= 0);
}
