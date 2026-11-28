#include "recordingassistanttools.h"

#include "controlserver.h"
#include "editproject.h"
#include "recordingconfig.h"
#include "recordingmarkers.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace
{
const QStringList VideoFormats{"mp4", "mov", "mkv", "avi", "flv", "webm"};
// A journal entry is written only when a recording finishes or fails to start.
QString HistoryFile() { return DataDirectory() + "/recording-history-" + ControlServerName() + ".ini"; }

QJsonObject Failure(const QString& code, const QString& field = {})
{
	QJsonObject result{{"ok", false}, {"code", code}};
	if (!field.isEmpty()) result["field"] = field;
	return result;
}

bool Integer(const QJsonValue& value, int low, int high)
{
	const double number = value.toDouble(-1);
	return number >= low && number <= high && std::floor(number) == number;
}

struct Search
{
	QStringList words;
	QDate from, until;
	int limit = 8;
};

bool ReadSearch(const QJsonObject& args, Search& search)
{
	for (auto i = args.begin(); i != args.end(); ++i)
	{
		if (!QStringList{"query", "from", "until", "limit"}.contains(i.key())) return false;
		if (i.key() != "limit" && !i.value().isString()) return false;
	}
	const auto query = args["query"].toString();
	if (query.size() > 120) return false;
	search.words = query.simplified().split(' ', Qt::SkipEmptyParts);
	if (args.contains("from")) search.from = QDate::fromString(args["from"].toString(), Qt::ISODate);
	if (args.contains("until")) search.until = QDate::fromString(args["until"].toString(), Qt::ISODate);
	if ((args.contains("from") && !search.from.isValid()) || (args.contains("until") && !search.until.isValid())) return false;
	if (search.from.isValid() && search.until.isValid() && search.from > search.until) return false;
	if (args.contains("limit"))
	{
		if (!Integer(args["limit"], 1, 12)) return false;
		search.limit = args["limit"].toInt();
	}
	return true;
}

bool ContainsWords(const QString& text, const QStringList& words)
{
	return std::all_of(words.cbegin(), words.cend(), [&text](const QString& word)
		{ return text.contains(word, Qt::CaseInsensitive); });
}

QDateTime RecordingTime(const QFileInfo& file)
{
	// Standard recording filenames preserve capture time when copied to another drive.
	static const QRegularExpression pattern("^recording-(\\d{8}-\\d{6})-\\d{3}$", QRegularExpression::CaseInsensitiveOption);
	const auto match = pattern.match(file.completeBaseName());
	const auto stamp = QDateTime::fromString(match.captured(1), "yyyyMMdd-HHmmss");
	return stamp.isValid() ? stamp : file.lastModified();
}

struct Match
{
	QString kind, text;
	qint64 at = -1;
};

Match FindMatch(const QFileInfo& file, const QStringList& words, bool& openedSidecar)
{
	const auto name = file.completeBaseName();
	const auto video = file.absoluteFilePath();
	const auto markers = ReadRecordingMarkers(video);
	for (const auto& value : markers)
	{
		const auto marker = value.toObject();
		const auto label = marker["label"].toString();
		if (ContainsWords(name + ' ' + label, words)) return {"marker", label.left(96), marker["at_ms"].toVariant().toLongLong()};
	}
	for (const auto& path : AdjacentEditSubtitlePaths(video))
	{
		const QFileInfo info(path);
		if (!info.isFile() || info.isSymLink() || info.size() > 262144) continue;
		QVector<KRREditCaption> captions;
		QString error;
		openedSidecar = true;
		if (!ReadEditSubtitles(path, captions, error)) continue;
		for (const auto& caption : captions)
			if (ContainsWords(name + ' ' + caption.text, words)) return {"subtitle", caption.text.simplified().left(96), caption.begin};
	}
	return {};
}

QJsonObject DiagnosticFacts(const QJsonObject& status, const QString& observedAt)
{
	const auto error = status["error"].toString();
	const auto stage = status["failure_stage"].toString();
	if (error.isEmpty() && stage.isEmpty()) return {};
	const auto message = error.toLower();
	QString finding = "unknown";
	if (message.contains("cannot load nvcuda") || message.contains("no capable devices") ||
		message.contains("cuda_error") || message.contains("driver does not support") ||
		message.contains("no h.264 encoding device") || message.contains("error creating a mfx session") ||
		message.contains("amfrt64.dll"))
		finding = "gpu_encoder_unavailable";
	else if (message.contains("ffmpeg.exe was not found")) finding = "ffmpeg_missing";
	else if (message.contains("disk is full") || message.contains("no space left") || message.contains("disk space"))
		finding = "storage_or_encoder_stall";
	else if (message.contains("selected window is unavailable") || message.contains("window no longer"))
		finding = "window_unavailable";
	else if (message.contains("audio device disconnected")) finding = "audio_disconnected";
	else if (stage == "open-audio" || message.contains("wasapi")) finding = "wasapi_init_failed";
	else if (stage == "open-camera") finding = "camera_init_failed";
	else if (stage == "select-encoder" || stage == "start-encoder" || stage == "write-video" || stage == "finalize-video")
		finding = "encoder_failed";
	else if (stage == "prepare" || stage == "publish" || stage == "mux") finding = "saving_failed";
	static const QRegularExpression hex("0x[0-9a-fA-F]{8}\\b");
	QJsonObject result{{"stage", stage}, {"finding", finding}, {"observed_at", observedAt},
		{"encoder", status["requested_encoder"].toString(status["encoder"].toString())},
		{"audio", status["audio_mode"].toString()},
		{"recoverable", !status["recovery_workspace"].toString().isEmpty()},
		{"published", status["output_published"].toBool()}};
	const auto code = hex.match(error).captured();
	if (!code.isEmpty()) result["hresult"] = code;
	static const QRegularExpression mfx("MFX session: (-?\\d+)\\b");
	const auto driverCode = mfx.match(error).captured(1);
	if (!driverCode.isEmpty()) result["mfx_status"] = driverCode.toInt();
	return result;
}

QJsonObject AudioEndpointFacts()
{
	const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
	HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
	QJsonObject endpoints;
	if (SUCCEEDED(result))
	{
		for (const auto& item : {qMakePair(eCapture, QString("mic")), qMakePair(eRender, QString("system"))})
		{
			Microsoft::WRL::ComPtr<IMMDevice> device;
			result = enumerator->GetDefaultAudioEndpoint(item.first, eConsole, &device);
			endpoints[item.second] = SUCCEEDED(result) ? "present" :
				result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ? "missing" : "query_failed";
		}
	}
	enumerator.Reset();
	if (SUCCEEDED(initialized)) CoUninitialize();
	return endpoints;
}

QJsonObject Choice(const QJsonArray& values, const QString& description)
{
	return {{"type", "string"}, {"enum", values}, {"description", description}};
}

bool ValidSetting(const QJsonObject& rule, const QJsonValue& value)
{
	const auto type = rule["type"].toString();
	const auto choices = rule["enum"].toArray();
	if (!choices.isEmpty()) return choices.contains(value);
	if (type == "integer") return Integer(value, rule["minimum"].toInt(), rule["maximum"].toInt());
	if (type == "boolean") return value.isBool();
	if (!value.isString()) return false;
	const auto size = value.toString();
	if (size == "auto") return true;
	static const QRegularExpression resolution("^(\\d{3,4})x(\\d{3,4})$");
	const auto match = resolution.match(size);
	const auto width = match.captured(1).toInt(), height = match.captured(2).toInt();
	return match.hasMatch() && width >= 160 && width <= 7680 && height >= 120 && height <= 4320 &&
		width % 2 == 0 && height % 2 == 0;
}
} // namespace

QString RecorderUiSettingsPath() { return DataDirectory() + "/ui-settings.ini"; }

KRRAssistantContext SavedAssistantContext(const QString& folder)
{
	QSettings settings(RecorderUiSettingsPath(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	KRRAssistantContext context;
	context.folder = folder.isEmpty() ? settings.value("directory",
		QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/WPSRecorder").toString() : folder;
	context.recent = settings.value("recent").toStringList();
	const QStringList sources{"none", "mic", "system", "mix"};
	auto resolution = settings.value("resolution", "auto").toString();
	if (resolution == QStringLiteral("自动（跟随来源）")) resolution = "auto";
	context.settings = {{"resolution", resolution}, {"fps", RecordingFrameRatePreset(settings.value("fps", 30).toInt())},
		{"bitrate_kbps", settings.value("bitrate", 6000).toInt()},
		{"audio", sources.value(settings.value("audio", 0).toInt(), "none")},
		{"encoder", settings.value("encoder", "auto").toString()},
		{"separate_audio", settings.value("separateAudio", false).toBool()},
		{"denoise", settings.value("denoise", false).toBool()},
		{"cursor", settings.value("cursor", true).toBool()},
		{"format", settings.value("format", "mp4").toString()}};
	return context;
}

void RememberRecordingOutcome(const QJsonObject& status)
{
	const QFileInfo output(status["output"].toString());
	const QDir directory(output.absolutePath());
	const bool trial = directory.dirName().startsWith(".wps-trial-");
	const bool published = status["output_published"].toBool();
	const auto failure = DiagnosticFacts(status, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
	if ((!published || trial) && failure.isEmpty()) return;
	QSettings settings(HistoryFile(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	if (!failure.isEmpty()) settings.setValue("failure", QJsonDocument(failure).toJson(QJsonDocument::Compact));
	if (published && !trial && !output.filePath().isEmpty())
	{
		const QJsonObject last{{"audio", status["audio_mode"]}, {"encoder", status["encoder"]},
			{"observed_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
		settings.setValue("last_recording", QJsonDocument(last).toJson(QJsonDocument::Compact));
		auto folders = settings.value("folders").toStringList();
		const auto folder = directory.absolutePath();
		if (folders.isEmpty() || folders.front().compare(folder, Qt::CaseInsensitive) != 0)
		{
			for (int i = folders.size() - 1; i >= 0; --i)
				if (folders[i].compare(folder, Qt::CaseInsensitive) == 0) folders.removeAt(i);
			folders.prepend(folder);
			while (folders.size() > 16) folders.removeLast();
			settings.setValue("folders", folders);
		}
	}
}

QJsonObject AssistantRecordingState()
{
	QString error;
	QJsonObject status;
	SendControl({{"command", "status"}}, status, error, 350);
	return status;
}

QJsonObject FindAssistantRecordings(const KRRAssistantContext& context, const QJsonObject& arguments,
	const std::atomic<bool>* cancel)
{
	Search query;
	if (!ReadSearch(arguments, query)) return Failure("invalid_search");
	if (AssistantRecordingState()["state"] == "recording") return Failure("wait_until_recording_ends");
	QSettings history(HistoryFile(), QSettings::IniFormat);
	history.setIniCodec("UTF-8");
	QStringList folders{context.folder};
	folders.append(history.value("folders").toStringList().mid(0, 16));
	for (const auto& recent : context.recent.mid(0, 10))
		if (!recent.isEmpty()) folders.append(QFileInfo(recent).absolutePath());
	QSet<QString> checkedFolders, visited;
	QVector<QJsonObject> matches;
	QElapsedTimer deadline;
	deadline.start();
	int examined = 0, openedSidecars = 0, inspectedMetadata = 0;
	const auto stopped = [&]
		{ return (cancel && cancel->load()) || deadline.elapsed() > 2500 || examined >= 2500; };
	auto inspect = [&](const QFileInfo& file)
	{
		if (!file.isFile() || file.isSymLink() || !VideoFormats.contains(file.suffix().toLower()) ||
			file.fileName().endsWith(".camera.mkv", Qt::CaseInsensitive)) return;
		const auto path = file.absoluteFilePath();
		if (visited.contains(path.toCaseFolded())) return;
		visited.insert(path.toCaseFolded());
		++examined;
		const auto time = RecordingTime(file);
		if ((query.from.isValid() && time.date() < query.from) || (query.until.isValid() && time.date() > query.until)) return;
		Match result;
		if (ContainsWords(file.completeBaseName(), query.words)) result.kind = query.words.isEmpty() ? "date" : "filename";
		else if (inspectedMetadata < 320)
		{
			bool readSubtitle = false;
			result = FindMatch(file, query.words, readSubtitle);
			++inspectedMetadata;
			if (readSubtitle) ++openedSidecars;
		}
		if (result.kind.isEmpty()) return;
		matches.append({{"name", file.fileName()}, {"path", path}, {"recorded_at", time.toString(Qt::ISODate)},
			{"match", result.kind}, {"excerpt", result.text}, {"at_ms", result.at}});
	};
	for (const auto& path : context.recent.mid(0, 10))
		if (!stopped()) inspect(QFileInfo(path));
	for (const auto& folder : folders)
	{
		if (stopped()) break;
		const auto path = QFileInfo(folder).canonicalFilePath();
		if (folder.isEmpty() || path.isEmpty() || checkedFolders.contains(path.toCaseFolded())) continue;
		checkedFolders.insert(path.toCaseFolded());
		QDirIterator files(path, {"*.mp4", "*.mov", "*.mkv", "*.avi", "*.flv", "*.webm"},
			QDir::Files | QDir::Readable | QDir::NoSymLinks);
		while (!stopped() && files.hasNext()) { files.next(); inspect(files.fileInfo()); }
	}
	if (cancel && cancel->load()) return Failure("cancelled");
	std::sort(matches.begin(), matches.end(), [](const QJsonObject& a, const QJsonObject& b)
		{ return a["recorded_at"].toString() > b["recorded_at"].toString(); });
	QJsonArray results;
	QElapsedTimer metadataDeadline;
	metadataDeadline.start();
	for (auto match : matches.mid(0, query.limit))
	{
		if (cancel && cancel->load()) return Failure("cancelled");
		const auto path = match["path"].toString();
		const auto markers = ReadRecordingMarkers(path);
		QJsonArray preview;
		for (int i = 0; i < qMin(markers.size(), 20); ++i) preview.append(markers.at(i));
		match["marker_count"] = markers.size();
		match["markers"] = preview;
		const auto remainingMs = 3500 - metadataDeadline.elapsed();
		KRREditProject media;
		QString error;
		if (remainingMs > 0 && ProbeEditSource(path, media, error, nullptr, static_cast<int>(qMin<qint64>(1000, remainingMs)), cancel))
			match["duration_ms"] = media.duration;
		results.append(match);
	}
	if (cancel && cancel->load()) return Failure("cancelled");
	return {{"ok", true}, {"results", results}, {"examined", examined},
		{"truncated", stopped() || inspectedMetadata >= 320 || matches.size() > query.limit},
		{"searched_subtitle_files", openedSidecars}};
}

QJsonObject DiagnoseAssistantRecording(const KRRAssistantContext& context, const QJsonObject& arguments)
{
	const auto topic = arguments["topic"].toString("latest");
	if (arguments.size() > (arguments.contains("topic") ? 1 : 0) ||
		(arguments.contains("topic") && !arguments["topic"].isString()) ||
		!QStringList{"latest", "devices", "encoder"}.contains(topic)) return Failure("invalid_diagnosis");
	const auto live = AssistantRecordingState();
	QSettings history(HistoryFile(), QSettings::IniFormat);
	auto prior = QJsonDocument::fromJson(history.value("failure").toByteArray()).object();
	if (prior.isEmpty()) prior = DiagnosticFacts(live, {});
	const bool active = live["state"] == "recording";
	QJsonObject facts{{"ok", true}, {"topic", topic}, {"settings", context.settings},
		{"last_failure", prior}, {"ffmpeg_found", !FindFFmpeg().isEmpty()},
		{"last_recording", QJsonDocument::fromJson(history.value("last_recording").toByteArray()).object()},
		{"recording_state", live["state"].toString("idle_or_service_not_started")}};
	if (!active && topic == "devices") facts["default_endpoint_presence"] = AudioEndpointFacts();
	return facts;
}

QJsonObject AssistantSettingSchema()
{
	QJsonArray frameRates;
	for (int fps : RecordingFrameRates) frameRates.append(fps);
	return {
		{"resolution", QJsonObject{{"type", "string"}, {"description", "auto or an even WIDTHxHEIGHT. 720p is 1280x720, 1080p is 1920x1080."}}},
		{"fps", QJsonObject{{"type", "integer"}, {"enum", frameRates}}},
		{"bitrate_kbps", QJsonObject{{"type", "integer"}, {"minimum", 100}, {"maximum", 100000}}},
		{"audio", Choice({"none", "mic", "system", "mix"}, "Silence, microphone only, system audio only, or both.")},
		{"encoder", Choice({"auto", "libx264", "h264_nvenc", "h264_amf", "h264_qsv"}, "auto prefers a usable GPU; libx264 uses the CPU.")},
		{"format", Choice({"mp4", "mov", "mkv", "avi", "flv"}, "Recording container.")},
		{"separate_audio", QJsonObject{{"type", "boolean"}}},
		{"denoise", QJsonObject{{"type", "boolean"}}},
		{"cursor", QJsonObject{{"type", "boolean"}}}
	};
}

QJsonObject PreviewAssistantSettings(const QJsonObject& current, const QJsonObject& changes)
{
	if (changes.isEmpty()) return Failure("empty_changes");
	const auto schema = AssistantSettingSchema();
	QJsonObject after = current, effective;
	QJsonArray diff;
	for (auto i = changes.begin(); i != changes.end(); ++i)
	{
		if (!schema.contains(i.key()) || !ValidSetting(schema[i.key()].toObject(), i.value()))
			return Failure("invalid_setting", i.key());
		if (current[i.key()] == i.value()) continue;
		diff.append(QJsonObject{{"field", i.key()}, {"before", current[i.key()]}, {"after", i.value()}});
		after[i.key()] = i.value();
		effective[i.key()] = i.value();
	}
	if (after["separate_audio"].toBool() &&
		!QStringList{"mp4", "mov", "mkv"}.contains(after["format"].toString()))
		return Failure("separate_audio_needs_container", "format");
	return {{"ok", true}, {"applied", false}, {"changes", effective}, {"diff", diff}};
}
