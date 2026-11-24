#include "editsequence.h"
#include "recordingconfig.h"
#include "uilanguage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>

namespace
{
constexpr qint64 ProjectBytes = 16 * 1024 * 1024;
constexpr qint64 MaxSequenceDuration = 24 * 60 * 60 * 1000;

QString Seconds(qint64 ms)
{
	return QString::number(ms / 1000.0, 'f', 3);
}

QString AssetIdentity(const QString& path)
{
	if (!QFileInfo(path).isFile()) return {};
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) return {};
	const auto size = file.size();
	constexpr qint64 sampleBytes = 64 * 1024;
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (size <= 3 * sampleBytes)
	{
		if (!hash.addData(&file)) return {};
	}
	else
	{
		for (auto offset : {qint64(0), (size - sampleBytes) / 2, size - sampleBytes})
		{
			if (!file.seek(offset)) return {};
			const auto sample = file.read(sampleBytes);
			if (sample.size() != sampleBytes) return {};
			hash.addData(sample);
		}
	}
	if (file.size() != size) return {};
	return QString::number(size) + ':' + QString::fromLatin1(hash.result().toHex());
}

QStringList ReferencedFiles(const QJsonObject& json)
{
	QStringList paths{json["input"].toString()};
	for (const auto& value : json["audio"].toArray()) paths << value.toObject()["file"].toString();
	for (const auto& value : json["stills"].toArray()) paths << value.toObject()["image"].toString();
	paths.removeAll(QString());
	paths.removeDuplicates();
	return paths;
}

bool Materialize(KRREditSequencePart& part, QString& error)
{
	const auto paths = ReferencedFiles(part.saved);
	const auto assets = part.saved["assets"].toObject();
	if (part.saved["version"].toInt() != 1 || !QDir::isAbsolutePath(part.source()) ||
		paths.size() != assets.size() || std::any_of(paths.begin(), paths.end(), [&](const auto& path)
			{ return !QDir::isAbsolutePath(path) || assets[path].toString().isEmpty(); }))
	{
		error = Ui("多素材工程中的素材清单无效。");
		return false;
	}
	part.unavailable = UnavailableSequenceFiles(part);
	part.edit.reset();
	if (!part.unavailable.isEmpty()) return true;
	KRREditProject edit;
	if (!KRREditProject::FromJson(part.saved, edit, error)) return false;
	part.edit = std::move(edit);
	return true;
}

bool WriteFile(const QString& path, const QByteArray& bytes)
{
	QSaveFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

void ReplaceFile(QJsonObject& json, const QString& oldPath, const QString& newPath)
{
	if (json["input"].toString() == oldPath) json["input"] = newPath;
	const QPair<const char*, const char*> references[] = {{"audio", "file"}, {"stills", "image"},
		{"captions", "dubbingFile"}, {"chapterNarration", "dubbingFile"}};
	for (const auto& fields : references)
	{
		if (!json.contains(fields.first)) continue;
		auto values = json[fields.first].toArray();
		for (int i = 0; i < values.size(); ++i)
		{
			auto value = values[i].toObject();
			if (value[fields.second].toString() == oldPath)
			{
				value[fields.second] = newPath;
				values[i] = value;
			}
		}
		json[fields.first] = values;
	}
}

bool ImportContainerExtras(const QJsonObject& container, KRREditProject& edit, QString& error)
{
	for (const auto& value : container["chapters"].toArray())
	{
		const auto chapter = value.toObject();
		bool ok = false;
		const auto seconds = chapter["start_time"].toString().toDouble(&ok);
		if (!ok || !std::isfinite(seconds) || seconds < 0 || seconds >= edit.duration / 1000.0) continue;
		auto title = chapter["tags"].toObject()["title"].toString().simplified().left(120);
		if (title.isEmpty()) title = Ui("章节 %1").arg(edit.chapters.size() + 1);
		QString ignored;
		edit.addChapter(qRound64(seconds * 1000), title, ignored);
	}
	edit.captions = ReadAdjacentEditSubtitles(edit.input, edit.duration);
	if (!edit.captions.isEmpty()) return true;
	const QStringList textCodecs{"subrip", "mov_text", "ass", "ssa", "webvtt", "text"};
	for (const auto& value : container["streams"].toArray())
	{
		const auto stream = value.toObject();
		if (stream["codec_type"].toString() != "subtitle" || !textCodecs.contains(stream["codec_name"].toString())) continue;
		QTemporaryDir temp;
		if (!temp.isValid())
		{
			error = Ui("无法为内嵌字幕创建临时目录。");
			return false;
		}
		const auto path = temp.filePath("captions.srt");
		if (!RunMediaProcess(FindFFmpeg(), {"-v", "error", "-nostdin", "-i", edit.input,
			"-map", QString("0:%1").arg(stream["index"].toInt()), "-c:s", "srt", path}, error, 30000, nullptr, true))
			return false;
		if (QFileInfo(path).size() == 0) continue;
		if (!ReadEditSubtitles(path, edit.captions, error)) return false;
		for (auto& cue : edit.captions) cue.end = qMin(cue.end, edit.duration);
		edit.captions.erase(std::remove_if(edit.captions.begin(), edit.captions.end(),
			[](const auto& cue) { return cue.begin >= cue.end; }), edit.captions.end());
		return true;
	}
	return true;
}

QString TrackRole(const KRREditTrack& track)
{
	if (track.chapterDubbing) return "chapter-dub";
	if (track.sentenceDubbing) return "sentence-dub";
	const auto title = track.title.simplified().toCaseFolded();
	if (track.file.isEmpty())
	{
		if (title == "system" || title == "system audio" || title == QStringLiteral("系统声音")) return "system";
		if (title == "mic" || title == "microphone" || title == QStringLiteral("麦克风")) return "mic";
		if (title.isEmpty() || title == Ui("声音 %1").arg(track.stream + 1).toCaseFolded() ||
			title == QStringLiteral("声音 %1").arg(track.stream + 1) || title == QString("audio %1").arg(track.stream + 1))
			return "source:" + QString::number(track.stream);
		return "source-title:" + title;
	}
	return "external:" + title;
}
}

QString KRREditSequencePart::source() const
{
	return saved["input"].toString();
}

QJsonObject KRREditSequencePart::snapshot() const
{
	if (!edit) return saved;
	auto json = edit->toJson();
	json["assets"] = saved["assets"];
	if (saved["highlight"].isObject()) json["highlight"] = saved["highlight"];
	return json;
}

bool ReadEditProjectJson(const QString& path, QJsonObject& json, QString& error)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > ProjectBytes)
	{
		error = Ui("无法读取剪辑工程，文件上限为 16 MiB。");
		return false;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject())
	{
		error = Ui("剪辑工程 JSON 无效。");
		return false;
	}
	json = document.object();
	return true;
}

bool MakeSequencePart(const KRREditProject& edit, KRREditSequencePart& part, QString& error)
{
	if (!edit.validate(error)) return false;
	auto json = edit.toJson();
	QJsonObject assets;
	for (const auto& path : ReferencedFiles(json))
	{
		const auto identity = AssetIdentity(path);
		if (identity.isEmpty() || !QDir::isAbsolutePath(path))
		{
			error = Ui("无法读取素材：") + path;
			return false;
		}
		assets[path] = identity;
	}
	json["assets"] = assets;
	part = {json, edit, {}};
	return true;
}

bool ImportSequencePart(const QString& path, KRREditSequencePart& part, QString& error)
{
	KRREditProject edit;
	if (QFileInfo(path).suffix().compare("json", Qt::CaseInsensitive) == 0)
	{
		QJsonObject json;
		if (!ReadEditProjectJson(path, json, error)) return false;
		if (json["kind"].toString() == "sequence")
		{
			error = Ui("多素材工程请使用「打开工程」。");
			return false;
		}
		if (!KRREditProject::FromJson(json, edit, error)) return false;
	}
	else
	{
		QJsonObject container;
		if (!ProbeEditSource(path, edit, error, &container) || !ImportContainerExtras(container, edit, error)) return false;
	}
	return MakeSequencePart(edit, part, error);
}

bool ReadEditSequence(const QJsonObject& json, KRREditSequence& sequence, QString& error)
{
	const auto values = json["parts"].toArray();
	if (json["kind"].toString() != "sequence" || json["version"].toInt() != 1 ||
		values.isEmpty() || values.size() > MaxSequenceParts)
	{
		error = Ui("多素材工程格式无效或版本不受支持。");
		return false;
	}
	KRREditSequence loaded;
	for (const auto& value : values)
	{
		KRREditSequencePart part{value.toObject()};
		if (!Materialize(part, error)) return false;
		loaded.append(std::move(part));
	}
	sequence = std::move(loaded);
	return true;
}

bool SaveEditSequence(const QString& path, const KRREditSequence& sequence, QString& error)
{
	if (sequence.isEmpty() || sequence.size() > MaxSequenceParts)
	{
		error = Ui("请先添加素材，单个工程最多包含 %1 段素材。").arg(MaxSequenceParts);
		return false;
	}
	QJsonArray parts;
	for (const auto& part : sequence) parts.append(part.snapshot());
	const auto json = QJsonDocument(QJsonObject{{"kind", "sequence"}, {"version", 1}, {"parts", parts}})
		.toJson(QJsonDocument::Compact);
	if (json.size() <= ProjectBytes && WriteFile(path, json)) return true;
	error = Ui("无法保存多素材工程，文件上限为 16 MiB。");
	return false;
}

QStringList UnavailableSequenceFiles(const KRREditSequencePart& part)
{
	QStringList unavailable;
	const auto assets = part.saved["assets"].toObject();
	for (auto i = assets.begin(); i != assets.end(); ++i)
		if (AssetIdentity(i.key()) != i.value().toString()) unavailable << i.key();
	return unavailable;
}

bool RelocateSequenceFile(KRREditSequencePart& part, const QString& oldPath, const QString& newPath, QString& error)
{
	auto saved = part.snapshot();
	auto assets = saved["assets"].toObject();
	const auto expected = assets[oldPath].toString();
	const auto path = QFileInfo(newPath).absoluteFilePath();
	if (expected.isEmpty() || AssetIdentity(path) != expected)
	{
		error = Ui("所选文件与原素材不匹配，请选择同一素材移动或改名后的文件。");
		return false;
	}
	assets.remove(oldPath);
	assets[path] = expected;
	saved["assets"] = assets;
	ReplaceFile(saved, oldPath, path);
	KRREditSequencePart candidate{saved};
	if (!Materialize(candidate, error)) return false;
	part = std::move(candidate);
	return true;
}

qint64 EditSequenceDuration(const KRREditSequence& sequence)
{
	qint64 length = 0;
	for (const auto& part : sequence)
		if (part.edit) length += part.edit->outputDuration();
	return length;
}

QString RenderEditSequence(const KRREditSequence& sequence, const QString& output, const QString& encoder,
	bool separateAudio, const std::atomic<bool>* cancel, const std::function<void(int, int)>& progress, bool preview,
	const KRRSequenceExportOptions& requested)
{
	if (sequence.isEmpty() || sequence.size() > MaxSequenceParts || QFileInfo(output).suffix().toLower() != "mp4")
		return Ui("请添加素材并选择 MP4 导出文件。");
	if (QFileInfo::exists(output)) return Ui("目标文件已存在，请选择新文件名。");
	const auto duration = EditSequenceDuration(sequence);
	if (duration > MaxSequenceDuration) return Ui("单个多素材成片最长为 24 小时。");
	QString error;
	QStringList audioTitles;
	QHash<QString, int> slotsByRole;
	QVector<QHash<int, int>> trackLayouts;
	for (const auto& part : sequence)
	{
		const auto unavailable = UnavailableSequenceFiles(part);
		if (!part.edit || !unavailable.isEmpty())
			return Ui("请先定位素材：") + (unavailable.isEmpty() ? part.source() : unavailable.join(", "));
		QHash<QString, int> occurrences;
		QHash<int, int> layout;
		for (int i = 0; i < part.edit->audio.size(); ++i)
		{
			const auto& track = part.edit->audio[i];
			const auto role = TrackRole(track);
			const auto key = role + QChar(0x1f) + QString::number(++occurrences[role]);
			if (!track.audible()) continue;
			if (!slotsByRole.contains(key))
			{
				slotsByRole[key] = audioTitles.size();
				audioTitles << track.title;
			}
			layout[slotsByRole.value(key)] = i;
		}
		trackLayouts.append(std::move(layout));
	}
	if (audioTitles.size() > 32 && separateAudio)
		return Ui("独立音轨超过 32 条，请选择混合音轨。");
	const auto selected = SelectVideoEncoder(encoder, error);
	if (selected.isEmpty()) return error;
	QTemporaryDir temp(QFileInfo(output).absolutePath() + "/.sequence-XXXXXX");
	if (!temp.isValid()) return Ui("无法在导出目录创建临时目录。");
	KRRTimelineExportOptions options;
	options.videoKbps = requested.videoKbps;
	options.sequenceCanvas = sequence.front().edit->crop.size();
	options.sequenceFps = sequence.front().edit->fps;
	if (qAbs(options.sequenceFps - qRound(options.sequenceFps)) < 0.001)
		options.sequenceFps = qRound(options.sequenceFps);
	if (preview)
	{
		const auto canvas = options.sequenceCanvas;
		if (canvas.width() > 960 || canvas.height() > 540)
		{
			const auto scaled = canvas.scaled(960, 540, Qt::KeepAspectRatio);
			options.sequenceCanvas = QSize(qMax(2, scaled.width() / 2 * 2), qMax(2, scaled.height() / 2 * 2));
		}
		options.sequenceFps = qMin(30.0, options.sequenceFps);
		options.videoKbps = 1800;
	}
	QVector<KRREditCaption> captions;
	QVector<KRREditChapter> chapters;
	qint64 offset = 0;
	QByteArray manifest = "ffconcat version 1.0\n";
	for (int i = 0; i < sequence.size(); ++i)
	{
		if (cancel && cancel->load()) return Ui("多素材导出已取消。");
		if (progress) progress(i, sequence.size());
		auto edit = *sequence[i].edit;
		if (!requested.burnCaptions)
		{
			for (auto caption : edit.outputCaptions(KRRSubtitleMode::Bilingual))
			{
				caption.begin += offset;
				caption.end += offset;
				captions.append(std::move(caption));
			}
			edit.captions.clear();
			for (auto& still : edit.stills) still.caption.clear();
		}
		for (auto chapter : edit.outputChapters())
		{
			chapter.at += offset;
			chapters.append(std::move(chapter));
		}
		edit.chapters.clear();
		options.sequenceAudio.clear();
		for (int slot = 0; slot < audioTitles.size(); ++slot)
		{
			const int track = trackLayouts[i].value(slot, -1);
			if (separateAudio || track >= 0) options.sequenceAudio.append(track);
		}
		if (!audioTitles.isEmpty() && options.sequenceAudio.isEmpty()) options.sequenceAudio.append(-1);
		const auto name = QString("part-%1.mov").arg(i);
		error = RenderEditProject(edit, temp.filePath(name), selected, separateAudio, requested.burnCaptions, cancel, options);
		if (!error.isEmpty())
			return Ui("第 %1 段 %2：").arg(i + 1).arg(QFileInfo(edit.input).fileName()) + error;
		const auto length = edit.outputDuration();
		manifest += "file '" + name.toLatin1() + "'\nduration " + Seconds(length).toLatin1() + '\n';
		offset += length;
	}
	if (progress) progress(sequence.size(), sequence.size());
	const auto list = temp.filePath("parts.ffconcat");
	if (!WriteFile(list, manifest)) return Ui("无法写入素材顺序。");
	const auto partial = temp.filePath("result.mp4");
	QStringList args{"-v", "error", "-nostdin", "-f", "concat", "-safe", "1", "-i", list};
	int nextInput = 1, chapterInput = -1;
	if (!captions.isEmpty())
	{
		const auto srt = temp.filePath("captions.srt");
		if (!WriteEditSubtitles(srt, captions, error)) return error;
		args << "-i" << srt;
	}
	const int subtitleInput = captions.isEmpty() ? -1 : nextInput++;
	if (!chapters.isEmpty())
	{
		if (chapters.front().at > 0) chapters.prepend({0, Ui("片头")});
		const auto metadata = temp.filePath("chapters.ffmeta");
		if (!WriteEditChapters(metadata, chapters, duration)) return Ui("无法写入成片章节。");
		chapterInput = nextInput;
		args << "-f" << "ffmetadata" << "-i" << metadata;
	}
	args << "-map" << "0:v:0";
	if (separateAudio && audioTitles.size() > 1)
	{
		QStringList filters, stems;
		QString mixInputs;
		for (int i = 0; i < audioTitles.size(); ++i)
		{
			const auto stem = QString("[stem%1]").arg(i);
			const auto mix = QString("[mix%1]").arg(i);
			filters << QString("[0:a:%1]aresample=async=1:first_pts=0,asplit=2").arg(i) + stem + mix;
			stems << stem;
			mixInputs += mix;
		}
		filters << mixInputs + QString("amix=inputs=%1:normalize=0,alimiter=limit=0.95:level=false:latency=1[fullmix]")
			.arg(audioTitles.size());
		args << "-filter_complex_threads" << "2" << "-filter_complex" << filters.join(';') << "-map" << "[fullmix]";
		for (const auto& stem : stems) args << "-map" << stem;
		audioTitles.prepend(Ui("WPS Recorder 成片混音"));
	}
	else if (!audioTitles.isEmpty())
		args << "-map" << "0:a?" << "-af" << "aresample=async=1:first_pts=0";
	args << "-c:v" << "copy" << "-c:a" << "aac" << "-b:a" << (preview ? "128k" : "192k")
		<< "-map_metadata:g" << "-1" << "-map_chapters" << QString::number(chapterInput);
	if (!separateAudio && !audioTitles.isEmpty()) audioTitles = QStringList{Ui("混合声音")};
	for (int i = 0; i < audioTitles.size(); ++i)
		args << QString("-metadata:s:a:%1").arg(i) << "handler_name=" + audioTitles[i]
			<< QString("-disposition:a:%1").arg(i) << (i == 0 ? "default" : "0");
	if (subtitleInput >= 0)
		args << "-map" << QString("%1:s:0").arg(subtitleInput) << "-c:s" << "mov_text"
			<< "-metadata:s:s:0" << "handler_name=" + Ui("剪辑字幕");
	args << "-movflags" << "+faststart" << "-t" << Seconds(duration) << partial;
	if (!RunMediaProcess(FindFFmpeg(), args, error, 7200000, cancel, true)) return error;
	if (!QFile::rename(partial, output)) return Ui("无法发布导出文件，请检查目标目录或文件占用。");
	return {};
}
