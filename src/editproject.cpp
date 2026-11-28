#include "uilanguage.h"
#include "editproject.h"
#include "recordingconfig.h"
#include "subtitletext.h"
#include "chapternarration.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <numeric>

static QString Seconds(qint64 ms)
{
	return QString::number(ms / 1000.0, 'f', 3);
}
QString EditTime(qint64 ms)
{
	ms = qMax<qint64>(0, ms);
	return QString("%1:%2:%3.%4")
		.arg(ms / 3600000, 2, 10, QLatin1Char('0'))
		.arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
		.arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
		.arg(ms % 1000, 3, 10, QLatin1Char('0'));
}
QImage LoadEditStill(const QString &file)
{
	static const bool initialized = []{ Q_INIT_RESOURCE(editassets); return true; }();
	Q_UNUSED(initialized);
	QImageReader reader(file.isEmpty() ? ":/edit/intermission.png" : file);
	reader.setAutoTransform(true);
	const auto size = reader.size();
	if (!size.isValid() || size.width() > 8192 || size.height() > 8192)
		return {};
	if (size.width() > 2560 || size.height() > 2560)
		reader.setScaledSize(size.scaled(2560, 2560, Qt::KeepAspectRatio));
	return reader.read();
}
QRect FitEditCrop(QRect desired, QSize frame, QSize ratio, bool byHeight, bool right, bool bottom)
{
	const int fw = frame.width() / 2 * 2, fh = frame.height() / 2 * 2;
	if (fw < 2 || fh < 2) return {};
	int stepW = 2, stepH = 2;
	if (ratio.width() > 0 && ratio.height() > 0)
	{
		const int divisor = std::gcd(ratio.width(), ratio.height());
		stepW = ratio.width() / divisor * 2;
		stepH = ratio.height() / divisor * 2;
	}
	if (stepW > fw || stepH > fh) return {};
	const int minW = stepW, minH = stepH;
	int ax = qBound(right ? minW : 0, (right ? desired.x()+desired.width() : desired.x()) / 2 * 2, fw - (right ? 0 : minW));
	int ay = qBound(bottom ? minH : 0, (bottom ? desired.y()+desired.height() : desired.y()) / 2 * 2, fh - (bottom ? 0 : minH));
	const int maxW = right ? ax : fw - ax, maxH = bottom ? ay : fh - ay;
	int w, h;
	if (ratio.width() > 0 && ratio.height() > 0)
	{
		int units = byHeight ? qRound(desired.height() / double(stepH)) : qRound(desired.width() / double(stepW));
		units = qBound(1, units, qMin(maxW / stepW, maxH / stepH));
		w = units * stepW; h = units * stepH;
	}
	else
	{
		w = qBound(2, desired.width() / 2 * 2, maxW);
		h = qBound(2, desired.height() / 2 * 2, maxH);
	}
	return QRect(right ? ax - w : ax, bottom ? ay - h : ay, w, h);
}
bool KRREditSubtitleStyle::valid() const
{
	return !font.trimmed().isEmpty() && font.size() <= 100 && !font.contains(QRegularExpression("[,\\x{0000}-\\x{001f}]"))
		&& (pixels == 0 || (pixels >= 8 && pixels <= 320)) && color.isValid();
}
QJsonObject KRREditSubtitleStyle::toJson() const
{
	return {{"font", font}, {"pixels", pixels}, {"color", color.name(QColor::HexArgb)}};
}
KRREditSubtitleStyle KRREditSubtitleStyle::fromJson(const QJsonObject &o)
{
	return {o["font"].toString("Microsoft YaHei"), o["pixels"].toInt(0), QColor(o["color"].toString("#ffffffff"))};
}
bool ReadEditSubtitleStyle(const QString &file, KRREditSubtitleStyle &style, QString &error)
{
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly) || f.size() > 4000000)
	{
		error = Ui("无法读取字幕样式文件，文件应小于 4 MB。");
		return false;
	}
	auto bytes = f.readAll();
	KRREditSubtitleStyle result;
	if (QFileInfo(file).suffix().compare("ass", Qt::CaseInsensitive) == 0)
	{
		bool styles = false, found = false;
		QStringList fields;
		for (auto line : QString::fromUtf8(bytes).split('\n'))
		{
			line = line.trimmed();
			if (line.startsWith('['))
				styles = line.compare("[V4+ Styles]", Qt::CaseInsensitive) == 0;
			else if (styles && line.startsWith("Format:", Qt::CaseInsensitive))
			{
				fields = line.mid(7).toLower().split(',');
				for (auto &field : fields) field = field.trimmed();
			}
			else if (styles && line.startsWith("Style:", Qt::CaseInsensitive))
			{
				const auto values = line.mid(6).split(',');
				const auto font = fields.indexOf("fontname"), size = fields.indexOf("fontsize");
				const auto color = fields.indexOf("primarycolour");
				if (font < 0 || size < 0 || color < 0 || values.size() < fields.size()) continue;
				QString hex = values[color].trimmed();
				bool ok = false, sizeOk = false;
				if (!hex.startsWith("&H", Qt::CaseInsensitive)) continue;
				hex = hex.mid(2);
				if (hex.endsWith('&')) hex.chop(1);
				const auto number = hex.toUInt(&ok, 16);
				const double pixels = values[size].toDouble(&sizeOk);
				if (!ok || !sizeOk || pixels < 8 || pixels > 320) continue;
				result = {values[font].trimmed(), qRound(pixels),
						  QColor(number & 255, (number >> 8) & 255, (number >> 16) & 255, 255 - (number >> 24))};
				found = true;
				break;
			}
		}
		if (!found) { error = Ui("未找到可用的 ASS 字幕样式。"); return false; }
	}
	else
	{
		auto doc = QJsonDocument::fromJson(bytes);
		if (!doc.isObject() || !doc.object()["font"].isString() || !doc.object()["pixels"].isDouble() || !doc.object()["color"].isString())
		{ error = Ui("样式 JSON 需包含 font、pixels、color。"); return false; }
		result = KRREditSubtitleStyle::fromJson(doc.object());
	}
	if (!result.valid()) { error = Ui("字幕字体、字号或颜色无效。"); return false; }
	style = result;
	return true;
}
QVector<KRREditSegment> KRREditProject::segments() const
{
	const auto& video = arranged() ? orderedVideo : kept;
	QVector<KRREditSegment> result;
	for (int r = 0; r < video.size(); ++r)
	{
		qint64 cursor = video[r].begin;
		for (int i = 0; i < stills.size(); ++i)
		{
			const auto &s = stills[i];
			if (s.at < cursor || s.at > video[r].end ||
				(s.at == video[r].end && std::any_of(video.cbegin(), video.cend(),
					[&s](const auto& clip) { return clip.begin == s.at; })))
				continue;
			if (s.at > cursor) result.append({cursor, s.at, -1});
			result.append({s.at, s.at + s.length, i});
			cursor = s.at;
		}
		if (cursor < video[r].end) result.append({cursor, video[r].end, -1});
	}
	return result;
}
QVector<KRREditRange> KRREditProject::videoClips() const
{
	if (arranged()) return orderedVideo;
	QVector<KRREditRange> clips;
	for (const auto& range : kept)
	{
		qint64 begin = range.begin;
		for (const auto at : splits)
		{
			if (at <= begin || at >= range.end) continue;
			clips.append({begin, at});
			begin = at;
		}
		clips.append({begin, range.end});
	}
	return clips;
}

static QVector<KRREditTrack::Clip> PlaceSourceAudio(const QVector<KRREditSegment>& segments,
	const KRREditTrack& track, qint64 duration, bool sourceCoordinates)
{
	QVector<KRREditTrack::Clip> result;
	const qint64 mediaBegin = track.file.isEmpty() ? 0 : track.offset;
	const qint64 mediaEnd = track.file.isEmpty() ? duration : track.offset + track.length;
	qint64 at = 0;
	for (const auto& segment : segments)
	{
		if (segment.still < 0)
			for (const auto& range : track.kept)
			{
				const qint64 begin = qMax(segment.begin, qMax(range.begin, mediaBegin));
				const qint64 end = qMin(segment.end, qMin(range.end, mediaEnd));
				if (begin >= end) continue;
				const qint64 shift = sourceCoordinates ? 0 : at - segment.begin;
				result.append({begin + shift, end + shift, begin - mediaBegin});
			}
		at += segment.end - segment.begin;
	}
	return result;
}

QVector<KRREditTrack::Clip> KRREditProject::audioClips(int track, bool sourceCoordinates) const
{
	if (track < 0 || track >= audio.size()) return {};
	if (arranged()) return audio[track].placed;
	return PlaceSourceAudio(segments(), audio[track], duration, sourceCoordinates);
}

void KRREditProject::arrange()
{
	if (arranged()) return;
	const auto sourceSegments = segments();
	orderedVideo = videoClips();
	for (auto& track : audio)
		track.placed = PlaceSourceAudio(sourceSegments, track, duration, false);
	QVector<KRREditCaption> placedCaptions;
	qint64 time = 0;
	for (const auto& segment : sourceSegments)
	{
		if (segment.still < 0)
		{
			for (auto cue : captions)
			{
				const qint64 begin = qMax(segment.begin, cue.begin), end = qMin(segment.end, cue.end);
				if (begin >= end) continue;
				cue.begin = time + begin - segment.begin;
				cue.end = time + end - segment.begin;
				placedCaptions.append(cue);
			}
		}
		time += segment.end - segment.begin;
	}
	captions = placedCaptions;
}

qint64 KRREditProject::editDuration() const
{
	if (!arranged()) return duration;
	qint64 timeline = outputDuration();
	for (const auto& track : audio)
		for (const auto& clip : track.placed) timeline = qMax(timeline, clip.end);
	for (const auto& cue : captions) timeline = qMax(timeline, cue.end);
	return timeline;
}

qint64 KRREditProject::outputTime(qint64 sourceTime) const
{
	qint64 time = 0;
	for (const auto& segment : segments())
	{
		if (segment.still < 0 && sourceTime >= segment.begin && sourceTime < segment.end)
			return time + sourceTime - segment.begin;
		time += segment.end - segment.begin;
	}
	return qMax<qint64>(0, time - 1);
}

bool KRREditProject::editArrangedVideo(qint64 begin, qint64 end, bool retain, QString& error)
{
	QVector<KRREditRange> clips;
	qint64 time = 0;
	for (const auto& segment : segments())
	{
		const qint64 length = segment.end - segment.begin;
		if (segment.still < 0)
		{
			const KRREditRange original{segment.begin, segment.end};
			const auto left = segment.begin + qBound<qint64>(0, begin - time, length);
			const auto right = segment.begin + qBound<qint64>(0, end - time, length);
			if (retain)
			{
				if (left < right) clips.append({left, right});
			}
			else
			{
				if (left > original.begin) clips.append({original.begin, left});
				if (right < original.end) clips.append({right, original.end});
			}
		}
		time += length;
	}
	if (clips.isEmpty() || clips.size() > 384)
	{
		error = Ui("视频至少需要保留一个区间。");
		return false;
	}
	const auto retained = MergeEditRanges(clips);
	for (const auto& protectedRange : protectedRanges)
		if (!EditRangesContain(retained, protectedRange))
		{
			error = Ui("删除区间与必保区间重叠。请先调整选区或解除必保。");
			return false;
		}
	orderedVideo = clips;
	kept = retained;
	return true;
}
qint64 KRREditProject::outputDuration() const
{
	qint64 total = 0;
	for (const auto &r : segments())
		total += r.end - r.begin;
	return total;
}
qint64 KRREditProject::nextKept(qint64 t) const
{
	for (const auto &r : kept)
	{
		if (t < r.begin)
			return r.begin;
		if (t < r.end)
			return t;
	}
	return duration;
}
static QVector<KRREditRange> Intersections(const QVector<KRREditRange> &ranges, qint64 begin, qint64 end, bool retain)
{
	QVector<KRREditRange> result;
	for (auto r : ranges)
	{
		if (retain)
		{
			qint64 b = qMax(r.begin, begin), e = qMin(r.end, end);
			if (e > b)
				result.append({b, e});
		}
		else
		{
			if (r.begin < begin)
				result.append({r.begin, qMin(r.end, begin)});
			if (r.end > end)
				result.append({qMax(r.begin, end), r.end});
		}
	}
	return result;
}
QVector<KRREditRange> MergeEditRanges(QVector<KRREditRange> ranges)
{
	std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
	QVector<KRREditRange> result;
	for (const auto& r : ranges)
	{
		if (!result.isEmpty() && r.begin <= result.back().end) result.back().end = qMax(result.back().end, r.end);
		else result.append(r);
	}
	return result;
}

bool EditRangesContain(const QVector<KRREditRange>& ranges, KRREditRange range)
{
	auto cursor = range.begin;
	for (const auto& part : ranges)
	{
		if (part.begin > cursor) return false;
		cursor = qMax(cursor, part.end);
		if (cursor >= range.end) return true;
	}
	return false;
}

bool KRREditProject::editVideoRanges(const QVector<KRREditRange>& ranges, bool retain, QString& error)
{
	if (ranges.isEmpty() || ranges.size() > 128)
	{
		error = Ui("请选择 1 至 128 个剪辑区间。");
		return false;
	}
	for (const auto& r : ranges)
		if (r.begin < 0 || r.end > duration || r.end <= r.begin || r.end - r.begin < 20)
		{
			error = Ui("请选择至少 0.02 秒的有效区间。");
			return false;
		}
	QVector<KRREditRange> result;
	if (retain)
	{
		const auto requested = MergeEditRanges(ranges + protectedRanges);
		for (const auto& r : requested)
			result += Intersections(kept, r.begin, r.end, true);
	}
	else
	{
		result = kept;
		for (const auto& r : MergeEditRanges(ranges))
			result = Intersections(result, r.begin, r.end, false);
		for (const auto& r : protectedRanges)
			if (!EditRangesContain(result, r))
			{
				error = Ui("删除区间与必保区间重叠。请先调整选区或解除必保。");
				return false;
			}
	}
	if (result.isEmpty() || result.size() > 128)
	{
		error = result.isEmpty() ? Ui("视频至少需要保留一个区间。") : Ui("区间过多，请先导出后继续剪辑。");
		return false;
	}
	kept = result;
	if (arranged())
	{
		QVector<KRREditRange> clips;
		for (const auto& clip : orderedVideo)
			for (const auto& range : kept)
				if (qMax(clip.begin, range.begin) < qMin(clip.end, range.end))
					clips.append({qMax(clip.begin, range.begin), qMin(clip.end, range.end)});
		orderedVideo = clips;
	}
	return true;
}

bool KRREditProject::editRange(qint64 begin, qint64 end, int track, bool retain, QString &error)
{
	if (begin < 0 || end > editDuration() || end - begin < 20 || track < -1 || track >= audio.size())
	{
		error = Ui("请选择至少 0.02 秒的有效区间。");
		return false;
	}
	if (track < 0) return arranged() ? editArrangedVideo(begin, end, retain, error) :
		editVideoRanges({{begin, end}}, retain, error);
	if (arranged())
	{
		QVector<KRREditTrack::Clip> placed;
		for (const auto& clip : audio[track].placed)
		{
			const auto append = [&](qint64 from, qint64 to)
			{
				if (from < to) placed.append({from, to, clip.mediaStart + from - clip.begin});
			};
			if (retain) append(qMax(clip.begin, begin), qMin(clip.end, end));
			else
			{
				append(clip.begin, qMin(clip.end, begin));
				append(qMax(clip.begin, end), clip.end);
			}
		}
		if (placed.size() > 384) { error = Ui("区间过多，请先导出后继续剪辑。"); return false; }
		audio[track].placed = placed;
		return true;
	}
	auto result = Intersections(audio[track].kept, begin, end, retain);
	if (result.size() > 128)
	{
		error = Ui("区间过多，请先导出后继续剪辑。");
		return false;
	}
	audio[track].kept = result;
	return true;
}
QString KRREditCaption::displayText() const
{
	return translation.isEmpty() ? text : text + "\n" + translation;
}
QVector<KRREditCaption> KRREditProject::captionsAt(qint64 time) const
{
	QVector<const KRREditCaption*> latest(subtitlePositions.size(), nullptr);
	for (const auto& cue : captions)
	{
		if (cue.track < 0 || cue.track >= latest.size() || cue.begin > time || hiddenSubtitleTracks.contains(cue.track)) continue;
		auto& active = latest[cue.track];
		if (!active || active->begin <= cue.begin) active = &cue;
	}
	QVector<KRREditCaption> result;
	for (const auto cue : latest)
		if (cue && time < cue->end) result.append(*cue);
	return result;
}

bool KRREditProject::captionFits(const KRREditCaption& cue, int ignored) const
{
	if (cue.track < 0 || cue.track >= subtitlePositions.size() || cue.begin < 0 ||
		cue.end > editDuration() || cue.begin >= cue.end) return false;
	for (int i = 0; i < captions.size(); ++i)
	{
		const auto& previous = captions[i];
		if (i != ignored && cue.track == previous.track && cue.begin < previous.end && previous.begin < cue.end)
			return false;
	}
	return true;
}

KRREditSubtitleStyle KRREditProject::styleFor(const KRREditCaption& cue) const
{
	if (cue.style) return *cue.style;
	return subtitleTrackStyles.value(cue.track, subtitleStyle);
}

QVector<KRREditCaption> KRREditProject::outputCaptions(KRRSubtitleMode mode, bool includeHidden) const
{
	QVector<KRREditCaption> result;
	qint64 offset = 0;
	for (auto r : segments())
	{
		if (r.still >= 0)
		{
			const auto &s = stills[r.still];
			if (mode != KRRSubtitleMode::Translation && !s.caption.trimmed().isEmpty())
				result.append({offset, offset + s.length, s.caption});
			offset += s.length;
			continue;
		}
		if (!arranged()) for (auto c : captions)
		{
			if (!includeHidden && hiddenSubtitleTracks.contains(c.track)) continue;
			qint64 b = qMax(r.begin, c.begin), e = qMin(r.end, c.end);
			if (e <= b) continue;
			const auto words = mode == KRRSubtitleMode::Bilingual ? c.displayText() :
				mode == KRRSubtitleMode::Translation ? c.translation : c.text;
			if (!words.isEmpty() || (includeHidden && !c.translation.isEmpty()))
			{
				c.begin = offset + b - r.begin;
				c.end = offset + e - r.begin;
				c.text = words;
				result.append(c);
			}
		}
		offset += r.end - r.begin;
	}
	if (arranged()) for (auto cue : captions)
	{
		if (!includeHidden && hiddenSubtitleTracks.contains(cue.track)) continue;
		cue.text = mode == KRRSubtitleMode::Bilingual ? cue.displayText() :
			mode == KRRSubtitleMode::Translation ? cue.translation : cue.text;
		cue.end = qMin(cue.end, offset);
		if (cue.begin < cue.end && (!cue.text.isEmpty() || (includeHidden && !cue.translation.isEmpty()))) result.append(cue);
	}
	std::stable_sort(result.begin(), result.end(),
					 [](const KRREditCaption &a, const KRREditCaption &b) { return a.begin < b.begin; });
	QVector<int> last(subtitlePositions.size(), -1);
	for (int i = 0; i < result.size(); ++i)
	{
		auto& cue = result[i];
		if (cue.track < 0 || cue.track >= last.size()) continue;
		auto& previous = last[cue.track];
		if (previous >= 0) result[previous].end = qMin(result[previous].end, cue.begin);
		previous = i;
	}
	result.erase(std::remove_if(result.begin(), result.end(), [](const auto& cue) { return cue.begin >= cue.end; }),
		result.end());
	return result;
}
static QJsonArray RangesJson(const QVector<KRREditRange>& ranges);
static bool ValidRanges(const QVector<KRREditRange> &ranges, qint64 duration, int limit = 128)
{
	if (ranges.size() > limit)
		return false;
	qint64 last = 0;
	for (auto r : ranges)
	{
		if (r.begin < last || r.end <= r.begin || r.end > duration)
			return false;
		last = r.end;
	}
	return true;
}

static bool ValidChapterTitle(const QString& title)
{
	return !title.trimmed().isEmpty() && title.size() <= 120 &&
		std::none_of(title.begin(), title.end(), [](QChar ch) { return ch.unicode() < 32 || ch.unicode() == 127; });
}

bool KRREditProject::addChapter(qint64 at, const QString& title, QString& error)
{
	auto where = std::lower_bound(chapters.begin(), chapters.end(), at,
		[](const KRREditChapter& chapter, qint64 time) { return chapter.at < time; });
	if (at < 0 || at >= duration || !ValidChapterTitle(title) || chapters.size() >= 128 ||
		(where != chapters.end() && where->at == at))
	{
		error = Ui("章节时间或标题无效，已有章节不能使用同一时间。");
		return false;
	}
	chapters.insert(where, {at, title.trimmed()});
	return true;
}

QVector<KRREditChapter> KRREditProject::outputChapters() const
{
	if (chapters.isEmpty()) return {};
	QVector<KRREditChapter> result;
	qint64 offset = 0;
	for (const auto& r : segments())
	{
		if (r.still < 0)
			for (const auto& chapter : chapters)
				if (chapter.at >= r.begin && chapter.at < r.end)
					result.append({offset + chapter.at - r.begin, chapter.title});
		offset += r.end - r.begin;
	}
	return result;
}

bool KRREditProject::dubbingTrackNeedsRefresh(bool chapter) const
{
	return std::any_of(audio.begin(), audio.end(), [chapter](const auto& track)
		{ return track.sentenceDubbing && track.chapterDubbing == chapter && track.needsRefresh; });
}

void KRREditProject::markDubbingTrackStale(bool chapter)
{
	for (auto& track : audio)
		if (track.sentenceDubbing && track.chapterDubbing == chapter) track.needsRefresh = true;
}

bool KRREditProject::validate(QString &error) const
{
	if (!QFileInfo(input).isFile() || duration <= 0 || duration > 86400000 || size.width() < 2 || size.height() < 2 ||
		size.width() > 8192 || size.height() > 8192 || !std::isfinite(fps) || fps < 1 || fps > RecordingFrameRates.back())
	{
		error = Ui("源视频不存在，或时长/尺寸不受支持（最长 24 小时、最大 8192 像素）。");
		return false;
	}
	if (kept.isEmpty() || !ValidRanges(kept, duration))
	{
		error = Ui("视频保留区间无效。");
		return false;
	}
	if (arranged())
	{
		auto bySource = orderedVideo;
		std::sort(bySource.begin(), bySource.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
		if (orderedVideo.size() > 384 || !ValidRanges(bySource, duration, 384) ||
			RangesJson(MergeEditRanges(bySource)) != RangesJson(MergeEditRanges(kept)))
		{
			error = Ui("视频保留区间无效。");
			return false;
		}
	}
	if (!ValidRanges(protectedRanges, duration) || std::any_of(protectedRanges.begin(), protectedRanges.end(),
			[this](const auto& r) { return !EditRangesContain(kept, r); }))
	{
		error = Ui("必保区间必须位于当前保留的视频内。");
		return false;
	}
	if (!QRect(QPoint(0, 0), size).contains(crop) || crop.width() < 2 || crop.height() < 2 || crop.width() % 2 ||
		crop.height() % 2 || crop.x() % 2 || crop.y() % 2 ||
		(!cropRatio.isEmpty() && (cropRatio.width() > 8192 || cropRatio.height() > 8192 ||
		qint64(crop.width()) * cropRatio.height() != qint64(crop.height()) * cropRatio.width())))
	{
		error = Ui("裁剪框须位于画面内，宽高和起点应为偶数像素。");
		return false;
	}
	if (audio.size() > 16 || captions.size() > 5000 || chapterNarration.size() > MaxChapterNarrationLines ||
		splits.size() > 256 || stills.size() > 24 || !subtitleStyle.valid() ||
		subtitlePositions.isEmpty() || subtitlePositions.size() > 16)
	{
		error = Ui("轨道、字幕或分割点数量超过限制。");
		return false;
	}
	for (const auto& point : subtitlePositions)
		if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
			point.x() < 0 || point.x() > 1 || point.y() < 0 || point.y() > 1)
		{
			error = Ui("字幕位置须在画面范围内。");
			return false;
		}
	for (int track : hiddenSubtitleTracks)
		if (track < 0 || track >= subtitlePositions.size())
		{
			error = Ui("字幕轨道设置无效。");
			return false;
		}
	for (auto it = subtitleTrackStyles.cbegin(); it != subtitleTrackStyles.cend(); ++it)
		if (it.key() < 0 || it.key() >= subtitlePositions.size() || !it.value().valid())
		{
			error = Ui("字幕轨道样式无效。");
			return false;
		}
	if (subtitleGlossary.size() > 1000)
	{
		error = Ui("翻译术语最多 1000 个字符。");
		return false;
	}
	KRRPronunciations pronunciations;
	if (!pronunciationGuide.isEmpty() && !ParsePronunciations(pronunciationGuide, pronunciations, error)) return false;
	qint64 lastChapter = -1;
	if (chapters.size() > 128) { error = Ui("一个剪辑工程最多可设 128 个章节。"); return false; }
	int draftCharacters = 0;
	const auto compositionLength = editDuration();
	if (chapterDrafts.size() > 129) { error = Ui("章节稿件数量超过限制。"); return false; }
	for (auto it = chapterDrafts.constBegin(); it != chapterDrafts.constEnd(); ++it)
	{
		bool validTime = false;
		const double at = it.key().toDouble(&validTime) * 1000;
		const auto draft = it.value().toObject();
		const auto script = draft["text"].toString();
		draftCharacters += script.size();
		if (!validTime || !std::isfinite(at) || at < 0 || at >= compositionLength + .5 ||
			!it.value().isObject() || draftCharacters > 60000 || draft["rules"].toString().size() > 1200)
		{
			error = Ui("章节稿件超过长度限制或位置无效。");
			return false;
		}
	}
	for (const auto& chapter : chapters)
	{
		if (chapter.at <= lastChapter || chapter.at >= duration || !ValidChapterTitle(chapter.title))
		{
			error = Ui("章节时间或标题无效，已有章节不能使用同一时间。");
			return false;
		}
		lastChapter = chapter.at;
	}
	bool chapterTrack = false;
	for (int i = 0; i < audio.size(); ++i)
	{
		const auto& t = audio[i];
		qint64 activeEnd = 0;
		const auto mediaLength = t.file.isEmpty() ? duration : t.length;
		for (const auto& clip : t.placed)
		{
			if (clip.begin < activeEnd || clip.end <= clip.begin || clip.end > 86400000 ||
				clip.mediaStart < 0 || clip.mediaStart + clip.end - clip.begin > mediaLength || t.placed.size() > 384)
			{
				error = Ui("声音轨道参数无效。");
				return false;
			}
			activeEnd = clip.end;
		}
		if ((t.file.isEmpty() ? t.stream != i : t.stream != -1 || !QFileInfo(t.file).isFile() || t.offset < 0 ||
			t.offset >= duration || t.length <= 0 || t.length > duration - t.offset) ||
			!std::isfinite(t.gain) || t.gain < 0 || t.gain > 1 || !ValidRanges(t.kept, duration))
		{
			error = Ui("声音轨道参数无效。");
			return false;
		}
		if (t.chapterDubbing)
		{
			if (chapterTrack || !t.sentenceDubbing || t.file.isEmpty() || t.offset != 0 || t.speech.isEmpty() ||
				!ValidRanges(t.speech, t.length, MaxChapterNarrationLines) ||
				!std::isfinite(t.originalGain) || t.originalGain < 0 || t.originalGain > 1)
			{
				error = Ui("章节旁白轨或原声压低设置无效。");
				return false;
			}
			chapterTrack = true;
		}
	}
	qint64 lastStill = -1;
	for (const auto &s : stills)
	{
		if (s.at < 0 || s.at > duration || s.at <= lastStill || s.length < 100 || s.length > 600000 ||
			s.caption.size() > 2000 || (!s.image.isEmpty() && !QFileInfo(s.image).isFile()))
		{
			error = Ui("静止画面的位置、时长或图片文件无效。");
			return false;
		}
		lastStill = s.at;
	}
	const auto validCue = [](const KRREditCaption& c, qint64 limit)
	{
		return c.begin >= 0 && c.end > c.begin && c.end <= limit && !c.text.trimmed().isEmpty() &&
			c.text.size() <= 2000 && c.translation.size() <= 2000 && c.dubbingText.size() <= 2000 &&
			c.dubbingFile.size() <= 2048 && c.narrationText.size() <= 2000;
	};
	const auto captionLimit = arranged() ? qint64(86400000) : duration;
	for (const auto& c : captions)
		if (!validCue(c, captionLimit) || c.track < 0 || c.track >= subtitlePositions.size())
		{
			error = Ui("字幕需有内容且处于视频有效时间内。");
			return false;
		}
		else if (c.style && !c.style->valid())
		{
			error = Ui("字幕片段样式无效。");
			return false;
		}
		else if (!c.pronunciation.isEmpty() && !ParsePronunciations(c.pronunciation, pronunciations, error)) return false;
	if (!chapterNarrationLanguage.isEmpty() && chapterNarrationLanguage != "en" && chapterNarrationLanguage != "zh")
	{
		error = Ui("章节旁白语言无效。");
		return false;
	}
	qint64 previousEnd = 0, previousAnchor = -1;
	for (const auto& c : chapterNarration)
	{
		if (!validCue(c, duration) || c.chapterAt < 0 || c.chapterAt < previousAnchor || c.chapterAt > c.begin ||
			c.begin < previousEnd || !c.translation.isEmpty() || !c.narrationText.isEmpty())
		{
			error = Ui("章节旁白的时间或文字无效。");
			return false;
		}
		previousEnd = c.end;
		previousAnchor = c.chapterAt;
	}
	for (auto t : splits)
		if (t <= 0 || t >= duration)
		{
			error = Ui("分割点超出时间轴。");
			return false;
		}
	return true;
}
bool ProbeEditAudio(const QString &file, qint64 offset, qint64 duration, KRREditTrack &track, QString &error)
{
	if (!QFileInfo(file).isFile() || offset < 0 || offset >= duration)
	{ error = Ui("请选择有效的本地音频和插入时间。"); return false; }
	QProcess p;
	HideConsoleProcess(p);
	p.start(FindFFprobe(), {"-v","error","-select_streams","a:0","-show_entries","stream=duration:format=duration","-of","json",file});
	if (!p.waitForFinished(10000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
	{ error = Ui("无法读取外部音频。"); return false; }
	auto doc = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
	auto streams = doc["streams"].toArray();
	if (streams.isEmpty()) { error = Ui("所选文件不含音频。"); return false; }
	double seconds = streams.first().toObject()["duration"].toString().toDouble();
	if (!std::isfinite(seconds) || seconds <= 0) seconds = doc["format"].toObject()["duration"].toString().toDouble();
	if (!std::isfinite(seconds) || seconds < .02) { error = Ui("无法读取有效的音频时长。"); return false; }
	const qint64 length = qRound64(qMin(seconds, (duration - offset) / 1000.) * 1000.);
	track = {QFileInfo(file).completeBaseName(), -1, true, 1., {{offset, offset + length}},
			 QFileInfo(file).absoluteFilePath(), offset, length};
	return true;
}
bool ProbeEditSource(const QString &input, KRREditProject &project, QString &error, QJsonObject *container,
	int timeoutMs, const std::atomic<bool>* cancel)
{
	if (cancel && cancel->load()) { error = Ui("任务已取消。"); return false; }
	QProcess p;
	HideConsoleProcess(p);
	QStringList args{"-v", "error", "-show_streams", "-show_format", "-of", "json"};
	if (container) args << "-show_chapters";
	p.start(FindFFprobe(), args << input);
	QElapsedTimer deadline;
	deadline.start();
	while (p.state() != QProcess::NotRunning && !p.waitForFinished(qMin(timeoutMs, 100)))
	{
		if (deadline.hasExpired(timeoutMs) || (cancel && cancel->load()))
		{
			p.kill();
			p.waitForFinished(1000);
			error = cancel && cancel->load() ? Ui("任务已取消。") : Ui("读取视频信息超时。");
			return false;
		}
	}
	if (p.error() == QProcess::FailedToStart || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
	{
		error = Ui("无法读取视频信息：") + QString::fromUtf8(p.readAllStandardError());
		return false;
	}
	auto doc = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
	KRREditProject result;
	result.input = QFileInfo(input).absoluteFilePath();
	result.sourceBytes = QFileInfo(input).size();
	double duration = doc["format"].toObject()["duration"].toString().toDouble();
	if (!std::isfinite(duration) || duration <= 0 || duration > 86400)
	{
		error = Ui("视频必须包含有效时长，且不超过 24 小时。");
		return false;
	}
	result.duration = qRound64(duration * 1000);
	bool video = false;
	for (auto value : doc["streams"].toArray())
	{
		auto s = value.toObject();
		QString type = s["codec_type"].toString();
		if (type == "video" && !video)
		{
			result.size = QSize(s["width"].toInt(), s["height"].toInt());
			video = true;
			auto rate = s["avg_frame_rate"].toString().split('/');
			double fps = rate.value(0).toDouble() / qMax(1.0, rate.value(1).toDouble());
			const double maxFps = RecordingFrameRates.back();
			if (fps > maxFps && fps < maxFps + .5 && s["r_frame_rate"].toString() == QString("%1/1").arg(maxFps))
				fps = maxFps;
			result.fps = std::isfinite(fps) && fps >= 1 && fps <= RecordingFrameRates.back() ? fps : 30;
			for (auto side : s["side_data_list"].toArray())
			{
				int rotation = qRound(side.toObject()["rotation"].toDouble());
				if (std::abs(rotation) % 180 == 90)
				{
					result.size.transpose();
					break;
				}
			}
		}
		if (type == "audio")
		{
			auto tags = s["tags"].toObject();
			QString name = tags["title"].toString(tags["handler_name"].toString());
			if (name.isEmpty() || name == "SoundHandler")
				name = Ui("声音 %1").arg(result.audio.size() + 1);
			if (name == "mic")
				name = Ui("麦克风");
			else if (name == "system")
				name = Ui("系统声音");
			result.audio.append({name, result.audio.size(), true, 1, {{0, result.duration}}});
		}
	}
	if (!video)
	{
		error = Ui("所选文件没有视频画面。");
		return false;
	}
	// A sequence export includes a playback mix of the following stems. Edit the stems by default on reimport.
	if (result.audio.size() > 1 &&
		(result.audio.front().title == QStringLiteral("WPS Recorder 成片混音") ||
		 result.audio.front().title == QStringLiteral("WPS Recorder Full mix")))
		result.audio.front().enabled = false;
	result.crop = QRect(0, 0, result.size.width() / 2 * 2, result.size.height() / 2 * 2);
	result.kept = {{0, result.duration}};
	if (!result.validate(error))
		return false;
	project = result;
	if (container) *container = std::move(doc);
	return true;
}
static QJsonArray RangesJson(const QVector<KRREditRange> &ranges)
{
	QJsonArray a;
	for (auto r : ranges)
		a.append(QJsonArray{r.begin, r.end});
	return a;
}
static QJsonArray CuesJson(const QVector<KRREditCaption>& cues)
{
	QJsonArray items;
	for (const auto& c : cues)
	{
		QJsonObject item{{"begin", c.begin}, {"end", c.end}, {"text", c.text}};
		if (!c.translation.isEmpty()) item["translation"] = c.translation;
		if (!c.dubbingFile.isEmpty())
		{
			item["dubbingFile"] = c.dubbingFile;
			item["dubbingText"] = c.dubbingText;
		}
		if (!c.narrationText.isEmpty()) item["narrationText"] = c.narrationText;
		if (!c.pronunciation.isEmpty()) item["pronunciation"] = c.pronunciation;
		if (c.dubbingNeedsRefresh) item["dubbingNeedsRefresh"] = true;
		if (c.chapterAt >= 0) item["chapterAt"] = c.chapterAt;
		if (c.track > 0) item["track"] = c.track;
		if (c.style) item["style"] = c.style->toJson();
		items.append(item);
	}
	return items;
}

void KRREditProject::setTrackOrder(const QJsonArray& order)
{
	static const QRegularExpression trackId(QStringLiteral("^(?:V1|[AT][1-9][0-9]?)$"));
	trackOrder.clear();
	for (const auto& value : order)
	{
		const auto id = value.toString();
		if (trackId.match(id).hasMatch() && !trackOrder.contains(id)) trackOrder.append(id);
		if (trackOrder.size() == 33) break;
	}
}

QJsonObject KRREditProject::toJson() const
{
	QJsonArray tracks, markers, images;
	for (const auto& t : audio)
	{
		QJsonObject track{{"stream", t.stream},
			{"title", t.title}, {"enabled", t.enabled}, {"gain", t.gain},
			{"kept", RangesJson(t.kept)}, {"file", t.file}, {"offset", t.offset}, {"length", t.length},
			{"sentenceDubbing", t.sentenceDubbing}};
		if (t.needsRefresh) track["needsRefresh"] = true;
		if (arranged())
		{
			QJsonArray clips;
			for (const auto& clip : t.placed) clips.append(QJsonArray{clip.begin, clip.end, clip.mediaStart});
			track["placed"] = clips;
		}
		if (t.chapterDubbing)
		{
			track["chapterDubbing"] = true;
			track["speech"] = RangesJson(t.speech);
			track["originalGain"] = t.originalGain;
		}
		tracks.append(track);
	}
	for (auto t : splits)
		markers.append(t);
	for (const auto &s : stills)
		images.append(QJsonObject{{"at", s.at}, {"length", s.length}, {"image", s.image}, {"caption", s.caption}});
	QJsonObject project{{"version", 1}, {"subtitleStyle", subtitleStyle.toJson()}, {"stills", images},
			{"subtitleLanguage", subtitleLanguage}, {"translationLanguage", translationLanguage},
			{"input", input},
			{"sourceBytes", sourceBytes},
			{"duration", duration},
			{"fps", fps},
			{"width", size.width()},
			{"height", size.height()},
			{"crop", QJsonArray{crop.x(), crop.y(), crop.width(), crop.height()}},
			{"cropRatio", QJsonArray{cropRatio.width(), cropRatio.height()}},
			{"kept", RangesJson(kept)},
			{"protected", RangesJson(protectedRanges)},
			{"audio", tracks},
			{"captions", CuesJson(captions)},
			{"splits", markers}};
	if (!chapterNarration.isEmpty()) project["chapterNarration"] = CuesJson(chapterNarration);
	if (!chapterDrafts.isEmpty()) project["chapterDrafts"] = chapterDrafts;
	if (!trackOrder.isEmpty()) project["trackOrder"] = QJsonArray::fromStringList(trackOrder);
	if (!chapterNarrationLanguage.isEmpty()) project["chapterNarrationLanguage"] = chapterNarrationLanguage;
	if (!subtitleGlossary.isEmpty()) project["subtitleGlossary"] = subtitleGlossary;
	if (!pronunciationGuide.isEmpty()) project["pronunciationGuide"] = pronunciationGuide;
	if (!subtitleTrackStyles.isEmpty())
	{
		QJsonObject styles;
		for (auto it = subtitleTrackStyles.cbegin(); it != subtitleTrackStyles.cend(); ++it)
			styles[QString::number(it.key())] = it.value().toJson();
		project["subtitleTrackStyles"] = styles;
	}
	if (arranged()) project["orderedVideo"] = RangesJson(orderedVideo);
	if (subtitlePositions != QVector<QPointF>{{.5, .9375}})
	{
		QJsonArray positions;
		for (const auto& p : subtitlePositions) positions.append(QJsonArray{p.x(), p.y()});
		project["subtitlePositions"] = positions;
	}
	if (!hiddenSubtitleTracks.isEmpty())
	{
		QJsonArray hidden;
		for (int track = 0; track < subtitlePositions.size(); ++track)
			if (hiddenSubtitleTracks.contains(track)) hidden.append(track);
		project["hiddenSubtitleTracks"] = hidden;
	}
	if (!chapters.isEmpty())
	{
		QJsonArray chapterData;
		for (const auto& chapter : chapters)
			chapterData.append(QJsonObject{{"at", chapter.at}, {"title", chapter.title}});
		project["chapters"] = chapterData;
	}
	return project;
}
static bool ParseRanges(const QJsonValue &value, QVector<KRREditRange> &ranges)
{
	if (!value.isArray())
		return false;
	ranges.clear();
	for (auto v : value.toArray())
	{
		auto r = v.toArray();
		if (r.size() != 2)
			return false;
		for (auto n : r)
			if (!n.isDouble() || !std::isfinite(n.toDouble()) || n.toDouble() < 0 || n.toDouble() > 86400000 ||
				std::floor(n.toDouble()) != n.toDouble())
				return false;
		ranges.append({static_cast<qint64>(r[0].toDouble()), static_cast<qint64>(r[1].toDouble())});
	}
	return true;
}
bool KRREditProject::FromJson(const QJsonObject &o, KRREditProject &project, QString &error)
{
	if (o["version"].toInt() != 1)
	{
		error = Ui("不支持的剪辑工程版本。");
		return false;
	}
	KRREditProject p;
	if (!ProbeEditSource(o["input"].toString(), p, error))
		return false;
	if (static_cast<qint64>(o["sourceBytes"].toDouble()) != p.sourceBytes ||
		std::abs(o["duration"].toDouble() - p.duration) > 100 || o["width"].toInt() != p.size.width() ||
		o["height"].toInt() != p.size.height())
	{
		error = Ui("工程对应的源视频尺寸、时长或文件大小已改变，请重新导入。");
		return false;
	}
	auto crop = o["crop"].toArray();
	if (crop.size() != 4 || !ParseRanges(o["kept"], p.kept) || !o["audio"].isArray() || !o["captions"].isArray() ||
		!o["splits"].isArray() || (o.contains("protected") && !ParseRanges(o["protected"], p.protectedRanges)) ||
		(o.contains("orderedVideo") && (!ParseRanges(o["orderedVideo"], p.orderedVideo) || p.orderedVideo.isEmpty())))
	{
		error = Ui("工程区间或裁剪框无效。");
		return false;
	}
	p.crop = QRect(crop[0].toInt(), crop[1].toInt(), crop[2].toInt(), crop[3].toInt());
	auto ratio = o["cropRatio"].toArray();
	if (ratio.size() == 2) p.cropRatio = QSize(ratio[0].toInt(), ratio[1].toInt());
	const auto time = [](const QJsonValue &v)
	{
		return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0 && v.toDouble() <= 86400000 &&
			   std::floor(v.toDouble()) == v.toDouble();
	};
	auto tracks = o["audio"].toArray();
	if (tracks.size() < p.audio.size() || tracks.size() > 16)
	{
		error = Ui("源声音轨道数量已改变。");
		return false;
	}
	const int sourceTracks = p.audio.size();
	for (int i = 0; i < tracks.size(); ++i)
	{
		auto t = tracks[i].toObject();
		if (i >= sourceTracks)
		{
			KRREditTrack track;
			const auto offset = t["offset"].toDouble(-1);
			if (!std::isfinite(offset) || offset < 0 || offset >= p.duration || std::floor(offset) != offset ||
				!ProbeEditAudio(t["file"].toString(), static_cast<qint64>(offset), p.duration, track, error))
			{
				if (error.isEmpty()) error = Ui("外部音频位置无效。");
				return false;
			}
			p.audio.append(track);
		}
		const auto title = t["title"].toString();
		if (i >= sourceTracks && !title.trimmed().isEmpty()) p.audio[i].title = title.left(120);
		p.audio[i].sentenceDubbing = i >= sourceTracks && t["sentenceDubbing"].toBool();
		p.audio[i].needsRefresh = p.audio[i].sentenceDubbing && t["needsRefresh"].toBool();
		p.audio[i].chapterDubbing = t["chapterDubbing"].toBool();
		if (p.audio[i].chapterDubbing)
		{
			p.audio[i].originalGain = t["originalGain"].toDouble(-1);
			if (!ParseRanges(t["speech"], p.audio[i].speech))
			{
				error = Ui("章节旁白轨或原声压低设置无效。");
				return false;
			}
		}
		p.audio[i].enabled = t["enabled"].toBool();
		p.audio[i].gain = t["gain"].toDouble(-1);
		if (t["stream"].toInt(-2) != p.audio[i].stream || !ParseRanges(t["kept"], p.audio[i].kept))
		{
			error = Ui("声音轨道数据无效。");
			return false;
		}
		if (p.arranged())
		{
			if (!t["placed"].isArray()) { error = Ui("声音轨道数据无效。"); return false; }
			for (const auto& value : t["placed"].toArray())
			{
				const auto clip = value.toArray();
				if (clip.size() != 3 || !time(clip[0]) || !time(clip[1]) || !time(clip[2]))
				{
					error = Ui("声音轨道数据无效。");
					return false;
				}
				p.audio[i].placed.append({clip[0].toVariant().toLongLong(), clip[1].toVariant().toLongLong(),
					clip[2].toVariant().toLongLong()});
			}
		}
	}
	if (o.contains("chapters"))
	{
		if (!o["chapters"].isArray()) { error = Ui("工程章节数据无效。"); return false; }
		const auto items = o["chapters"].toArray();
		if (items.size() > 128) { error = Ui("一个剪辑工程最多可设 128 个章节。"); return false; }
		for (const auto& value : items)
		{
			const auto chapter = value.toObject();
			if (!time(chapter["at"]) || !chapter["title"].isString()) { error = Ui("工程章节数据无效。"); return false; }
			p.chapters.append({chapter["at"].toVariant().toLongLong(), chapter["title"].toString()});
		}
	}
	p.subtitleStyle = KRREditSubtitleStyle::fromJson(o["subtitleStyle"].toObject());
	p.chapterDrafts = o["chapterDrafts"].toObject();
	p.setTrackOrder(o["trackOrder"].toArray());
	const auto trackStyles = o["subtitleTrackStyles"].toObject();
	for (auto it = trackStyles.begin(); it != trackStyles.end(); ++it)
	{
		bool indexOk = false;
		const int index = it.key().toInt(&indexOk);
		if (!indexOk || !it.value().isObject())
		{
			error = Ui("字幕轨道样式无效。");
			return false;
		}
		p.subtitleTrackStyles.insert(index, KRREditSubtitleStyle::fromJson(it.value().toObject()));
	}
	if (o.contains("subtitlePositions"))
	{
		p.subtitlePositions.clear();
		for (const auto& value : o["subtitlePositions"].toArray())
		{
			const auto pair = value.toArray();
			if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble())
			{
				error = Ui("字幕位置须在画面范围内。");
				return false;
			}
			p.subtitlePositions.append(QPointF(pair[0].toDouble(), pair[1].toDouble()));
		}
	}
	p.subtitleLanguage = o["subtitleLanguage"].toString().left(16);
	for (const auto& track : o["hiddenSubtitleTracks"].toArray())
	{
		const auto index = track.toInt(-1);
		if (index < 0 || index >= p.subtitlePositions.size() || track.toDouble() != index)
		{
			error = Ui("字幕轨道设置无效。");
			return false;
		}
		p.hiddenSubtitleTracks.insert(index);
	}
	p.translationLanguage = o["translationLanguage"].toString().left(16);
	p.subtitleGlossary = o["subtitleGlossary"].toString();
	p.pronunciationGuide = o["pronunciationGuide"].toString();
	for (auto v : o["stills"].toArray())
	{
		const auto s = v.toObject();
		if (!time(s["at"]) || !time(s["length"])) { error = Ui("静止画面时间无效。"); return false; }
		p.stills.append({s["at"].toVariant().toLongLong(), s["length"].toVariant().toLongLong(),
						 s["image"].toString(), s["caption"].toString()});
	}
	const auto readCues = [&](const QJsonValue& value, QVector<KRREditCaption>& cues, bool narration)
	{
		if (!value.isArray()) return false;
		const auto items = value.toArray();
		if (items.size() > (narration ? MaxChapterNarrationLines : 5000)) return false;
		for (const auto& v : items)
		{
			const auto c = v.toObject();
			if (!time(c["begin"]) || !time(c["end"]) || (narration && !time(c["chapterAt"]))) return false;
			cues.append({c["begin"].toVariant().toLongLong(), c["end"].toVariant().toLongLong(),
				c["text"].toString(), c["translation"].toString(), c["dubbingFile"].toString(),
				c["dubbingText"].toString(), c["narrationText"].toString(), c["dubbingNeedsRefresh"].toBool(),
				narration ? c["chapterAt"].toVariant().toLongLong() : -1, narration ? 0 : c["track"].toInt(),
				c["pronunciation"].toString()});
			if (!narration && c.contains("style"))
			{
				if (!c["style"].isObject()) return false;
				cues.last().style = KRREditSubtitleStyle::fromJson(c["style"].toObject());
			}
		}
		return true;
	};
	if (!readCues(o["captions"], p.captions, false) ||
		(o.contains("chapterNarration") && !readCues(o["chapterNarration"], p.chapterNarration, true)))
	{
		error = Ui("字幕或旁白时间数据无效。");
		return false;
	}
	p.chapterNarrationLanguage = o["chapterNarrationLanguage"].toString();
	for (auto v : o["splits"].toArray())
	{
		if (!time(v))
		{
			error = Ui("分割时间数据无效。");
			return false;
		}
		p.splits.append(static_cast<qint64>(v.toDouble()));
	}
	if (!p.validate(error))
		return false;
	project = p;
	return true;
}
bool ReadEditSubtitles(const QString &file, QVector<KRREditCaption> &captions, QString &error)
{
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly) || f.size() > 4000000)
	{
		error = Ui("无法读取 SRT，文件应小于 4MB。");
		return false;
	}
	QString text = QString::fromUtf8(f.readAll());
	text.remove(QChar(0xfeff));
	text.replace("\r\n", "\n");
	QRegularExpression timing(
		"(\\d{1,3}):(\\d{2}):(\\d{2})[,.](\\d{3})\\s*-->\\s*(\\d{1,3}):(\\d{2}):(\\d{2})[,.](\\d{3})");
	QVector<KRREditCaption> result;
	for (auto block : text.split(QRegularExpression("\n[ \\t]*\n"), Qt::SkipEmptyParts))
	{
		auto match = timing.match(block);
		if (!match.hasMatch())
			continue;
		auto time = [&](int i)
		{
			return match.captured(i).toLongLong() * 3600000 + match.captured(i + 1).toLongLong() * 60000 +
				   match.captured(i + 2).toLongLong() * 1000 + match.captured(i + 3).toLongLong();
		};
		QString words = block.mid(match.capturedEnd()).trimmed();
		if (!words.isEmpty())
			result.append({time(1), time(5), words});
	}
	if (result.isEmpty())
	{
		error = Ui("没有找到有效的 UTF-8 SRT 字幕。");
		return false;
	}
	captions = result;
	return true;
}
QStringList AdjacentEditSubtitlePaths(const QString& input)
{
	const QFileInfo video(input);
	return {input + ".srt", video.absolutePath() + '/' + video.completeBaseName() + ".srt"};
}

QVector<KRREditCaption> ReadAdjacentEditSubtitles(const QString& input, qint64 duration)
{
	for (const auto& path : AdjacentEditSubtitlePaths(input))
	{
		const QFileInfo info(path);
		if (!info.isFile() || info.isSymLink() || info.size() > 262144) continue;
		QVector<KRREditCaption> cues;
		QString error;
		if (!ReadEditSubtitles(path, cues, error)) continue;
		cues.erase(std::remove_if(cues.begin(), cues.end(), [duration](const KRREditCaption& cue)
			{ return cue.begin < 0 || cue.begin >= duration || cue.end <= cue.begin; }), cues.end());
		for (auto& cue : cues) cue.end = qMin(duration, cue.end);
		if (!cues.isEmpty()) return cues;
	}
	return {};
}

bool WriteEditSubtitles(const QString &file, const QVector<KRREditCaption> &captions, QString &error)
{
	QSaveFile f(file);
	if (!f.open(QIODevice::WriteOnly))
	{
		error = Ui("无法保存字幕。");
		return false;
	}
	int n = 0;
	for (auto c : captions)
	{
		QString line = QString("%1\n%2 --> %3\n%4\n\n")
						   .arg(++n)
						   .arg(EditTime(c.begin).replace('.', ','), EditTime(c.end).replace('.', ','), c.text);
		if (f.write(line.toUtf8()) < 0)
		{
			error = Ui("字幕写入失败。");
			return false;
		}
	}
	if (!f.commit())
	{
		error = Ui("字幕保存失败。");
		return false;
	}
	return true;
}
bool WriteEditChapters(const QString &path, const QVector<KRREditChapter> &chapters, qint64 duration)
{
	QString metadata = ";FFMETADATA1\n";
	for (int i = 0; i < chapters.size(); ++i)
	{
		auto title = chapters[i].title;
		title.replace('\\', "\\\\").replace('=', "\\=").replace(';', "\\;").replace('#', "\\#");
		metadata += QString("[CHAPTER]\nTIMEBASE=1/1000\nSTART=%1\nEND=%2\ntitle=%3\n")
			.arg(chapters[i].at).arg(i + 1 < chapters.size() ? chapters[i + 1].at : duration).arg(title);
	}
	QFile file(path);
	const auto bytes = metadata.toUtf8();
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}
static QString AssTime(qint64 ms)
{
	return QString("%1:%2:%3.%4")
		.arg(ms / 3600000)
		.arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
		.arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
		.arg(ms / 10 % 100, 2, 10, QLatin1Char('0'));
}
QSize KRRTimelineExportOptions::outputSize(const QSize& crop) const
{
	if (!crop.isValid() || maxHeight <= 0 || (crop.height() <= maxHeight && crop.width() <= maxHeight * 16 / 9))
		return crop;
	const auto scaled = crop.scaled(maxHeight * 16 / 9, maxHeight, Qt::KeepAspectRatio);
	return {qMax(2, scaled.width() / 2 * 2), qMax(2, scaled.height() / 2 * 2)};
}

KRRTimelineExportOptions SubmissionPreset(const QString& preset)
{
	KRRTimelineExportOptions options;
	if (preset == "small")
	{
		options.maxHeight = 720;
		options.videoKbps = 2400;
		options.audioKbps = 128;
	}
	else
		options.maxHeight = 1080;
	return options;
}

QString RenderEditProject(const KRREditProject &p, const QString &output, const QString &encoder, bool separateAudio,
						  bool burnCaptions, const std::atomic<bool> *cancel, const KRRTimelineExportOptions& options)
{
	QString error;
	if (!p.validate(error))
		return error;
	const auto duration = p.outputDuration();
	const bool stage = options.sequenceCanvas.isValid();
	const auto rate = stage ? options.sequenceFps : p.fps;
	if (options.maxHeight < 0 || options.maxHeight > 4320 || options.videoKbps < 100 || options.videoKbps > 100000 ||
		options.audioKbps < 32 || options.audioKbps > 320 || options.previewStart < 0 || options.previewLength < 0 ||
		(options.previewLength > 0 && (options.previewStart >= duration || (!burnCaptions && !p.captions.isEmpty()))))
		return Ui("提交预设或预览区间无效。");
	if (QFileInfo::exists(output))
		return Ui("目标文件已存在，请选择新文件名。");
	QString extension = QFileInfo(output).suffix().toLower();
	if (!QStringList{"mp4", "mov", "mkv"}.contains(extension))
		return Ui("时间轴工程支持导出 MP4、MOV 或 MKV。");
	QTemporaryDir temp(QFileInfo(output).absolutePath() + "/.timeline-XXXXXX");
	if (!temp.isValid())
		return Ui("无法创建导出临时目录。");
	auto captions = p.outputCaptions(KRRSubtitleMode::Bilingual);
	auto segments = p.segments();
	qint64 length = duration;
	qint64 seek = 0;
	if (options.previewLength > 0)
	{
		const auto from = options.previewStart;
		length = qMin(options.previewLength, duration - from);
		const auto to = from + length;
		QVector<KRREditSegment> selected;
		qint64 offset = 0;
		qint64 firstVideo = p.duration;
		for (const auto& segment : segments)
		{
			const auto next = offset + segment.end - segment.begin;
			const auto begin = qMax(from, offset);
			const auto end = qMin(to, next);
			if (begin < end)
			{
				const auto at = segment.begin + begin - offset;
				selected.append({at, at + end - begin, segment.still});
				if (segment.still < 0) firstVideo = qMin(firstVideo, at);
			}
			offset = next;
			if (offset >= to) break;
		}
		segments = selected;
		// Leave room for decoding the video and its embedded audio before cutting the sample.
		if (firstVideo < p.duration) seek = qMax<qint64>(0, firstVideo - 2000);
		QVector<KRREditCaption> selectedCaptions;
		for (const auto& caption : captions)
		{
			const auto begin = qMax(from, caption.begin);
			const auto end = qMin(to, caption.end);
			if (begin < end)
			{
				auto cue = caption;
				cue.begin = begin - from;
				cue.end = end - from;
				selectedCaptions.append(cue);
			}
		}
		captions = selectedCaptions;
	}
	const bool hasCaptions = !captions.isEmpty();
	QVector<QPair<int, QString>> subtitleFiles;
	if (hasCaptions && !burnCaptions)
	{
		QVector<QVector<KRREditCaption>> byTrack(p.subtitlePositions.size());
		for (const auto& cue : captions) byTrack[cue.track].append(cue);
		for (int i = 0; i < byTrack.size(); ++i)
		{
			if (byTrack[i].isEmpty()) continue;
			const auto path = temp.filePath(QString("captions-%1.srt").arg(i));
			if (!WriteEditSubtitles(path, byTrack[i], error)) return error;
			subtitleFiles.append({i, path});
		}
	}
	if (hasCaptions && burnCaptions)
	{
		auto hex = [](int n){ return QString("%1").arg(n, 2, 16, QLatin1Char('0')).toUpper(); };
		QString ass = QString(
			"[Script Info]\nScriptType: v4.00+\nPlayResX: %1\nPlayResY: %2\nWrapStyle: 0\n\n[V4+ Styles]\n"
			"Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,Bold,Italic,Underline,"
			"StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,Alignment,MarginL,MarginR,MarginV,Encoding\n")
			.arg(p.crop.width()).arg(p.crop.height());
		QString events = "\n[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n";
		QHash<QByteArray, QString> styleNames;
		for (const auto& c : captions)
		{
			const auto style = p.styleFor(c);
			const auto styleKey = QJsonDocument(style.toJson()).toJson(QJsonDocument::Compact);
			auto name = styleNames.value(styleKey);
			if (name.isEmpty())
			{
				name = QString("Caption%1").arg(styleNames.size());
				styleNames.insert(styleKey, name);
				const auto color = style.color;
				const auto assColor = "&H" + hex(255 - color.alpha()) + hex(color.blue()) + hex(color.green()) + hex(color.red());
				const auto outline = "&H" + hex(255 - color.alpha()) + "101520";
				const auto shadow = "&H" + hex(255 - color.alpha() / 2) + "000000";
				ass += QString("Style: %1,%2,%3,%4,%4,%5,%6,0,0,0,0,100,100,0,0,1,2,1,2,20,20,%7,1\n")
					.arg(name, style.font).arg(style.pixels ? style.pixels : qBound(14, p.crop.height() / 20, 80))
					.arg(assColor, outline, shadow).arg(qMax(12, p.crop.height() / 16));
			}
			QString text = c.text;
			text.replace('\\', "\\\\");
			text.replace('{', "\\{");
			text.replace('}', "\\}");
			text.replace('\n', "\\N");
			const auto position = p.subtitlePositions.value(c.track, QPointF(.5, .9375));
			if (position != QPointF(.5, .9375))
				text.prepend(QString("{\\an2\\pos(%1,%2)}").arg(qRound(position.x() * p.crop.width()))
					.arg(qRound(position.y() * p.crop.height())));
			events += QString("Dialogue: %1,%2,%3,%4,,0,0,0,,%5\n").arg(c.track)
				.arg(AssTime(c.begin), AssTime(c.end), name, text);
		}
		QFile f(temp.filePath("captions.ass"));
		const auto bytes = (ass + events).toUtf8();
		if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
			return Ui("无法生成字幕样式。");
	}
	const auto outputSize = options.outputSize(p.crop.size());
	if (p.arranged()) seek = 0;
	else if (stage && !segments.isEmpty() && segments.front().still < 0)
		seek = qMax<qint64>(0, segments.front().begin - 2000);
	QStringList inputs;
	if (seek > 0) inputs << "-ss" << Seconds(seek);
	inputs << "-i" << p.input;
	int nextInput = 1;
	QStringList filters;
	QString videos;
	for (int i = 0; i < segments.size(); ++i)
	{
		const auto r = segments[i];
		QString filter;
		if (r.still < 0)
			filter = QString("[0:v:0]trim=start=%1:end=%2,setpts=PTS-STARTPTS,crop=%3:%4:%5:%6")
				.arg(Seconds(r.begin - seek), Seconds(r.end - seek)).arg(p.crop.width()).arg(p.crop.height()).arg(p.crop.x()).arg(p.crop.y());
		else
		{
			const auto image = LoadEditStill(p.stills[r.still].image);
			if (image.isNull()) return Ui("无法读取插入的静止图片。");
			const auto path = temp.filePath(QString("still-%1.png").arg(i));
			if (!image.save(path, "PNG")) return Ui("无法准备静止图片。");
			inputs << "-loop" << "1" << "-framerate" << QString::number(rate, 'g', 10) << "-i" << path;
			filter = QString("[%1:v:0]trim=duration=%2,setpts=PTS-STARTPTS,scale=%3:%4:force_original_aspect_ratio=decrease:"
				"force_divisible_by=2,pad=%3:%4:(ow-iw)/2:(oh-ih)/2")
				.arg(nextInput++).arg(Seconds(r.end-r.begin)).arg(p.crop.width()).arg(p.crop.height());
		}
		filters << filter + QString(",format=yuv420p,setsar=1[v%1]").arg(i);
		videos += QString("[v%1]").arg(i);
	}
	QString videoFilters = videos + QString("concat=n=%1:v=1:a=0").arg(segments.size());
	if (stage) videoFilters += ",tpad=stop_mode=clone:stop_duration=1";
	videoFilters += QString(",fps=%1").arg(rate, 0, 'g', 10);
	if (stage && options.sequenceCanvas != p.crop.size())
		videoFilters += QString(",scale=%1:%2:force_original_aspect_ratio=decrease:force_divisible_by=2,"
								"pad=%1:%2:(ow-iw)/2:(oh-ih)/2,setsar=1")
			.arg(options.sequenceCanvas.width()).arg(options.sequenceCanvas.height());
	else if (!stage && outputSize != p.crop.size())
		videoFilters += QString(",scale=%1:%2:flags=lanczos,setsar=1").arg(outputSize.width()).arg(outputSize.height());
	if (hasCaptions && burnCaptions) videoFilters += ",ass=filename=captions.ass";
	filters << videoFilters + "[vout]";
	QStringList audioLabels, audioNames;
	const auto ducking = NarrationDucking(p);
	QVector<int> trackOrder = options.sequenceAudio;
	if (!stage)
		for (int t = 0; t < p.audio.size(); ++t) trackOrder.append(t);
	for (int slot = 0; slot < trackOrder.size(); ++slot)
	{
		const int t = trackOrder[slot];
		if (t < 0)
		{
			const auto label = QString("silent%1").arg(slot);
			filters << QString("anullsrc=r=48000:cl=stereo,atrim=duration=%1[%2]").arg(Seconds(length), label);
			audioLabels << label;
			audioNames << QString();
			continue;
		}
		const auto track = p.audio[t];
		if (!track.audible()) continue;
		const auto audioSeek = track.file.isEmpty() ? seek : 0;
		QString stream = QString("[0:a:%1]").arg(track.stream), align;
		if (!track.file.isEmpty())
		{
			stream = QString("[%1:a:0]").arg(nextInput++);
			inputs << "-i" << track.file;
			align = QString("atrim=duration=%1,asetpts=PTS-STARTPTS,adelay=%2:all=1,")
						.arg(Seconds(track.length)).arg(track.offset);
		}
		if (p.arranged())
		{
			QString pieces;
			int count = 0;
			const auto from = options.previewLength > 0 ? options.previewStart : qint64(0);
			for (const auto& clip : track.placed)
			{
				const auto begin = qMax(clip.begin, from), end = qMin(clip.end, from + length);
				if (begin >= end) continue;
				const auto start = clip.mediaStart + begin - clip.begin;
				const auto label = QString("[piece%1_%2]").arg(t).arg(count++);
				filters << stream + QString("atrim=start=%1:end=%2,"
					"aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,asetpts=N/SR/TB,volume=%3")
					.arg(Seconds(start), Seconds(start + end - begin)).arg(track.gain, 0, 'f', 4)
					+ (track.file.isEmpty() ? ducking.filter({begin, end}) : QString())
					+ QString(",adelay=%1:all=1").arg(begin - from) + label;
				pieces += label;
			}
			const auto label = QString("aout%1").arg(t);
			if (count == 0)
				filters << QString("anullsrc=r=48000:cl=stereo,atrim=duration=%1[%2]").arg(Seconds(length), label);
			else
				filters << pieces + (count > 1 ? QString("amix=inputs=%1:normalize=0,").arg(count) : QString()) +
					QString("apad=whole_len=%1,atrim=end_sample=%1,asetpts=N/SR/TB[%2]")
						.arg(length * 48).arg(label);
			audioLabels << label;
			audioNames << track.title;
			continue;
		}
		QString audible;
		for (auto r : track.kept)
		{
			if (!audible.isEmpty()) audible += '+';
			audible += QString("gte(t,%1)*lt(t,%2)").arg(Seconds(r.begin - audioSeek), Seconds(r.end - audioSeek));
		}
		if (audible.isEmpty()) audible = "0";
		QString pieces;
		for (int i = 0; i < segments.size(); ++i)
		{
			const auto r = segments[i];
			const auto label = QString("[a%1_%2]").arg(t).arg(i);
			if (r.still >= 0)
				filters << "anullsrc=r=48000:cl=stereo,atrim=duration=" + Seconds(r.end-r.begin) + label;
			else
				filters << stream + "aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo," + align +
					QString("apad,atrim=duration=%1,volume=%2,volume=0:enable='not(%3)',"
							"atrim=start=%4:end=%5,asetpts=PTS-STARTPTS")
								.arg(Seconds(p.duration - audioSeek)).arg(track.gain, 0, 'f', 4)
								.arg(audible, Seconds(r.begin - audioSeek), Seconds(r.end - audioSeek)) +
					(track.file.isEmpty() ? ducking.filter({r.begin, r.end}) : QString()) + label;
			pieces += label;
		}
		QString label = QString("aout%1").arg(t);
		filters << pieces + QString("concat=n=%1:v=0:a=1[%2]").arg(segments.size()).arg(label);
		audioLabels << label;
		audioNames << track.title;
	}
	if (!separateAudio && audioLabels.size() > 1)
	{
		QString labels;
		for (auto a : audioLabels)
			labels += '[' + a + ']';
		filters << labels + QString("amix=inputs=%1:normalize=0,alimiter=limit=0.95:level=false[amixed]")
								.arg(audioLabels.size());
		audioLabels = QStringList{"amixed"};
		audioNames = QStringList{Ui("混合声音")};
	}
	QFile graph(temp.filePath("filters.txt"));
	if (!graph.open(QIODevice::WriteOnly) || graph.write(filters.join(';').toUtf8()) < 0)
		return Ui("无法生成剪辑滤镜。");
	graph.close();
	QString selected = SelectVideoEncoder(encoder, error);
	if (selected.isEmpty())
		return error;
	QString partial = temp.filePath("result." + extension);
	QStringList args{"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-filter_complex_threads", "2"};
	args << inputs;
	const int subtitleInput = nextInput;
	int chapterInput = -1;
	for (const auto& file : subtitleFiles)
	{
		args << "-i" << file.second;
		++nextInput;
	}
	auto chapters = options.previewLength > 0 ? QVector<KRREditChapter>{} : p.outputChapters();
	if (!chapters.isEmpty())
	{
		// MP4/MOV readers can place the first chapter at zero. Give the leading interval its own chapter.
		if (extension != "mkv" && chapters.front().at > 0)
			chapters.prepend({0, Ui("片头")});
		const auto file = temp.filePath("chapters.ffmeta");
		if (!WriteEditChapters(file, chapters, duration))
			return Ui("无法写入成片章节。");
		chapterInput = nextInput;
		args << "-f" << "ffmetadata" << "-i" << file;
	}
	if (chapterInput >= 0 || !p.chapters.isEmpty())
		args << "-map_chapters" << QString::number(chapterInput);
	args << "-filter_complex_script" << temp.filePath("filters.txt") << "-map" << "[vout]";
	for (auto a : audioLabels)
		args << "-map" << '[' + a + ']';
	args << VideoEncoderArguments(selected, options.videoKbps, false);
	if (stage)
		args << "-bf" << "0" << "-video_track_timescale" << "90000" << "-c:a" << "pcm_s16le";
	else
		args << "-c:a" << "aac" << "-b:a" << QString::number(options.audioKbps) + "k";
	for (int i = 0; i < audioNames.size(); ++i)
		args << QString("-metadata:s:a:%1").arg(i) << "title=" + audioNames[i] << QString("-metadata:s:a:%1").arg(i)
			 << "handler_name=" + audioNames[i];
	if (!subtitleFiles.isEmpty())
	{
		for (int i = 0; i < subtitleFiles.size(); ++i)
		{
			const auto title = Ui("字幕 %1").arg(subtitleFiles[i].first + 1);
			args << "-map" << QString("%1:s:0").arg(subtitleInput + i)
				<< QString("-metadata:s:s:%1").arg(i) << "title=" + title
				<< QString("-metadata:s:s:%1").arg(i) << "handler_name=" + title;
		}
		args << "-c:s" << (extension == "mkv" ? "srt" : "mov_text");
	}
	if (!stage && extension != "mkv")
		args << "-movflags" << "+faststart";
	args << "-t" << Seconds(length) << partial;
	QProcess process;
	HideConsoleProcess(process, true);
	process.setWorkingDirectory(temp.path());
	process.start(FindFFmpeg(), args);
	if (!process.waitForStarted(10000))
		return Ui("无法启动时间轴编码器。");
	QElapsedTimer timer;
	timer.start();
	QByteArray errors;
	do
	{
		process.waitForFinished(100);
		errors = (errors + process.readAllStandardError()).right(6000);
		process.readAllStandardOutput();
		if ((cancel && cancel->load()) || timer.elapsed() > 7200000)
		{
			process.kill();
			process.waitForFinished(3000);
			return Ui("时间轴导出已取消或超时。");
		}
	} while (process.state() != QProcess::NotRunning);
	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
		return Ui("时间轴导出失败：") + QString::fromUtf8(errors);
	if (!QFile::rename(partial, output))
		return Ui("无法发布导出文件，请检查目标目录或文件占用。");
	return {};
}
