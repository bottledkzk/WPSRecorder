// climain.cpp | 3226535826@qq.com | 2026-10-31 | Scriptable foreground and background recording.
#include "controlserver.h"
#include "compositor.h"
#include "embeddedcli.h"
#include "displaygeometry.h"
#include "mediaeditor.h"
#include "framecapture.h"
#include "cameracapture.h"
#include "recordingcheck.h"
#include "recordingrecovery.h"
#include "recordingmarkers.h"
#include "recordertasks.h"
#include "speechservices.h"
#include "windowidentity.h"

#include <windows.h>
#include <objbase.h>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <string_view>

static std::atomic<bool> gs_interrupted{false};
static constexpr int StopRequestTimeoutMs = 1920000;
static BOOL WINAPI ConsoleInterrupt(DWORD type)
{
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
	{
		gs_interrupted.store(true);
		return TRUE;
	}
	return FALSE;
}
static int Fail(const QString& message)
{
	std::cerr << message.toUtf8().constData() << std::endl;
	return 1;
}
static void PrintJson(const QJsonObject& object)
{
	std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << std::endl;
}
static QSize ParseResolution(const QString& value)
{
	static const QRegularExpression pattern("^(\\d+)x(\\d+)$");
	const auto resolution = pattern.match(value);
	return resolution.hasMatch() ? QSize(resolution.captured(1).toInt(), resolution.captured(2).toInt()) : QSize();
}
static QJsonDocument ReadStdinObject()
{
	QFile input;
	if (!input.open(stdin, QIODevice::ReadOnly)) return {};
	constexpr int limit = RecordingEffectsByteLimit + 64 * 1024;
	QJsonParseError parse;
	const auto bytes = input.read(limit + 1);
	const auto document = QJsonDocument::fromJson(bytes, &parse);
	if (bytes.size() > limit || parse.error != QJsonParseError::NoError || !document.isObject())
		return {};
	return document;
}
static int CaptureCheck()
{
	const auto document = ReadStdinObject();
	if (document.isNull()) return Fail("Expected a capture configuration JSON object on stdin");
	auto config = KRRRecordingConfig::FromJson(document.object());
	const bool liveCamera = document.object()["liveCamera"].toBool();
	const bool livePreview = document.object()["livePreview"].toBool();
	QString captureError, encoderError;
	QJsonObject info;
	QByteArray jpeg;
	bool sourceReady = false;
	if (config.validate(captureError))
	{
		if (liveCamera && config.source == "camera")
		{
			info = {{"api", "Media Foundation"}};
			sourceReady = true;
		}
		else if (livePreview && config.source != "camera")
			sourceReady = true; // The dialog owns its continuous capture and reports source health.
		else
		{
			if (liveCamera)
			{
				if (config.cameraEnabled) config.effects = {};
				config.cameraEnabled = config.cameraTrack = false;
			}
			const auto image = CaptureRecordingPreview(config, info, captureError);
			QBuffer buffer(&jpeg);
			buffer.open(QIODevice::WriteOnly);
			sourceReady = !image.isNull() && image.save(&buffer, "JPEG", 94);
		}
	}
	const auto encoder = sourceReady ? SelectVideoEncoder(config.encoder, encoderError) : QString();
	PrintJson({{"ok", sourceReady && !encoder.isEmpty()}, {"capture", info},
			   {"image", QString::fromLatin1(jpeg.toBase64())}, {"capture_error", captureError},
			   {"encoder", encoder}, {"encoder_error", encoderError}});
	return 0;
}
int main(int argc, char* argv[])
{
	InitializeDisplayCoordinates();
	// Recording and the capture check paint text into frames, which requires a Qt font backend.
	const bool paintsFrames = std::any_of(argv + 1, argv + argc, [](const char* arg)
	{
		const std::string_view option(arg);
		return option == "--serve" || option == "--capture-check";
	});
	struct ComLifetime
	{
		HRESULT result;
		~ComLifetime() { if (SUCCEEDED(result)) CoUninitialize(); }
	} com{CoInitializeEx(nullptr, paintsFrames ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED)};
	if (FAILED(com.result)) return Fail("Could not initialize the CLI COM apartment");
	std::unique_ptr<QCoreApplication> application;
	if (paintsFrames)
	{
		QCoreApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
		application = std::make_unique<QGuiApplication>(argc, argv);
	}
	else application = std::make_unique<QCoreApplication>(argc, argv);
	auto& app = *application;
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder");
	app.setApplicationVersion(WPSRECORDER_VERSION);
	if (app.arguments().contains("--version") || app.arguments().contains("-v"))
	{
		std::cout << "wpsrecorder " WPSRECORDER_VERSION << std::endl;
		return 0;
	}
	QCommandLineParser parser;
	parser.setApplicationDescription("Windows screen, window and camera recording with WASAPI audio and FFmpeg editing");
	parser.addHelpOption();
	parser.addVersionOption();
	const QStringList commands{"record", "start", "stop", "status", "pause", "resume", "serve", "trim", "split", "merge", "export", "edit-project", "transcribe", "devices", "apply-scene", "schedule", "cancel-schedule", "service-exit","synthesize","narrate","voices", "capture-check", "recover", "mark", "markers", "task", "translate-subtitles", "export-bilingual", "dub-track"};
	for (const QString& command : commands)
		parser.addOption({command, command + " operation"});
	parser.addOption({"create-shortcut", "Create a Windows shortcut to this package's recorder"});
	parser.addOptions({{{"o", "output"}, "Output video path", "path"}, {"duration", "Active recording seconds", "seconds"},
		{"res", "Output resolution for recording or merge (merge defaults to first input)", "WIDTHxHEIGHT", "1920x1080"}, {"fps", "Frames per second", "number", "30"},
		{"bitrate", "Video kbps", "number", "6000"}, {"audio", "none/mic/system/mix", "source", "none"},
		{"source", "screen/region/window/camera/image", "source", "screen"}, {"region", "Physical x,y,width,height", "rectangle"},
		{"window", "Native HWND as decimal", "handle"}, {"no-cursor", "Hide cursor"}, {"clicks", "Highlight held mouse clicks"},
		{"input", "Input media path, repeat for merge", "path"}, {"begin", "Clip start seconds", "seconds", "0"},
		{"end", "Clip end seconds", "seconds", "-1"}, {"intervals", "Split ranges e.g. 0-2;2-5", "ranges"},
		{"output-color", "sdr/hdr-to-sdr/hdr/sdr-to-hdr", "mode", "sdr"}, {"screen-color","auto/highlights HDR-to-SDR mapping","mode","auto"},{"screen-white","HDR SDR white reference nits; 0 follows Windows","nits","0"},{"screen-exposure","Screen exposure compensation -2..2 EV","EV","0"},
		{"monitor", "Monitor index from --devices", "index", "0"}, {"encoder", "auto/libx264/h264_nvenc/h264_qsv/h264_amf", "name", "auto"},
		{"separate-audio", "Keep microphone and system audio as independent tracks"}, {"camera-track", "Save raw camera track beside output"},
		{"camera", "Camera index from --devices", "index", "0"}, {"pip", "Overlay the camera"}, {"image", "Image source path", "path"},
		{"preset", "Version 1 JSON scene preset", "path"}, {"denoise", "Audio noise reduction"}, {"max-duration", "Auto-stop active seconds", "seconds", "0"},
		{"start-at", "Scheduled ISO8601 time with timezone", "time"}, {"whisper", "whisper-cli executable", "path"}, {"model", "Offline GGML model", "path"},
		{"language", "Subtitle language or auto", "code", "auto"}, {"subtitles", "Embed existing SRT in video export", "path"}, {"copy-streams", "Export or merge compatible video streams without re-encoding"}, {"mix-tracks","Mix timeline audio tracks"}, {"soft-captions","Keep timeline captions as an optional subtitle stream"},
		{"burn-captions", "Burn timeline captions into the picture, including multi-source projects"},
		{"label", "Optional recording marker label", "text"},
		{"text-file","UTF-8 narration text file","path"},{"tts-engine","audio8 or a configured speech service","name","audio8"},{"voice","Voice ID for the selected engine","id"},{"speed","Speech speed 0.5 to 2","number","1"},{"voice-volume","Speech volume percent 0 to 150","number","100"},{"original-volume","Original audio volume percent 0 to 100","number","30"},{"speech","Narration WAV input","path"},
		{"glossary","Optional subtitle translation glossary, up to 1000 characters","text"},
		{"line","Translate a single one-based line number","number"}});
	if (app.arguments().contains("--help") || app.arguments().contains("-h"))
	{
		std::cout << parser.helpText().toUtf8().constData() << std::endl;
		return 0;
	}
	if (!parser.parse(app.arguments())) return Fail(parser.errorText());
	if (qEnvironmentVariableIsSet(EmbeddedCliEnvironment) && parser.isSet("whisper"))
		return Fail("The embedded terminal uses the bundled recognition engine");
	if (parser.isSet("devices")) { PrintJson({{"monitors", KRRFrameCapture::Monitors()}, {"cameras", KRRCameraCapture::Devices()}, {"process_app_container", IsContainerProcess()}, {"process_integrity_rid", ProcessIntegrityRid()}}); return 0; }
	if (parser.isSet("serve"))
	{
		KRRRecordingEngine engine;
		KRRControlServer server(engine);
		QString error;
		if (!server.listen(error)) return Fail(error);
		QTimer idle;
		int idleTicks = 0;
		QObject::connect(&idle, &QTimer::timeout, &app, [&]
		{
			idleTicks = engine.isActive() || server.hasScheduledTask() ? 0 : idleTicks + 1;
			if (idleTicks >= 60) app.quit();
		});
		idle.start(1000);
		return app.exec();
	}
	QStringList selected;
	for (const QString& command : commands)
		if (parser.isSet(command)) selected << command;
	if (parser.isSet("create-shortcut")) selected << "create-shortcut";
	if (selected.size() != 1) return Fail("Select exactly one operation; use --help");
	QString command = selected.front();
	QString error;
	if (command == "create-shortcut")
		return CreateRecorderShortcut(parser.value("output"), error) ? 0 : Fail(error);
	if (command == "task")
	{
		SetConsoleCtrlHandler(ConsoleInterrupt, TRUE);
		const auto request = ReadStdinObject();
		const auto result = request.isNull() ? QJsonObject{{"ok", false}, {"code", "invalid_json"}} :
			RunRecorderTask(request.object(), parser.value("input"), parser.value("output"), &gs_interrupted);
		PrintJson(result);
		return result["ok"].toBool() ? 0 : 1;
	}
	if (command == "capture-check") return CaptureCheck();
	if (command == "mark")
	{
		QJsonObject response;
		if (!SendControl({{"command", "mark"}, {"label", parser.value("label")}}, response, error, 3000))
			return Fail(error);
		PrintJson(response);
		return 0;
	}
	if (command == "markers")
	{
		if (!QFileInfo(parser.value("input")).isFile()) return Fail("Use --input with an existing video");
		PrintJson({{"ok", true}, {"markers", ReadRecordingMarkers(parser.value("input"))}});
		return 0;
	}
	if (command == "recover")
	{
		if (!parser.isSet("input") || !parser.isSet("output"))
			return Fail("Recovery needs --input workspace and --output new.mp4");
		const auto output = QFileInfo(parser.value("output")).absoluteFilePath();
		const auto failure = RecoverRecording(QFileInfo(parser.value("input")).absoluteFilePath(), output);
		if (!failure.isEmpty()) return Fail(failure);
		PrintJson({{"ok", true}, {"output", output}});
		return 0;
	}
	if (command == "voices")
	{
		PrintJson({{"audio8_available", Audio8Available()}, {"audio8", QJsonArray::fromStringList(Audio8Voices())}});
		return 0;
	}
	if(command=="synthesize")
	{
		QFile file(parser.value("text-file"));if(!parser.isSet("output")||!file.open(QIODevice::ReadOnly)||file.size()>20000)return Fail("Use --text-file UTF-8 text (up to 4000 characters) and --output new.wav");
		KRRVoiceOptions options;options.engine=parser.value("tts-engine");options.language=parser.value("language")=="en"?"en":"zh";options.voice=parser.isSet("voice")?parser.value("voice"):DefaultVoice(options.engine,options.language);bool speedOk=false,volumeOk=false;options.speed=parser.value("speed").toDouble(&speedOk);options.volume=parser.value("voice-volume").toDouble(&volumeOk)/100.;
		if(!speedOk||!volumeOk)return Fail("Invalid speech speed or volume");
		SetConsoleCtrlHandler(ConsoleInterrupt, TRUE);
		const auto output=QFileInfo(parser.value("output")).absoluteFilePath();
		error=SynthesizeSpeech(QString::fromUtf8(file.readAll()).remove(QChar(0xfeff)),output,options,&gs_interrupted);
		if(!error.isEmpty())return Fail(error);
		PrintJson({{"ok",true},{"output",output},{"duration_ms",SpeechWaveDuration(output)}});return 0;
	}
	if (command == "translate-subtitles" || command == "export-bilingual" || command == "dub-track")
	{
		const auto input = parser.value("input");
		const auto output = QFileInfo(parser.value("output")).absoluteFilePath();
		if (input.isEmpty() || !parser.isSet("output") || QFileInfo::exists(output)) return Fail("Use --input and a new --output path");
		SetConsoleCtrlHandler(ConsoleInterrupt, TRUE);
		KRREditProject project;
		if (command == "translate-subtitles")
		{
			if (!ProbeEditSource(input, project, error) ||
				!ReadEditSubtitles(parser.value("subtitles"), project.captions, error)) return Fail(error);
			for (auto& cue : project.captions) cue.end = qMin(cue.end, project.duration);
			if (!project.validate(error)) return Fail(error);
			const auto target = parser.value("language");
			int only = -1;
			if (parser.isSet("line"))
			{
				bool valid = false;
				const int line = parser.value("line").toInt(&valid);
				if (!valid || line < 1) return Fail("Subtitle line starts at 1");
				only = line - 1;
			}
			QVector<QString> text, translated;
			for (const auto& cue : project.captions) text.append(cue.text);
			error = TranslateSubtitleLines(text, target, parser.value("glossary"), translated, &gs_interrupted, {}, only);
			if (!error.isEmpty()) return Fail(error);
			project.subtitleLanguage = target == "en" ? "zh" : "en";
			project.translationLanguage = target;
			project.subtitleGlossary = parser.value("glossary");
			for (int i = 0; i < translated.size(); ++i) project.captions[i].translation = translated[i];
			QSaveFile file(output);
			const auto bytes = QJsonDocument(project.toJson()).toJson();
			if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
				return Fail("Could not save the translated project");
		}
		else
		{
			QFile file(input);
			if (!file.open(QIODevice::ReadOnly) || file.size() > 4000000) return Fail("Could not read bounded input JSON");
			QJsonParseError parse;
			const auto object = QJsonDocument::fromJson(file.readAll(), &parse).object();
			if (parse.error != QJsonParseError::NoError || object.isEmpty()) return Fail("Invalid input JSON");
			if (command == "export-bilingual")
			{
				if (!KRREditProject::FromJson(object, project, error)) return Fail(error);
				error = ExportSubtitleFiles(project, output);
			}
			else
			{
				QVector<KRRDubLine> lines;
				for (const auto& value : object["lines"].toArray())
				{
					const auto line = value.toObject();
					lines.append({line["begin_ms"].toVariant().toLongLong(), line["end_ms"].toVariant().toLongLong(),
						QFileInfo(line["audio"].toString()).absoluteFilePath()});
				}
				if (QFileInfo(output).suffix().toLower() != "wav") return Fail("Dubbing requires a WAV path");
				error = MixDubbingTrack(object["duration_ms"].toVariant().toLongLong(), lines, output, &gs_interrupted);
			}
			if (!error.isEmpty()) return Fail(error);
		}
		PrintJson({{"ok", true}, {"output", output}});
		return 0;
	}
	if(command=="narrate")
	{
		if(!parser.isSet("input")||!parser.isSet("speech")||!parser.isSet("output"))return Fail("Use --input video --speech WAV --output new.mp4");bool beginOk=false,volumeOk=false;double begin=parser.value("begin").toDouble(&beginOk),volume=parser.value("original-volume").toDouble(&volumeOk)/100.;if(!beginOk||!volumeOk)return Fail("Invalid narration time or volume");
		error=MixNarration(parser.value("input"),parser.value("speech"),QFileInfo(parser.value("output")).absoluteFilePath(),begin,volume);if(!error.isEmpty())return Fail(error);PrintJson({{"ok",true},{"output",QFileInfo(parser.value("output")).absoluteFilePath()}});return 0;
	}
	if(command=="edit-project")
	{
		if (!parser.isSet("input") || !parser.isSet("output")) return Fail("Timeline requires JSON --input and --output");
		bool bitrateOk = false;
		const int videoKbps = parser.value("bitrate").toInt(&bitrateOk);
		if (!bitrateOk || videoKbps < 100 || videoKbps > 100000) return Fail("Timeline bitrate must be 100..100000 kbps");
		const bool burn = parser.isSet("burn-captions");
		const bool soft = parser.isSet("soft-captions");
		if (burn && soft) return Fail("Choose one caption mode: --burn-captions or --soft-captions");
		QJsonObject object;
		if (!ReadEditProjectJson(parser.value("input"), object, error)) return Fail(error);
		const auto output = QFileInfo(parser.value("output")).absoluteFilePath();
		const bool separate = !parser.isSet("mix-tracks");
		qint64 duration = 0;
		if (object["kind"].toString() == "sequence")
		{
			KRREditSequence sequence;
			if (!ReadEditSequence(object, sequence, error)) return Fail(error);
			duration = EditSequenceDuration(sequence);
			error = RenderEditSequence(sequence, output, parser.value("encoder"), separate, nullptr, {}, false, {videoKbps, burn});
		}
		else
		{
			KRREditProject project;
			if (!KRREditProject::FromJson(object, project, error)) return Fail(error);
			duration = project.outputDuration();
			KRRTimelineExportOptions options;
			options.videoKbps = videoKbps;
			error = RenderEditProject(project, output, parser.value("encoder"), separate, !soft, nullptr, options);
		}
		if (!error.isEmpty()) return Fail(error);
		PrintJson({{"ok", true}, {"output", output}, {"duration_ms", duration}});
		return 0;
	}
	QJsonObject response;
	QJsonObject preset;
	if (parser.isSet("preset"))
	{
		QFile file(parser.value("preset"));
		if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * RecordingEffectsByteLimit) return Fail("Cannot read bounded scene preset");
		QJsonParseError parse; auto document = QJsonDocument::fromJson(file.readAll(), &parse);
		if (parse.error != QJsonParseError::NoError || document.object()["version"].toInt() != 1 || !document.object()["scene"].isObject()) return Fail("Preset must contain version:1 and scene object");
		preset = document.object()["scene"].toObject();
	}
	if (command == "apply-scene")
	{
		if (preset.isEmpty()) return Fail("Use --preset for the scene to apply");
		if (!SendControl({{"command", "scene"}, {"scene", preset}}, response, error, 5000)) return Fail(error);
		PrintJson(response); return 0;
	}
	if (command == "trim" || command == "split" || command == "merge" || command == "export" || command == "transcribe")
	{
		KRRMediaEditor editor;
		int result = 0;
		QObject::connect(&editor, &KRRMediaEditor::completed, &app, [&](const QString& output, const QString& failure)
		{
			if (!failure.isEmpty()) result = Fail(failure);
			else PrintJson({{"ok", true}, {"output", output}});
			app.quit();
		});
		QStringList inputs = parser.values("input");
		QString output = QFileInfo(parser.value("output")).absoluteFilePath();
		if (inputs.isEmpty() || !parser.isSet("output")) return Fail("Editing requires --input and --output");
		KRRExportOptions options; options.encoder = parser.value("encoder"); options.denoise = parser.isSet("denoise"); options.subtitles = parser.value("subtitles");
		options.copyStreams = parser.isSet("copy-streams");
		if (parser.isSet("bitrate"))
		{
			if (command != "merge" && command != "export") return Fail("Use --export or --merge to set the video bitrate");
			bool valid = false;
			options.videoKbps = parser.value("bitrate").toInt(&valid);
			if (!valid || options.videoKbps < 100 || options.videoKbps > 100000) return Fail("Use 100 to 100000 kbps for --bitrate");
		}
		if (command == "transcribe")
		{
			QString executable = parser.value("whisper"), model = parser.value("model");
			if (executable.isEmpty()) executable = app.applicationDirPath() + "/whisper/whisper-cli.exe";
			if (model.isEmpty()) model = app.applicationDirPath() + "/whisper/ggml-base.bin";
			editor.transcribe(inputs[0], output, executable, model, parser.value("language"));
		}
		else if (command == "export" || command == "trim")
		{
			bool beginOk = false, endOk = false;
			const auto begin = parser.value("begin").toDouble(&beginOk);
			const auto end = parser.value("end").toDouble(&endOk);
			if (!beginOk || !endOk || !std::isfinite(begin) || !std::isfinite(end) || begin < 0 || (end != -1 && end <= begin))
				return Fail("Use a non-negative --begin and a later --end in seconds; -1 means end of video");
			if (command == "trim") editor.trim(inputs[0], output, begin, end);
			else editor.exportMedia(inputs[0], output, begin, end, options);
		}
		else if (command == "merge")
		{
			QSize resolution;
			if (parser.isSet("fps"))
			{
				bool valid = false;
				options.fps = parser.value("fps").toInt(&valid);
				if (!valid || options.fps < 1 || options.fps > RecordingFrameRates.back()) return Fail("Invalid --fps for merge");
			}
			if (parser.isSet("res"))
			{
				resolution = ParseResolution(parser.value("res"));
				if (resolution.isEmpty() || resolution.width() % 2 || resolution.height() % 2)
					return Fail("Use positive even WIDTHxHEIGHT for --merge --res");
			}
			editor.merge(inputs, output, options, resolution);
		}
		else
		{
			const QFileInfo directory(output);
			if (!directory.isDir() && QStringList{"mp4", "mov", "mkv", "avi", "flv", "webm", "m4v"}.contains(directory.suffix().toLower()))
				return Fail("--split --output requires a directory path");
			QList<QPair<double, double>> intervals;
			for (const auto& range : parser.value("intervals").split(';', Qt::SkipEmptyParts))
			{
				QStringList parts = range.split('-');
				if (parts.size() != 2) return Fail("Invalid split interval");
				bool beginOk = false, endOk = false;
				const auto begin = parts[0].toDouble(&beginOk);
				const auto end = parts[1].toDouble(&endOk);
				if (!beginOk || !endOk || !std::isfinite(begin) || !std::isfinite(end) || begin < 0 || end <= begin)
					return Fail("Invalid split interval");
				intervals.append({begin, end});
			}
			editor.split(inputs[0], output, intervals);
		}
		app.exec();
		return result;
	}
	if (command == "status")
	{
		if (!SendControl({{"command", command}}, response, error, 2000))
			response = {{"state", "idle"}, {"duration_sec", 0}, {"frames_written", 0}, {"avg_fps", 0}, {"mem_mb", 0}, {"cpu_pct", 0}};
		PrintJson(response);
		return 0;
	}
	if (command == "stop" || command == "pause" || command == "resume" || command == "service-exit" || command == "cancel-schedule")
	{
		if (!SendControl({{"command", command}}, response, error, command == "stop" ? StopRequestTimeoutMs : 5000)) return Fail(error);
		PrintJson(response);
		return 0;
	}
	KRRRecordingConfig config;
	config.output = QFileInfo(parser.value("output")).absoluteFilePath();
	if (!parser.isSet("output")) return Fail("Recording requires --output");
	const auto resolution = ParseResolution(parser.value("res"));
	if (resolution == QSize()) return Fail("Use WIDTHxHEIGHT for --res");
	config.width = resolution.width();
	config.height = resolution.height();
	bool fpsOk = false, bitrateOk = false, monitorOk = false, cameraOk = false, windowOk = false;
	config.fps = parser.value("fps").toInt(&fpsOk);
	config.bitrate = parser.value("bitrate").toInt(&bitrateOk);
	config.audio = parser.value("audio");
	config.source = parser.value("source");
	config.window = static_cast<quintptr>(parser.value("window").toULongLong(&windowOk));
	config.cursor = !parser.isSet("no-cursor");
	config.clicks = parser.isSet("clicks");
	config.monitor = parser.value("monitor").toInt(&monitorOk); config.encoder = parser.value("encoder");
	config.separateAudio = parser.isSet("separate-audio"); config.cameraTrack = parser.isSet("camera-track");
	config.camera = parser.value("camera").toInt(&cameraOk); config.cameraEnabled = parser.isSet("pip"); config.image = parser.value("image");
	if (!fpsOk || !bitrateOk || !monitorOk || !cameraOk || (parser.isSet("window") && !windowOk))
		return Fail("Invalid numeric value for --fps, --bitrate, --monitor, --camera or --window");
	config.denoise = parser.isSet("denoise");
	config.outputColor = parser.value("output-color");
	config.screenColor=parser.value("screen-color");bool whiteOk=false,exposureOk=false;config.screenWhiteNits=parser.value("screen-white").toDouble(&whiteOk);config.screenExposureEV=parser.value("screen-exposure").toDouble(&exposureOk);if(!whiteOk||!exposureOk)return Fail("Invalid screen color number");
	bool maxOk = false; config.maxDuration = parser.value("max-duration").toDouble(&maxOk);
	if (!maxOk) return Fail("Invalid --max-duration");
	if (parser.isSet("region"))
	{
		QStringList parts = parser.value("region").split(',');
		if (parts.size() != 4) return Fail("Use x,y,width,height for --region");
		QVector<int> rectangle;
		for (const auto& part : parts)
		{
			bool valid = false;
			rectangle.append(part.toInt(&valid));
			if (!valid) return Fail("Use integer x,y,width,height for --region");
		}
		config.region = QRect(rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
	}
	if (!preset.isEmpty())
	{
		auto object = config.toJson(); for (auto it = preset.begin(); it != preset.end(); ++it) object[it.key()] = it.value();
		config = KRRRecordingConfig::FromJson(object);
	}
	bool durationOk = false;
	double duration = parser.value("duration").toDouble(&durationOk);
	if (command == "record" && (!durationOk || !std::isfinite(duration) || duration <= 0))
		return Fail("--record requires a positive finite --duration");
	if (command == "record")
		config.maxDuration = config.maxDuration > 0 ? qMin(config.maxDuration, duration) : duration;
	if (!config.validate(error) || !EnsureControlService(error)) return Fail(error);
	if (command == "schedule")
	{
		if (!SendControl({{"command", "schedule"}, {"config", config.toJson()}, {"startAt", parser.value("start-at")}}, response, error, 3000)) return Fail(error);
		PrintJson(response); return 0;
	}
	if (!SendControl({{"command", "start"}, {"config", config.toJson()}}, response, error, 15000)) return Fail(error);
	auto finished = [&]
	{
		if (response["output"] != config.output || !response["output_published"].toBool() || !response["error"].toString().isEmpty())
			return Fail(response["error"].toString("Recording ended before publishing the output"));
		PrintJson(response);
		return 0;
	};
	// Do not report start success until device and encoder initialization have completed.
	for (int attempt = 0; attempt < 200; ++attempt)
	{
		QThread::msleep(50);
		if (!SendControl({{"command", "status"}}, response, error, 2000)) return Fail(error);
		if (response["state"] == "idle") return command == "record" ? finished() : Fail(response["error"].toString("Recording failed to start"));
		if (response["phase"] == "recording" || (command == "record" && response["phase"] == "finalizing")) break;
		if (attempt == 199) return Fail("Recording initialization timed out");
	}
	if (command == "start") { PrintJson(response); return 0; }
	SetConsoleCtrlHandler(ConsoleInterrupt, TRUE);
	while (!gs_interrupted.load())
	{
		QThread::msleep(100);
		if (!SendControl({{"command", "status"}}, response, error, 2000)) return Fail(error);
		if (response["state"] == "idle") return finished();
	}
	if (!SendControl({{"command", "stop"}}, response, error, StopRequestTimeoutMs)) return Fail(error);
	PrintJson(response);
	return 0;
}
