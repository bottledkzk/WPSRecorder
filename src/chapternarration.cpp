#include "chapternarration.h"
#include "uilanguage.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>

namespace
{
constexpr qint64 SentenceGap = 150;
constexpr qint64 Attack = 80;
constexpr qint64 Release = 120;

int FindChapter(const KRREditProject& project, const QString& heading)
{
	static const QRegularExpression numbered(R"(^([1-9][0-9]*)(?:\.\s+(.*))?$)");
	const auto match = numbered.match(heading);
	if (match.hasMatch())
	{
		const int number = match.captured(1).toInt();
		const auto title = match.captured(2);
		if (number >= 1 && number <= project.chapters.size() &&
			(title.isEmpty() || project.chapters[number - 1].title.compare(title, Qt::CaseInsensitive) == 0))
			return number - 1;
	}
	int found = -1;
	for (int i = 0; i < project.chapters.size(); ++i)
	{
		const auto& title = project.chapters[i].title;
		if (title.compare(heading, Qt::CaseInsensitive) != 0 &&
			(match.captured(2).isEmpty() || title.compare(match.captured(2), Qt::CaseInsensitive) != 0)) continue;
		if (found >= 0) return -1;
		found = i;
	}
	return found;
}

qint64 EstimateDuration(const QString& line)
{
	static const QRegularExpression words(R"([A-Za-z0-9]+(?:[-.'][A-Za-z0-9]+)*)");
	qint64 estimate = 400;
	auto match = words.globalMatch(line);
	while (match.hasNext()) { match.next(); estimate += 360; }
	for (const auto ch : line)
		if (ch.isLetter() && ch.unicode() > 127) estimate += 230;
	return qMax<qint64>(650, estimate);
}

double FadeAt(KRREditRange range, qint64 time)
{
	return qBound(0.0, qMin((time - range.begin + Attack) / double(Attack),
		(range.end + Release - time) / double(Release)), 1.0);
}

QString Seconds(qint64 time)
{
	return QString::number(time / 1000., 'f', 3);
}

QVector<KRREditRange> Intersect(const QVector<KRREditRange>& a, const QVector<KRREditRange>& b)
{
	QVector<KRREditRange> result;
	for (int i = 0, j = 0; i < a.size() && j < b.size();)
	{
		const KRREditRange overlap{qMax(a[i].begin, b[j].begin), qMin(a[i].end, b[j].end)};
		if (overlap.begin < overlap.end) result.append(overlap);
		if (a[i].end <= b[j].end) ++i;
		else ++j;
	}
	return result;
}
}

QString ChapterNarrationTitle(const KRREditProject& project, qint64 at)
{
	for (const auto& chapter : project.chapters)
		if (chapter.at == at) return chapter.title;
	return Ui("章节已移除");
}

QString ChapterNarrationScript(const KRREditProject& project)
{
	QStringList parts;
	for (int i = 0; i < project.chapters.size(); ++i)
	{
		const auto& chapter = project.chapters[i];
		QStringList section{QString("## %1. %2").arg(i + 1).arg(chapter.title)};
		for (const auto& line : project.chapterNarration)
			if (line.chapterAt == chapter.at) section.append(line.text);
		parts.append(section.join('\n'));
	}
	return parts.join("\n\n");
}

bool ReadChapterNarration(const KRREditProject& project, const QString& script,
	QVector<KRREditCaption>& lines, QString& error)
{
	if (project.chapters.isEmpty() || script.size() > 60000)
	{
		error = Ui("先添加章节。长稿最多 60000 字。");
		return false;
	}
	static const QRegularExpression boundary(R"((?<=[。！？!?])\s*|(?<=\.)\s+)");
	QVector<KRREditCaption> parsed;
	QSet<int> used;
	int chapter = project.chapters.size() == 1 ? 0 : -1;
	const auto text = script.startsWith(QChar::ByteOrderMark) ? script.mid(1) : script;
	for (const auto& raw : text.split('\n'))
	{
		const auto line = raw.trimmed();
		if (line.isEmpty()) continue;
		if (line.startsWith("## "))
		{
			chapter = FindChapter(project, line.mid(3).trimmed());
			if (chapter < 0 || used.contains(chapter))
			{
				error = Ui("无法唯一匹配章节「%1」，请核对模板或使用章节序号。").arg(line.mid(3));
				return false;
			}
			used.insert(chapter);
			continue;
		}
		if (chapter < 0)
		{
			error = Ui("请在旁白前写入「## 章节标题」。");
			return false;
		}
		used.insert(chapter);
		for (const auto& sentence : line.split(boundary, Qt::SkipEmptyParts))
		{
			if (sentence.trimmed().isEmpty()) continue;
			if (sentence.size() > 2000 || parsed.size() >= MaxChapterNarrationLines)
			{
				error = Ui("最多导入 512 句，每句最多 2000 字。");
				return false;
			}
			KRREditCaption cue;
			cue.text = sentence.trimmed();
			cue.chapterAt = project.chapters[chapter].at;
			parsed.append(cue);
		}
	}
	if (parsed.isEmpty())
	{
		error = Ui("请输入至少一句章节旁白。");
		return false;
	}
	std::stable_sort(parsed.begin(), parsed.end(), [](const auto& a, const auto& b) { return a.chapterAt < b.chapterAt; });
	if (!PlaceChapterNarration(project, parsed, {}, error)) return false;
	lines = parsed;
	return true;
}

bool PlaceChapterNarration(const KRREditProject& project, QVector<KRREditCaption>& lines,
	const QVector<qint64>& durations, QString& error)
{
	const bool estimated = durations.isEmpty();
	if (lines.isEmpty() || (!estimated && durations.size() != lines.size()))
	{
		error = Ui("请先生成所有待配句。");
		return false;
	}
	auto placed = lines;
	for (int first = 0; first < placed.size();)
	{
		const auto at = placed[first].chapterAt;
		int last = first + 1;
		while (last < placed.size() && placed[last].chapterAt == at) ++last;
		const auto chapter = std::lower_bound(project.chapters.begin(), project.chapters.end(), at,
			[](const auto& item, qint64 time) { return item.at < time; });
		if (chapter == project.chapters.end() || chapter->at != at)
		{
			error = Ui("这段旁白的章节已移除，请按当前章节重新导入。");
			return false;
		}
		const auto end = chapter + 1 == project.chapters.end() ? project.duration : (chapter + 1)->at;
		QVector<KRREditRange> available;
		qint64 capacity = 0, estimatedDuration = 0;
		for (const auto range : project.kept)
		{
			const KRREditRange usable{qMax(at, range.begin), qMin(end, range.end)};
			if (usable.begin >= usable.end) continue;
			capacity += usable.end - usable.begin;
			available.append(usable);
		}
		for (int i = first; estimated && i < last; ++i) estimatedDuration += EstimateDuration(placed[i].text);
		const auto fit = [&](qint64 pause, bool placeholders = false)
		{
			int clip = 0;
			qint64 cursor = at;
			const auto budget = qMax<qint64>(0, capacity - pause * (last - first - 1));
			const double scale = estimated ? qMin(1.0, budget / double(estimatedDuration)) : 1.0;
			for (int i = first; i < last; ++i)
			{
				const qint64 length = placeholders ? 100 : estimated ?
					qMax<qint64>(100, qRound64(EstimateDuration(placed[i].text) * scale)) : durations[i];
				if (length <= 0 || length > project.duration) return false;
				while (clip < available.size() && qMax(cursor, available[clip].begin) + length > available[clip].end) ++clip;
				if (clip == available.size()) return false;
				placed[i].begin = qMax(cursor, available[clip].begin);
				placed[i].end = placed[i].begin + length;
				cursor = placed[i].end + pause;
			}
			return true;
		};
		if (!fit(SentenceGap) && !fit(0) && !(estimated && fit(0, true)))
		{
			error = Ui("章节「%1」的保留区间容不下旁白。请缩短稿件、调快语速或调整剪切。").arg(chapter->title);
			return false;
		}
		first = last;
	}
	lines = placed;
	return true;
}

KRRNarrationDucking NarrationDucking(const KRREditProject& project)
{
	for (const auto& track : project.audio)
	{
		if (!track.chapterDubbing || !track.audible() || track.gain <= 0 || track.originalGain >= 1) continue;
		KRRNarrationDucking result;
		result.reducedGain = track.originalGain;
		if (project.arranged())
		{
			QVector<KRREditRange> placed;
			for (const auto& clip : track.placed)
			{
				const auto end = clip.mediaStart + clip.end - clip.begin;
				for (const auto& speech : track.speech)
				{
					const auto begin = qMax(speech.begin, clip.mediaStart), stop = qMin(speech.end, end);
					if (begin < stop)
						placed.append({clip.begin + begin - clip.mediaStart, clip.begin + stop - clip.mediaStart});
				}
			}
			result.speech = MergeEditRanges(std::move(placed));
		}
		else result.speech = MergeEditRanges(Intersect(Intersect(track.speech, track.kept), project.kept));
		return result;
	}
	return {};
}

double KRRNarrationDucking::gainAt(qint64 time) const
{
	double weight = 0;
	const auto next = std::lower_bound(speech.begin(), speech.end(), time,
		[](const auto& range, qint64 at) { return range.end + Release < at; });
	for (auto it = next; it != speech.end() && it->begin - Attack <= time; ++it)
		weight = qMax(weight, FadeAt(*it, time));
	return 1 - (1 - reducedGain) * weight;
}

QString KRRNarrationDucking::filter(KRREditRange segment) const
{
	QStringList weights;
	for (const auto range : speech)
	{
		if (range.end + Release <= segment.begin || range.begin - Attack >= segment.end) continue;
		weights.append(QString("clip(min((t-(%1))/%2,((%3)-t)/%4),0,1)")
			.arg(Seconds(range.begin - Attack - segment.begin), Seconds(Attack),
				Seconds(range.end + Release - segment.begin), Seconds(Release)));
	}
	if (weights.isEmpty()) return {};
	while (weights.size() > 1)
	{
		QStringList next;
		for (int i = 0; i < weights.size(); i += 2)
			next.append(i + 1 == weights.size() ? weights[i] : QString("max(%1,%2)").arg(weights[i], weights[i + 1]));
		weights = next;
	}
	return QString(",asetnsamples=n=480:p=0,volume='1-%1*(%2)':eval=frame")
		.arg(QString::number(1 - reducedGain, 'f', 4), weights.first());
}
