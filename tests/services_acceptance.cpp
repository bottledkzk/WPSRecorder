#include "subtitledialog.h"
#include "dubbing.h"
#include "recordingassistanttools.h"
#include "recordingconfig.h"
#include "recordingmarkers.h"
#include "speech.h"
#include "speechservices.h"
#include "speechsettingsdialog.h"
#include "uitheme.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <functional>
#include <limits>

static bool WaitUntil(const std::function<bool()>& ready, int ms = 6000)
{
	QElapsedTimer clock;
	clock.start();
	while (clock.elapsed() < ms)
	{
		QApplication::processEvents();
		if (ready()) return true;
		QThread::msleep(10);
	}
	return false;
}

class Fixture final : public QTcpServer
{
public:
	explicit Fixture(const QString& root) : files(QDir(root).filePath("service-fixture-XXXXXX"))
	{
		connect(this, &QTcpServer::newConnection, this, [this]
		{
			auto socket = nextPendingConnection();
			connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
			connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readRequest(socket); });
			if (socket->bytesAvailable()) readRequest(socket);
		});
	}
	QString endpoint(const QString& route) const { return QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(route); }
	bool initializeAudio()
	{
		const auto file = files.filePath("tone.wav");
		QString error;
		if (!RunMediaProcess(FindFFmpeg(), {"-v", "error", "-nostdin", "-f", "lavfi", "-i",
			"sine=frequency=550:duration=0.6", "-ac", "1", "-ar", "48000", "-c:a", "pcm_s16le", file},
			error, 8000)) return false;
		QFile source(file);
		if (!source.open(QIODevice::ReadOnly)) return false;
		wave = source.readAll();
		return wave.size() > 5000;
	}
	QString voice;
	int requests = 0, pictureReads = 0;
	bool hasToolResult = false;

private:
	QString readColor(const QJsonObject& media, bool& isVideo)
	{
		isVideo = media["type"] == "video_url";
		const auto url = media[isVideo ? "video_url" : "image_url"].toObject()["url"].toString();
		const auto base64 = url.section(',', 1).toLatin1();
		if (base64.isEmpty()) return {};
		const auto bytes = QByteArray::fromBase64(base64);
		QColor pixel;
		if (isVideo)
		{
			QFile source(files.filePath("received.mp4"));
			if (!source.open(QIODevice::WriteOnly | QIODevice::Truncate) || source.write(bytes) != bytes.size()) return {};
			source.close();
			QProcess ffmpeg;
			HideConsoleProcess(ffmpeg);
			ffmpeg.start(FindFFmpeg(), {"-v", "error", "-nostdin", "-i", source.fileName(), "-vf", "scale=1:1",
				"-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"});
			if (!ffmpeg.waitForFinished(3000) || ffmpeg.exitCode() != 0) return {};
			const auto rgb = ffmpeg.readAllStandardOutput();
			if (rgb.size() != 3) return {};
			pixel = QColor(uchar(rgb[0]), uchar(rgb[1]), uchar(rgb[2]));
		}
		else
		{
			const auto image = QImage::fromData(bytes);
			if (image.isNull()) return {};
			pixel = image.pixelColor(image.width() / 2, image.height() / 2);
		}
		if (!pixel.isValid()) return {};
		++pictureReads;
		int distance = std::numeric_limits<int>::max();
		QString found;
		for (const QString& color : {"red", "green", "blue", "yellow", "purple"})
		{
			const QColor sample(color);
			const auto delta = qAbs(pixel.red() - sample.red()) + qAbs(pixel.green() - sample.green()) +
				qAbs(pixel.blue() - sample.blue());
			if (delta < distance) { distance = delta; found = color; }
		}
		return found;
	}

	void readRequest(QTcpSocket* socket)
	{
		const auto received = socket->property("http").toByteArray() + socket->readAll();
		socket->setProperty("http", received);
		const auto split = received.indexOf("\r\n\r\n");
		if (split < 0) return;
		const auto headers = received.left(split).split('\n');
		int length = 0;
		for (const auto& header : headers)
			if (header.toLower().startsWith("content-length:")) length = header.mid(15).trimmed().toInt();
		if (received.size() < split + 4 + length) return;
		const auto route = headers.value(0).split(' ').value(1);
		const auto body = QJsonDocument::fromJson(received.mid(split + 4, length)).object();
		++requests;
		auto reply = [&](int code, const QJsonObject& object)
		{
			const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
			const auto status = code == 200 ? QByteArray("200 OK") : QByteArray::number(code) + " Error";
			socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
				QByteArray::number(json.size()) + "\r\n\r\n" + json);
			socket->disconnectFromHost();
		};
		if (route == "/auth") { reply(401, {}); return; }
		if (route == "/reject-video") { reply(415, {}); return; }
		if (route == "/v1/messages")
		{
			const auto http = received.left(split).toLower();
			if (!http.contains("x-api-key: fixture-claude-key") || !http.contains("anthropic-version: 2023-06-01"))
			{
				reply(401, {});
				return;
			}
			const auto messages = body["messages"].toArray();
			const auto last = messages.at(messages.size() - 1).toObject();
			const auto content = last["content"].toArray();
			const auto blocks = [&reply](const QJsonArray& parts, const QString& stopReason)
			{
				reply(200, {{"role", "assistant"}, {"content", parts}, {"stop_reason", stopReason}});
			};
			const auto respond = [&blocks](const QString& answer)
			{
				blocks(QJsonArray{QJsonObject{{"type", "text"}, {"text", answer}}}, "end_turn");
			};
			if (!body["tools"].toArray().isEmpty())
			{
				if (content.at(0).toObject()["type"] == "tool_result")
				{
					const auto first = content.at(0).toObject();
					hasToolResult = last["role"] == "user" && first["tool_use_id"] == "toolu_probe" &&
						messages.at(1).toObject()["content"].toArray().at(0).toObject()["type"] == "tool_use";
					respond(hasToolResult ? first["content"].toString() : QString());
					return;
				}
				const auto tool = body["tools"].toArray().at(0).toObject();
				if (tool["name"] != "recorder_capability_probe" || !tool["input_schema"].isObject()) { reply(400, {}); return; }
				blocks(QJsonArray{QJsonObject{{"type", "tool_use"}, {"id", "toolu_probe"},
					{"name", "recorder_capability_probe"}, {"input", QJsonObject{{"value", 7}}}}}, "tool_use");
				return;
			}
			if (body["system"].toString().startsWith("Translate timed subtitles"))
			{
				const bool english = body["system"].toString().contains("English");
				respond(english ? R"([{"id":1,"text":"Start recording."}])" : QStringLiteral(R"([{"id":1,"text":"开始录制。"}])"));
				return;
			}
			QString color;
			for (const auto& item : content)
			{
				const auto part = item.toObject();
				if (part["type"] != "image") continue;
				const auto source = part["source"].toObject();
				const QString url = "data:" + source["media_type"].toString() + ";base64," + source["data"].toString();
				bool video = false;
				color = readColor(QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", url}}}}, video);
			}
			if (color.isEmpty()) { reply(415, {}); return; }
			respond(QString(R"({"parts":[{"at_ms":500,"title":"%1"}]})").arg(color));
			return;
		}
		if (route == "/ollama" || route == "/no-tools")
		{
			QJsonObject message{{"role", "assistant"}};
			const auto messages = body["messages"].toArray();
			const auto last = messages.at(messages.size() - 1).toObject();
			const auto name = QString("recorder_capability_probe");
			if (route == "/no-tools") message["content"] = "I cannot call tools.";
			else if (last["role"] == "tool")
			{
				hasToolResult = last["tool_name"].toString() == name &&
					messages.at(1).toObject()["tool_calls"].toArray().at(0).toObject()["function"].toObject()["arguments"].isObject();
				message["content"] = hasToolResult ? last["content"].toString() : QString();
			}
			else if (body["tools"].toArray().size() == 1)
			{
				const QJsonObject function{{"name", name}, {"arguments", QJsonObject{{"value", 7}}}};
				message["tool_calls"] = QJsonArray{QJsonObject{{"function", function}, {"type", "function"}}};
			}
			reply(200, {{"done", true}, {"message", message}});
			return;
		}
		if (route == "/translate")
		{
			if (!received.left(split).toLower().contains("x-service-key: fixture-key")) { reply(401, {}); return; }
			const bool english = body["instruction"].toString().contains("English");
			const QString word = english ? QStringLiteral("Start recording.") : QStringLiteral("开始录制。");
			reply(200, {{"result", QJsonObject{{"lines", QJsonArray{QJsonObject{{"id", 1}, {"text", word}}}}}}});
			return;
		}
		if (route == "/vision")
		{
			bool mp4 = false;
			const auto color = readColor(body["inputs"].toArray().at(0).toObject(), mp4);
			reply(color.isEmpty() ? 415 : 200, {{"highlight", QJsonObject{{"parts",
				QJsonArray{QJsonObject{{"at_ms", 500}, {"title", color}}}}}}});
			return;
		}
		voice = body["speaker"].toString();
		if (route == "/voice") reply(200, {{"payload", QJsonArray{QJsonObject{{"wav", QString::fromLatin1(wave.toBase64())}}}}});
		else reply(200, {{"payload", QJsonArray{QJsonObject{{"wav", "YWJj"}}}}});
	}

	QTemporaryDir files;
	QByteArray wave;
};

static KRRSpeechService VoiceService(const Fixture& mock)
{
	KRRSpeechService service{mock.endpoint("voice"), "fixture-voice", {}, "json"};
	service.voice = "configured-voice";
	service.credentialHeader.clear();
	service.requestTemplate = R"({"model":"$model","phrase":"$text","speaker":"$voice","speed":"$speed"})";
	service.audioEncoding = "base64";
	service.responsePath = "payload.0.wav";
	return service;
}

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	RecorderUi::Apply(app);
	const auto args = app.arguments();
	if (args.size() != 3) return 2;
	const auto mode = args[1];
	const auto report = args[2];
	qputenv("LOCALAPPDATA", (QFileInfo(report).absolutePath() + "/profiles/" + mode).toUtf8());
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder-services-qa-" + mode);
	QStandardPaths::setTestModeEnabled(true);
	Fixture mock(QFileInfo(report).absolutePath());
	if (mode != "recording-metadata" && mode != "audio8-batch" && !mock.listen(QHostAddress::LocalHost)) return 2;
	std::atomic<bool> cancel{false};
	QJsonObject result{{"case", mode}};
	bool passed = false;
	if (mode == "recording-metadata")
	{
		QTemporaryDir fixture;
		if (!fixture.isValid()) return 2;
		const auto video = fixture.filePath("recording-metadata.mp4");
		QString error;
		if (!RunMediaProcess(FindFFmpeg(), {"-v", "error", "-nostdin", "-f", "lavfi", "-i", "color=s=320x180:r=30:d=1.2",
			"-c:v", "libx264", video}, error, 15000)) return 2;
		const QJsonArray marks{QJsonObject{{"at_ms", 200}, {"label", "overview"}},
			QJsonObject{{"at_ms", 820}, {"label", "camera-switch"}}};
		if (!WriteRecordingMarkers(RecordingMarkersPath(video), marks, error)) return 2;
		const KRRAssistantContext context{fixture.path(), {}, {}};
		const auto find = [&](const QJsonObject& query)
		{ return FindAssistantRecordings(context, query)["results"].toArray().first().toObject(); };
		const auto filename = find({{"query", "recording-metadata"}});
		const auto date = find({});
		const auto marker = find({{"query", "camera-switch"}});
		passed = filename["match"] == "filename" && date["match"] == "date" && marker["match"] == "marker" &&
			marker["at_ms"] == 820;
		for (const auto& row : {filename, date, marker})
			passed = passed && row["marker_count"] == 2 && row["markers"].toArray() == marks && row["duration_ms"] == 1200;
		result["duration_ms"] = filename["duration_ms"];
		result["markers"] = filename["markers"];
	}
	else if (mode == "audio8-batch")
	{
		if (!Audio8Available()) return 2;
		QTemporaryDir fixture;
		if (!fixture.isValid()) return 2;
		KRRVoiceOptions voice;
		voice.voice = "mandarin-female";
		const QVector<KRRVoiceLine> batch{{QStringLiteral("准备录制。"), fixture.filePath("first.wav")},
			{QStringLiteral("现在开始剪辑。"), fixture.filePath("second.wav")}};
		QElapsedTimer elapsed;
		elapsed.start();
		auto error = SynthesizeAudio8Speech(batch, voice, &cancel);
		result["batch_ms"] = elapsed.restart();
		QJsonArray durations;
		passed = error.isEmpty();
		for (int i = 0; passed && i < batch.size(); ++i)
		{
			const auto target = fixture.filePath(QString("single-%1.wav").arg(i));
			error = SynthesizeSpeech(batch[i].text, target, voice, &cancel);
			const auto length = SpeechWaveDuration(batch[i].output);
			passed = error.isEmpty() && length > 0 && length == SpeechWaveDuration(target);
			durations.append(length);
		}
		result["individual_ms"] = elapsed.elapsed();
		result["speech_ms"] = durations;
		if (!error.isEmpty()) result["error"] = error;
	}
	else if (mode == "fresh")
	{
		qputenv("WPSRECORDER_VIDEO_KEY", "unused-fixture-key");
		passed = TtsServiceIds().isEmpty();
		for (const auto id : {"assistant", "translate", "video", "qwen", "minimax", "custom"})
		{
			const auto service = SpeechService(id);
			passed = passed && service.endpoint.isEmpty() && service.model.isEmpty() &&
				service.protocol.isEmpty() && service.key.isEmpty();
		}
		bool checkedDialog = false;
		QTimer::singleShot(40, &app, [&]
		{
			auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
			if (!dialog) return;
			auto protocol = dialog->findChild<QComboBox*>("speechProtocol");
			auto endpoint = dialog->findChild<QLineEdit*>("speechEndpoint");
			auto model = dialog->findChild<QLineEdit*>("speechModel");
			auto role = dialog->findChild<QComboBox*>("speechServiceRole");
			auto profile = dialog->findChild<QComboBox*>("ttsProfile");
			auto save = dialog->findChild<QPushButton*>("saveSpeechService");
			auto add = dialog->findChild<QPushButton*>("addSpeechProfile");
			if (protocol && endpoint && model && role && profile && save && add)
			{
				checkedDialog = endpoint->text().isEmpty() && model->text().isEmpty() && protocol->currentData().toString().isEmpty();
				protocol->setCurrentIndex(protocol->findData("anthropic"));
				checkedDialog = checkedDialog && endpoint->text().isEmpty();
				role->setCurrentIndex(role->findData("tts"));
				checkedDialog = checkedDialog && !profile->isEnabled() && !save->isEnabled();
				add->click();
				checkedDialog = checkedDialog && endpoint->text().isEmpty() && model->text().isEmpty() &&
					protocol->currentData().toString().isEmpty();
			}
			dialog->reject();
		});
		ConfigureSpeechServices(nullptr, "video");
		result["fresh_dialog"] = checkedDialog;
		passed = passed && checkedDialog && mock.requests == 0;
	}
	else if (mode == "profiles")
	{
		// The numeric ID models a saved profile from an earlier release.
		QString error;
		KRRSpeechService legacy{mock.endpoint("legacy-voice"), "legacy-model"};
		if (!SaveSpeechService("tts-47", legacy, false, error)) return 2;
		QString created;
		QTimer::singleShot(40, &app, [&]
		{
			auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
			if (!dialog) return;
			auto profile = dialog->findChild<QComboBox*>("ttsProfile");
			auto name = dialog->findChild<QLineEdit*>("speechProfileName");
			auto model = dialog->findChild<QLineEdit*>("speechModel");
			auto endpoint = dialog->findChild<QLineEdit*>("speechEndpoint");
			auto protocol = dialog->findChild<QComboBox*>("speechProtocol");
			auto key = dialog->findChild<QLineEdit*>("speechCredential");
			auto add = dialog->findChild<QPushButton*>("addSpeechProfile");
			auto save = dialog->findChild<QPushButton*>("saveSpeechService");
			auto remove = dialog->findChild<QPushButton*>("removeSpeechProfile");
			if (!profile || !name || !model || !endpoint || !protocol || !key || !add || !save || !remove)
			{
				dialog->reject();
				return;
			}
			const bool oldVisible = profile->currentData() == "tts-47" && model->text() == legacy.model;
			add->click();
			created = profile->currentData().toString();
			name->setText("Fixture voice");
			model->setText("fixture-model");
			endpoint->setText(mock.endpoint("first-voice"));
			protocol->setCurrentIndex(protocol->findData("openai"));
			key->setText("fixture-only-key");
			save->click();
			const auto saved = SpeechService(created);
			const bool added = created.size() == 36 && saved.label == "Fixture voice" && saved.key == "fixture-only-key";
			profile->setCurrentIndex(profile->findData("tts-47"));
			const bool switched = model->text() == legacy.model;
			profile->setCurrentIndex(profile->findData(created));
			const bool loaded = name->text() == saved.label && endpoint->text() == saved.endpoint && key->text().isEmpty();
			name->setText("Renamed voice");
			model->setText("updated-fixture-model");
			endpoint->setText(mock.endpoint("second-voice"));
			save->click();
			const auto edited = SpeechService(created);
			const bool updated = edited.label == "Renamed voice" && edited.model == "updated-fixture-model" &&
				edited.endpoint == mock.endpoint("second-voice") && edited.key.isEmpty();
			QTimer::singleShot(35, dialog, []
			{
				if (auto confirm = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
					if (auto yes = confirm->button(QMessageBox::Yes)) yes->click();
			});
			remove->click();
			const bool deleted = profile->findData(created) < 0 && TtsServiceIds() == QStringList{"tts-47"};
			result["legacy_profile_editable"] = oldVisible && switched;
			result["created_and_reselected"] = added && loaded;
			result["updated_without_sending_key_to_new_address"] = updated;
			result["deleted_selected_profile"] = deleted;
			passed = oldVisible && added && switched && loaded && updated && deleted;
			dialog->reject();
		});
		ConfigureSpeechServices(nullptr, "tts-47");
		if (created.size() == 36)
		{
			KRRSpeechService original{mock.endpoint("first-voice"), "fixture-model"};
			const bool oldKeyRemoved = SaveSpeechService(created, original, false, error) && SpeechService(created).key.isEmpty();
			result["old_address_key_removed"] = oldKeyRemoved;
			passed = passed && oldKeyRemoved;
			const bool cleaned = RemoveTtsService(created, error);
			passed = passed && cleaned;
		}
		passed = passed && mock.requests == 0;
	}
	else if (mode == "anthropic")
	{
		KRRSpeechService service{mock.endpoint("v1"), "fixture-claude", "fixture-claude-key", "anthropic"};
		service.credentialHeader = "x-api-key";
		service.credentialPrefix.clear();
		const auto assistant = CheckSpeechService("assistant", service, &cancel);
		const auto translation = CheckSpeechService("translate", service, &cancel);
		const auto visual = CheckSpeechService("video", service, &cancel);
		passed = assistant.isEmpty() && mock.hasToolResult && translation.isEmpty() && visual.isEmpty() &&
			mock.requests == 6 && mock.pictureReads == 5;
		result["assistant_error"] = assistant;
		result["translation_error"] = translation;
		result["visual_error"] = visual;
		result["decoded_images"] = mock.pictureReads;
		result["tool_result_roundtrip"] = mock.hasToolResult;
	}
	else if (mode == "assistant")
	{
		KRRSpeechService service{mock.endpoint("ollama"), "fixture-tool-model", {}, "ollama"};
		const auto success = CheckSpeechService("assistant", service, &cancel);
		service.endpoint = mock.endpoint("no-tools");
		const auto rejected = CheckSpeechService("assistant", service, &cancel);
		passed = success.isEmpty() && mock.hasToolResult && rejected.contains(QStringLiteral("工具调用")) && mock.requests == 3;
		result["success_error"] = success;
		result["missing_tools"] = rejected;
	}
	else if (mode == "translate")
	{
		KRRSpeechService service{mock.endpoint("translate"), "fixture-translator", "fixture-key", "json"};
		service.credentialHeader = "X-Service-Key";
		service.credentialPrefix.clear();
		service.responsePath = "result.lines";
		service.requestTemplate = R"({"model":"$model","instruction":"$system","input":"$text"})";
		const auto success = CheckSpeechService("translate", service, &cancel);
		service.endpoint = mock.endpoint("auth");
		const auto rejected = CheckSpeechService("translate", service, &cancel);
		passed = success.isEmpty() && rejected.contains(QStringLiteral("鉴权失败")) && mock.requests == 3;
		result["success_error"] = success;
		result["bad_auth"] = rejected;
	}
	else if (mode == "video")
	{
		KRRSpeechService service{mock.endpoint("vision"), "fixture-vision", {}, "json"};
		service.requestTemplate = R"({"name":"$model","inputs":"$media","question":"$text"})";
		service.responsePath = "highlight";
		const auto success = CheckSpeechService("video", service, &cancel);
		service.endpoint = mock.endpoint("reject-video");
		const auto rejected = CheckSpeechService("video", service, &cancel);
		passed = success.isEmpty() && mock.pictureReads == 2 && rejected.contains("415");
		result["success_error"] = success;
		result["unsupported_video_input"] = rejected;
		result["decoded_visual_samples"] = mock.pictureReads;
	}
	else if (mode == "tts")
	{
		if (!mock.initializeAudio()) return 2;
		auto service = VoiceService(mock);
		const auto success = CheckSpeechService("tts-4", service, &cancel);
		const bool defaultVoice = mock.voice == service.voice;
		QString saveError;
		const bool saved = SaveSpeechService("tts-4", service, false, saveError);
		service.endpoint = mock.endpoint("bad-audio");
		const auto rejected = CheckSpeechService("tts-4", service, &cancel);
		KRREditProject project;
		project.duration = 5000;
		project.kept = {{0, 5000}};
		KRRSubtitleDialog narration(project, QFileInfo(report).absolutePath(), nullptr);
		narration.findChild<QTabWidget*>("speechSourceTabs")->setCurrentIndex(1);
		auto engine = narration.findChild<QComboBox*>("sentenceEngine");
		auto voice = narration.findChild<QComboBox*>("sentenceVoice");
		auto input = narration.findChild<QPlainTextEdit*>("freeNarrationText");
		auto generate = narration.findChild<QPushButton*>("synthesizeNarration");
		auto play = narration.findChild<QPushButton*>("playDubLine");
		bool rendered = false;
		if (engine && voice && input && generate && play && engine->findData("tts-4") >= 0 && engine->findData("audio8") >= 0)
		{
			engine->setCurrentIndex(engine->findData("tts-4"));
			voice->setEditText("one-off-voice");
			input->setPlainText(QStringLiteral("配音接口测试。"));
			generate->click();
			rendered = WaitUntil([&] { return play->isEnabled() && mock.voice == "one-off-voice"; });
		}
		passed = success.isEmpty() && defaultVoice && rejected.contains(QStringLiteral("音频")) && saved && rendered;
		result["success_error"] = success;
		result["bad_audio"] = rejected;
		result["save_error"] = saveError;
		result["narration_custom_voice"] = rendered;
	}
	else if (mode == "settings")
	{
		auto service = VoiceService(mock);
		if (!mock.initializeAudio()) return 2;
		QTimer::singleShot(40, &app, [&]
		{
			auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
			if (!dialog) return;
			auto role = dialog->findChild<QComboBox*>("speechServiceRole");
			auto protocol = dialog->findChild<QComboBox*>("speechProtocol");
			auto endpoint = dialog->findChild<QLineEdit*>("speechEndpoint");
			auto model = dialog->findChild<QLineEdit*>("speechModel");
			auto request = dialog->findChild<QPlainTextEdit*>("speechRequestTemplate");
			auto response = dialog->findChild<QLineEdit*>("speechResponsePath");
			auto status = dialog->findChild<QLabel*>("speechCheckStatus");
			auto check = dialog->findChild<QPushButton*>("checkSpeechService");
			auto add = dialog->findChild<QPushButton*>("addSpeechProfile");
			if (!role || role->count() != 4 || role->findData("audio8") >= 0 || !protocol || !endpoint ||
				!model || !request || !response || !status || !check || !add) { dialog->reject(); return; }
			add->click();
			bool foundRawJson = false;
			for (auto combo : dialog->findChildren<QComboBox*>())
				if (combo->findData("base64") >= 0)
				{
					combo->setCurrentIndex(combo->findData("base64"));
					foundRawJson = true;
				}
			protocol->setCurrentIndex(protocol->findData("json"));
			endpoint->setText(service.endpoint);
			model->setText(service.model);
			request->setPlainText(service.requestTemplate);
			response->setText(service.responsePath);
			check->click();
			const bool success = WaitUntil([&] { return status->property("checkResult").toString() != "running"; }) &&
				status->property("checkResult").toString() == "success";
			result["label"] = status->text();
			passed = success && foundRawJson && TtsServiceIds().isEmpty() && mock.requests == 1;
			dialog->reject();
		});
		ConfigureSpeechServices(nullptr, "tts");
	}
	else return 2;
	result["passed"] = passed;
	result["requests"] = mock.requests;
	QFile output(report);
	if (!output.open(QIODevice::WriteOnly)) return 2;
	output.write(QJsonDocument(result).toJson());
	return passed ? 0 : 1;
}
