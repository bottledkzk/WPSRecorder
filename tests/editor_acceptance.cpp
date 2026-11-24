#include "clipeditor.h"
#include "displaygeometry.h"
#include "editwidgets.h"
#include "uitheme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <cmath>

static void Mouse(QWidget *w, QEvent::Type type, QPoint p, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
	QMouseEvent e(type, p, w->mapToGlobal(p), Qt::LeftButton,
				  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
	QApplication::sendEvent(w, &e);
}
static QString ImageHash(const QImage &image)
{
	return QString::fromLatin1(
		QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()),
								 QCryptographicHash::Sha256)
			.toHex());
}
class KRREditAcceptance : public QObject
{
  public:
	KRREditAcceptance(const QString &source, const QString &report, const QString &output, bool asr, bool drag)
		: m_report(report), m_output(output), m_asr(asr), m_drag(drag)
	{
		m_editor.show();
		m_editor.openFile(source, QFileInfo(output).absolutePath());
		m_wall.start();
		m_tick.start(100);
		connect(&m_tick, &QTimer::timeout, this, [this] { tick(); });
		connect(&m_editor.m_asr, &KRRMediaEditor::completed, this,
				[this](const QString &, const QString &error)
				{
					if (!m_asr)
						return;
					if (!error.isEmpty())
					{
						fail(error);
						return;
					}
					QString words;
					for (auto c : m_editor.m_project.captions)
						words += c.text;
					if (m_editor.m_project.captions.isEmpty() || !words.contains("fellow", Qt::CaseInsensitive))
					{
						fail("Selected-track ASR did not produce expected speech");
						return;
					}
					record("selected_track_automatic_captions", {{"audio_track", 1},
																 {"captions", m_editor.m_project.captions.size()},
																 {"expected_word_found", true}});
					m_editor.m_exporter.renderTimeline(m_editor.m_project, m_output, "libx264");
					m_stage = 6;
				});
		connect(&m_editor.m_exporter, &KRRMediaEditor::completed, this,
				[this](const QString &, const QString &error)
				{
					if (!error.isEmpty())
					{
						fail(error);
						return;
					}
					record("timeline_export",
						   {{"output", m_output}, {"duration_ms", m_editor.m_project.outputDuration()}});
					finish(true);
				});
	}

  private:
	void record(const QString &name, QJsonObject details = {})
	{
		details["case"] = name;
		details["passed"] = true;
		m_results.append(details);
		write(false);
	}
	void write(bool done)
	{
		QFile f(m_report);
		f.open(QIODevice::WriteOnly);
		f.write(QJsonDocument(QJsonObject{{"complete", done},
										  {"passed", done && m_error.isEmpty()},
										  {"error", m_error},
										  {"stage", m_stage},
										  {"wall_ms", m_wall.elapsed()},
										  {"cases", m_results},
										  {"last_ui_status", m_editor.m_status->text()}})
					.toJson());
	}
	void fail(const QString &error)
	{
		m_error = error;
		finish(false);
	}
	void finish(bool success)
	{
		m_tick.stop();
		write(true);
		m_editor.m_player->pause();
		QCoreApplication::exit(success ? 0 : 1);
	}
	bool press(const QString &text)
	{
		for (auto b : m_editor.findChildren<QPushButton *>())
			if (b->objectName() == text || b->text() == text)
			{
				b->click();
				return true;
			}
		return false;
	}
	void checkTimelineDragging()
	{
		auto& editor = m_editor;
		auto& project = editor.m_project;
		auto timeline = editor.m_timeline;
		project.splits = {1000, 3600};
		project.audio[0].kept = {{0, 1000}, {1900, 2700}};
		project.subtitlePositions.append(QPointF(.5, .75));
		KRREditCaption a, b, secondTrack;
		a.begin = 1100; a.end = 2200; a.text = "A";
		b.begin = 2300; b.end = 3700; b.text = "B";
		secondTrack.begin = 1500; secondTrack.end = 2400; secondTrack.text = "Other"; secondTrack.track = 1;
		project.captions = {a, secondTrack, b};
		editor.refresh();
		editor.select(0, 0);
		timeline->setSnapEnabled(false);

		int seeks = 0;
		const auto seekConnection = connect(timeline, &KRREditTimeline::seekRequested, this, [&] { ++seeks; });
		bool immediate = true;
		qint64 longestMoveUs = 0;
		auto drag = [&](qint64 from, qint64 to, int y, bool cancel = false, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
		{
			const QPoint begin(timeline->xForTime(from), y), end(timeline->xForTime(to), y);
			const auto beforePicture = timeline->grab().toImage();
			Mouse(timeline, QEvent::MouseButtonPress, begin, modifiers);
			const auto beforeProject = project.toJson();
			const qint64 beforeBegin = editor.m_begin, beforeEnd = editor.m_end;
			seeks = 0;
			QElapsedTimer moveTime;
			moveTime.start();
			Mouse(timeline, QEvent::MouseMove, end, modifiers);
			longestMoveUs = qMax(longestMoveUs, moveTime.nsecsElapsed() / 1000);
			immediate &= beforePicture != timeline->grab().toImage() && project.toJson() == beforeProject &&
				editor.m_begin == beforeBegin && editor.m_end == beforeEnd && seeks == 0;
			if (cancel)
			{
				QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
				QApplication::sendEvent(timeline, &escape);
			}
			Mouse(timeline, QEvent::MouseButtonRelease, end, modifiers);
		};
		auto arranged = project;
		arranged.arrange();
		const auto original = arranged.toJson();
		const int undoCount = editor.m_undo.size();
		drag(2400, 240, timeline->videoY());
		const auto video = project.orderedVideo;
		const bool reordered = video.size() == 3 && video[0].begin == 1000 && video[1].begin == 0 &&
			video[2].begin == 3600 && project.outputDuration() == arranged.outputDuration() &&
			project.toJson()["audio"] == original["audio"] && editor.m_undo.size() == undoCount + 1;
		editor.select(0, 0);
		drag(2100, 4150, RecorderUi::Px(219));
		const auto audio = project.audio[0].placed;
		const bool audioMove = audio.size() == 2 && audio[0].begin == 0 && qAbs(audio[1].begin - 3950) < 18 &&
			audio[1].mediaStart == 1900 && audio[1].end - audio[1].begin == 800 &&
			project.toJson()["audio"].toArray().at(1) == original["audio"].toArray().at(1);
		editor.select(0, 0);
		drag(2850, 400, RecorderUi::Px(125));
		const bool subtitleMove = project.captions.size() == 3 && project.captions[2].begin == 1100 &&
			project.captions[2].end == 2500 && project.captions[0].begin == 2600 &&
			project.captions[1].begin == secondTrack.begin && project.captions[1].end == secondTrack.end;

		editor.chooseTrack(-1);
		editor.select(900, 1900);
		const auto beforeSelection = project.toJson();
		drag(1160, 2600, RecorderUi::Px(38), false, Qt::AltModifier);
		const qint64 selected = editor.m_begin;
		const bool movingSelection = qAbs(selected - 2340) < 18 && editor.m_end - selected == 1000 && seeks == 1;
		drag(selected, selected + 200, RecorderUi::Px(38), false, Qt::AltModifier);
		const bool resize = qAbs(editor.m_begin - selected - 200) < 18 && editor.m_end == selected + 1000 && seeks == 1;
		const qint64 resized = editor.m_begin;
		drag(resized + 250, 3300, RecorderUi::Px(38), true, Qt::AltModifier);
		const bool cancel = editor.m_begin == resized && editor.m_end == selected + 1000 && seeks == 0 &&
			project.toJson() == beforeSelection;
		QString error;
		const bool marked = project.addChapter(5100, "Snap target", error);
		editor.refresh();
		editor.select(0, 0);
		timeline->setSnapEnabled(true);
		drag(4350, 4691, RecorderUi::Px(219));
		const bool markerSnap = marked && project.audio[0].placed[1].end == 5100;
		drag(4700, 2791, RecorderUi::Px(219));
		const bool clipSnap = project.audio[0].placed[1].begin == secondTrack.end;

		const auto tracks = project.toJson()["audio"].toArray();
		editor.m_linkTracks = true;
		editor.m_lockedTracks.insert(1);
		editor.refresh();
		editor.select(0, 0);
		drag(1400, 5700, timeline->videoY());
		const auto remapped = project.toJson()["audio"].toArray();
		const bool linked = tracks[0] != remapped[0] && tracks[1] == remapped[1] && project.validate(error);
		disconnect(seekConnection);
		record("drag_interactions", {{"immediate_visual_feedback", immediate}, {"video_reorder", reordered},
			{"independent_audio", audioMove}, {"subtitle_reorder_without_overlap", subtitleMove},
			{"move_selection", movingSelection}, {"resize_selection", resize}, {"escape_cancel", cancel},
			{"snap_tail_to_marker", markerSnap}, {"snap_head_to_clip", clipSnap}, {"link_respects_track_lock", linked},
			{"longest_move_event_us", longestMoveUs}});
		if (!(immediate && reordered && audioMove && subtitleMove && movingSelection && resize && cancel && markerSnap && clipSnap && linked))
		{
			fail("Timeline dragging did not match the drop preview or affected another track");
			return;
		}
		// Export through the same renderer used by the editor after applying the real drags.
		m_stage = 901;
		if (!editor.m_exporter.renderTimeline(project, m_output, "libx264"))
			fail("Could not export the dragged timeline");
	}
	void tick()
	{
		if (m_wall.elapsed() > (m_asr ? 180000 : 60000))
		{
			fail("UI acceptance timeout");
			return;
		}
		auto &e = m_editor;
		if (m_stage == 0)
		{
			if (e.m_project.input.isEmpty() || e.m_canvas->frameCount() == 0 || e.m_timeline->waveformCount() != 2 ||
				e.m_timeline->thumbnailCount() != 12)
				return;
			record("source_analysis", {{"duration_ms", e.m_project.duration},
									   {"audio_tracks", e.m_project.audio.size()},
									   {"waveforms", e.m_timeline->waveformCount()},
									   {"thumbnails", e.m_timeline->thumbnailCount()}});
			if (m_drag)
			{
				checkTimelineDragging();
				return;
			}
			if (m_asr)
			{
				e.m_asrTrack->setCurrentIndex(1);
				e.m_language->setCurrentIndex(e.m_language->findData("en"));
				e.autoCaptions();
				m_stage = 5;
				return;
			}
			double originalFps = e.m_project.fps;
			for (double fps : {24., 25., 29.97, 30., 59.94, 60., 120.})
			{
				e.m_project.fps = fps;
				e.seek(0);
				for (int frame = 1; frame <= 120; ++frame)
				{
					if (!press("stepNextFrame")) { fail("Next frame control missing"); return; }
					if (qRound64(e.m_position * fps / 1000.) != frame)
					{
						fail("Forward frame got stuck at " + QString::number(e.m_position));
						return;
					}
				}
				for (int frame = 119; frame >= 0; --frame)
				{
					if (!press("stepPreviousFrame")) { fail("Previous frame control missing"); return; }
					if (qRound64(e.m_position * fps / 1000.) != frame)
					{
						fail("Backward frame skipped an index");
						return;
					}
				}
				e.seek(e.m_project.duration);
				auto end = e.m_position;
				press("stepNextFrame");
				if (end != e.m_position)
				{
					fail("Frame step escaped video end");
					return;
				}
			}
			e.m_project.fps = originalFps;
			e.seek(299);
			press("stepNextFrame");
			if (e.m_position != 333)
			{
				fail("299 ms regression persists");
				return;
			}
			record("frame_stepping", {{"fps", QJsonArray{24, 25, 29.97, 30, 59.94, 60, 120}},
									  {"button_steps", 1680},
									  {"299ms_next", e.m_position},
									  {"end_clamped", true}});
			bool disabled = !e.m_scroll->isEnabled();
			e.m_zoom->setValue(40);
			e.m_scroll->setValue(10000);
			int visibleBegin = e.m_timeline->xForTime(0);
			bool moved = visibleBegin < -100 && e.m_scroll->isEnabled();
			e.m_zoom->setValue(10);
			if (!disabled || !moved || e.m_scroll->isEnabled())
			{
				fail("Timeline pan enable/range failed");
				return;
			}
			record("timeline_zoom_and_pan",
				   {{"disabled_at_1x", true}, {"enabled_at_4x", true}, {"source_zero_x", visibleBegin}});
			e.seek(300); m_before = e.m_canvas->frameCount(); m_stage = 100;
		}
		else if (m_stage == 100) {
			if (e.m_canvas->frameCount() <= m_before || e.m_frameTime != 300 || e.m_frameProcess.state() != QProcess::NotRunning) return;
			e.m_canvas->currentFrame().save(m_output + "-frame9.png");
			m_before = e.m_canvas->frameCount(); press("stepNextFrame"); m_stage = 101;
		}
		else if (m_stage == 101) {
			if (e.m_canvas->frameCount() <= m_before || e.m_frameTime != 333 || e.m_frameProcess.state() != QProcess::NotRunning) return;
			e.m_canvas->currentFrame().save(m_output + "-frame10.png");
			record("decoded_frame_step", {{"before_ms",300},{"after_ms",333},{"frames_saved_for_independent_comparison",true}});
			m_before = e.m_canvas->frameCount();
			int x = e.m_timeline->xForTime(500);
			Mouse(e.m_timeline, QEvent::MouseButtonPress, QPoint(x, RecorderUi::Px(18)));
			Mouse(e.m_timeline, QEvent::MouseButtonRelease, QPoint(x, RecorderUi::Px(18)));
			m_stage = 1;
		}
		else if (m_stage == 1)
		{
			if (e.m_canvas->frameCount() <= m_before)
				return;
			if (std::abs(e.m_position - 500) > 10)
			{
				fail("Ruler did not seek to expected frame");
				return;
			}
			m_firstHash = ImageHash(e.m_canvas->currentFrame());
			m_before = e.m_canvas->frameCount();
			int x = e.m_timeline->xForTime(4500);
			Mouse(e.m_timeline, QEvent::MouseButtonPress, QPoint(x, RecorderUi::Px(18)));
			Mouse(e.m_timeline, QEvent::MouseButtonRelease, QPoint(x, RecorderUi::Px(18)));
			m_stage = 2;
		}
		else if (m_stage == 2)
		{
			if (e.m_canvas->frameCount() <= m_before)
				return;
			QString second = ImageHash(e.m_canvas->currentFrame());
			if (second == m_firstHash || std::abs(e.m_position - 4500) > 10)
			{
				fail("Seeking did not change decoded preview frame");
				return;
			}
			record("ruler_seek_and_picture", {{"source_times_ms", QJsonArray{500, 4500}},
											  {"first_frame_sha256", m_firstHash},
											  {"second_frame_sha256", second}});
			e.m_cropButton->click();
			auto r = e.m_canvas->videoRect();
			QPoint begin(r.left() + r.width() * .15, r.top() + r.height() * .1),
				end(r.left() + r.width() * .85, r.top() + r.height() * .9);
			Mouse(e.m_canvas, QEvent::MouseButtonPress, begin);
			Mouse(e.m_canvas, QEvent::MouseMove, end);
			Mouse(e.m_canvas, QEvent::MouseButtonRelease, end);
			auto crop = e.m_project.crop;
			if (crop.width() >= 640 || crop.height() >= 360 || crop.width() < 400 || crop.height() < 260)
			{
				fail("Canvas crop did not update project");
				return;
			}
			record("canvas_crop", {{"crop", QJsonArray{crop.x(), crop.y(), crop.width(), crop.height()}}});
			e.m_cropRatio->setCurrentIndex(1);
			QMetaObject::invokeMethod(e.m_cropRatio, "activated", Q_ARG(int, 1));
			auto input = e.m_cropW->findChild<QLineEdit*>();
			QMetaObject::invokeMethod(input, "textEdited", Q_ARG(QString, QString("480 px")));
			if (e.m_project.crop.size() != QSize(480,270) || e.m_cropH->value() != 270)
			{
				fail("Typed width did not update the exact locked-ratio crop immediately");
				return;
			}
			QMetaObject::invokeMethod(e.m_cropW, "editingFinished");
			r = e.m_canvas->videoRect();
			QPoint corner(qRound(r.left()+r.width()*.75),qRound(r.top()+r.height()*.75));
			QPoint smaller(qRound(r.left()+r.width()*.60),qRound(r.top()+r.height()*.60));
			Mouse(e.m_canvas,QEvent::MouseButtonPress,corner);
			Mouse(e.m_canvas,QEvent::MouseMove,smaller);
			Mouse(e.m_canvas,QEvent::MouseButtonRelease,smaller);
			const auto resized = e.m_project.crop;
			if (resized.size()!=QSize(384,216) || !e.m_cropButton->isChecked())
			{
				fail("Resizing a corner did not retain aspect ratio and handles");
				return;
			}
			record("crop_locked_pixel_input_and_corner",{{"width",resized.width()},{"height",resized.height()}});
			e.m_cropButton->setChecked(false);
			e.select(1000, 2500);
			e.applyRange(true);
			if (e.m_project.outputDuration() != 1500)
			{
				fail("Retain selection failed");
				return;
			}
			e.undo();
			if (e.m_project.outputDuration() != 6000)
			{
				fail("Undo failed");
				return;
			}
			e.select(2500, 4000);
			e.applyRange(false);
			if (e.m_project.outputDuration() != 4500)
			{
				fail("Delete selection failed");
				return;
			}
			record("retain_delete_undo", {{"retained_ms", 1500}, {"after_undo_ms", 6000}, {"after_delete_ms", 4500}});
			e.chooseTrack(0);
			e.select(1000, 2000);
			e.applyRange(false);
			if (e.m_project.audio[0].kept.size() != 2 || e.m_project.audio[1].kept.size() != 1 ||
				e.m_project.outputDuration() != 4500)
			{
				fail("Independent sound edit changed wrong track");
				return;
			}
			e.m_captionBegin->setValue(.5);
			e.m_captionEnd->setValue(1.5);
			e.m_captionText->setPlainText("同屏剪辑预览");
			press("添加");
			if (e.m_project.captions.size() != 1)
			{
				fail("Manual subtitle was not added");
				return;
			}
			record("audio_and_manual_caption", {{"mic_ranges", 2}, {"system_ranges", 1}, {"caption_count", 1}});
			e.m_captionBegin->setValue(4.0);
			e.m_captionEnd->setValue(4.5);
			e.m_captionText->setPlainText("另一条字幕");
			press("添加");
			e.chooseCaption(0);
			e.m_captionBegin->setValue(4.6);
			e.m_captionEnd->setValue(5.6);
			press("更新");
			e.m_captionText->setPlainText("调整时间后继续编辑");
			press("更新");
			if (e.m_caption != 1 || e.m_project.captions[0].text != "另一条字幕" ||
				e.m_project.captions[1].text != "调整时间后继续编辑")
			{
				fail("Caption selection did not follow reordered cue");
				return;
			}
			record("caption_reorder_preserves_selection", {{"selected_row", e.m_caption}, {"caption_count", 2}});
			if (!press("removeSubtitleCue") || e.m_project.captions.size() != 1 ||
				e.m_project.captions[0].text != "另一条字幕")
			{
				fail("Subtitle delete removed wrong cue");
				return;
			}
			record("caption_delete", {{"remaining_captions", 1}});
			e.undo();
			e.undo();
			e.undo();
			e.undo();
			e.chooseTrack(-1);
			e.seek(2400);
			m_before = e.m_canvas->frameCount();
			e.togglePlayback();
			m_playback.start();
			m_stage = 3;
		}
		else if (m_stage == 3)
		{
			if (m_playback.elapsed() < 650 || e.m_position < 4000)
				return;
			if (e.m_canvas->frameCount() <= m_before + 3)
			{
				if (m_playback.elapsed() < 3500)
					return;
				fail(QString("Decoded playback frames=%1 state=%2 position=%3 error=%4")
						 .arg(e.m_canvas->frameCount() - m_before)
						 .arg(e.m_player->state())
						 .arg(e.m_position)
						 .arg(e.m_player->errorString()));
				return;
			}
			if (e.m_player->peakBufferedFrames() > 4)
			{
				fail("Preview frame buffer grew beyond cap");
				return;
			}
			record("playback_skips_deleted_interval",
				   {{"source_position_ms", e.m_position},
					{"peak_buffered_frames", e.m_player->peakBufferedFrames()},
					{"decoded_preview_frames_added", e.m_canvas->frameCount() - m_before},
					{"kept_after_cut_ms", 4000}});
			e.m_player->pause();
			e.seek(4200);
			e.syncAudio(false);
			e.chooseTrack(0);
			if (e.m_audioPlayers[0]->volume() == 0 || e.m_audioPlayers[1]->volume() == 0)
			{
				fail(QString("Expected both sound tracks at %1 ms, got %2 and %3")
					 .arg(e.m_position).arg(e.m_audioPlayers[0]->volume()).arg(e.m_audioPlayers[1]->volume()));
				return;
			}
			e.m_trackEnabled->click();
			if (e.m_audioPlayers[0]->volume() != 0 || e.m_audioPlayers[1]->volume() == 0)
			{
				fail(QString("Sound monitor mute state mismatch at %1 ms: %2 and %3")
					 .arg(e.m_position).arg(e.m_audioPlayers[0]->volume()).arg(e.m_audioPlayers[1]->volume()));
				return;
			}
			record("per_track_sound_monitor",
				   {{"mic_volume", e.m_audioPlayers[0]->volume()}, {"system_volume", e.m_audioPlayers[1]->volume()}});
			QString error;
			if (!e.m_project.validate(error))
			{
				fail(error);
				return;
			}
			e.m_exporter.renderTimeline(e.m_project, m_output, "libx264");
			e.reject();
			if (!e.isVisible())
			{
				fail("Busy export closed the editor");
				return;
			}
			m_stage = 4;
		}
	}
	KRRClipEditor m_editor;
	QString m_report, m_output, m_error, m_firstHash;
	QJsonArray m_results;
	QTimer m_tick;
	QElapsedTimer m_wall, m_playback;
	int m_stage = 0;
	qint64 m_before = 0;
	bool m_asr, m_drag;
};
int main(int argc, char **argv)
{
	InitializeDisplayCoordinates();
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication app(argc, argv);
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder-editor-qa");
	RecorderUi::Apply(app);
	if (argc < 4)
		return 2;
	KRREditAcceptance qa(app.arguments()[1], app.arguments()[2], app.arguments()[3],
		app.arguments().contains("--asr"), app.arguments().contains("--drag"));
	return app.exec();
}
