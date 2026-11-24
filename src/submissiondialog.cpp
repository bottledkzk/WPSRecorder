#include "submissiondialog.h"
#include "mediaeditor.h"
#include "recordingconfig.h"
#include "uilanguage.h"
#include "uitheme.h"
#include "playbackslider.h"
#include "playerwindow.h"
#include "previewvideo.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <climits>

bool ChooseSubmission(QWidget* parent, const KRREditProject& project, const QString& directory,
					  qint64 previewAt, KRRSubmission& submission)
{
	QString error;
	if (!project.validate(error)) return false;
	QTemporaryDir temporary(DataDirectory() + "/submission-preview-XXXXXX");
	KRRMediaEditor renderer;
	QDialog dialog(parent);
	dialog.setObjectName("submissionPreviewDialog");
	dialog.setWindowTitle(Ui("提交档与成片预览"));
	dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog.resize(RecorderUi::Px(760), RecorderUi::Px(660));
	auto layout = new QVBoxLayout(&dialog);
	auto form = new QFormLayout;
	auto quality = new QComboBox(&dialog);
	quality->setObjectName("submissionQuality");
	quality->addItem(Ui("清晰 · 最高 1080p"), "clear");
	quality->addItem(Ui("小体积 · 最高 720p"), "small");
	form->addRow(Ui("画质"), quality);
	auto audioMode = new QComboBox(&dialog);
	audioMode->setObjectName("submissionAudioMode");
	audioMode->addItem(Ui("混合为一轨"), "mix");
	audioMode->addItem(Ui("保留所选分轨"), "separate");
	audioMode->addItem(Ui("静音"), "mute");
	form->addRow(Ui("声音输出"), audioMode);

	QVector<QCheckBox*> tracks;
	auto trackGrid = new QGridLayout;
	for (int i = 0; i < project.audio.size(); ++i)
	{
		const auto& track = project.audio[i];
		auto check = new QCheckBox(track.needsRefresh ? Ui("配音轨待更新") : track.title, &dialog);
		check->setChecked(track.audible());
		tracks.append(check);
		trackGrid->addWidget(check, i / 3, i % 3);
	}
	form->addRow(trackGrid);
	auto captions = new QCheckBox(Ui("将当前字幕写入画面"), &dialog);
	const bool hasCaptions = !project.outputCaptions().isEmpty();
	captions->setChecked(hasCaptions);
	captions->setEnabled(hasCaptions);
	form->addRow(captions);
	layout->addLayout(form);
	auto summary = new QLabel(&dialog);
	summary->setObjectName("submissionSummary");
	summary->setWordWrap(true);
	layout->addWidget(summary);
	auto status = new QLabel(&dialog);
	status->setObjectName("submissionPreviewStatus");
	status->setTextFormat(Qt::PlainText);
	status->setWordWrap(true);
	layout->addWidget(status);
	auto stage = new QStackedWidget(&dialog);
	auto playback = new KRRPreviewVideo(stage);
	playback->setMinimumHeight(RecorderUi::Px(215));
	stage->addWidget(playback);
	layout->addWidget(stage, 1);
	QMediaPlayer player(&dialog);
	player.setObjectName("submissionPlayer");
	playback->attach(&player);
	KRRPlayerWindow largePreview(&player, playback, &dialog);
	auto seek = new KRRPlaybackSlider(&dialog);
	seek->setRange(0, 0);
	layout->addWidget(seek);
	BindPlaybackSeek(seek, &player);
	QObject::connect(&player, &QMediaPlayer::durationChanged, seek, [seek](qint64 time) { seek->setMaximum(static_cast<int>(qMin<qint64>(time, INT_MAX))); });
	QObject::connect(&player, &QMediaPlayer::positionChanged, seek, [seek](qint64 time)
	{
		if (!seek->isSliderDown()) seek->setValue(static_cast<int>(time));
	});
	auto play = new QPushButton(Ui("播放 / 暂停"), &dialog);
	play->setEnabled(false);
	auto fullScreen = new QPushButton(Ui("放大预览"), &dialog);
	fullScreen->setObjectName("submissionFullScreen");
	fullScreen->setEnabled(false);
	QObject::connect(fullScreen, &QPushButton::clicked, &largePreview, &KRRPlayerWindow::enter);
	auto rows = new QHBoxLayout;
	rows->addWidget(play);
	rows->addWidget(fullScreen);
	rows->addStretch(1);
	layout->addLayout(rows);
	QObject::connect(play, &QPushButton::clicked, &dialog, [&player]
	{
		if (player.state() == QMediaPlayer::PlayingState) player.pause();
		else
		{
			if (player.position() >= player.duration() - 50) player.setPosition(0);
			player.play();
		}
	});
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	auto makePreview = buttons->addButton(Ui("生成 8 秒预览"), QDialogButtonBox::ActionRole);
	makePreview->setObjectName("renderSubmissionPreview");
	auto save = buttons->addButton(Ui("选择位置并导出"), QDialogButtonBox::ActionRole);
	save->setObjectName("exportSubmission");
	save->setProperty("role", "primary");
	layout->addWidget(buttons);
	auto current = [&]
	{
		KRRSubmission plan;
		plan.project = project;
		plan.options = SubmissionPreset(quality->currentData().toString());
		plan.separateAudio = audioMode->currentData() == "separate";
		const bool muted = audioMode->currentData() == "mute";
		for (int i = 0; i < tracks.size(); ++i)
			plan.project.audio[i].enabled = !muted && tracks[i]->isChecked();
		plan.burnCaptions = captions->isChecked();
		if (!plan.burnCaptions)
		{
			plan.project.captions.clear();
			for (auto& still : plan.project.stills) still.caption.clear();
		}
		return plan;
	};
	auto clearPreview = [&]
	{
		player.stop();
		player.setMedia({});
		playback->clear();
		play->setEnabled(false);
		fullScreen->setEnabled(false);
		const auto plan = current();
		const auto size = plan.options.outputSize(project.crop.size());
		int audioTracks = 0;
		for (const auto& track : plan.project.audio) audioTracks += track.audible();
		if (!plan.separateAudio && audioTracks > 1) audioTracks = 1;
		for (int i = 0; i < tracks.size(); ++i)
			tracks[i]->setEnabled(audioMode->currentData() != "mute" && !project.audio[i].needsRefresh);
		const auto seconds = static_cast<double>(project.outputDuration()) / 1000.0;
		const double estimate = seconds * (plan.options.videoKbps + audioTracks * plan.options.audioKbps) / 8000.0;
		summary->setText(Ui("%1 × %2 · %3 · 约 %4 MB · %5 条声音轨道")
			.arg(size.width()).arg(size.height()).arg(EditTime(project.outputDuration()))
			.arg(estimate, 0, 'f', 1).arg(audioTracks));
		status->setText(Ui("可先生成所选画质与声音的短片，核对小字、遮挡和旁白。预计体积会随内容变化。"));
	};
	QObject::connect(quality, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, clearPreview);
	QObject::connect(audioMode, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, clearPreview);
	QObject::connect(captions, &QCheckBox::toggled, &dialog, clearPreview);
	for (const auto* track : tracks) QObject::connect(track, &QCheckBox::toggled, &dialog, clearPreview);

	auto busy = [&](bool processing)
	{
		for (auto* widget : QList<QWidget*>{quality, audioMode, captions, makePreview, save})
			widget->setEnabled(!processing);
		captions->setEnabled(!processing && hasCaptions);
		for (auto* track : tracks) track->setEnabled(!processing && audioMode->currentData() != "mute");
	};
	int generation = 0;
	qint64 sampledFrom = 0;
	qint64 sampledTo = 0;
	QObject::connect(makePreview, &QPushButton::clicked, &dialog, [&]
	{
		if (!temporary.isValid())
		{
			status->setText(Ui("无法创建提交预览目录。请检查应用数据目录的写权限。"));
			return;
		}
		player.stop();
		player.setMedia({});
		play->setEnabled(false);
		fullScreen->setEnabled(false);
		auto plan = current();
		const auto length = project.outputDuration();
		plan.options.previewStart = qBound<qint64>(0, previewAt, qMax<qint64>(0, length - 8000));
		plan.options.previewLength = 8000;
		sampledFrom = plan.options.previewStart;
		sampledTo = qMin(length, sampledFrom + plan.options.previewLength);
		const auto output = temporary.filePath(QString("preview-%1.mp4").arg(++generation));
		if (!renderer.renderTimeline(plan.project, output, "auto", plan.separateAudio, plan.burnCaptions, plan.options))
			return;
		busy(true);
		status->setText(Ui("正在生成提交档预览…"));
	});
	QObject::connect(&renderer, &KRRMediaEditor::completed, &dialog, [&](const QString& output, const QString& failure)
	{
		busy(false);
		if (!failure.isEmpty()) { status->setText(UiDiagnostic(failure)); return; }
		status->setText(Ui("成片 %1 至 %2 · 应用当前提交档设置")
			.arg(EditTime(sampledFrom), EditTime(sampledTo)));
		play->setEnabled(true);
		fullScreen->setEnabled(true);
		player.setMedia(QUrl::fromLocalFile(output));
		player.play();
	});
	QObject::connect(&player, qOverload<QMediaPlayer::Error>(&QMediaPlayer::error), &dialog, [&](QMediaPlayer::Error)
	{
		status->setText(Ui("播放器：") + player.errorString());
	});
	QObject::connect(save, &QPushButton::clicked, &dialog, [&]
	{
		const auto suggested = QDir(directory).filePath("submission-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + ".mp4");
		QFileDialog file(&dialog, Ui("保存提交成片"), suggested, "MP4 (*.mp4)");
		file.setAcceptMode(QFileDialog::AcceptSave);
		file.setFileMode(QFileDialog::AnyFile);
		file.setDefaultSuffix("mp4");
		file.setOption(QFileDialog::DontConfirmOverwrite);
		if (file.exec() != QDialog::Accepted) return;
		const auto output = file.selectedFiles().value(0);
		if (QFileInfo::exists(output))
		{
			status->setText(Ui("目标文件已存在，请选择新文件名。"));
			return;
		}
		if (QFileInfo(output).suffix().toLower() != "mp4")
		{
			status->setText(Ui("提交成片请保存为 MP4。"));
			return;
		}
		submission = current();
		submission.output = output;
		dialog.accept();
	});
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	clearPreview();
	RecorderUi::PolishButtons(&dialog);
	const bool accepted = dialog.exec() == QDialog::Accepted;
	largePreview.reject();
	player.stop();
	player.setMedia({});
	renderer.cancel();
	return accepted && !submission.output.isEmpty();
}
