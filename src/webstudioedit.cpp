#include "webstudio.h"

#include "clipeditor.h"
#include "embeddedcli.h"
#include "mainwindow.h"
#include "recordingassistant.h"
#include "recordingmarkers.h"
#include "sequencesmartcut.h"
#include "speechservices.h"
#include "subtitlereview.h"
#include "dubbing.h"
#include "uilanguage.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTextCodec>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace
{
qint64 Millis(const QJsonValue& input, qint64 max = 86400000)
{
	const auto seconds = input.toDouble();
	return std::isfinite(seconds) ? qRound64(qBound(0., seconds, max / 1000.) * 1000.) : 0;
}

QString NewId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool SaveJson(const QString& file, const QJsonObject& object)
{
	QSaveFile saved(file);
	const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
	return saved.open(QIODevice::WriteOnly) && saved.write(bytes) == bytes.size() && saved.commit();
}

QString DraftFor(const QString& id)
{
	return id.isEmpty() ? QString{} : DataDirectory() + "/studio-drafts/" + id + ".krrproj";
}

QString RateEncoder(const QString& label)
{
	if (label.contains("NVENC", Qt::CaseInsensitive)) return "h264_nvenc";
	if (label.contains("AMF", Qt::CaseInsensitive)) return "h264_amf";
	if (label.contains("QSV", Qt::CaseInsensitive)) return "h264_qsv";
	if (label.contains("x264", Qt::CaseInsensitive)) return "libx264";
	return "auto";
}

QJsonObject ReadVideoInfo(const QString& file, QString& error)
{
	KRREditProject project;
	QJsonObject container;
	if (!ProbeEditSource(file, project, error, &container)) return {};
	QJsonObject video, format = container["format"].toObject();
	for (const auto& stream : container["streams"].toArray())
		if (stream.toObject()["codec_type"] == "video") { video = stream.toObject(); break; }
	const auto tags = format["tags"].toObject();
	return {{"duration", project.duration / 1000.}, {"width", project.size.width()}, {"height", project.size.height()},
		{"frameRate", project.fps}, {"codec", video["codec_name"].toString()}, {"bitrate", format["bit_rate"].toString().toDouble() / 1000.},
		{"frames", video["nb_frames"].toString().toDouble()}, {"audioTracks", project.audio.size()},
		{"encoder", tags["encoder"].toString()}};
}
}

QJsonObject KRRWebStudio::inspectMedia(const QString& file, QString& error)
{
	if (file.isEmpty()) return {};
	if (!m_inspected.contains(file))
	{
		const auto metadata = ReadVideoInfo(file, error);
		if (metadata.isEmpty()) return {};
		m_inspected[file] = metadata;
	}
	return libraryEntry(file);
}

QJsonObject KRRWebStudio::editView(const KRREditProject& edit, const QString& path, const QString& id)
{
	QJsonArray tracks, video, markers, stills;
	double position = 0;
	for (const auto& segment : edit.segments())
	{
		const double length = (segment.end - segment.begin) / 1000.;
		QJsonObject item{{"id", NewId()}, {"at", position}, {"length", length}, {"source", segment.begin / 1000.}};
		if (segment.still >= 0)
		{
			const auto still = edit.stills.value(segment.still);
			const auto stillId = QString("still-%1").arg(segment.still);
			const auto url = still.image.isEmpty() ? QString("assets/intermission.png") : m_surface->localResource(still.image);
			if (!url.isEmpty()) m_tracks[stillId] = still.image;
			stills.append(QJsonObject{{"id", stillId}, {"image", url}, {"name", still.caption.isEmpty() ? Ui("静止画面") : still.caption}});
			item["stillId"] = stillId;
			item["name"] = still.caption;
			item["source"] = still.at / 1000.;
		}
		video.append(item);
		position += length;
	}
	tracks.append(QJsonObject{{"id", "V1"}, {"kind", "video"}, {"name", Ui("主视频")}, {"clips", video}});
	for (int i = 0; i < edit.audio.size(); ++i)
	{
		const auto& audio = edit.audio[i];
		QJsonArray clips;
		for (const auto& chunk : edit.audioClips(i))
			clips.append(QJsonObject{{"id", NewId()}, {"at", chunk.begin / 1000.}, {"length", (chunk.end - chunk.begin) / 1000.},
				{"source", chunk.mediaStart / 1000.}, {"name", audio.title}});
		const auto trackId = QString("A%1").arg(i + 1);
		const auto url = m_surface->localResource(audio.file);
		auto resource = url.section('/', -1);
		if (!resource.isEmpty()) m_tracks[resource] = audio.file;
		QJsonObject payload{{"id", trackId}, {"kind", "audio"}, {"name", audio.title}, {"clips", clips}, {"index", i},
			{"stream", audio.stream}, {"gain", audio.gain}, {"resource", resource}, {"src", url}, {"muted", !audio.enabled},
			{"chapterDubbing", audio.chapterDubbing}, {"originalGain", audio.originalGain}};
		if (audio.chapterDubbing)
		{
			QJsonArray speech;
			for (const auto& range : audio.speech)
				speech.append(QJsonObject{{"at", range.begin / 1000.}, {"length", (range.end - range.begin) / 1000.}});
			payload["speech"] = speech;
		}
		tracks.append(payload);
	}
	const auto captions = edit.outputCaptions(KRRSubtitleMode::Original, true);
	for (int i = 0; i < edit.subtitlePositions.size(); ++i)
	{
		QJsonArray clips;
		for (const auto& cue : captions)
		{
			if (cue.track != i) continue;
			QJsonObject clip{{"id", NewId()}, {"at", cue.begin / 1000.}, {"length", (cue.end - cue.begin) / 1000.},
				{"name", cue.text}, {"translation", cue.translation}, {"pronunciation", cue.pronunciation}};
			if (cue.style) clip["style"] = cue.style->toJson();
			clips.append(clip);
		}
		const auto point = edit.subtitlePositions[i];
		QJsonObject track{{"id", QString("T%1").arg(i + 1)}, {"kind", "subtitle"}, {"name", Ui("字幕 %1").arg(i + 1)},
			{"x", point.x()}, {"y", point.y()}, {"muted", edit.hiddenSubtitleTracks.contains(i)}, {"clips", clips}};
		if (edit.subtitleTrackStyles.contains(i)) track["style"] = edit.subtitleTrackStyles.value(i).toJson();
		tracks.append(track);
	}
	for (const auto& chapter : edit.outputChapters())
		markers.append(QJsonObject{{"at", chapter.at / 1000.}, {"name", chapter.title}});
	const auto crop = edit.crop;
	return {{"id", id}, {"path", path}, {"tracks", tracks}, {"trackOrder", QJsonArray::fromStringList(edit.trackOrder)},
		{"markers", markers}, {"stills", stills},
		{"subtitleSize", edit.subtitleStyle.pixels}, {"subtitleColor", edit.subtitleStyle.color.name()},
		{"subtitleFont", edit.subtitleStyle.font}, {"subtitleOpacity", qRound(edit.subtitleStyle.color.alphaF() * 100)},
		{"chapterDrafts", edit.chapterDrafts},
		{"crop", QJsonArray{crop.x(), crop.y(), crop.width(), crop.height()}}};
}

bool KRRWebStudio::saveDraft()
{
	if (m_edit.input.isEmpty() || m_pendingEdit.isEmpty()) return true;
	const auto draft = DraftFor(m_editId);
	return QDir().mkpath(QFileInfo(draft).absolutePath()) && SaveJson(draft, m_edit.toJson());
}

bool KRRWebStudio::editCommand(const QString& name, const QString& request, const QJsonObject& data)
{
	auto& w = m_window;
	const auto done = [this, &request](const QJsonValue& result = {}, const QString& error = {}) { reply(request, result, UiDiagnostic(error)); };
	const auto id = data["id"].toString();
	if (name == "choose-video" || name == "inspect-video")
	{
		const bool choosing = name == "choose-video";
		const auto file = choosing ? openMediaDialog("video") : mediaFile(id);
		if (file.isEmpty()) { done(); return true; }
		if (!choosing)
		{
			if (m_inspected.contains(file)) { done(libraryEntry(file)); return true; }
			background([file]
			{
				QString error;
				const auto metadata = ReadVideoInfo(file, error);
				return qMakePair(metadata, error);
			}, [this, request, file](const auto& result)
			{
				if (!result.first.isEmpty()) m_inspected[file] = result.first;
				reply(request, result.first.isEmpty() ? QJsonObject{} : libraryEntry(file), UiDiagnostic(result.second));
			});
			return true;
		}
		QString error;
		auto asset = inspectMedia(file, error);
		if (choosing && !asset.isEmpty())
		{
			w.openPreview(file);
		}
		done(asset, error);
	}
	else if (name == "reveal-media")
	{
		const auto file = mediaFile(id);
		if (!file.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
		done();
	}
	else if (name == "remove-video")
	{
		if (!data["confirmed"].toBool()) { done({}, Ui("请确认移到回收站。")); return true; }
		const auto file = mediaFile(id);
		if (file.isEmpty() || !QFileInfo(file).isFile()) { done({}, Ui("视频已不存在。")); return true; }
		const auto inEdit = [&]
		{
			return m_jobs.isBusy() || file.compare(m_edit.input, Qt::CaseInsensitive) == 0 ||
				std::any_of(m_edit.audio.cbegin(), m_edit.audio.cend(), [&](const auto& track)
				{ return track.file.compare(file, Qt::CaseInsensitive) == 0; });
		};
		if (inEdit()) { done({}, Ui("请先关闭正在使用此素材的工程或等待导出结束。")); return true; }
		QString error, recovered;
		m_surface->releaseResource(id);
		if (!w.removeRecording(file, error, &recovered))
		{
			if (m_surface) m_surface->localResource(file);
			done({}, error);
			return true;
		}
		m_media.remove(id);
		m_inspected.remove(file);
		done(QJsonObject{{"removed", true}, {"entries", library()}, {"recoveryPath", recovered}});
	}
	else if (name == "marks")
	{
		const auto file = mediaFile(id);
		if (file.isEmpty()) { done({}, Ui("请先选择录像。")); return true; }
		QJsonArray markers;
		for (const auto& value : data["marks"].toArray())
		{
			const auto mark = value.toObject();
			markers.append(QJsonObject{{"at_ms", Millis(mark["at"], 604800000)}, {"label", mark["name"].toString().left(120)}});
			if (markers.size() >= 500) break;
		}
		QString error;
		WriteRecordingMarkers(RecordingMarkersPath(file), markers, error);
		done({}, error);
	}
	else if (name == "browser-video")
	{
		const auto file = mediaFile(id);
		if (file.isEmpty()) { done({}, Ui("找不到录像。")); return true; }
		const auto sourceTime = QFileInfo(file).lastModified().toMSecsSinceEpoch();
		const auto target = m_cache.filePath(QString("%1-%2-preview.mp4").arg(id).arg(sourceTime));
		if (QFileInfo(target).size() > 0)
		{
			done(m_surface->localResource(target));
			return true;
		}
		const auto temporary = m_cache.filePath(NewId() + ".mp4");
		background([file, target, temporary]
		{
			QString error;
			if (!RunMediaProcess(FindFFmpeg(), {"-nostdin", "-v", "error", "-i", file, "-map", "0:v:0", "-map", "0:a:0?",
				"-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "17",
				"-c:a", "aac", "-movflags", "+faststart", temporary}, error, 600000))
			{
				QFile::remove(temporary);
				return error;
			}
			if (!QFile::rename(temporary, target)) QFile::remove(temporary);
			return QFileInfo(target).size() > 0 ? QString{} : Ui("无法生成兼容预览。");
		}, [this, request, target](const QString& error)
		{ reply(request, m_surface->localResource(target), UiDiagnostic(error)); });
	}
	else if (name == "audio-preview")
	{
		const auto input = mediaFile(id);
		const int stream = data["stream"].toInt(-1);
		if (input.isEmpty() || stream < 0 || stream >= 16) { done(); return true; }
		const auto sourceTime = QFileInfo(input).lastModified().toMSecsSinceEpoch();
		const auto target = m_cache.filePath(QString("%1-%2-%3.m4a").arg(id).arg(sourceTime).arg(stream));
		if (QFileInfo(target).size() > 0) { done(m_surface->localResource(target)); return true; }
		const auto temporary = m_cache.filePath(NewId() + ".m4a");
		background([input, target, temporary, stream]
		{
			QString error;
			if (!RunMediaProcess(FindFFmpeg(), {"-nostdin", "-v", "error", "-n", "-i", input,
				"-map", QString("0:a:%1").arg(stream), "-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart", temporary}, error, 420000))
			{
				QFile::remove(temporary);
				return error;
			}
			if (!QFile::rename(temporary, target)) QFile::remove(temporary);
			return QFileInfo(target).size() > 0 ? QString{} : Ui("无法准备音轨预览。");
		}, [this, request, target](const QString& error) { reply(request, m_surface->localResource(target), UiDiagnostic(error)); });
	}
	else if (name == "editor-open")
	{
		QString path = m_projects.value(data["project"].toString());
		if (path.isEmpty() && QFileInfo(data["path"].toString()).suffix().compare("krrproj", Qt::CaseInsensitive) == 0)
			path = data["path"].toString();
		if (data["pick"].toBool()) path = openMediaDialog("project");
		const auto file = mediaFile(id);
		if (path.isEmpty() && file.isEmpty()) { done(); return true; }
		if (m_jobs.isBusy()) { done({}, Ui("请等待当前导出完成。")); return true; }
		saveDraft();
		QString error;
		KRREditProject edit;
		if (!path.isEmpty())
		{
			QJsonObject content;
			if (!ReadEditProjectJson(path, content, error) || !KRREditProject::FromJson(content, edit, error))
			{ done({}, error); return true; }
		}
		else if (QFileInfo::exists(DraftFor(id)))
		{
			QJsonObject draft;
			ReadEditProjectJson(DraftFor(id), draft, error);
			if (!KRREditProject::FromJson(draft, edit, error)) edit = {};
		}
		if (edit.input.isEmpty())
		{
			if (!ProbeEditSource(file, edit, error)) { done({}, error); return true; }
			edit.captions = ReadAdjacentEditSubtitles(file, edit.duration);
			for (const auto& v : ReadRecordingMarkers(file))
			{
				const auto mark = v.toObject();
				edit.chapters.append({mark["at_ms"].toVariant().toLongLong(), mark["label"].toString()});
			}
		}
		error.clear();
		auto asset = inspectMedia(edit.input, error);
		if (asset.isEmpty()) { done({}, error); return true; }
		if (!path.isEmpty() && !data["project"].toString().isEmpty()) m_projects[data["project"].toString()] = path;
		m_editId = asset["id"].toString();
		m_edit = edit;
		m_pendingEdit = {};
		m_projectPath = path;
		done(QJsonObject{{"asset", asset}, {"edit", editView(edit, path, m_editId)}});
	}
	else if (name == "editor-change")
	{
		if (m_edit.input.isEmpty() || id != m_editId) { done({}, Ui("当前工程已切换。")); return true; }
		const auto timeline = data["timeline"].toObject();
		const auto tracks = timeline["tracks"].toArray();
		if (tracks.isEmpty() || tracks.size() > 30) { done({}, Ui("轨道数量不受支持。")); return true; }
		auto edit = m_edit;
		if (timeline["trackOrder"].isArray()) edit.setTrackOrder(timeline["trackOrder"].toArray());
		edit.orderedVideo.clear(); edit.splits.clear(); edit.stills.clear();
		edit.chapters.clear(); edit.audio.clear(); edit.captions.clear();
		edit.subtitlePositions.clear(); edit.hiddenSubtitleTracks.clear(); edit.subtitleTrackStyles.clear();
		const auto settings = data["settings"].toObject();
		if (settings["chapterDrafts"].isObject()) edit.chapterDrafts = settings["chapterDrafts"].toObject();
		const auto crop = settings["cropRect"].toArray();
		if (crop.size() == 4)
			edit.crop = FitEditCrop(QRect(crop[0].toInt(), crop[1].toInt(), crop[2].toInt(), crop[3].toInt()), edit.size, {});
		edit.cropRatio = {};
		auto subtitleStyle = edit.subtitleStyle;
		const double alpha = subtitleStyle.color.alphaF();
		subtitleStyle.font = settings["subtitleFont"].toString(subtitleStyle.font);
		subtitleStyle.pixels = settings["subtitleSize"].toInt(subtitleStyle.pixels);
		subtitleStyle.color = QColor(settings["subtitleColor"].toString(subtitleStyle.color.name()));
		if (subtitleStyle.color.isValid())
			subtitleStyle.color.setAlphaF(qBound(0., settings["subtitleOpacity"].toDouble(alpha * 100.) / 100., 1.));
		if (subtitleStyle.valid()) edit.subtitleStyle = subtitleStyle;
		const auto video = tracks[0].toObject()["clips"].toArray();
		qint64 sourceAnchor = 0;
		for (const auto& v : video)
		{
			const auto clip = v.toObject();
			const auto start = Millis(clip["source"], edit.duration);
			const auto length = Millis(clip["length"]);
			const auto still = clip["stillId"].toString();
			if (length <= 0) continue;
			if (!still.isEmpty())
			{
				const auto key = clip["resource"].toString();
				auto path = m_tracks.value(key.isEmpty() ? still : key);
				const auto builtin = QHash<QString, QString>{{"builtin-alpine", "alpine-lake.png"}, {"builtin-study", "study-at-dusk.png"},
					{"builtin-intermission", "intermission.png"}}.value(still);
				if (!builtin.isEmpty()) path = QCoreApplication::applicationDirPath() + "/studio/assets/" + builtin;
				edit.stills.append({qMin(sourceAnchor, edit.duration - 1), length, path, clip["name"].toString()});
			}
			else if (start < edit.duration)
			{
				sourceAnchor = qMin(edit.duration, start + length);
				edit.orderedVideo.append({start, sourceAnchor});
			}
		}
		edit.kept = MergeEditRanges(edit.orderedVideo);
		const auto length = edit.outputDuration();
		for (int i = 1; i < tracks.size(); ++i)
		{
			const auto track = tracks[i].toObject();
			if (track["kind"] != "audio") continue;
			auto audio = track["index"].toInt(-1) >= 0 ? m_edit.audio.value(track["index"].toInt()) : KRREditTrack{};
			const auto resource = track["resource"].toString();
			if (!resource.isEmpty() && trackFile(resource) != audio.file)
			{
				QString error;
				if (!ProbeEditAudio(trackFile(resource), 0, edit.duration, audio, error)) { done({}, error); return true; }
			}
			if (audio.file.isEmpty() && !track.contains("stream")) continue;
			audio.title = track["name"].toString().left(100);
			audio.enabled = !track["muted"].toBool();
			audio.gain = qBound(0., track["gain"].toDouble(1.), 1.);
			audio.placed.clear();
			for (const auto& v : track["clips"].toArray())
			{
				const auto clip = v.toObject();
				const auto at = Millis(clip["at"], length);
				const auto count = qMin(length - at, Millis(clip["length"]));
				const auto start = Millis(clip["source"]);
				if (count > 0) audio.placed.append({at, at + count, start});
			}
			std::stable_sort(audio.placed.begin(), audio.placed.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
			if (track["chapterDubbing"].toBool() && !audio.file.isEmpty())
			{
				QVector<KRREditRange> speech;
				for (const auto& value : track["speech"].toArray())
				{
					const auto range = value.toObject();
					const auto at = Millis(range["at"], audio.length);
					const auto end = qMin(audio.length, at + Millis(range["length"]));
					if (end > at) speech.append({at, end});
				}
				audio.speech = MergeEditRanges(speech);
				audio.chapterDubbing = !audio.speech.isEmpty();
				audio.sentenceDubbing = audio.chapterDubbing;
				audio.originalGain = qBound(0., track["originalGain"].toDouble(1.), 1.);
			}
			edit.audio.append(audio);
		}
		for (int i = 1; i < tracks.size(); ++i)
		{
			const auto track = tracks[i].toObject();
			if (track["kind"] != "subtitle" || edit.subtitlePositions.size() == 16) continue;
			const int index = edit.subtitlePositions.size();
			edit.subtitlePositions.append(QPointF(qBound(.01, track["x"].toDouble(.5), .99),
				qBound(.01, track["y"].toDouble(.9375 - .07 * index), .99)));
			if (track["muted"].toBool()) edit.hiddenSubtitleTracks.insert(index);
			if (track["style"].isObject())
				edit.subtitleTrackStyles[index] = KRREditSubtitleStyle::fromJson(track["style"].toObject());
			for (const auto& v : track["clips"].toArray())
			{
				const auto clip = v.toObject();
				KRREditCaption caption;
				caption.begin = Millis(clip["at"], length);
				caption.end = qMin(length, caption.begin + Millis(clip["length"]));
				caption.text = clip["name"].toString().left(8192);
				caption.translation = clip["translation"].toString().left(8192);
				caption.pronunciation = clip["pronunciation"].toString().left(8192);
				caption.track = index;
				if (clip["style"].isObject()) caption.style = KRREditSubtitleStyle::fromJson(clip["style"].toObject());
				if (caption.begin < caption.end && (!caption.text.isEmpty() || !caption.translation.isEmpty()) &&
					edit.captionFits(caption)) edit.captions.append(caption);
			}
		}
		if (edit.subtitlePositions.isEmpty()) edit.subtitlePositions.append({.5, .9375});
		for (const auto& v : timeline["markers"].toArray())
		{
			const auto marker = v.toObject();
			QString ignored;
			edit.addChapter(Millis(marker["at"], length), marker["name"].toString().left(120), ignored);
		}
		QString error;
		if (!edit.validate(error)) { done({}, error); return true; }
		m_edit = std::move(edit);
		m_pendingEdit = timeline;
		done({}, saveDraft() ? QString{} : Ui("无法自动保存剪辑草稿。请检查存储路径后重试。"));
	}
	else if (name == "editor-save")
	{
		if (m_edit.input.isEmpty()) { done({}, Ui("请先选择剪辑工程。")); return true; }
		auto path = m_projectPath;
		if (path.isEmpty() || data["saveAs"].toBool())
			path = QFileDialog::getSaveFileName(&w, Ui("保存剪辑工程"),
				QFileInfo(m_edit.input).path() + "/" + QFileInfo(m_edit.input).completeBaseName() + ".krrproj", Ui("剪辑工程 (*.krrproj)"));
		if (path.isEmpty()) { done(); return true; }
		if (!path.endsWith(".krrproj", Qt::CaseInsensitive)) path += ".krrproj";
		if (!SaveJson(path, m_edit.toJson())) { done({}, Ui("无法保存剪辑工程。")); return true; }
		m_projectPath = path;
		done(path);
	}
	else if (name == "export")
	{
		if (m_jobs.isBusy()) { done({}, Ui("已有导出任务。")); return true; }
		const bool edited = data["kind"] == "editor";
		const auto source = edited ? m_edit.input : mediaFile(id);
		const auto folder = data["folder"].toString();
		const auto basename = data["name"].toString().trimmed();
		const auto ext = data["format"].toString("mp4").toLower();
		const bool supported = QStringList{"mp4", "mkv", "mov"}.contains(ext) ||
			(!edited && QStringList{"webm", "gif", "m4a", "mp3", "wav"}.contains(ext));
		if (source.isEmpty() || !QFileInfo(folder).isDir() || basename.isEmpty() || QFileInfo(basename).fileName() != basename ||
			!supported)
		{ done({}, Ui("请选择素材和有效的保存路径。")); return true; }
		const auto target = QDir(folder).filePath(basename + "." + ext);
		if (QFileInfo::exists(target)) { done({}, Ui("该文件名已存在。")); return true; }
		const auto encoder = RateEncoder(data["encoder"].toString());
		if (edited)
		{
			KRRTimelineExportOptions options;
			options.maxHeight = data["height"].toInt();
			options.videoKbps = data["bitrate"].toInt(6000);
			options.audioKbps = data["audioBitrate"].toInt(192);
			if (data["selected"].toBool())
			{
				options.previewStart = Millis(data["start"]);
				options.previewLength = Millis(data["length"]);
			}
			if (!m_jobs.renderTimeline(m_edit, target, encoder, !data["mix"].toBool(), data["burn"].toBool(true), options))
			{ done({}, Ui("无法启动导出。")); return true; }
		}
		else if (!m_jobs.exportMedia(source, target, 0, -1, KRRExportOptions{encoder, data["denoise"].toBool(),
			trackFile(data["subtitle"].toString()), data["copy"].toBool()}))
		{ done({}, Ui("无法启动导出。")); return true; }
		auto connection = std::make_shared<QMetaObject::Connection>();
		*connection = connect(&m_jobs, &KRRMediaEditor::completed, this, [this, request, connection](const QString& path, const QString& error)
		{
			disconnect(*connection);
			reply(request, path, UiDiagnostic(error));
			if (error.isEmpty()) event("library", library());
		});
	}
	else if (name == "subtitle-style-fonts")
	{
		const QFontDatabase database;
		QStringList families;
		for (const auto& family : database.families())
		{
			if (family.startsWith('@') || database.isPrivateFamily(family)) continue;
			if (KRREditSubtitleStyle{family, 0, Qt::white}.valid()) families.append(family);
		}
		done(QJsonArray::fromStringList(families));
	}
	else if (name == "subtitle-style-import")
	{
		const auto path = QFileDialog::getOpenFileName(&w, Ui("导入字幕样式"), w.m_directory->text(),
			Ui("字幕样式 (*.json *.ass)"));
		if (path.isEmpty()) { done(); return true; }
		KRREditSubtitleStyle style;
		QString error;
		if (ReadEditSubtitleStyle(path, style, error)) done(style.toJson());
		else done({}, error);
	}
	else if (name == "chapter-script-import")
	{
		const auto path = QFileDialog::getOpenFileName(&w, Ui("导入章节稿件"), w.m_directory->text(),
			Ui("文本稿件 (*.txt *.md)"));
		if (path.isEmpty()) { done(); return true; }
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly) || file.size() > 240008)
		{ done({}, Ui("无法读取稿件，文件最多可包含 60000 字。")); return true; }
		const auto bytes = file.read(240009);
		const auto codec = QTextCodec::codecForUtfText(bytes, QTextCodec::codecForName("UTF-8"));
		QTextCodec::ConverterState conversion;
		auto text = codec->toUnicode(bytes.constData(), bytes.size(), &conversion);
		if (conversion.invalidChars || conversion.remainingChars || text.contains(QChar(0)))
		{ done({}, Ui("稿件文件需使用 UTF-8 或带 BOM 的 UTF-16 编码。")); return true; }
		if (text.startsWith(QChar::ByteOrderMark)) text.remove(0, 1);
		if (text.size() > 60000 || text.trimmed().isEmpty())
		{ done({}, Ui("请输入有效稿件，最多 60000 字。")); return true; }
		done(text);
	}
	else if (name == "srt-import")
	{
		const auto path = openMediaDialog("srt");
		QVector<KRREditCaption> lines;
		QString error;
		QJsonArray result;
		if (!path.isEmpty() && ReadEditSubtitles(path, lines, error))
			for (const auto& cue : lines) result.append(QJsonObject{{"id", NewId()}, {"at", cue.begin / 1000.},
				{"length", (cue.end - cue.begin) / 1000.}, {"name", cue.text}});
		done(result, error);
	}
	else if (name == "select-srt")
	{
		const auto path = openMediaDialog("srt");
		if (path.isEmpty()) { done(); return true; }
		const auto key = NewId();
		m_tracks[key] = path;
		done(QJsonObject{{"id", key}, {"name", QFileInfo(path).fileName()}});
	}
	else if (name == "save-still")
	{
		const auto encoded = data["data"].toString();
		const auto separator = encoded.indexOf(',');
		const auto bytes = QByteArray::fromBase64(encoded.mid(separator + 1).toLatin1());
		QImage image;
		if (separator < 0 || bytes.size() > 1500000 || !image.loadFromData(bytes) || image.width() > 4096 || image.height() > 4096)
		{ done({}, Ui("无法保存静止画面。")); return true; }
		const auto target = DataDirectory() + "/studio-stills/" + NewId() + ".png";
		QDir().mkpath(QFileInfo(target).absolutePath());
		if (!image.save(target)) { done({}, Ui("无法写入静止画面。")); return true; }
		const auto url = m_surface->localResource(target);
		const auto key = url.section('/', -1);
		m_tracks[key] = target;
		done(QJsonObject{{"id", key}, {"url", url}});
	}
	else if (name == "srt-export")
	{
		const auto path = QFileDialog::getSaveFileName(&w, Ui("导出字幕"), w.m_directory->text() + "/subtitles.srt", "SubRip (*.srt)");
		QString error;
		auto cues = m_edit.outputCaptions();
		const auto track = data["track"].toInt(-1);
		if (track >= 0) cues.erase(std::remove_if(cues.begin(), cues.end(), [track](const auto& cue) { return cue.track != track; }), cues.end());
		if (!path.isEmpty()) WriteEditSubtitles(path, cues, error);
		done(path, error);
	}
	else if (name == "recognize")
	{
		if (m_edit.input.isEmpty() || m_jobs.isBusy()) { done({}, Ui("请先打开工程或等待当前任务结束。")); return true; }
		const int index = data["track"].toInt(-1);
		if (index < 0 || index >= m_edit.audio.size()) { done({}, Ui("请选择有声音的轨道。")); return true; }
		const auto track = m_edit.audio[index];
		const auto dir = QCoreApplication::applicationDirPath() + "/whisper/";
		const auto target = m_cache.filePath(NewId() + ".srt");
		const auto model = QFileInfo(dir + "ggml-small-q5_1.bin").isFile() ? "ggml-small-q5_1.bin" : "ggml-base.bin";
		const auto language = data["lang"] == "en" ? "en" : "zh";
		if (!m_jobs.transcribe(track.file.isEmpty() ? m_edit.input : track.file, target,
			dir + "whisper-cli.exe", dir + model, language, qMax(0, track.stream)))
		{ done({}, Ui("无法启动本地识别。")); return true; }
		const auto project = m_edit;
		auto connection = std::make_shared<QMetaObject::Connection>();
		*connection = connect(&m_jobs, &KRRMediaEditor::completed, this, [this, request, connection, project, track](const QString& path, const QString& failure)
		{
			disconnect(*connection);
			QVector<KRREditCaption> cues;
			QString error = failure;
			QJsonArray lines;
			if (error.isEmpty() && ReadEditSubtitles(path, cues, error))
			{
				const auto append = [&lines](const KRREditCaption& line, qint64 begin, qint64 end)
				{
					if (end > begin) lines.append(QJsonObject{{"id", NewId()}, {"at", begin / 1000.},
						{"length", (end - begin) / 1000.}, {"name", line.text}});
				};
				if (project.arranged())
					for (const auto& clip : track.placed)
						for (const auto& cue : cues)
						{
							const auto begin = qMax(cue.begin, clip.mediaStart);
							const auto end = qMin(cue.end, clip.mediaStart + clip.end - clip.begin);
							append(cue, clip.begin + begin - clip.mediaStart, clip.begin + end - clip.mediaStart);
						}
				else
					for (const auto& cue : cues) append(cue, project.outputTime(cue.begin), project.outputTime(cue.end));
			}
			reply(request, lines, UiDiagnostic(error));
		});
	}
	else if (name == "translate" || name == "proofread")
	{
		const auto cues = data["lines"].toArray();
		const auto target = data["lang"] == "zh" ? QString("zh") : QString("en");
		const auto glossary = data["glossary"].toString().left(1000);
		const bool review = name == "proofread";
		KRREditProject project;
		if (review)
		{
			const int track = data["track"].toInt(-1);
			project = m_edit;
			auto& captions = project.captions;
			captions.erase(std::remove_if(captions.begin(), captions.end(), [track](const auto& cue)
				{ return cue.track != track; }), captions.end());
			if (track < 0 || captions.isEmpty() || captions.size() != cues.size())
			{ done({}, Ui("当前字幕轨已变化，请重新校对。")); return true; }
			for (int i = 0; i < captions.size(); ++i)
				if (captions[i].text != cues[i].toString())
				{ done({}, Ui("当前字幕轨已变化，请重新校对。")); return true; }
			project.subtitleGlossary = glossary;
			project.translationLanguage = target;
		}
		background([cues, target, glossary, review, project]
		{
			QVector<QString> source, translations;
			QJsonArray result;
			QString error;
			if (review)
			{
				KRRSubtitleReview changes;
				error = ReviewSubtitles(project, 0, project.captions.size() - 1, target, {}, changes);
				for (const auto& item : changes.suggestions)
					result.append(QJsonObject{{"index", item.row}, {"translation", item.translation.value_or(QString{})}, {"reason", item.reason}});
			}
			else
			{
				for (const auto& val : cues) source.append(val.toString());
				error = TranslateSubtitleLines(source, target, glossary, translations, nullptr);
				for (const auto& line : translations) result.append(line);
			}
			return std::make_pair(result, error);
		}, [this, request](const auto& result) { reply(request, result.first, UiDiagnostic(result.second)); });
	}
	else if (name == "dubbing")
	{
		if (m_voiceJob) { done({}, Ui("正在生成旁白，请稍候…")); return true; }
		const auto samples = data["lines"].toArray();
		const auto duration = Millis(data["duration"], data["audition"].toBool() ? 30000 : m_edit.outputDuration());
		if (samples.isEmpty() || samples.size() > 200 || duration < 100) { done({}, Ui("请选择有效的配音内容。")); return true; }
		auto opts = KRRVoiceOptions{};
		opts.engine = "audio8";
		if (data["service"] == "online")
		{
			const auto services = TtsServiceIds();
			opts.engine = data["engine"].toString(services.value(0));
			if (!services.contains(opts.engine)) { done({}, Ui("请先配置并选择在线配音服务。")); return true; }
		}
		opts.voice = opts.engine == "audio8" ? QString(data["voice"] == "普通话女声" ? "mandarin-female" : "default") :
			SpeechService(opts.engine).voice;
		opts.speed = qBound(.5, data["speed"].toDouble(1.), 2.);
		opts.volume = qBound(.01, data["volume"].toDouble(95.) / 100., 1.);
		const auto folder = DataDirectory() + "/studio-narration";
		QDir().mkpath(folder);
		const auto output = folder + "/" + NewId() + ".wav";
		const bool chapter = data["mode"] == "chapter" && !data["audition"].toBool();
		m_voiceJob = true;
		background([samples, duration, output, opts, chapter, cancel = m_backgroundCancel]() -> QPair<QJsonArray, QString>
		{
			QTemporaryDir temp;
			if (!temp.isValid()) return {{}, Ui("无法创建语音缓存。")};
			struct Cue
			{
				qint64 begin, end;
				QString title;
				QVector<KRRVoiceLine> parts;
			};
			QVector<Cue> cues;
			QVector<KRRVoiceLine> batch;
			for (const auto& value : samples)
			{
				if (cancel->load()) return {{}, Ui("任务已取消。")};
				const auto item = value.toObject();
				auto text = item["text"].toString().trimmed();
				const int limit = chapter ? 60000 : 4000;
				if (text.size() > limit)
					return {{}, Ui("稿件超过配音字数限制。")};
				const auto rules = item["rules"].toString().left(1200).split('\n', Qt::SkipEmptyParts);
				for (const auto& rule : rules)
				{
					const int split = rule.indexOf('=');
					const auto from = split > 0 ? rule.left(split).trimmed() : QString{};
					if (from.isEmpty() || split == rule.size() - 1) continue;
					text.replace(from, rule.mid(split + 1).trimmed());
					if (text.size() > limit) return {{}, Ui("应用读音规则后的稿件超过限制。")};
				}
				if (text.isEmpty()) continue;
				const auto begin = Millis(item["at"], duration);
				const auto end = qMin(duration, begin + Millis(item["length"]));
				if (end - begin < 100) continue;
				Cue cue{begin, end, item["title"].toString().left(120), {}};
				while (!text.isEmpty())
				{
					int count = text.size();
					if (chapter && count > 800)
					{
						count = 800;
						while (count > 400 && !QStringLiteral("。！？!?；; .\n").contains(text[count - 1])) --count;
						if (count == 400) count = text.at(799).isHighSurrogate() ? 799 : 800;
					}
					const auto segment = text.left(count).trimmed();
					text = text.mid(count).trimmed();
					cue.parts.append({segment, temp.filePath(NewId() + ".wav")});
					if (opts.engine == "audio8") batch.append(cue.parts.back());
				}
				cues.append(std::move(cue));
			}
			if (cues.isEmpty()) return {{}, Ui("配音文本为空。")};
			if (opts.engine == "audio8")
			{
				const auto error = SynthesizeAudio8Speech(batch, opts, cancel.get());
				if (!error.isEmpty()) return {{}, error};
			}
			QVector<KRRDubLine> lines;
			QJsonArray speech;
			for (const auto& cue : cues)
			{
				auto cursor = cue.begin;
				for (const auto& part : cue.parts)
				{
					if (opts.engine != "audio8")
					{
						const auto error = SynthesizeSpeech(part.text, part.output, opts, cancel.get());
						if (!error.isEmpty()) return {{}, error};
					}
					const auto actual = SpeechWaveDuration(part.output);
					if (actual <= 0) return {{}, Ui("无法读取合成的配音。")};
					if (cursor + actual > cue.end)
						return {{}, chapter ? Ui("章节「%1」的稿件超过时长，请缩短稿件或提高语速。").arg(cue.title)
							: Ui("配音超过可用时长，请缩短文本或提高语速。")};
					speech.append(QJsonObject{{"at", cursor / 1000.}, {"length", actual / 1000.}});
					lines.append({cursor, cursor + actual, part.output});
					cursor += actual;
				}
			}
			return {speech, MixDubbingTrack(duration, lines, output, cancel.get())};
		}, [this, request, output, duration](const QPair<QJsonArray, QString>& result)
		{
			m_voiceJob = false;
			const auto& error = result.second;
			const auto url = error.isEmpty() ? m_surface->localResource(output) : QString{};
			const auto key = url.section('/', -1);
			if (!key.isEmpty()) m_tracks[key] = output;
			reply(request, QJsonObject{{"id", key}, {"url", url}, {"duration", duration / 1000.}, {"speech", result.first}}, UiDiagnostic(error));
		});
	}
	else return false;
	return true;
}

KRRWebStudio::~KRRWebStudio()
{
	m_previewWorker.clear();
	m_previewWorker.waitForDone();
	cancelExitWork();
	m_window.m_webStudio = nullptr;
}

bool KRRWebStudio::assistantCommand(const QString& name, const QString& request, const QJsonObject& data)
{
	auto& w = m_window;
	const auto done = [this, &request](const QJsonValue& value = {}, const QString& error = {}) { reply(request, value, UiDiagnostic(error)); };
	const auto jobId = data["job"].toString().left(64);
	if (name == "assistant-cancel")
	{
		if (const auto cancel = m_assistantJobs.value(jobId)) cancel->store(true);
		done();
		return true;
	}
	const auto newJob = [this, &jobId]() -> std::shared_ptr<std::atomic<bool>>
	{
		if (m_assistantJobs.size() >= 8 || (!jobId.isEmpty() && m_assistantJobs.contains(jobId))) return {};
		auto cancel = std::make_shared<std::atomic<bool>>(false);
		if (!jobId.isEmpty()) m_assistantJobs.insert(jobId, cancel);
		return cancel;
	};
	if (name == "assistant")
	{
		const auto context = w.assistantContext();
		const auto service = SpeechService("assistant");
		const auto prompt = data["prompt"].toString().trimmed();
		const auto history = data["history"].toArray();
		const auto summary = data["summary"].toString().left(12000);
		if (prompt.isEmpty()) { done({}, Ui("请输入问题。")); return true; }
		QJsonObject editing;
		if (data["editing"].toBool())
		{
			const auto periods = data["params"].toObject();
			editing = {{"scope", periods["range"]}, {"target_seconds", periods["target"]},
				{"subtitles", periods["subtitles"]}, {"markers", periods["markers"]}, {"picture", periods["picture"]},
				{"material_count", data["inputs"].toArray().size()}, {"inside_editor", data["side"].toBool()},
				{"current_edit_duration_seconds", data["side"].toBool() ? m_edit.outputDuration() / 1000. : 0.}};
			QJsonArray picks;
			for (const auto& value : data["materials"].toArray())
			{
				if (picks.size() >= 16) break;
				const auto item = value.toObject();
				const auto path = mediaFile(item["asset"].toString());
				if (path.isEmpty()) continue;
				picks.append(QJsonObject{{"source", QFileInfo(path).fileName()}, {"start", item["from"]}, {"end", item["to"]},
					{"name", item["name"].toString().left(80)}, {"note", item["note"].toString().left(220)}, {"marked", item["marked"].toBool()}});
			}
			if (!picks.isEmpty()) editing["materials"] = picks;
			if (data["side"].toBool())
			{
				const auto selection = data["selection"].toObject();
				if (!selection.isEmpty()) editing["selected_clip"] = selection;
				editing["time_reference"] = "output_timeline";
				QJsonArray audio;
				for (const auto& track : m_edit.audio)
					audio.append(QJsonObject{{"title", track.title}, {"enabled", track.enabled}, {"gain", track.gain}});
				editing["audio_tracks"] = audio;
				editing["subtitle_count"] = m_edit.captions.size();
				editing["chapter_count"] = m_edit.chapters.size();
			}
		}
		const auto cancel = newJob();
		if (!cancel) { done({}, Ui("当前助手任务较多，请稍后再试。")); return true; }
		background([context, service, prompt, history, summary, editing, cancel]
		{ return AskRecordingAssistant(prompt, context, service, history, cancel.get(), summary, editing); },
		[this, request, jobId, before = context.settings,
			revision = data["side"].toBool() && !m_edit.input.isEmpty() ? SmartCutRevision(m_edit) : QString{}](const KRRAssistantReply& answer)
		{
			m_assistantJobs.remove(jobId);
			auto result = answer.toJson();
			QJsonArray recordings;
			for (const auto& entry : answer.recordings)
			{
				auto row = entry.toObject();
				row["asset"] = libraryEntry(row["path"].toString());
				recordings.append(row);
			}
			result["recordings"] = recordings;
			auto preview = answer.preview;
			if (!preview.isEmpty()) preview["before"] = before;
			result["preview"] = preview;
			if (!revision.isEmpty()) result["edit_revision"] = revision;
			reply(request, result, UiDiagnostic(answer.error));
		});
	}
	else if (name == "suggest-cut")
	{
		const auto cancel = newJob();
		if (!cancel) { done({}, Ui("当前助手任务较多，请稍后再试。")); return true; }
		QVector<QPair<QString, QString>> files;
		for (const auto& value : data["inputs"].toArray())
		{
			const auto asset = value.toString();
			const auto file = mediaFile(asset);
			if (!file.isEmpty() && files.size() < 16 && !files.contains({asset, file})) files.append({asset, file});
		}
		const auto project = m_edit;
		const bool side = data["side"].toBool();
		if (side && (files.value(0).first != m_editId || data["revision"].toString() != SmartCutRevision(project)))
		{
			m_assistantJobs.remove(jobId);
			done({}, Ui("剪辑工程已改变，请重新生成草稿。"));
			return true;
		}
		const auto plan = data["plan"].toObject();
		const auto goal = plan["query"].toString(data["prompt"].toString()).left(MaxSequenceGoalChars);
		const auto params = data["params"].toObject();
		const auto projectPath = data["projectPath"].toString();
		const auto selection = data["selection"].toObject();
		const auto materials = data["materials"].toArray();
		background([files, project, side, goal, params, projectPath, selection, materials, plan, cancel]
		{
			QString error;
			QJsonArray targets;
			QVector<QPair<QString, KRREditProject>> edits;
			if (side && !project.input.isEmpty()) edits.append({files.value(0).first, project});
			else for (const auto& file : files)
			{
				KRREditProject edit;
				if (cancel->load()) return std::make_pair(QJsonObject{}, Ui("任务已取消。"));
				if (!ProbeEditSource(file.second, edit, error)) return std::make_pair(QJsonObject{}, error);
				edit.captions = ReadAdjacentEditSubtitles(file.second, edit.duration);
				for (const auto& value : ReadRecordingMarkers(file.second))
				{
					const auto marker = value.toObject();
					edit.chapters.append({marker["at_ms"].toVariant().toLongLong(), marker["label"].toString()});
				}
				edits.append({file.first, edit});
			}
			if (edits.isEmpty() && !projectPath.isEmpty())
			{
				QJsonObject json;
				KRREditProject recovered;
				if (ReadEditProjectJson(projectPath, json, error) && KRREditProject::FromJson(json, recovered, error))
					edits.append({{}, recovered});
			}
			if (edits.isEmpty()) return std::make_pair(QJsonObject{}, error.isEmpty() ? Ui("请先选择要分析的视频。") : error);
			const qint64 target = plan.contains("max_ms") ? qBound<qint64>(0, plan["max_ms"].toVariant().toLongLong(), 86400000) : Millis(params["target"]);
			qint64 remaining = target;
			QStringList unavailable;
			bool picture = params["picture"].toBool(true);
			// Use the same discovery, visual review and shared budget as native multi-source editing.
			if (!side && edits.size() > 1 && picture && params["range"] == "full" &&
				!plan.contains("begin_ms") && !plan.contains("end_ms") && plan["mode"] == "highlights")
			{
				KRREditSequence sequence;
				for (const auto& entry : std::as_const(edits))
				{
					auto edit = entry.second;
					if (!params["subtitles"].toBool(true)) edit.captions.clear();
					if (!params["markers"].toBool(true)) edit.chapters.clear();
					KRREditSequencePart part;
					if (!MakeSequencePart(edit, part, error)) break;
					sequence.append(std::move(part));
				}
				KRRSequenceSmartCutDraft sequenceDraft;
				if (sequence.size() == edits.size())
				{
					KRRSequenceCutOptions options;
					options.targetMs = target;
					error = SuggestSequenceSmartCut(sequence, goal, options, sequenceDraft, cancel.get());
				}
				if (cancel->load()) return std::make_pair(QJsonObject{}, Ui("任务已取消。"));
				if (error.isEmpty())
				{
					for (const auto& choice : sequenceDraft.highlights)
					{
						if (!choice.cut.selected || targets.size() >= 20) continue;
						const auto& entry = edits.at(choice.source);
						const auto& cut = choice.cut;
						const auto scope = std::find_if(entry.second.kept.cbegin(), entry.second.kept.cend(), [&cut](const auto& item)
						{ return item.begin <= cut.range.begin && item.end >= cut.range.end; });
						if (scope == entry.second.kept.cend()) continue;
						targets.append(QJsonObject{{"id", entry.first}, {"input", entry.second.input}, {"action", "review"},
							{"at", cut.range.begin / 1000.}, {"start", cut.range.begin / 1000.}, {"end", cut.range.end / 1000.},
							{"scope_start", scope->begin / 1000.}, {"scope_end", scope->end / 1000.},
							{"label", cut.title}, {"reason", cut.reason}});
					}
					return std::make_pair(QJsonObject{{"text", Ui("已找到 %1 个候选片段。").arg(targets.size())},
						{"targets", targets}, {"target", target / 1000.}, {"chapters", plan["chapters"].toBool()}}, QString{});
				}
				unavailable.append(error);
				picture = false;
			}
			for (auto& entry : edits)
			{
				if (cancel->load()) return std::make_pair(QJsonObject{}, Ui("任务已取消。"));
				const auto& edit = entry.second;
				const auto revision = side ? SmartCutRevision(edit) : QString{};
				const auto range = params["range"].toString();
				const bool explicitBounds = plan.contains("begin_ms") && plan.contains("end_ms");
				const auto requestStart = explicitBounds ? plan["begin_ms"].toVariant().toLongLong() : 0;
				const auto requestEnd = explicitBounds ? plan["end_ms"].toVariant().toLongLong() : 86400000;
				const auto limit = [requestStart, requestEnd](const KRREditRange& range)
				{
					return KRREditRange{qMax(range.begin, requestStart), qMin(range.end, requestEnd)};
				};
				QVector<KRREditRange> scopes;
				if (side)
				{
					auto rangeOnTimeline = KRREditRange{0, edit.outputDuration()};
					if (range == "custom") rangeOnTimeline = {Millis(params["from"]), Millis(params["to"])};
					if (range == "selected")
					{
						if (selection.isEmpty() || Millis(selection["length"]) < 20)
							return std::make_pair(QJsonObject{}, Ui("请在时间轴选中一个视频片段。"));
						const auto start = selection.contains("start") ? Millis(selection["start"]) : edit.outputTime(Millis(selection["source"]));
						rangeOnTimeline = {start, start + Millis(selection["length"])};
					}
					rangeOnTimeline = limit(rangeOnTimeline);
					qint64 timelineAt = 0;
					for (const auto& item : edit.segments())
					{
						const auto end = timelineAt + item.end - item.begin;
						const auto beginAt = qMax(rangeOnTimeline.begin, timelineAt);
						const auto endAt = qMin(rangeOnTimeline.end, end);
						if (item.still < 0 && endAt - beginAt >= 20)
							scopes.append({item.begin + beginAt - timelineAt, item.begin + endAt - timelineAt});
						timelineAt = end;
					}
				}
				else if (range == "custom") scopes.append(limit({Millis(params["from"]), qMin(Millis(params["to"]), edit.duration)}));
				else if (range == "selected")
				{
					for (const auto& value : materials)
					{
						const auto material = value.toObject();
						if (material["asset"] == entry.first)
							scopes.append(limit({Millis(material["from"]), qMin(Millis(material["to"]), edit.duration)}));
					}
				}
				else
					for (const auto& kept : edit.kept) scopes.append(limit(kept));
				scopes = MergeEditRanges(scopes);
				for (const auto& scope : scopes)
				{
					if (cancel->load()) return std::make_pair(QJsonObject{}, Ui("任务已取消。"));
					if (scope.end - scope.begin < 20) continue;
					KRRSmartCutOptions options;
					options.query = goal.left(MaxSmartCutQueryChars);
					options.maxDuration = remaining;
					options.scope = scope;
					options.mode = plan["mode"].toString() == "rough" ? "rough" : "highlights";
					options.useCaptions = params["subtitles"].toBool(true);
					options.useMarkers = params["markers"].toBool(true);
					KRRSmartCutDraft draft;
					if (plan["mode"] == "extract")
					{
						for (const auto& kept : edit.kept)
						{
							const KRREditRange part{qMax(scope.begin, kept.begin), qMin(scope.end, kept.end)};
							if (part.end - part.begin >= 20)
								draft.parts.append({part, part.begin, Ui("指定片段"), {}, 1, true});
						}
					}
					else if (picture)
					{
						error = SuggestSmartCut(edit, options, draft, cancel.get());
						if (!error.isEmpty() && !unavailable.contains(error)) unavailable.append(error);
					}
					if (draft.parts.isEmpty())
					{
						if (params["markers"].toBool(true))
							for (const auto& chapter : edit.chapters)
								if (chapter.at >= scope.begin && chapter.at < scope.end)
									draft.parts.append({{chapter.at, qMin(scope.end, chapter.at + 5000)}, chapter.at, chapter.title, Ui("打点"), 1, true});
						if (params["subtitles"].toBool(true))
							for (const auto& cue : edit.captions)
								if (cue.begin >= scope.begin && cue.end <= scope.end)
									draft.parts.append({{cue.begin, cue.end}, cue.begin, cue.text.left(80), Ui("字幕"), 1, true});
					}
					for (const auto& part : draft.parts)
					{
						if (!part.selected || targets.size() >= 20 || (target && remaining < 20)) continue;
						const auto start = part.range.begin;
						const auto end = target ? qMin(part.range.end, start + remaining) : part.range.end;
						if (end - start < 20 || !EditRangesContain(edit.kept, {start, end})) continue;
						targets.append(QJsonObject{{"id", entry.first}, {"input", edit.input}, {"revision", revision},
							{"action", side ? "edit" : "review"}, {"at", (side ? edit.outputTime(start) : start) / 1000.},
							{"start", start / 1000.}, {"end", end / 1000.}, {"scope_start", scope.begin / 1000.},
							{"scope_end", scope.end / 1000.}, {"label", part.title}, {"reason", part.reason}});
						if (target) remaining -= end - start;
					}
					if ((target && remaining < 20) || targets.size() >= 20) break;
				}
				if ((target && remaining < 20) || targets.size() >= 20) break;
			}
			if (cancel->load()) return std::make_pair(QJsonObject{}, Ui("任务已取消。"));
			auto text = targets.isEmpty() ? Ui("当前范围没有找到候选片段。") : Ui("已找到 %1 个候选片段。").arg(targets.size());
			if (!unavailable.isEmpty()) text += "\n" + Ui("画面分析：") + unavailable.join("\n");
			return std::make_pair(QJsonObject{{"text", text}, {"targets", targets}, {"target", target / 1000.}, {"chapters", plan["chapters"].toBool()}}, QString{});
		}, [this, request, jobId](auto result)
		{
			m_assistantJobs.remove(jobId);
			QJsonArray targets;
			for (const auto& value : result.first["targets"].toArray())
			{
				auto row = value.toObject();
				if (row["id"].toString().isEmpty()) row["id"] = libraryEntry(row["input"].toString())["id"];
				row.remove("input");
				targets.append(row);
			}
			result.first["targets"] = targets;
			reply(request, result.first, UiDiagnostic(result.second));
		});
	}
	else if (name == "apply-cut")
	{
		const auto revision = data["revision"].toString();
		if (m_edit.input.isEmpty() || data["id"] != m_editId ||
			(!revision.isEmpty() && revision != SmartCutRevision(m_edit)))
		{ done({}, Ui("剪辑工程已改变，请重新生成草稿。")); return true; }
		const auto selected = data["ranges"].toArray();
		if (selected.isEmpty() || selected.size() > 20 || m_jobs.isBusy())
		{ done({}, Ui("请确认候选已选中，且没有正在执行的导出。")); return true; }
		auto candidate = m_edit;
		QVector<KRREditRange> ranges;
		QString error;
		for (const auto& value : selected)
		{
			const auto item = value.toObject();
			const KRREditRange range{Millis(item["start"]), Millis(item["end"])};
			if (range.end - range.begin < 20 || !EditRangesContain(candidate.kept, range))
			{ done({}, Ui("候选超出当前视频范围，请重新生成。")); return true; }
			ranges.append(range);
			const bool hasChapter = std::any_of(candidate.chapters.begin(), candidate.chapters.end(), [&](const auto& chapter) { return chapter.at == range.begin; });
			if (data["chapters"].toBool() && !hasChapter)
			{
				const auto label = item["label"].toString().simplified().left(120);
				if (!candidate.addChapter(range.begin, label.isEmpty() ? Ui("保留片段") : label, error))
				{
					done({}, error);
					return true;
				}
			}
		}
		if (!candidate.editVideoRanges(ranges, true, error) || !candidate.validate(error)) { done({}, error); return true; }
		const auto target = Millis(data["target"]);
		if (target && candidate.outputDuration() > target)
		{ done({}, Ui("所选片段超过目标时长，请缩短范围或减少候选。")); return true; }
		done(editView(candidate, m_projectPath, m_editId));
	}
	else if (name == "save-cut-sequence")
	{
		const auto selected = data["ranges"].toArray();
		if (selected.size() < 2 || selected.size() > 20) { done({}, Ui("请勾选需要保存的片段。")); return true; }
		QVector<QPair<QString, KRREditRange>> clips;
		for (const auto& value : selected)
		{
			const auto row = value.toObject();
			const auto path = mediaFile(row["id"].toString());
			const KRREditRange range{Millis(row["start"]), Millis(row["end"])};
			if (path.isEmpty() || range.end - range.begin < 20) { done({}, Ui("所选素材或片段范围无效。")); return true; }
			clips.append({path, range});
		}
		auto output = QFileDialog::getSaveFileName(&w, Ui("保存多素材草稿"), w.m_directory->text() + "/sequence.json", Ui("剪辑工程 (*.json)"));
		if (output.isEmpty()) { done(); return true; }
		if (!output.endsWith(".json", Qt::CaseInsensitive)) output += ".json";
		background([clips, output, target = Millis(data["target"])]
		{
			QString error;
			KRREditSequence sequence;
			QHash<QString, KRREditProject> sources;
			for (const auto& [file, range] : clips)
			{
				if (!sources.contains(file))
				{
					KRREditProject project;
					if (!ProbeEditSource(file, project, error)) return error;
					project.captions = ReadAdjacentEditSubtitles(file, project.duration);
					sources.insert(file, project);
				}
				auto segment = sources.value(file);
				KRREditSequencePart part;
				if (!segment.editVideoRanges({range}, true, error) || !MakeSequencePart(segment, part, error)) return error;
				sequence.append(std::move(part));
			}
			if (target && EditSequenceDuration(sequence) > target) return Ui("所选片段超过目标时长，请缩短范围或减少候选。");
			SaveEditSequence(output, sequence, error);
			return error;
		}, [this, request, output](const auto& error) { reply(request, output, UiDiagnostic(error)); });
	}
	else if (name == "compact")
	{
		const auto service = SpeechService("assistant");
		const auto history = data["messages"].toArray();
		const auto cancel = newJob();
		if (!cancel) { done({}, Ui("当前助手任务较多，请稍后再试。")); return true; }
		background([service, history, cancel]
		{
			QJsonObject result;
			const auto error = RequestAssistantReply(service, {{"messages", history}, {"max_tokens", 700}}, result, cancel.get());
			const auto choices = result["choices"].toArray();
			return std::make_pair(choices.isEmpty() ? result["content"].toString() :
				choices[0].toObject()["message"].toObject()["content"].toString(), error);
		}, [this, request, jobId](const auto& r) { m_assistantJobs.remove(jobId); reply(request, r.first, UiDiagnostic(r.second)); });
	}
	else if (name == "apply-assistant")
	{
		QString error;
		if (!w.applyAssistantSettings(data["before"].toObject(), data["changes"].toObject(), error)) { done({}, error); return true; }
		done(snapshot()["settings"]);
	}
	else if (name == "cli")
	{
		auto args = QProcess::splitCommand(data["input"].toString().left(4096));
		if (!args.isEmpty() && (args.first() == "wpsrecorder_cli" || args.first() == "wpsrecorder_cli.exe")) args.removeFirst();
		if (args.isEmpty()) args << "--help";
		if (!EmbeddedCliAllows(args)) { done({}, Ui("内置终端请使用随软件提供的识别引擎。自定义识别程序及服务命令可在系统终端使用。")); return true; }
		auto process = new QProcess(this);
		ConfigureEmbeddedCli(*process);
		HideConsoleProcess(*process);
		process->setProcessChannelMode(QProcess::MergedChannels);
		const auto root = m_workspaces.value(data["workspace"].toString(), data["folder"].toString());
		if (!root.isEmpty() && QFileInfo(root).isDir()) process->setWorkingDirectory(root);
		connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, request, process](int code, QProcess::ExitStatus)
		{
			reply(request, QJsonObject{{"code", code}, {"text", QString::fromUtf8(process->readAll()).left(48000)}});
			process->deleteLater();
		});
		connect(process, &QProcess::errorOccurred, this, [this, request, process](QProcess::ProcessError error)
		{
			if (error != QProcess::FailedToStart) return;
			reply(request, {}, process->errorString());
			process->deleteLater();
		});
		process->start(QCoreApplication::applicationDirPath() + "/wpsrecorder_cli.exe", args);
		process->closeWriteChannel();
		QTimer::singleShot(7200000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
	}
	else if (name == "workspace-media")
	{
		const auto file = mediaFile(data["id"].toString());
		if (file.isEmpty()) { done({}, Ui("请刷新工作区后重新选择视频。")); return true; }
		background([file]
		{
			KRREditProject video;
			QString error;
			ProbeEditSource(file, video, error);
			return std::make_pair(video, error);
		}, [this, request, file](auto result)
		{
			if (!result.second.isEmpty()) { reply(request, {}, UiDiagnostic(result.second)); return; }
			auto asset = libraryEntry(file);
			asset["duration"] = result.first.duration / 1000.;
			asset["frameRate"] = result.first.fps;
			asset["width"] = result.first.size.width();
			asset["height"] = result.first.size.height();
			reply(request, asset);
		});
	}
	else if (name == "workspace-project" || name == "workspace-folder" || name == "workspace-refresh")
	{
		const bool refresh = name == "workspace-refresh";
		const auto project = refresh ? data["project"].toString() : name == "workspace-project" ? openMediaDialog("project") : QString{};
		const auto root = refresh ? data["folder"].toString() : name == "workspace-folder" ? openFolder(Ui("选择工作区")) :
			QFileInfo(project).isFile() ? QFileInfo(project).absolutePath() : QString{};
		if (root.isEmpty()) { done(); return true; }
		const auto key = refresh ? data["workspace"].toString().left(64) : NewId();
		if (refresh && (key.isEmpty() || !QDir::isAbsolutePath(root))) { done({}, Ui("工作区路径无效。")); return true; }
		background([root]
		{
			QJsonArray files;
			const QDir directory(root);
			if (!directory.exists()) return std::make_tuple(files, false, false);
			QDirIterator iterator(root, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
				QDirIterator::Subdirectories);
			while (files.size() < 4000 && iterator.hasNext())
			{
				const auto absolute = iterator.next();
				const auto file = iterator.fileInfo();
				const auto suffix = file.suffix().toLower();
				files.append(QJsonObject{{"path", directory.relativeFilePath(absolute)}, {"directory", file.isDir()},
					{"name", file.fileName()}, {"project", suffix == "krrproj"},
					{"video", QStringList{"mov", "mp4", "mkv", "webm", "avi", "flv", "m4v"}.contains(suffix)}});
			}
			return std::make_tuple(files, iterator.hasNext(), true);
		}, [this, request, key, root, project](auto result)
		{
			if (!std::get<2>(result)) { reply(request, {}, Ui("工作区目录不存在或无法访问。")); return; }
			QJsonArray files;
			for (const auto& value : std::get<0>(result))
			{
				auto file = value.toObject();
				const auto absolute = QDir(root).absoluteFilePath(file["path"].toString());
				if (file["video"].toBool() && !file["directory"].toBool())
				{
					const auto url = m_surface->localResource(absolute);
					const auto id = url.section('/', -1);
					if (!id.isEmpty())
					{
						m_media[id] = absolute;
						file["asset"] = QJsonObject{{"id", id}, {"src", url}, {"name", file["name"]},
							{"title", QFileInfo(absolute).completeBaseName()}, {"duration", 0}};
					}
				}
				if (file["project"].toBool() && !file["directory"].toBool()) file["projectPath"] = absolute;
				files.append(file);
			}
			m_workspaces[key] = root;
			if (!project.isEmpty()) m_projects[key] = project;
			const auto folderName = QFileInfo(QDir::cleanPath(root)).fileName();
			reply(request, QJsonObject{{"id", key}, {"name", project.isEmpty() ? (folderName.isEmpty() ? root : folderName) :
				QFileInfo(project).completeBaseName()}, {"root_name", folderName.isEmpty() ? root : folderName},
				{"folder", root}, {"project", project}, {"files", files}, {"truncated", std::get<1>(result)}});
		});
	}
	else return false;
	return true;
}
