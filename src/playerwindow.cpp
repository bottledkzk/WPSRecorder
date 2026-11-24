#include "uilanguage.h"
#include "playerwindow.h"
#include "playbackslider.h"
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWindow>
#include <functional>
#include <limits>
static QString ClockText(qint64 ms)
{
	qint64 seconds = ms / 1000;
	return QString("%1:%2:%3")
		.arg(seconds / 3600, 2, 10, QLatin1Char('0'))
		.arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
		.arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
KRRPlayerWindow::KRRPlayerWindow(QMediaPlayer *player, QWidget *inlineVideo, QWidget *parent)
	: QDialog(parent, Qt::Window | Qt::FramelessWindowHint), m_player(player), m_inline(inlineVideo)
{
	setWindowTitle(Ui("视频预览 · Esc 返回"));
	setObjectName("fullscreenPlayer");
	setModal(false);
	m_layout = new QVBoxLayout(this);
	m_layout->setContentsMargins(16, 12, 16, 12);
	m_header = new QWidget(this);
	m_header->setObjectName("previewHeader");
	auto header = new QHBoxLayout(m_header);
	header->setContentsMargins(8, 4, 8, 4);
	header->addWidget(new QLabel(Ui("视频预览"), m_header));
	header->addStretch();
	m_mode = new QComboBox(m_header);
	m_mode->setObjectName("previewWindowMode");
	m_mode->addItems({Ui("铺满工作区 · 保留任务栏"), Ui("完全全屏 · 覆盖任务栏")});
	header->addWidget(m_mode);
	m_chaptersToggle = new QPushButton(Ui("录制分节"), m_header);
	m_chaptersToggle->setObjectName("previewChaptersToggle");
	m_chaptersToggle->setCheckable(true);
	header->addWidget(m_chaptersToggle);
	m_recentToggle = new QPushButton(Ui("最近录制"), m_header);
	m_recentToggle->setCheckable(true);
	header->addWidget(m_recentToggle);
	auto close = new QPushButton("×", m_header);
	close->setObjectName("previewWindowClose");
	close->setToolTip(Ui("退出放大预览 · Esc"));
	close->setFixedWidth(44);
	header->addWidget(close);
	m_layout->addWidget(m_header);
	connect(close, &QPushButton::clicked, this, &QDialog::reject);
	connect(m_mode, qOverload<int>(&QComboBox::currentIndexChanged), this,
			[this]
			{
				if (isVisible())
					fit();
			});
	for (auto screen : QGuiApplication::screens())
		connect(screen, &QScreen::availableGeometryChanged, this,
				[this]
				{
					if (isVisible() && m_mode->currentIndex() == 0)
						fit();
				});
	connect(qApp, &QGuiApplication::screenRemoved, this,
			[this]
			{
				if (isVisible())
				{
					m_screen = QGuiApplication::primaryScreen();
					fit();
				}
			});

	auto stage = new QWidget(this);
	stage->setStyleSheet("background:#111827;");
	m_stage = new QVBoxLayout(stage);
	m_stage->setContentsMargins(0, 0, 0, 0);
	m_content = new QHBoxLayout;
	m_content->addWidget(stage, 1);
	m_chaptersPanel = new QWidget(this);
	auto chaptersLayout = new QVBoxLayout(m_chaptersPanel);
	chaptersLayout->addWidget(new QLabel(Ui("录制分节"), m_chaptersPanel));
	m_markers = new QListWidget(m_chaptersPanel);
	m_markers->setObjectName("previewRecordingChapters");
	chaptersLayout->addWidget(m_markers);
	m_chaptersPanel->hide();
	m_content->addWidget(m_chaptersPanel);
	m_recentPanel = new QWidget(this);
	auto recentLayout = new QVBoxLayout(m_recentPanel);
	recentLayout->addWidget(new QLabel(Ui("最近录制"), m_recentPanel));
	m_recent = new QListWidget(m_recentPanel);
	m_recent->setObjectName("previewRecentRecordings");
	recentLayout->addWidget(m_recent);
	m_recentPanel->hide();
	m_content->addWidget(m_recentPanel);
	m_layout->addLayout(m_content, 1);
	for (auto toggle : {m_chaptersToggle, m_recentToggle})
		connect(toggle, &QPushButton::toggled, this, [this] { setChromeVisible(m_chromeVisible); });
	connect(m_recent, &QListWidget::itemActivated, this, [this](QListWidgetItem* item)
	{
		emit recordingRequested(item->data(Qt::UserRole).toString());
	});
	connect(m_markers, &QListWidget::itemClicked, this, [this](QListWidgetItem* item)
	{
		if (m_player->duration() > 0)
			m_player->setPosition(qMin(m_player->duration() - 1, item->data(Qt::UserRole).toLongLong()));
	});
	m_controls = new QWidget(this);
	m_controls->setObjectName("previewControls");
	auto footer = new QVBoxLayout(m_controls);
	footer->setContentsMargins(8, 4, 8, 4);
	auto seek = new KRRPlaybackSlider(m_controls);
	seek->setObjectName("fullscreenSeek");
	seek->setToolTip(Ui("点击或拖动定位；左右方向键前后跳转 5 秒"));
	footer->addWidget(seek);
	auto controls = new QHBoxLayout;
	auto play = new QPushButton(Ui("播放"), m_controls);
	play->setObjectName("fullscreenPlay");
	auto time = new QLabel(m_controls);
	m_exit = new QPushButton(Ui("返回主窗口 · Esc"), m_controls);
	controls->addWidget(play);
	controls->addWidget(time);
	controls->addStretch();
	controls->addWidget(m_exit);
	footer->addLayout(controls);
	m_layout->addWidget(m_controls);
	QList<QWidget*> activityWidgets{m_inline, m_header, m_controls};
	activityWidgets.append(m_header->findChildren<QWidget*>());
	activityWidgets.append(m_controls->findChildren<QWidget*>());
	for (auto widget : activityWidgets)
	{
		widget->setMouseTracking(true);
		widget->installEventFilter(this);
	}
	m_hideControls.setSingleShot(true);
	m_hideControls.setInterval(2500);
	connect(&m_hideControls, &QTimer::timeout, this, [this, seek]
	{
		if (QApplication::activePopupWidget() || seek->isSliderDown() ||
			m_chaptersToggle->isChecked() || m_recentToggle->isChecked())
			m_hideControls.start();
		else
			setChromeVisible(false);
	});
	auto refresh = [=]
	{
		seek->setRange(0, static_cast<int>(qMin<qint64>(m_player->duration(), std::numeric_limits<int>::max())));
		if (!seek->isSliderDown())
			seek->setValue(static_cast<int>(m_player->position()));
		time->setText(ClockText(m_player->position()) + " / " + ClockText(m_player->duration()));
		play->setText(m_player->state() == QMediaPlayer::PlayingState ? Ui("暂停") : Ui("播放"));
	};
	connect(m_player, &QMediaPlayer::positionChanged, this, refresh);
	connect(m_player, &QMediaPlayer::durationChanged, this, refresh);
	connect(m_player, &QMediaPlayer::stateChanged, this, refresh);
	BindPlaybackSeek(seek,m_player);
	auto toggle = [this]
	{
		if (m_player->state() == QMediaPlayer::PlayingState)
			m_player->pause();
		else
		{
			if (m_player->position() >= m_player->duration() - 50)
				m_player->setPosition(0);
			m_player->play();
		}
	};
	connect(play, &QPushButton::clicked, this, toggle);
	connect(m_exit, &QPushButton::clicked, this, &KRRPlayerWindow::exitMode);
	connect(this, &QDialog::finished, this,
			[this]
			{
				m_hideControls.stop();
				m_inline->setProperty("immersivePreview", false);
				if (!m_home)
					return;
				m_stage->removeWidget(m_inline);
				m_home->insertWidget(m_homeIndex, m_inline);
				m_home->setCurrentWidget(m_inline);
				m_inline->show();
				m_home = nullptr;
			});
	auto shortcut = [this](const QKeySequence &key, const std::function<void()> &action)
	{
		auto s = new QShortcut(key, this);
		s->setContext(Qt::WidgetWithChildrenShortcut);
		connect(s, &QShortcut::activated, this, action);
	};
	shortcut(QKeySequence(Qt::Key_Space), toggle);
	shortcut(QKeySequence(Qt::Key_Left),
			 [this] { m_player->setPosition(qMax<qint64>(0, m_player->position() - 5000)); });
	shortcut(QKeySequence(Qt::Key_Right),
			 [this] { m_player->setPosition(qMin(m_player->duration(), m_player->position() + 5000)); });
	shortcut(QKeySequence(Qt::Key_F11), [this] { m_mode->setCurrentIndex(1 - m_mode->currentIndex()); });
	shortcut(QKeySequence(Qt::Key_C), [this]
	{
		if (m_markers->count() == 0) return;
		if (m_immersive) setChromeVisible(true);
		m_chaptersToggle->toggle();
	});
	setChromeVisible(true);
	refresh();
}
void KRRPlayerWindow::setRecordings(const QJsonArray& markers, const QStringList& recent, const QString& active)
{
	m_markers->clear();
	QVector<int> points;
	if (!markers.isEmpty() && markers.first().toObject()["at_ms"].toDouble() > 0)
	{
		auto item = new QListWidgetItem(ClockText(0) + "  " + Ui("片头"), m_markers);
		item->setData(Qt::UserRole, 0);
	}
	for (int i = 0; i < markers.size(); ++i)
	{
		const auto marker = markers[i].toObject();
		const auto time = static_cast<qint64>(marker["at_ms"].toDouble());
		auto label = marker["label"].toString();
		if (label.isEmpty()) label = Ui("标记 %1").arg(i + 1);
		auto item = new QListWidgetItem(ClockText(time) + "  " + label, m_markers);
		item->setData(Qt::UserRole, time);
		item->setToolTip(item->text());
		points.append(static_cast<int>(qBound<qint64>(0, time, std::numeric_limits<int>::max())));
	}
	if (const auto seek = m_controls->findChild<QSlider*>("fullscreenSeek"))
		static_cast<KRRPlaybackSlider*>(seek)->setMarkers(points);
	m_chaptersToggle->setEnabled(!markers.isEmpty());
	if (!m_immersive) m_chaptersToggle->setChecked(!markers.isEmpty());
	m_recent->clear();
	for (const auto& path : recent)
	{
		auto item = new QListWidgetItem(QFileInfo(path).fileName(), m_recent);
		item->setData(Qt::UserRole, path);
		item->setToolTip(path);
		if (path == active) m_recent->setCurrentItem(item);
	}
	setChromeVisible(m_chromeVisible);
}

void KRRPlayerWindow::exitMode()
{
	if (m_mode->currentIndex() == 1) m_mode->setCurrentIndex(0);
	else reject();
}

void KRRPlayerWindow::keyPressEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Escape)
	{
		exitMode();
		event->accept();
		return;
	}
	QDialog::keyPressEvent(event);
}

void KRRPlayerWindow::enter(bool fullscreen)
{
	if (isVisible())
	{
		if (fullscreen) m_mode->setCurrentIndex(1);
		raise();
		activateWindow();
		return;
	}
	m_home = qobject_cast<QStackedWidget *>(m_inline->parentWidget());
	if (!m_home)
		return;
	m_homeIndex = m_home->indexOf(m_inline);
	m_home->removeWidget(m_inline);
	m_stage->addWidget(m_inline);
	m_inline->show();
	m_screen = QGuiApplication::screenAt(parentWidget()->frameGeometry().center());
	m_mode->setCurrentIndex(fullscreen ? 1 : 0);
	fit();
	raise();
	activateWindow();
	setFocus();
}

bool KRRPlayerWindow::fillsWorkArea() const
{
	return m_mode->currentIndex() == 0;
}
void KRRPlayerWindow::fit()
{
	if (!m_screen)
		m_screen = QGuiApplication::primaryScreen();
	bool full = m_mode->currentIndex() == 1;
	m_exit->setText(full ? Ui("退出全屏 · Esc") : Ui("返回主窗口 · Esc"));
	setImmersive(full);
	setWindowFlag(Qt::WindowStaysOnTopHint, full);
	if (windowHandle()) windowHandle()->setScreen(m_screen);
	if (full)
	{
		setGeometry(m_screen->geometry());
		showFullScreen();
	}
	else
	{
		setWindowState(Qt::WindowNoState);
		setGeometry(m_screen->availableGeometry());
		show();
	}
	raise();
	activateWindow();
}

void KRRPlayerWindow::setImmersive(bool immersive)
{
	m_inline->setProperty("immersivePreview", immersive);
	m_inline->update();
	if (m_immersive != immersive)
	{
		m_immersive = immersive;
		m_layout->setContentsMargins(immersive ? QMargins() : QMargins(16, 12, 16, 12));
		if (immersive)
		{
			m_layout->removeWidget(m_header);
			m_layout->removeWidget(m_controls);
			m_content->removeWidget(m_chaptersPanel);
			m_content->removeWidget(m_recentPanel);
		}
		else
		{
			m_layout->insertWidget(0, m_header);
			m_layout->addWidget(m_controls);
			m_content->addWidget(m_chaptersPanel);
			m_content->addWidget(m_recentPanel);
		}
		const auto style = immersive ? QStringLiteral("background:#202d43;color:white;") : QString();
		for (auto widget : {m_header, m_controls, m_chaptersPanel, m_recentPanel})
			widget->setStyleSheet(style);
	}
	m_chaptersToggle->setChecked(!immersive && m_markers->count() > 0);
	m_recentToggle->setChecked(false);
	m_hideControls.stop();
	setChromeVisible(!immersive);
}

void KRRPlayerWindow::setChromeVisible(bool visible)
{
	m_chromeVisible = visible;
	m_header->setVisible(visible);
	m_controls->setVisible(visible);
	m_chaptersPanel->setVisible(visible && m_chaptersToggle->isChecked() && m_markers->count() > 0);
	m_recentPanel->setVisible(visible && m_recentToggle->isChecked());
	if (m_immersive && visible)
	{
		placeChrome();
		m_hideControls.start();
	}
}

void KRRPlayerWindow::placeChrome()
{
	const int margin = 12;
	const int top = m_header->sizeHint().height();
	const int bottom = m_controls->sizeHint().height();
	const int side = qMin(width() / 3, 340);
	const int sideHeight = qMax(1, height() - top - bottom - margin * 4);
	m_header->setGeometry(margin, margin, width() - margin * 2, top);
	m_controls->setGeometry(margin, height() - bottom - margin, width() - margin * 2, bottom);
	m_chaptersPanel->setGeometry(width() - side - margin, top + margin * 2, side, sideHeight);
	m_recentPanel->setGeometry(margin, top + margin * 2, side, sideHeight);
	for (auto widget : {m_header, m_controls, m_chaptersPanel, m_recentPanel})
		widget->raise();
}

void KRRPlayerWindow::resizeEvent(QResizeEvent* event)
{
	QDialog::resizeEvent(event);
	if (m_immersive && m_chromeVisible) placeChrome();
}

bool KRRPlayerWindow::eventFilter(QObject* object, QEvent* event)
{
	if (isVisible())
	{
		const auto type = event->type();
		if (object == m_inline && type == QEvent::MouseButtonDblClick &&
			static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
		{
			m_mode->setCurrentIndex(1 - m_mode->currentIndex());
			return true;
		}
		if (m_immersive && (type == QEvent::MouseMove || type == QEvent::MouseButtonPress ||
			type == QEvent::MouseButtonRelease || type == QEvent::KeyPress))
		{
			if (m_chromeVisible) m_hideControls.start();
			else setChromeVisible(true);
		}
	}
	return QDialog::eventFilter(object, event);
}
