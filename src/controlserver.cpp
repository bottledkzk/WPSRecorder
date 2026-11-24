#include "controlserver.h"
#include "recordingassistanttools.h"

#include <windows.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QThread>
#include <algorithm>
#include <iterator>
#include <QTimer>
#include <QUuid>
#include <QVersionNumber>

static const auto BusyService = "The previous recorder service has a recording or schedule in progress";

static void Reply(QLocalSocket* socket, const QJsonObject& response)
{
	socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
	socket->flush();
	socket->disconnectFromServer();
}
KRRControlServer::KRRControlServer(KRRRecordingEngine& engine, QObject* parent)
	: QObject(parent), m_engine(engine)
{
	connect(&m_scheduleTimer, &QTimer::timeout, this, &KRRControlServer::startDueSchedules);
	connect(&m_server, &QLocalServer::newConnection, this, [this]
	{
		while (auto socket = m_server.nextPendingConnection())
		{
			connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
			connect(socket, &QLocalSocket::readyRead, this, [this, socket]
			{
				if (socket->bytesAvailable() > 1024 * 1024) { socket->abort(); return; }
				if (!socket->canReadLine()) return;
				QJsonParseError parseError;
				auto document = QJsonDocument::fromJson(socket->readLine(), &parseError);
				if (parseError.error != QJsonParseError::NoError || !document.isObject())
					Reply(socket, {{"ok", false}, {"error", "Invalid JSON request"}});
				else
					dispatch(socket, document.object());
			});
		}
	});
	connect(&m_engine, &KRRRecordingEngine::completed, this, [this](const QString& output, const QString& error)
	{
		QJsonObject response = m_engine.status();
		response["ok"] = error.isEmpty();
		for (auto socket : m_stopClients)
			if (socket) Reply(socket, response);
		m_stopClients.clear();
		if (response["output"] == output && response["state"] == "idle")
		{
			response["requested_encoder"] = m_requestedEncoder;
			RememberRecordingOutcome(response);
		}
		for (auto& task : m_schedules)
		{
			if (task.state != "recording" || task.config.output != output) continue;
			task.state = error.isEmpty() ? "completed" : "failed";
			task.error = error;
			saveSchedules(m_scheduleError);
			break;
		}
	});
}

static QString ScheduleFile()
{
	return DataDirectory() + "/" + ControlServerName() + "-schedule.json";
}

bool KRRControlServer::hasScheduledTask() const
{
	return std::any_of(m_schedules.cbegin(), m_schedules.cend(),
		[](const auto& task) { return task.state == "waiting"; });
}

void KRRControlServer::loadSchedules()
{
	QFile file(ScheduleFile());
	if (!file.open(QIODevice::ReadOnly)) return;
	const auto now = QDateTime::currentDateTimeUtc();
	const auto items = QJsonDocument::fromJson(file.read(64 * 1024 * 1024)).array();
	for (const auto& value : items)
	{
		const auto row = value.toObject();
		const auto when = QDateTime::fromString(row["startAt"].toString(), Qt::ISODate);
		const auto id = row["id"].toString();
		if (id.isEmpty() || !when.isValid() || m_schedules.size() >= 100) continue;
		QString state = row["state"].toString();
		if (state == "waiting" && when <= now) state = "expired";
		if (state == "recording") state = "failed";
		m_schedules.append({id, row["name"].toString(), state, row["error"].toString(), when,
			KRRRecordingConfig::FromJson(row["config"].toObject())});
	}
	std::stable_sort(m_schedules.begin(), m_schedules.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
	refreshScheduleTimer();
}

bool KRRControlServer::saveSchedules(QString& error) const
{
	QJsonArray values;
	for (const auto& task : m_schedules)
		values.append(QJsonObject{{"id", task.id}, {"name", task.name}, {"state", task.state},
			{"error", task.error}, {"startAt", task.at.toString(Qt::ISODateWithMs)}, {"config", task.config.toJson()}});
	QSaveFile file(ScheduleFile());
	const auto bytes = QJsonDocument(values).toJson(QJsonDocument::Compact);
	if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
	{
		error.clear();
		return true;
	}
	error = file.errorString();
	return false;
}

void KRRControlServer::refreshScheduleTimer()
{
	if (!hasScheduledTask()) m_scheduleTimer.stop();
	else if (!m_scheduleTimer.isActive()) m_scheduleTimer.start(200);
}

QJsonArray KRRControlServer::listSchedules() const
{
	QJsonArray rows;
	QDateTime occupiedUntil;
	bool indefinite = false;
	QString occupyingName;
	for (const auto& task : m_schedules)
	{
		QString state = task.state, conflict;
		if (state == "waiting" || state == "recording")
		{
			if (state == "waiting" && (indefinite || (occupiedUntil.isValid() && task.at < occupiedUntil)))
			{
				state = "conflict";
				conflict = occupyingName;
			}
			else
			{
				occupiedUntil = task.at.addMSecs(qRound64(task.config.maxDuration * 1000.));
				indefinite = task.config.maxDuration <= 0;
				occupyingName = task.name;
			}
		}
		rows.append(QJsonObject{{"id", task.id}, {"name", task.name}, {"state", state},
			{"startAt", task.at.toString(Qt::ISODateWithMs)}, {"duration", task.config.maxDuration},
			{"output", task.config.output}, {"conflict", conflict}, {"error", task.error}});
	}
	return rows;
}

void KRRControlServer::startDueSchedules()
{
	const auto now = QDateTime::currentDateTimeUtc();
	bool changed = false;
	for (auto& task : m_schedules)
	{
		if (task.at > now) break;
		if (task.state != "waiting") continue;
		changed = true;
		if (task.at.msecsTo(now) > 5000) task.state = "expired";
		else if (m_engine.isActive()) task.state = "missed";
		else if (m_engine.start(task.config, task.error))
		{
			task.state = "recording";
			m_requestedEncoder = task.config.encoder;
		}
		else task.state = "failed";
	}
	if (changed) saveSchedules(m_scheduleError);
	refreshScheduleTimer();
}

bool KRRControlServer::listen(QString& error)
{
	m_lock = std::make_unique<QLockFile>(DataDirectory() + "/" + ControlServerName() + ".lock");
	if (!m_lock->tryLock(0)) { error = "Another recorder owns the control service"; return false; }
	QLocalServer::removeServer(ControlServerName());
	m_server.setSocketOptions(QLocalServer::UserAccessOption);
	if (!m_server.listen(ControlServerName())) { error = m_server.errorString(); return false; }
	loadSchedules();
	return true;
}
void KRRControlServer::dispatch(QLocalSocket* socket, const QJsonObject& request)
{
	if (!m_server.isListening())
	{
		Reply(socket, {{"ok", false}, {"error", "Control request timed out or disconnected"}});
		return;
	}
	QString command = request["command"].toString();
	if (command == "preview")
	{
		Reply(socket, m_engine.preview());
	}
	else if (command == "meter") Reply(socket, m_engine.audioLevels());
	else if (command == "publish-preview")
	{
		m_engine.setPreviewPublication(request["enabled"].toBool());
		Reply(socket, {{"ok", true}});
	}
	else if (command == "scene")
	{
		QString error; bool ok = m_engine.updateScene(request["scene"].toObject(), error); Reply(socket, {{"ok", ok}, {"error", error}});
	}
	else if (command == "mark")
	{
		QString error;
		QJsonObject marker;
		const bool saved = m_engine.addMarker(request["label"].toString(), marker, error);
		Reply(socket, {{"ok", saved}, {"error", error}, {"marker", marker}});
	}
	else if (command == "cancel-schedule")
	{
		const auto id = request["id"].toString();
		const auto before = m_schedules;
		m_schedules.erase(std::remove_if(m_schedules.begin(), m_schedules.end(), [&](const auto& task)
		{
			return id.isEmpty() ? task.state == "waiting" : task.id == id && task.state != "recording";
		}), m_schedules.end());
		QString error;
		const bool saved = m_schedules.size() == before.size() || saveSchedules(error);
		if (!saved) m_schedules = before;
		refreshScheduleTimer();
		Reply(socket, {{"ok", saved}, {"error", error}});
	}
	else if (command == "schedules") Reply(socket, {{"ok", true}, {"schedules", listSchedules()}});
	else if (command == "schedule")
	{
		QString error;
		auto when = QDateTime::fromString(request["startAt"].toString(), Qt::ISODate).toUTC();
		auto config = KRRRecordingConfig::FromJson(request["config"].toObject());
		bool valid = config.validate(error);
		if (!when.isValid() || when <= QDateTime::currentDateTimeUtc()) { valid = false; error = "Schedule must have a future ISO8601 start time"; }
		if (m_schedules.size() >= 100) { valid = false; error = "The schedule list is full"; }
		if (std::any_of(m_schedules.cbegin(), m_schedules.cend(), [&config](const auto& task)
		{
			return task.state == "waiting" && task.config.output.compare(config.output, Qt::CaseInsensitive) == 0;
		})) { valid = false; error = "A scheduled recording already uses this output"; }
		QString id;
		if (valid)
		{
			id = QUuid::createUuid().toString(QUuid::WithoutBraces);
			auto name = request["name"].toString().simplified().left(120);
			if (name.isEmpty()) name = when.toLocalTime().toString("MM-dd HH:mm");
			const auto position = std::upper_bound(m_schedules.begin(), m_schedules.end(), when,
				[](const auto& at, const auto& task) { return at < task.at; });
			const auto inserted = m_schedules.insert(position, {id, name, "waiting", {}, when, config});
			valid = saveSchedules(error);
			if (!valid) m_schedules.erase(inserted);
			refreshScheduleTimer();
		}
		Reply(socket, {{"ok", valid}, {"error", error}, {"id", id}});
	}
	else if (command == "service-exit")
	{
		const auto expectedPid = request["expected_pid"].toVariant().toUInt();
		if (expectedPid && expectedPid != GetCurrentProcessId())
		{
			Reply(socket, {{"ok", false}, {"error", "Control request timed out or disconnected"}});
			return;
		}
		if (m_engine.isActive() || hasScheduledTask())
		{
			Reply(socket, {{"ok", false}, {"error", BusyService}});
			return;
		}
		Reply(socket, {{"ok", true}});
		m_server.close();
		QTimer::singleShot(50, QCoreApplication::instance(), &QCoreApplication::quit);
	}
	else if (command == "start")
	{
		QString error;
		const auto config = KRRRecordingConfig::FromJson(request["config"].toObject());
		bool started = m_engine.start(config, error);
		if (started) m_requestedEncoder = config.encoder;
		else if (!m_engine.isActive())
			RememberRecordingOutcome({{"error", error}, {"failure_stage", "validate"},
				{"requested_encoder", config.encoder}, {"audio_mode", config.audio}});
		Reply(socket, {{"ok", started}, {"error", error}, {"pid", static_cast<qint64>(GetCurrentProcessId())}});
	}
	else if (command == "stop" && m_engine.isActive())
	{
		m_stopClients.append(socket);
		m_engine.requestStop();
	}
	else if (command == "pause" || command == "resume")
	{
		m_engine.setPaused(command == "pause");
		Reply(socket, {{"ok", true}});
	}
	else if (command == "status" || command == "stop")
	{
		QJsonObject response = m_engine.status();
		const auto next = std::find_if(m_schedules.cbegin(), m_schedules.cend(),
			[](const auto& task) { return task.state == "waiting"; });
		response["scheduled_start"] = next != m_schedules.cend() ? next->at.toString(Qt::ISODate) : QString();
		response["scheduled_output"] = next != m_schedules.cend() ? next->config.output : QString();
		response["schedule_error"] = m_scheduleError;
		static const int integrity = ProcessIntegrityRid();
		static const bool appContainer = IsContainerProcess();
		response["service_version"] = QCoreApplication::applicationVersion();
		response["service_integrity_rid"] = integrity;
		response["service_app_container"] = appContainer;
		response["ok"] = true;
		Reply(socket, response);
	}
	else Reply(socket, {{"ok", false}, {"error", "Unknown command"}});
}
bool SendControl(const QJsonObject& request, QJsonObject& response, QString& error, int timeout)
{
	error.clear();
	response = {};
	QLocalSocket socket;
	socket.connectToServer(ControlServerName());
	if (!socket.waitForConnected(std::min(timeout, 1000))) { error = socket.errorString(); return false; }
	const auto payload = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
	if (socket.write(payload) != payload.size() || (socket.bytesToWrite() && !socket.waitForBytesWritten(1000)))
	{
		error = socket.errorString();
		return false;
	}
	QElapsedTimer timer;
	timer.start();
	while (!socket.canReadLine())
	{
		int remaining = timeout - static_cast<int>(timer.elapsed());
		if (remaining <= 0 || (!socket.waitForReadyRead(remaining) && !socket.canReadLine()))
		{
			error = "Control request timed out or disconnected";
			return false;
		}
	}
	QJsonParseError parseError;
	auto document = QJsonDocument::fromJson(socket.readLine(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) { error = "Invalid service response"; return false; }
	response = document.object();
	if (!response["ok"].toBool()) { error = response["error"].toString(); return false; }
	return true;
}

static bool IsLegacyRecorder(const QJsonObject& status)
{
	const auto pid = status["pid"].toVariant().toUInt();
	if (!pid) return false;
	std::unique_ptr<void, decltype(&CloseHandle)> process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), CloseHandle);
	WCHAR path[32768];
	DWORD size = static_cast<DWORD>(std::size(path));
	return process && QueryFullProcessImageNameW(process.get(), 0, path, &size) &&
		QFileInfo(QString::fromWCharArray(path, size)).fileName().compare("wpsrecorder_cli.exe", Qt::CaseInsensitive) == 0;
}

static bool MustReplaceService(const QJsonObject& status)
{
	const auto version = QVersionNumber::fromString(QCoreApplication::applicationVersion());
	if (version.isNull()) return false;
	const auto serviceVersion = QVersionNumber::fromString(status["service_version"].toString());
	if (serviceVersion.isNull() ? IsLegacyRecorder(status) : serviceVersion < version) return true;
	if (status["service_app_container"].toBool() && !IsContainerProcess()) return true;
	const int integrity = status["service_integrity_rid"].toInt(-1);
	return integrity >= 0 && integrity < ProcessIntegrityRid();
}

bool EnsureControlService(QString& error, bool requireScheduleQueue)
{
	const QJsonObject statusRequest{{"command", "status"}};
	const auto supportsQueue = [&]
	{
		QJsonObject queue;
		return !requireScheduleQueue || SendControl({{"command", "schedules"}}, queue, error, 500);
	};
	QJsonObject response;
	if (SendControl(statusRequest, response, error, 500))
	{
		if (!MustReplaceService(response) && supportsQueue()) return true;
		if (response["state"] != "idle" || !response["scheduled_start"].toString().isEmpty())
		{
			error = BusyService;
			return false;
		}
		const auto pid = response["pid"].toVariant().toUInt();
		std::unique_ptr<void, decltype(&CloseHandle)> previous(pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr, CloseHandle);
		QJsonObject stopped;
		if (!SendControl({{"command", "service-exit"}, {"expected_pid", qint64(pid)}}, stopped, error, 500))
		{
			const auto stopError = error;
			if (SendControl(statusRequest, response, error, 200))
			{
				if (!MustReplaceService(response) && supportsQueue()) return true;
				error = stopError;
				return false;
			}
			if (!previous) { error = stopError; return false; }
		}
		if (previous) WaitForSingleObject(previous.get(), 1200);
		for (int attempt = 0; attempt < 25; ++attempt)
		{
			QThread::msleep(40);
			if (!SendControl(statusRequest, response, error, 100)) break;
			if (!MustReplaceService(response) && supportsQueue()) return true;
		}
	}
	QProcess process;
	process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments)
	{
		arguments->flags |= CREATE_NO_WINDOW;
		// A detached daemon must not retain the CLI caller's redirected pipe handles.
		arguments->inheritHandles = false;
	});
	process.setProgram(QCoreApplication::applicationDirPath() + "/wpsrecorder_cli.exe");
	process.setArguments({"--serve"});
	process.setStandardInputFile(QProcess::nullDevice());
	process.setStandardOutputFile(QProcess::nullDevice());
	process.setStandardErrorFile(QProcess::nullDevice());
	if (!process.startDetached()) { error = "Could not start recorder service"; return false; }
	for (int attempt = 0; attempt < 50; ++attempt)
	{
		QThread::msleep(100);
		if (SendControl(statusRequest, response, error, 200) && !MustReplaceService(response) && supportsQueue()) return true;
	}
	if (error.isEmpty()) error = "Could not start recorder service";
	return false;
}
