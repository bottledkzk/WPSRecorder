#include "uilanguage.h"
#include "annotationoverlay.h"
#include "compositor.h"
#include "recordingcheck.h"
#include "uitheme.h"
#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QAbstractItemView>
#include <QCursor>
#include <QDateTime>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QRegion>
#include <QScreen>
#include <QSignalBlocker>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <windows.h>
namespace
{
QJsonArray PointArray(const QPointF& point)
{
	// Compact coordinates keep a long stroke responsive over the recording control channel.
	return {qRound(point.x() * 10000) / 10000., qRound(point.y() * 10000) / 10000.};
}

class ToolChoice : public QComboBox
{
public:
	ToolChoice(const QString& icon, QWidget* parent) : QComboBox(parent), m_icon(RecorderUi::Icon(icon))
	{
		setSizeAdjustPolicy(QComboBox::AdjustToContents);
		view()->setMinimumWidth(RecorderUi::Px(118));
		setIconSize(QSize(RecorderUi::Px(14), RecorderUi::Px(14)));
	}
	QSize sizeHint() const override
	{
		auto size = QComboBox::sizeHint();
		size.rwidth() += iconSize().width() + RecorderUi::Px(5);
		return size;
	}
	QSize minimumSizeHint() const override { return sizeHint(); }
	void setTheme(const RecorderUi::ToolbarTheme& theme)
	{
		m_icon = theme.icon(m_icon.name());
		update();
	}
protected:
	void paintEvent(QPaintEvent*) override
	{
		QStylePainter painter(this);
		QStyleOptionComboBox option;
		initStyleOption(&option);
		option.currentIcon = m_icon;
		painter.drawComplexControl(QStyle::CC_ComboBox, option);
		painter.drawControl(QStyle::CE_ComboBoxLabel, option);
	}
private:
	QIcon m_icon;
};
}

KRRAnnotationOverlay::KRRAnnotationOverlay()
{
	setWindowTitle(Ui("讲解画布"));
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
	setAttribute(Qt::WA_TranslucentBackground);
	setMouseTracking(true);
	m_backgroundTimer.setInterval(45);
	connect(&m_backgroundTimer, &QTimer::timeout, this, [this]
	{
		if (!m_capture) return;
		const auto frame = m_capture->latest();
		if (frame.sequence == m_backgroundSequence || frame.image.isNull()) return;
		m_backgroundSequence = frame.sequence;
		m_background = frame.image;
		update();
	});
	m_textInput = new QLineEdit(this);
	m_textInput->setObjectName("lectureTextInput");
	m_textInput->setPlaceholderText(Ui("输入文字"));
	m_textInput->setMaxLength(500);
	m_textInput->hide();
	connect(m_textInput, &QLineEdit::editingFinished, this, [this] { finishText(true); });
	m_toolbarWindow = new QWidget(this, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
	m_toolbarWindow->setWindowTitle(Ui("讲解工具"));
	m_toolbarWindow->setObjectName("lectureToolbarWindow");
	m_toolbarWindow->setAttribute(Qt::WA_StyledBackground);
	auto panel = new QVBoxLayout(m_toolbarWindow);
	panel->setContentsMargins(4, 4, 4, 4);
	panel->setSpacing(2);
	m_toolbar = new QToolBar(m_toolbarWindow);
	m_secondary = new QToolBar(m_toolbarWindow);
	auto primaryRow = new QHBoxLayout;
	primaryRow->setSpacing(RecorderUi::Px(4));
	m_toolbar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	primaryRow->addWidget(m_toolbar);
	panel->addLayout(primaryRow);
	panel->addWidget(m_secondary);
	m_toolbar->setObjectName("lectureToolbar");
	m_secondary->setObjectName("lectureSecondaryToolbar");
	m_toolbarWindow->installEventFilter(this);
	for (auto bar : {m_toolbar, m_secondary})
	{
		bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
		bar->setIconSize(QSize(RecorderUi::Px(16), RecorderUi::Px(16)));
	}
	auto grip = new QLabel(m_toolbar);
	grip->setObjectName("lectureToolbarGrip");
	grip->setMinimumWidth(RecorderUi::Px(16));
	grip->setAlignment(Qt::AlignCenter);
	grip->setCursor(Qt::OpenHandCursor);
	grip->setToolTip(Ui("拖动讲解工具条"));
	m_toolbarGrip = grip;
	auto gripAction = m_toolbar->addWidget(grip);
	gripAction->setProperty("compactVisible", true);
	grip->installEventFilter(this);
	for (const auto &pair : QList<QPair<QString, QString>>{{Ui("操作"), "pointer"},
														   {Ui("画笔"), "pen"},
														   {Ui("箭头"), "arrow"},
														   {Ui("矩形"), "rectangle"},
														   {Ui("高亮"), "highlight"},
														   {Ui("文字"), "text"},
														   {Ui("橡皮"), "eraser"},
														   {Ui("马赛克"), "pixel"},
														   {Ui("遮挡"), "solid"}})
	{
		auto action = m_toolbar->addAction(RecorderUi::Icon(pair.second), pair.first);
		action->setData(pair.second);
		action->setCheckable(true);
		if (pair.second == "pixel" || pair.second == "solid")
			m_toolbar->widgetForAction(action)->setProperty("privacy", true);
		connect(action, &QAction::triggered, this, [this, pair] { setTool(pair.second); });
	}
	for (const auto &pair :
		 QList<QPair<QString, QString>>{{Ui("聚光"), "spotlight"}, {Ui("放大"), "magnifier"}, {Ui("白板"), "whiteboard"}})
	{
		auto action = m_secondary->addAction(RecorderUi::Icon(pair.second), pair.first);
		action->setData(pair.second);
		action->setProperty("effectToggle", true);
		action->setCheckable(true);
		connect(action, &QAction::triggered, this,
				[this, pair]
				{
					checkpoint();
					m_effects[pair.second] = !m_effects[pair.second].toBool();
					publish();
				});
	}
	auto properties = new QWidget(m_toolbarWindow);
	auto propertyLayout = new QHBoxLayout(properties);
	propertyLayout->setContentsMargins(RecorderUi::Px(4), 0, 0, 0);
	propertyLayout->setSpacing(RecorderUi::Px(3));
	primaryRow->addWidget(properties);
	primaryRow->addStretch();
	m_colorButton = new QToolButton(properties);
	m_colorButton->setToolTip(Ui("标注颜色"));
	m_colorButton->setAccessibleName(m_colorButton->toolTip());
	m_colorButton->setFixedSize(RecorderUi::Px(23), RecorderUi::Px(23));
	propertyLayout->addWidget(m_colorButton);
	connect(m_colorButton, &QToolButton::clicked, this,
			[this]
			{
				auto& ink = m_tool == "highlight" ? m_highlightColor : m_color;
				const QColor color = QColorDialog::getColor(QColor(ink), m_toolbarWindow, Ui("标注颜色"));
				if (color.isValid())
				{
					ink = color.name();
					updateToolOptions();
				}
			});
	auto width = new ToolChoice("stroke-width", properties);
	width->setObjectName("brushWidth");
	width->setToolTip(Ui("笔画粗细"));
	for (int value : {2, 3, 6, 10, 16})
		width->addItem(QString("%1 px").arg(value), value);
	width->setCurrentIndex(1);
	m_widthChoice = width;
	width->setAccessibleName(width->toolTip());
	propertyLayout->addWidget(width);
	connect(width, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			[this, width] { m_width = width->currentData().toInt(); });
	auto highlightWidth = new ToolChoice("stroke-width", properties);
	highlightWidth->setObjectName("lectureHighlightWidth");
	highlightWidth->setToolTip(Ui("荧光笔粗细"));
	for (int value : {10, 15, 20, 28, 40})
		highlightWidth->addItem(QString("%1 px").arg(value), value);
	highlightWidth->setCurrentIndex(highlightWidth->findData(m_highlightWidth));
	m_highlightWidthChoice = highlightWidth;
	highlightWidth->setAccessibleName(highlightWidth->toolTip());
	propertyLayout->addWidget(highlightWidth);
	connect(highlightWidth, qOverload<int>(&QComboBox::currentIndexChanged), this,
		[this, highlightWidth] { m_highlightWidth = highlightWidth->currentData().toInt(); });
	auto textSize = new ToolChoice("text", properties);
	textSize->setObjectName("lectureTextSize");
	textSize->setToolTip(Ui("文字大小"));
	for (int value : {16, 20, 24, 32, 42, 56})
		textSize->addItem(QString("%1 px").arg(value), value);
	textSize->setCurrentIndex(textSize->findData(m_textSize));
	m_textSizeChoice = textSize;
	textSize->setAccessibleName(textSize->toolTip());
	propertyLayout->addWidget(textSize);
	connect(textSize, qOverload<int>(&QComboBox::currentIndexChanged), this,
		[this, textSize] { m_textSize = textSize->currentData().toInt(); });
	auto blurWidth = new ToolChoice("stroke-width", properties);
	blurWidth->setObjectName("lectureBlurWidth");
	blurWidth->setToolTip(Ui("马赛克笔刷粗细"));
	for (int diameter : {16, 32, 48, 80, 128})
		blurWidth->addItem(QString("%1 px").arg(diameter), diameter);
	blurWidth->setCurrentIndex(blurWidth->findData(m_blurWidth));
	m_blurWidthChoice = blurWidth;
	blurWidth->setAccessibleName(blurWidth->toolTip());
	propertyLayout->addWidget(blurWidth);
	connect(blurWidth, qOverload<int>(&QComboBox::currentIndexChanged), this,
		[this, blurWidth] { m_blurWidth = blurWidth->currentData().toInt(); });
	auto blurStrength = new ToolChoice("pixel", properties);
	blurStrength->setObjectName("lectureBlurStrength");
	blurStrength->setToolTip(Ui("马赛克模糊程度"));
	blurStrength->addItem(Ui("轻度模糊"), 6);
	blurStrength->addItem(Ui("中度模糊"), 18);
	blurStrength->addItem(Ui("重度模糊"), 36);
	blurStrength->setCurrentIndex(blurStrength->findData(m_blurStrength));
	m_blurStrengthChoice = blurStrength;
	blurStrength->setAccessibleName(blurStrength->toolTip());
	propertyLayout->addWidget(blurStrength);
	connect(blurStrength, qOverload<int>(&QComboBox::currentIndexChanged), this,
		[this, blurStrength] { m_blurStrength = blurStrength->currentData().toInt(); });
	auto clickStyle = new ToolChoice("click-effect", m_secondary);
	clickStyle->setObjectName("lectureClickStyle");
	clickStyle->setToolTip(Ui("点击效果"));
	clickStyle->setAccessibleName(clickStyle->toolTip());
	clickStyle->addItem(Ui("无"), 0);
	clickStyle->addItem(Ui("涟漪"), 1);
	clickStyle->addItem(Ui("光圈"), 2);
	clickStyle->addItem(Ui("高光"), 3);
	m_secondary->addWidget(clickStyle);
	connect(clickStyle, qOverload<int>(&QComboBox::currentIndexChanged), this,
		[this, clickStyle]
		{
			m_clickStyle = clickStyle->currentData().toInt();
			m_effects.remove("click");
			m_leftDown = GetAsyncKeyState(VK_LBUTTON) < 0;
			publish();
		});
	m_secondary->addSeparator();
	m_undoAction = m_secondary->addAction(RecorderUi::Icon("undo"), Ui("撤销"));
	m_redoAction = m_secondary->addAction(RecorderUi::Icon("redo"), Ui("重做"));
	m_clearAction = m_secondary->addAction(RecorderUi::Icon("trash"), Ui("清空"));
	connect(m_undoAction, &QAction::triggered, this, [this] { undo(); });
	connect(m_redoAction, &QAction::triggered, this, [this] { undo(true); });
	connect(m_clearAction, &QAction::triggered, this,
			[this]
			{
				checkpoint();
				m_effects["shapes"] = QJsonArray{};
				m_effects["masks"] = QJsonArray{};
				publish();
			});
	auto collapse = m_secondary->addAction(RecorderUi::Icon("collapse"), Ui("收起 / F8"));
	collapse->setObjectName("compact");
	connect(collapse, &QAction::triggered, this, [this] { setCompact(!m_compact); });
	m_expandAction = m_toolbar->addAction(RecorderUi::Icon("full"), Ui("展开 / F8"));
	m_expandAction->setObjectName("expand");
	connect(m_expandAction, &QAction::triggered, this, [this] { setCompact(false); });
	auto quit = m_secondary->addAction(RecorderUi::Icon("close"), Ui("退出讲解"));
	quit->setObjectName("exit");
	connect(quit, &QAction::triggered, this, &KRRAnnotationOverlay::dismiss);
	qApp->installEventFilter(this);
	connect(&m_pointerTimer, &QTimer::timeout, this, [this]
	{
		const auto point = QCursor::pos();
		const bool changed = point != m_pointer;
		m_pointer = point;
		if (m_clickStyle)
		{
			const bool down = GetAsyncKeyState(VK_LBUTTON) < 0;
			const auto local = mapFromGlobal(point);
			if (down && !m_leftDown && mask().contains(local))
			{
				m_effects["click"] = QJsonObject{{"point", PointArray(normalized(local))},
					{"at", QDateTime::currentMSecsSinceEpoch()}, {"style", m_clickStyle}};
				emit effectsChanged(m_effects);
			}
			m_leftDown = down;
		}
		const auto started = static_cast<qint64>(m_effects["click"].toObject()["at"].toDouble());
		if (started && QDateTime::currentMSecsSinceEpoch() - started > 450)
		{
			m_effects.remove("click");
			update();
		}
		else if (started || (changed && (m_effects["spotlight"].toBool() || m_effects["magnifier"].toBool()))) update();
	});
	setTheme(RecorderUi::ReadToolbarTheme());
	updateToolOptions();
}

void KRRAnnotationOverlay::setTheme(const RecorderUi::ToolbarTheme& theme)
{
	QString style = QStringLiteral(R"(
		#lectureToolbarWindow{background:#151a23;border:1px solid #3a434f;border-radius:4px;}
		#lectureToolbarWindow QToolBar{background:transparent;border:0;spacing:2px;padding:0;}
		#lectureToolbarWindow QToolBar#lectureSecondaryToolbar{border-top:1px solid #373f4b;}
		#lectureToolbarWindow QToolButton{background:transparent;border:1px solid transparent;border-radius:4px;padding:3px;}
		#lectureToolbarWindow QToolButton:hover{background:#2f3949;border-color:#455166;}
		#lectureToolbarWindow QToolButton:checked{background:#1d3558;border-color:#46628b;}
		#lectureToolbarWindow QToolButton[privacy="true"]:checked{background:#4b3c27;border-color:#826137;}
		#lectureToolbarWindow QComboBox{background:#171d27;border:1px solid #424957;border-radius:4px;padding:2px 20px 2px 5px;}
	)");
	if (theme.isLight())
		style += QString(R"(
			#lectureToolbarWindow{background:%1;border-color:%2;}
			#lectureToolbarWindow QToolBar#lectureSecondaryToolbar{border-top-color:%3;}
			#lectureToolbarWindow QToolButton:hover{background:%4;border-color:%2;}
			#lectureToolbarWindow QToolButton:checked{background:%5;border-color:%6;}
			#lectureToolbarWindow QToolButton[privacy="true"]:checked{background:#f5e7d2;border-color:#d6b68a;}
			#lectureToolbarWindow QComboBox{background:%7;border-color:%2;color:%8;}
			#lectureToolbarWindow QComboBox QAbstractItemView{background:%7;color:%8;selection-background-color:%5;selection-color:%8;}
		)").arg(theme.background.name(), theme.border.name(), theme.divider.name(), theme.hover.name(),
			theme.selected.name(), theme.selectedBorder.name(), theme.field.name(), theme.text.name());
	m_toolbarWindow->setStyleSheet(style);
	m_toolbarGrip->setPixmap(theme.icon("grip").pixmap(RecorderUi::Px(14), RecorderUi::Px(14)));
	for (const auto bar : {m_toolbar, m_secondary})
		for (auto action : bar->actions())
		{
			const auto name = action->icon().name();
			if (!name.isEmpty()) action->setIcon(theme.icon(name));
		}
	for (auto combo : m_toolbarWindow->findChildren<QComboBox*>())
		if (auto choice = dynamic_cast<ToolChoice*>(combo)) choice->setTheme(theme);
}
KRRAnnotationOverlay::~KRRAnnotationOverlay()
{
	dismiss();
	qApp->removeEventFilter(this);
}
bool KRRAnnotationOverlay::present(const QRect &area, const QJsonObject &effects, QString &error)
{
	QRegion desktop;
	for (auto screen : QGuiApplication::screens())
		if (screen->geometry().intersects(area)) desktop += screen->geometry();
	if (!area.isValid() || desktop.isEmpty())
	{
		error = Ui("无法确定讲解区域");
		return false;
	}
	m_effects = effects;
	m_effects.remove("click");
	m_leftDown = GetAsyncKeyState(VK_LBUTTON) < 0;
	m_undo.clear();
	m_redo.clear();
	const QRect surface = desktop.boundingRect();
	m_projection = area.translated(-surface.topLeft());
	m_desktopRegion = desktop.translated(-surface.topLeft());
	setGeometry(surface);
	auto screen = QGuiApplication::screenAt(area.center());
	if (!screen)
		screen = QGuiApplication::screenAt(surface.topLeft());
	const QRect safe = screen ? screen->availableGeometry() : surface;
	m_toolbarWindow->move(safe.center().x(), safe.top() + RecorderUi::Px(12));
	setCompact(false);
	m_toolbarWindow->move(safe.center().x() - m_toolbarWindow->width() / 2, m_toolbarWindow->y());
	updateToolbarHole();
	// The overlay is rendered once, into the encoded frame. Fail closed if Windows cannot exclude it.
	if (!SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()), 0x11) ||
		!SetWindowDisplayAffinity(reinterpret_cast<HWND>(m_toolbarWindow->winId()), 0x11))
	{
		dismiss();
		error = Ui("当前 Windows 会话无法排除讲解工具窗口，已关闭覆盖层以避免重复录入。可通过场景预设使用合成效果。");
		return false;
	}
	RegisterHotKey(reinterpret_cast<HWND>(winId()), 780, MOD_NOREPEAT, VK_F8);
	RegisterHotKey(reinterpret_cast<HWND>(winId()), 781, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_ESCAPE);
	setTool("pointer");
	show();
	m_toolbarWindow->show();
	m_toolbarWindow->raise();
	publish();
	emit activeChanged(true);
	m_toolbarWindow->activateWindow();
	return true;
}
void KRRAnnotationOverlay::dismiss()
{
	const bool hadStroke = m_drawing;
	m_drawing = false;
	m_erasing = false;
	finishText(false);
	m_backgroundTimer.stop();
	m_pointerTimer.stop();
	m_capture.reset();
	m_background = {};
	m_layer = {};
	m_backgroundSequence = 0;
	hide();
	m_toolbarWindow->hide();
	if (hadStroke) emit effectsChanged(m_effects);
	emit activeChanged(false);
	if (testAttribute(Qt::WA_WState_Created))
	{
		UnregisterHotKey(reinterpret_cast<HWND>(winId()), 780);
		UnregisterHotKey(reinterpret_cast<HWND>(winId()), 781);
	}
}
void KRRAnnotationOverlay::toggleInput()
{
	setTool(m_input ? "pointer" : "pen");
}
void KRRAnnotationOverlay::setTool(const QString &tool)
{
	finishText(true);
	const bool hadStroke = m_drawing;
	const bool wasInput = m_input;
	m_tool = tool;
	m_input = tool != "pointer";
	m_drawing = false;
	m_erasing = false;
	for (auto action : m_toolbar->actions())
		if (action->isCheckable() && !action->property("effectToggle").toBool())
			action->setChecked(action->data().toString() == tool);
	HWND handle = reinterpret_cast<HWND>(winId());
	LONG_PTR style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
	SetWindowLongPtrW(handle, GWL_EXSTYLE, m_input ? style & ~WS_EX_TRANSPARENT : style | WS_EX_TRANSPARENT);
	setAttribute(Qt::WA_TransparentForMouseEvents, !m_input);
	setCursor(tool == "text" ? Qt::IBeamCursor : m_input ? Qt::CrossCursor : Qt::ArrowCursor);
	updateToolOptions();
	updateBackgroundCapture();
	updateToolbarHole();
	m_toolbarWindow->raise();
	if (hadStroke) emit effectsChanged(m_effects);
	// A layered window must paint its hit-test pixels as soon as drawing is enabled.
	if (isVisible() && wasInput != m_input) repaint();
	else update();
}
void KRRAnnotationOverlay::updateToolOptions()
{
	const bool show = m_tool == "pixel" && !m_compact;
	m_widthChoice->setVisible(!m_compact && (m_tool == "pen" || m_tool == "arrow" || m_tool == "rectangle" || m_tool == "eraser"));
	m_highlightWidthChoice->setVisible(!m_compact && m_tool == "highlight");
	m_textSizeChoice->setVisible(!m_compact && m_tool == "text");
	m_blurWidthChoice->setVisible(show);
	m_blurStrengthChoice->setVisible(show);
	m_colorButton->setStyleSheet(QString("QToolButton{background:%1;border:2px solid #647184;border-radius:%2px;}")
		.arg(m_tool == "highlight" ? m_highlightColor : m_color).arg(RecorderUi::Px(11)));
	adjustToolbar();
}
void KRRAnnotationOverlay::adjustToolbar()
{
	auto options = m_colorButton->parentWidget();
	options->layout()->activate();
	options->updateGeometry();
	m_toolbar->layout()->invalidate();
	m_toolbarWindow->layout()->invalidate();
	m_toolbarWindow->layout()->activate();
	m_toolbarWindow->resize(m_toolbarWindow->sizeHint());
	if (auto screen = QGuiApplication::screenAt(m_toolbarWindow->pos()))
	{
		const auto safe = screen->geometry();
		m_toolbarWindow->move(qBound(safe.left(), m_toolbarWindow->x(), qMax(safe.left(), safe.right() - m_toolbarWindow->width() + 1)),
			qBound(safe.top(), m_toolbarWindow->y(), qMax(safe.top(), safe.bottom() - m_toolbarWindow->height() + 1)));
	}
}
void KRRAnnotationOverlay::updateToolbarHole()
{
	// A real input hole keeps tool buttons reachable even if Windows changes topmost Z order.
	QRect hole(mapFromGlobal(m_toolbarWindow->pos()), m_toolbarWindow->size());
	setMask(m_desktopRegion.subtracted(QRegion(hole.adjusted(-2, -2, 2, 2))));
}
void KRRAnnotationOverlay::updateBackgroundCapture()
{
	const auto masks = m_effects["masks"].toArray();
	bool needPicture = m_tool == "pixel" || m_effects["magnifier"].toBool();
	for (const auto& mask : masks) needPicture |= mask.toObject()["type"] != "solid";
	if (needPicture && isVisible())
	{
		if (m_capture) return;
		KRRRecordingConfig config;
		config.source = "region";
		config.region = geometry();
		config.width = width();
		config.height = height();
		config.cursor = false;
		m_capture = std::make_unique<KRRLiveCapturePreview>(config, 45, QSize(), false);
		m_backgroundTimer.start();
	}
	else
	{
		m_backgroundTimer.stop();
		m_capture.reset();
		m_background = {};
		m_backgroundSequence = 0;
	}
}
void KRRAnnotationOverlay::setCompact(bool compact)
{
	m_compact = compact;
	for (auto action : m_toolbar->actions())
		action->setVisible(action == m_expandAction ? compact : !compact || action->property("compactVisible").toBool());
	m_colorButton->parentWidget()->setVisible(!compact);
	m_secondary->setVisible(!compact);
	updateToolOptions();
	updateToolbarHole();
}
void KRRAnnotationOverlay::revealTools()
{
	setTool("pointer");
	setCompact(false);
	m_toolbarWindow->show();
	m_toolbarWindow->raise();
	m_toolbarWindow->activateWindow();
}
bool KRRAnnotationOverlay::eventFilter(QObject *object, QEvent *event)
{
	if (object == m_toolbarGrip)
	{
		if (event->type() == QEvent::MouseButtonPress)
		{
			const auto mouse = static_cast<QMouseEvent*>(event);
			if (mouse->button() == Qt::LeftButton)
			{
				m_toolbarDragOffset = mouse->globalPos() - m_toolbarWindow->pos();
				m_draggingToolbar = true;
				return true;
			}
		}
		if (m_draggingToolbar && event->type() == QEvent::MouseMove)
		{
			const auto mouse = static_cast<QMouseEvent*>(event);
			auto screen = QGuiApplication::screenAt(mouse->globalPos());
			if (screen)
			{
				const auto available = screen->geometry();
				const auto desired = mouse->globalPos() - m_toolbarDragOffset;
				m_toolbarWindow->move(qBound(available.left(), desired.x(), qMax(available.left(), available.right() - m_toolbarWindow->width() + 1)),
					qBound(available.top(), desired.y(), qMax(available.top(), available.bottom() - m_toolbarWindow->height() + 1)));
			}
			return true;
		}
		if (event->type() == QEvent::MouseButtonRelease && m_draggingToolbar)
		{
			m_draggingToolbar = false;
			return true;
		}
	}
	if (object == m_toolbarWindow && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
		updateToolbarHole();
	auto widget = qobject_cast<QWidget *>(object);
	if (isVisible() && widget && (widget->window() == this || widget->window() == m_toolbarWindow) &&
		event->type() == QEvent::KeyPress)
	{
		auto key = static_cast<QKeyEvent *>(event);
		if (key->key() == Qt::Key_Escape)
		{
			if (m_textInput->isVisible())
				finishText(false);
			else if (m_input || m_compact)
				revealTools();
			else
				dismiss();
			return true;
		}
	}
	return QWidget::eventFilter(object, event);
}
bool KRRAnnotationOverlay::nativeEvent(const QByteArray &, void *message, long *result)
{
	auto msg = static_cast<MSG *>(message);
	if (msg->message == WM_HOTKEY && (msg->wParam == 780 || msg->wParam == 781))
	{
		if (msg->wParam == 781)
			revealTools();
		else
		{
			setTool("pointer");
			setCompact(!m_compact);
			m_toolbarWindow->raise();
		}
		*result = 0;
		return true;
	}
	return false;
}
QPointF KRRAnnotationOverlay::normalized(const QPoint &p) const
{
	const auto offset = p - m_projection.topLeft();
	return QPointF(offset.x() / double(qMax(1, m_projection.width())), offset.y() / double(qMax(1, m_projection.height())));
}
void KRRAnnotationOverlay::publish()
{
	updateBackgroundCapture();
	for (auto action : m_secondary->actions())
		if (action->property("effectToggle").toBool())
		{
			bool active = m_effects[action->data().toString()].toBool();
			action->setChecked(active);
		}
	m_undoAction->setEnabled(!m_undo.isEmpty());
	m_redoAction->setEnabled(!m_redo.isEmpty());
	m_clearAction->setEnabled(!m_effects["shapes"].toArray().isEmpty() || !m_effects["masks"].toArray().isEmpty());
	updatePointerTimer();
	emit effectsChanged(m_effects);
	update();
}
void KRRAnnotationOverlay::updatePointerTimer()
{
	const bool trackPointer = isVisible() && (m_clickStyle || m_effects["spotlight"].toBool() || m_effects["magnifier"].toBool());
	const int interval = m_clickStyle ? 16 : 32;
	if (trackPointer && (!m_pointerTimer.isActive() || m_pointerTimer.interval() != interval)) m_pointerTimer.start(interval);
	if (!trackPointer) m_pointerTimer.stop();
}
void KRRAnnotationOverlay::paintEvent(QPaintEvent *)
{
	auto effects = m_effects;
	if (m_drawing) appendCurrent(effects);
	QPainter painter(this);
	const bool visibleEffects = effects["whiteboard"].toBool() || effects["spotlight"].toBool() ||
		effects["magnifier"].toBool() || effects.contains("click") || !effects["shapes"].toArray().isEmpty() || !effects["masks"].toArray().isEmpty();
	const auto surface = m_input ? QColor(0, 0, 0, 1) : QColor(Qt::transparent);
	if (!visibleEffects)
	{
		painter.fillRect(rect(), surface);
		return;
	}
	if (m_layer.size() != size()) m_layer = QImage(size(), QImage::Format_ARGB32_Premultiplied);
	m_layer.fill(surface);
	const auto cursor = mapFromGlobal(QCursor::pos());
	const QPointF pointer(cursor.x() / double(qMax(1, width())), cursor.y() / double(qMax(1, height())));
	ApplyEffects(m_layer, effects, pointer, true, m_background, m_projection);
	painter.drawImage(0, 0, m_layer);
}
void KRRAnnotationOverlay::mousePressEvent(QMouseEvent *event)
{
	if (!m_input || event->button() != Qt::LeftButton)
		return;
	if (m_tool == "eraser")
	{
		m_erasing = true;
		m_eraseCheckpoint = false;
		m_erasePoint = event->pos();
		eraseTo(event->pos());
		return;
	}
	finishText(true);
	QJsonArray points;
	points.append(PointArray(normalized(event->pos())));
	m_current = {
		{"type", m_tool}, {"color", m_color}, {"width", m_width * 1280.0 / qMax(1, m_projection.width())},
		{"points", points}, {"desktop", true}};
	if (m_tool == "highlight")
	{
		m_current["freehand"] = true;
		m_current["width"] = m_highlightWidth * 1280.0 / qMax(1, m_projection.width());
		m_current["color"] = m_highlightColor;
	}
	if (m_tool == "text")
	{
		startText(event->pos());
		return;
	}
	if (m_tool == "pixel")
	{
		m_current["width"] = m_blurWidth / double(qMax(1, m_projection.width()));
		m_current["strength"] = m_blurStrength;
	}
	m_drawing = true;
	publishStroke();
}
void KRRAnnotationOverlay::mouseMoveEvent(QMouseEvent *event)
{
	if (m_erasing)
	{
		eraseTo(event->pos());
		return;
	}
	if (!m_drawing)
		return;
	updateStroke(event->pos());
	publishStroke();
}
void KRRAnnotationOverlay::mouseReleaseEvent(QMouseEvent *event)
{
	if (m_erasing && event->button() == Qt::LeftButton)
	{
		eraseTo(event->pos());
		m_erasing = false;
		return;
	}
	if (!m_drawing || event->button() != Qt::LeftButton)
		return;
	updateStroke(event->pos());
	m_drawing = false;
	auto result = m_effects;
	QString error;
	if (appendCurrent(result) && ValidateEffects(result, error))
	{
		checkpoint();
		m_effects = result;
	}
	publish();
}

void KRRAnnotationOverlay::startText(const QPoint& point)
{
	m_current["fontSize"] = m_textSize * 1280.0 / qMax(1, m_projection.width());
	const QFont font = AnnotationTextFont(m_current, m_projection.width());
	m_textInput->setStyleSheet(QString("QLineEdit{background:#1d232d;color:#f1f4fa;border:1px solid #5576a4;"
		"border-radius:4px;padding:3px 7px;font-size:%1px;}").arg(font.pixelSize()));
	m_textInput->setFont(font);
	const int height = qMin(this->height(), m_textInput->sizeHint().height());
	const int fieldWidth = qMin(width(), qMax(220, font.pixelSize() * 8));
	const QPoint at(qBound(0, point.x(), width() - fieldWidth), qBound(0, point.y(), this->height() - height));
	m_textInput->setGeometry(QRect(at, QSize(fieldWidth, height)));
	const auto baseline = at + QPoint(8, (height + m_textInput->fontMetrics().ascent() - m_textInput->fontMetrics().descent()) / 2);
	QJsonArray points;
	points.append(PointArray(normalized(baseline)));
	m_current["points"] = points;
	m_textInput->clear();
	m_textInput->show();
	m_textInput->raise();
	activateWindow();
	m_textInput->setFocus();
}

void KRRAnnotationOverlay::finishText(bool accept)
{
	if (!m_textInput->isVisible()) return;
	const auto text = m_textInput->text().trimmed();
	{
		const QSignalBlocker blocker(m_textInput);
		m_textInput->hide();
	}
	if (!accept || text.isEmpty()) return;
	m_current["text"] = text;
	auto result = m_effects;
	QString error;
	if (appendCurrent(result) && ValidateEffects(result, error))
	{
		checkpoint();
		m_effects = result;
		publish();
	}
}

void KRRAnnotationOverlay::updateStroke(const QPoint& position)
{
	auto points = m_current["points"].toArray();
	const auto point = PointArray(normalized(position));
	if (points.last().toArray() == point) return;
	if (m_tool == "pen" || m_tool == "pixel" || m_tool == "highlight")
	{
		if (points.size() >= 2)
		{
			const auto a = points[points.size() - 2].toArray();
			const auto b = points.last().toArray();
			const auto fixed = [](const QJsonValue& value) { return qRound(value.toDouble() * 10000); };
			const qint64 dx = fixed(b[0]) - fixed(a[0]), dy = fixed(b[1]) - fixed(a[1]);
			const qint64 nx = fixed(point[0]) - fixed(b[0]), ny = fixed(point[1]) - fixed(b[1]);
			// A point on the same straight segment adds no coverage.
			if (dx * ny == dy * nx && dx * nx + dy * ny > 0) points.removeLast();
		}
		points.append(point);
	}
	else if (points.size() == 1) points.append(point);
	else points.replace(1, point);
	m_current["points"] = points;
}

bool KRRAnnotationOverlay::appendCurrent(QJsonObject& effects) const
{
	auto object = m_current;
	const auto type = object["type"].toString();
	const bool mask = type == "solid" || type == "pixel";
	const auto points = object["points"].toArray();
	if (type == "solid")
	{
		if (points.size() < 2) return false;
		const auto first = points.first().toArray(), last = points.last().toArray();
		const QRectF rect = QRectF(QPointF(first[0].toDouble(), first[1].toDouble()),
			QPointF(last[0].toDouble(), last[1].toDouble())).normalized();
		if (rect.isEmpty()) return false;
		object = {{"type", "solid"}, {"desktop", true}, {"rect", QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()}}};
	}
	else if (type != "pen" && type != "pixel" && type != "text" && type != "highlight" && points.size() < 2)
		return false;
	const QString key = mask ? "masks" : "shapes";
	auto items = effects[key].toArray();
	if (items.size() >= (mask ? 40 : 200)) return false;
	items.append(object);
	effects[key] = items;
	return true;
}

void KRRAnnotationOverlay::publishStroke()
{
	auto draft = m_effects;
	if (appendCurrent(draft)) emit effectsChanged(draft);
	update();
}

void KRRAnnotationOverlay::checkpoint()
{
	m_undo.append(m_effects);
	if (m_undo.size() > 40)
		m_undo.removeFirst();
	m_redo.clear();
}
void KRRAnnotationOverlay::undo(bool redo)
{
	auto &from = redo ? m_redo : m_undo;
	auto &to = redo ? m_undo : m_redo;
	if (from.isEmpty())
		return;
	to.append(m_effects);
	m_effects = from.takeLast();
	m_drawing = false;
	m_erasing = false;
	publish();
}
void KRRAnnotationOverlay::eraseTo(const QPoint &point)
{
	QPainterPath sweep;
	sweep.moveTo(m_erasePoint);
	sweep.lineTo(point + QPoint(1, 1));
	QPainterPathStroker eraser;
	eraser.setWidth(qMax(14, m_width * 2));
	auto hit = eraser.createStroke(sweep);
	auto shapes = m_effects["shapes"].toArray();
	bool changed = false;
	for (int i = shapes.size() - 1; i >= 0; --i)
	{
		auto shape = shapes[i].toObject();
		auto points = shape["points"].toArray();
		if (points.isEmpty())
			continue;
		auto mapped = [this](const QJsonValue &v)
		{
			auto a = v.toArray();
			return QPointF(m_projection.topLeft()) + QPointF(a[0].toDouble() * m_projection.width(), a[1].toDouble() * m_projection.height());
		};
		auto first = mapped(points.first()), last = mapped(points.last());
		QPainterPath path;
		auto type = shape["type"].toString();
		if (type == "rectangle" || (type == "highlight" && !shape["freehand"].toBool()))
			path.addRect(QRectF(first, last).normalized());
		else if (type == "text")
		{
			const QFontMetricsF metrics(AnnotationTextFont(shape, m_projection.width()));
			path.addRect(metrics.boundingRect(shape["text"].toString()).translated(first));
		}
		else if (points.size() == 1)
			path.addEllipse(first, 2, 2);
		else
		{
			path.moveTo(first);
			for (auto v : points)
				path.lineTo(mapped(v));
		}
		QPainterPathStroker stroke;
		stroke.setWidth(qMax(6., shape["width"].toDouble(3) * m_projection.width() / 1280.));
		if (hit.intersects(stroke.createStroke(path)) ||
			((type == "highlight" || type == "text" || points.size() == 1) && hit.intersects(path)))
		{
			if (!m_eraseCheckpoint)
			{
				checkpoint();
				m_eraseCheckpoint = true;
			}
			shapes.removeAt(i);
			changed = true;
		}
	}
	m_erasePoint = point;
	if (changed)
	{
		m_effects["shapes"] = shapes;
		publish();
	}
}
