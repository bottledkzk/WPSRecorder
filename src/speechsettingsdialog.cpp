#include "speechsettingsdialog.h"
#include "speechservices.h"
#include "uilanguage.h"
#include "uitheme.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUuid>
#include <thread>

void RefreshTtsServiceChoices(QComboBox* engines)
{
	const auto selected = engines->currentData();
	const QSignalBlocker block(engines);
	for (int i = engines->count() - 1; i >= 0; --i)
		if (IsTtsService(engines->itemData(i).toString())) engines->removeItem(i);
	for (const auto& id : TtsServiceIds())
	{
		const auto service = SpeechService(id);
		engines->addItem(Ui("服务 · %1").arg(service.label.isEmpty() ? service.model : service.label), id);
	}
	const int index = engines->findData(selected);
	if (index >= 0) engines->setCurrentIndex(index);
}

bool ConfigureAudio8Directory(QWidget* owner)
{
	const auto directory = QFileDialog::getExistingDirectory(owner, Ui("选择 Audio8 本地运行包"), Audio8Directory());
	if (directory.isEmpty()) return false;
	if (!Audio8Available(directory))
	{
		QMessageBox::warning(owner, Ui("Audio8 本地模型"),
			Ui("请选择包含 Python、runtime、model 和 voices 的完整运行包目录。"));
		return false;
	}
	SetAudio8Directory(directory);
	return true;
}

void ConfigureSpeechServices(QWidget* owner, const QString& initial)
{
	QDialog dialog(owner);
	dialog.setObjectName("speechServices");
	dialog.setWindowTitle(Ui("模型服务设置"));
	dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog.setMinimumWidth(RecorderUi::Px(635));
	auto form = new QFormLayout(&dialog);
	auto role = new QComboBox(&dialog);
	role->setObjectName("speechServiceRole");
	role->addItem(Ui("录屏助手"), "assistant");
	role->addItem(Ui("字幕翻译"), "translate");
	role->addItem(Ui("视频理解"), "video");
	role->addItem(Ui("语音合成"), "tts");
	form->addRow(Ui("服务"), role);
	auto profileRow = new QWidget(&dialog);
	auto profiles = new QHBoxLayout(profileRow);
	profiles->setContentsMargins(0, 0, 0, 0);
	auto profile = new QComboBox(profileRow);
	profile->setObjectName("ttsProfile");
	profile->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	profile->setMinimumContentsLength(16);
	profile->setToolTip(Ui("选择现有配置，在下方编辑后保存。"));
	auto addProfile = new QPushButton(Ui("新增配置"), profileRow);
	addProfile->setObjectName("addSpeechProfile");
	auto removeProfile = new QPushButton(Ui("删除配置"), profileRow);
	removeProfile->setObjectName("removeSpeechProfile");
	profiles->addWidget(profile, 1);
	profiles->addWidget(addProfile);
	profiles->addWidget(removeProfile);
	form->addRow(Ui("配音配置"), profileRow);
	auto profileName = new QLineEdit(&dialog);
	profileName->setObjectName("speechProfileName");
	profileName->setMaxLength(80);
	profileName->setPlaceholderText(Ui("选填，留空使用模型名称"));
	form->addRow(Ui("配置名称"), profileName);
	auto protocol = new QComboBox(&dialog);
	protocol->setObjectName("speechProtocol");
	form->addRow(Ui("接口协议"), protocol);
	auto endpoint = new QLineEdit(&dialog);
	endpoint->setObjectName("speechEndpoint");
	auto model = new QLineEdit(&dialog);
	model->setObjectName("speechModel");
	auto voice = new QLineEdit(&dialog);
	voice->setPlaceholderText(Ui("选填，可在配音时修改"));
	auto key = new QLineEdit(&dialog);
	key->setEchoMode(QLineEdit::Password);
	key->setObjectName("speechCredential");
	form->addRow(Ui("接口地址"), endpoint);
	form->addRow(Ui("模型"), model);
	form->addRow(Ui("默认音色 ID"), voice);
	form->addRow(Ui("API 密钥"), key);
	auto authToggle = new QPushButton(Ui("认证选项"), &dialog);
	authToggle->setCheckable(true);
	auto auth = new QWidget(&dialog);
	auto authForm = new QFormLayout(auth);
	authForm->setContentsMargins(0, 0, 0, 0);
	auto header = new QLineEdit(auth);
	header->setPlaceholderText(Ui("留空表示不发送密钥"));
	auto prefix = new QLineEdit(auth);
	prefix->setPlaceholderText(Ui("例如 Bearer 加一个空格；也可留空"));
	auto clear = new QPushButton(Ui("移除此地址的已存密钥"), auth);
	authForm->addRow(Ui("请求头名称"), header);
	authForm->addRow(Ui("密钥前缀"), prefix);
	authForm->addRow(clear);
	form->addRow(authToggle);
	form->addRow(auth);
	auth->hide();
	QObject::connect(authToggle, &QPushButton::toggled, auth, &QWidget::setVisible);

	auto custom = new QWidget(&dialog);
	auto customForm = new QFormLayout(custom);
	customForm->setContentsMargins(0, 0, 0, 0);
	auto request = new QPlainTextEdit(custom);
	request->setObjectName("speechRequestTemplate");
	request->setFixedHeight(RecorderUi::Px(82));
	auto response = new QLineEdit(custom);
	response->setObjectName("speechResponsePath");
	response->setPlaceholderText(Ui("例如 result.0.text；留空读取整个 JSON"));
	auto encoding = new QComboBox(custom);
	encoding->addItem(Ui("音频二进制"), "binary");
	encoding->addItem(Ui("JSON 中的 Base64"), "base64");
	encoding->addItem(Ui("JSON 中的十六进制"), "hex");
	auto variables = new QLabel(custom);
	variables->setWordWrap(true);
	variables->setTextFormat(Qt::PlainText);
	customForm->addRow(Ui("请求 JSON"), request);
	customForm->addRow(Ui("音频结果"), encoding);
	customForm->addRow(Ui("响应字段"), response);
	customForm->addRow(variables);
	form->addRow(custom);

	auto status = new QLabel(&dialog);
	status->setObjectName("speechCheckStatus");
	status->setWordWrap(true);
	status->setTextFormat(Qt::PlainText);
	form->addRow(status);
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	auto save = buttons->addButton(Ui("保存当前配置"), QDialogButtonBox::ActionRole);
	save->setObjectName("saveSpeechService");
	save->setProperty("role", "primary");
	auto check = buttons->addButton(Ui("检验能力"), QDialogButtonBox::ActionRole);
	check->setObjectName("checkSpeechService");
	form->addRow(buttons);

	QHash<QString, KRRSpeechService> drafts;
	QString loaded;
	bool busy = false;
	auto formValue = [&]
	{
		KRRSpeechService service;
		service.endpoint = endpoint->text().trimmed();
		service.model = model->text().trimmed();
		service.key = key->text().trimmed().toUtf8();
		service.protocol = protocol->currentData().toString();
		service.voice = voice->text().trimmed();
		if (IsTtsService(loaded)) service.label = profileName->text().trimmed();
		service.requestTemplate = request->toPlainText().trimmed();
		service.responsePath = response->text().trimmed();
		service.audioEncoding = encoding->currentData().toString();
		service.credentialHeader = header->text().trimmed();
		service.credentialPrefix = prefix->text();
		if (service.credentialHeader.isEmpty()) service.key.clear();
		return service;
	};
	const auto tts = [&] { return role->currentData() == "tts"; };
	auto refreshEnabled = [&]
	{
		const bool editable = !busy && !loaded.isEmpty();
		role->setEnabled(!busy);
		profileRow->setEnabled(!busy);
		for (auto field : QList<QWidget*>{profileName, protocol, model, voice, endpoint, key, authToggle, auth, custom, save})
			field->setEnabled(editable);
		removeProfile->setEnabled(editable && tts());
		check->setEnabled(busy || editable);
	};
	auto toggleRow = [](QFormLayout* layout, QWidget* field, bool visible)
	{
		field->setVisible(visible);
		if (auto label = layout->labelForField(field)) label->setVisible(visible);
	};
	auto refreshProtocol = [&]
	{
		const auto id = protocol->currentData().toString();
		const bool customProtocol = id == "json";
		custom->setVisible(customProtocol);
		toggleRow(customForm, encoding, tts());
		toggleRow(customForm, response, !tts() || encoding->currentData() != "binary");
		variables->setText(tts()
			? Ui("变量写成完整的 JSON 字符串值：$model、$text、$voice、$language、$speed、$format、$sample_rate。")
			: Ui("变量写成完整的 JSON 字符串值：$model、$messages、$tools、$tool_choice、$text、$system、$media、$video_data、$request。"));
		if (id == "anthropic") endpoint->setPlaceholderText("https://api.anthropic.com/v1/messages");
		else if (id == "ollama") endpoint->setPlaceholderText("http://127.0.0.1:11434/api/chat");
		else endpoint->setPlaceholderText(Ui("填写完整的请求 URL"));
		if (customProtocol && request->toPlainText().trimmed().isEmpty())
		{
			request->setPlainText(tts()
				? R"({"model":"$model","text":"$text","voice":"$voice","format":"$format","speed":"$speed"})"
				: R"({"model":"$model","messages":"$messages","tools":"$tools","stream":false})");
		}
	};
	auto refillProfiles = [&](const QString& selection)
	{
		const QSignalBlocker block(profile);
		const auto ids = TtsServiceIds();
		profile->clear();
		QStringList all = ids;
		for (const auto& id : drafts.keys())
			if (IsTtsService(id) && !all.contains(id)) all << id;
		for (const auto& id : all)
		{
			const auto service = drafts.contains(id) ? drafts.value(id) : SpeechService(id);
			const auto name = service.label.isEmpty() ? service.model : service.label;
			profile->addItem(name.isEmpty() ? Ui("新配置（未保存）") : name, id);
		}
		profile->setEnabled(!all.isEmpty());
		if (all.isEmpty()) profile->addItem(Ui("暂无配音配置"));
		profile->setCurrentIndex(qMax(0, profile->findData(selection)));
	};
	auto load = [&]
	{
		loaded = (tts() ? profile->currentData() : role->currentData()).toString();
		const auto configured = SpeechService(loaded);
		auto service = drafts.value(loaded, configured);
		if (!drafts.contains(loaded)) service.key.clear();
		toggleRow(form, profileRow, tts());
		toggleRow(form, profileName, tts());
		toggleRow(form, voice, tts());
		{
			const QSignalBlocker block(protocol);
			protocol->clear();
			protocol->addItem(Ui("选择接口协议"), QString{});
			protocol->addItem(Ui("OpenAI 兼容"), "openai");
			if (tts())
			{
				protocol->addItem(Ui("阿里云 DashScope TTS"), "dashscope");
				protocol->addItem(Ui("MiniMax TTS 原生"), "minimax");
			}
			else
			{
				protocol->addItem(QStringLiteral("Anthropic Messages"), "anthropic");
				if (loaded != "video") protocol->addItem(Ui("Ollama 原生对话"), "ollama");
			}
			protocol->addItem(Ui("自定义 JSON HTTP"), "json");
			protocol->setCurrentIndex(qMax(0, protocol->findData(service.protocol)));
		}
		endpoint->setText(service.endpoint);
		model->setText(service.model);
		voice->setText(service.voice);
		profileName->setText(service.label);
		key->setText(QString::fromUtf8(service.key));
		key->setPlaceholderText(configured.key.isEmpty() ? Ui("如服务需要，请填写密钥") : Ui("已配置 · 留空保留"));
		header->setText(service.credentialHeader);
		prefix->setText(service.credentialPrefix);
		request->setPlainText(service.requestTemplate);
		response->setText(service.responsePath);
		encoding->setCurrentIndex(qMax(0, encoding->findData(service.audioEncoding)));
		refreshProtocol();
		refreshEnabled();
		status->setText(loaded.isEmpty() ? Ui("新增一项配音配置后即可填写接口。") :
			Ui("检验使用当前填写的配置，只发送测试数据。保存后才应用到录屏工具。"));
		status->setProperty("checkResult", "");
	};
	auto remember = [&] { if (!loaded.isEmpty()) drafts[loaded] = formValue(); };
	auto persist = [&](bool remove)
	{
		QString error;
		if (!SaveSpeechService(loaded, formValue(), remove, error)) { status->setText(error); return; }
		drafts.remove(loaded);
		if (tts()) refillProfiles(loaded);
		load();
		status->setText(remove ? Ui("已移除此地址保存的密钥。") : Ui("当前配置已保存。"));
	};
	QObject::connect(profile, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { remember(); load(); });
	QObject::connect(role, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { remember(); load(); });
	QObject::connect(protocol, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&]
	{
		const bool anthropic = protocol->currentData() == "anthropic";
		if (anthropic && header->text() == "Authorization" && prefix->text() == "Bearer ")
		{
			header->setText("x-api-key");
			prefix->clear();
		}
		else if (!anthropic && header->text() == "x-api-key" && prefix->text().isEmpty())
		{
			header->setText("Authorization");
			prefix->setText("Bearer ");
		}
		if (anthropic)
			status->setText(loaded == "video" ? Ui("Anthropic Messages 使用带时间点的抽帧图片进行视频分析。")
				: Ui("填写 Anthropic 接口地址、模型 ID 和 API 密钥，再检验能力。"));
		refreshProtocol();
	});
	QObject::connect(encoding, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, refreshProtocol);
	QObject::connect(save, &QPushButton::clicked, &dialog, [&] { persist(false); });
	QObject::connect(clear, &QPushButton::clicked, &dialog, [&] { persist(true); });
	QObject::connect(addProfile, &QPushButton::clicked, &dialog, [&]
	{
		remember();
		const auto id = "tts-" + QUuid::createUuid().toString(QUuid::Id128);
		KRRSpeechService service;
		service.protocol.clear();
		drafts.insert(id, service);
		refillProfiles(id);
		load();
		profileName->setFocus();
	});
	QObject::connect(removeProfile, &QPushButton::clicked, &dialog, [&]
	{
		if (TtsServiceIds().contains(loaded))
		{
			if (QMessageBox::question(&dialog, Ui("删除配音配置"),
				Ui("删除「%1」及此配置保存的密钥？").arg(profile->currentText()),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
			QString error;
			if (!RemoveTtsService(loaded, error)) { status->setText(error); return; }
		}
		drafts.remove(loaded);
		refillProfiles({});
		load();
		status->setText(Ui("配音配置已删除。"));
	});

	std::thread worker;
	std::atomic<bool> cancel{false};
	auto setBusy = [&](bool active)
	{
		busy = active;
		refreshEnabled();
		check->setText(active ? Ui("取消检验") : Ui("检验能力"));
	};
	QObject::connect(check, &QPushButton::clicked, &dialog, [&]
	{
		if (busy)
		{
			cancel.store(true);
			check->setEnabled(false);
			status->setText(Ui("正在取消…"));
			return;
		}
		if (worker.joinable()) worker.join();
		const auto id = loaded;
		const auto service = formValue();
		cancel.store(false);
		setBusy(true);
		status->setProperty("checkResult", "running");
		status->setText(Ui("正在发送测试数据并检验服务能力…"));
		worker = std::thread([&, id, service]
		{
			const auto error = CheckSpeechService(id, service, &cancel);
			QMetaObject::invokeMethod(&dialog, [&, id, error]
			{
				setBusy(false);
				status->setProperty("checkResult", error.isEmpty() ? "success" : "error");
				status->setText(!error.isEmpty() ? error :
					id == "assistant" ? Ui("通过：模型能调用工具并读取工具结果。") :
					id == "translate" ? Ui("通过：模型返回了可用的中英双向译文。") :
					id == "video" ? Ui("通过：模型识别了视觉样本并返回带时间点的结果。") :
					Ui("通过：服务返回的配音可解码播放。"));
			}, Qt::QueuedConnection);
		});
	});
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	const auto startTts = initial == "tts" || IsTtsService(initial);
	{
		const QSignalBlocker block(role);
		role->setCurrentIndex(qMax(0, role->findData(startTts ? QString("tts") : initial)));
	}
	refillProfiles(initial);
	load();
	RecorderUi::PolishButtons(&dialog);
	dialog.exec();
	cancel.store(true);
	if (worker.joinable()) worker.join();
}
