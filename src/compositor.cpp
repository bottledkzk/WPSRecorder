#include "compositor.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <cmath>
#include <algorithm>
static QPointF ScaledPoint(const QJsonArray& point, const QRectF& projection)
{
	return projection.topLeft() + QPointF(point[0].toDouble() * projection.width(), point[1].toDouble() * projection.height());
}
static QRectF ScaledRect(const QJsonArray& a, const QRectF& projection)
{
	if (a.size() != 4) return {};
	return QRectF(projection.x() + a[0].toDouble() * projection.width(), projection.y() + a[1].toDouble() * projection.height(),
		a[2].toDouble() * projection.width(), a[3].toDouble() * projection.height()).normalized();
}
static bool NormalizedCoordinates(const QJsonArray& values, int size, bool desktop)
{
	if (values.size() != size) return false;
	return std::all_of(values.begin(), values.end(), [desktop](const QJsonValue& value)
	{
		const auto n = value.toDouble();
		return value.isDouble() && std::isfinite(n) && (desktop ? std::abs(n) <= 4096 : n >= 0 && n <= 1);
	});
}
bool ValidateEffects(const QJsonObject& effects, QString& error)
{
	if (QJsonDocument(effects).toJson(QJsonDocument::Compact).size() > RecordingEffectsByteLimit ||
		effects["shapes"].toArray().size() > 200 || effects["masks"].toArray().size() > 40)
	{ error = "Effects exceed the bounded scene size"; return false; }
	for (const auto& mask : effects["masks"].toArray())
	{
		const auto object = mask.toObject();
		const bool desktop = object["desktop"].toBool();
		if (object.contains("points") && object["type"] != "solid")
		{
			const auto points = object["points"].toArray();
			const auto width = object["width"].toDouble();
			const auto strength = object["strength"].toInt();
			if (points.isEmpty() || points.size() > 30000 || !std::isfinite(width) || width <= 0 || width > (desktop ? 4096 : 1) ||
				strength < 4 || strength > 48 || std::any_of(points.begin(), points.end(), [desktop](const QJsonValue& point)
				{
					return !NormalizedCoordinates(point.toArray(), 2, desktop);
				}))
			{
				error = "Invalid blur stroke";
				return false;
			}
		}
		else if (!NormalizedCoordinates(object["rect"].toArray(), 4, desktop))
		{
			error = "Mask coordinates must be within 0..1";
			return false;
		}
	}
	return true;
}
void DrawCamera(QImage& output, const QImage& camera, const KRRRecordingConfig& config)
{
	if (camera.isNull()) return;
	QRectF r(config.cameraRect.x() * output.width(), config.cameraRect.y() * output.height(), config.cameraRect.width() * output.width(), config.cameraRect.height() * output.height());
	if (config.cameraShape == "circle") { double side = std::min(r.width(), r.height()); r = QRectF(r.center() - QPointF(side / 2, side / 2), QSizeF(side, side)); }
	QPainterPath path;
	if (config.cameraShape == "circle") path.addEllipse(r);
	else if (config.cameraShape == "rounded") path.addRoundedRect(r, 18, 18);
	else path.addRect(r);
	const auto scale = qMax(r.width() / camera.width(), r.height() / camera.height());
	const QSizeF visible(r.width() / scale, r.height() / scale);
	const QRectF crop(QPointF((camera.width() - visible.width()) / 2, (camera.height() - visible.height()) / 2), visible);
	QPainter painter(&output);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	painter.save();
	painter.setClipPath(path);
	if (config.cameraMirror)
	{
		painter.translate(2 * r.center().x(), 0);
		painter.scale(-1, 1);
	}
	painter.drawImage(r, camera, crop);
	painter.restore();
	const QColor border(config.cameraBorder);
	if (border.alpha() > 0)
	{
		painter.setPen(QPen(border, 3));
		painter.drawPath(path);
	}
}
static QVector<QPointF> ScaledPoints(const QJsonArray& points, const QRectF& projection)
{
	QVector<QPointF> result;
	result.reserve(points.size());
	for (const auto& point : points)
	{
		const auto a = point.toArray();
		if (a.size() == 2) result.append(ScaledPoint(a, projection));
	}
	return result;
}
static QPainterPath MaskCoverage(const QJsonObject& mask, const QRectF& projection)
{
	QPainterPath coverage;
	if (!mask.contains("points"))
	{
		coverage.addRect(ScaledRect(mask["rect"].toArray(), projection));
		return coverage;
	}
	const auto points = ScaledPoints(mask["points"].toArray(), projection);
	if (points.isEmpty()) return coverage;
	const auto diameter = qMax(1., mask["width"].toDouble() * projection.width());
	if (points.size() == 1)
	{
		coverage.addEllipse(points.first(), diameter / 2, diameter / 2);
		return coverage;
	}
	QPainterPathStroker brush;
	brush.setWidth(diameter);
	brush.setCapStyle(Qt::RoundCap);
	brush.setJoinStyle(Qt::RoundJoin);
	// Short overlapping subpaths keep a long, self-crossing brush stroke stable.
	constexpr int segments = 48;
	for (int start = 0; start + 1 < points.size(); start += segments)
	{
		QPainterPath path(points[start]);
		for (int i = start + 1; i <= qMin(start + segments, points.size() - 1); ++i)
			path.lineTo(points[i]);
		coverage.addPath(brush.createStroke(path));
	}
	coverage.setFillRule(Qt::WindingFill);
	return coverage;
}
static void Masks(QImage& output, const QJsonArray& masks, const QImage& background, const QRectF& projection)
{
	if (masks.isEmpty()) return;
	QPainter painter(&output);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	QHash<int, QImage> reducedFrames;
	for (const auto& value : masks)
	{
		const auto mask = value.toObject();
		if (background.isNull() || mask["type"] == "solid") continue;
		const auto coverage = MaskCoverage(mask, projection);
		if (coverage.isEmpty() || !coverage.controlPointRect().intersects(output.rect())) continue;
		const int strength = qBound(4, mask["strength"].toInt(24), 48);
		auto reduced = reducedFrames.constFind(strength);
		if (reduced == reducedFrames.cend())
		{
			// Keep the blur sample grid fixed to the frame as the brush path grows.
			reducedFrames.insert(strength, background.scaled(qMax(1, output.width() / strength),
				qMax(1, output.height() / strength), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
			reduced = reducedFrames.constFind(strength);
		}
		painter.save();
		painter.setClipPath(coverage);
		painter.drawImage(output.rect(), *reduced);
		painter.restore();
	}
	// The solid privacy layer is always on top, including over a later blur or magnifier.
	for (const auto& value : masks)
	{
		const auto mask = value.toObject();
		if (mask["type"] == "solid")
			painter.fillRect(ScaledRect(mask["rect"].toArray(), projection).intersected(output.rect()), QColor("#20252d"));
	}
}
QFont AnnotationTextFont(const QJsonObject& shape, qreal width)
{
	QFont font("Microsoft YaHei UI");
	font.setPixelSize(qBound(12, qRound(shape["fontSize"].toDouble(25.6) * width / 1280.0), 180));
	font.setWeight(QFont::DemiBold);
	return font;
}
void ApplyEffects(QImage& output, const QJsonObject& effects, const QPointF& pointer, bool overlay,
	const QImage& background, const QRectF& projection)
{
	if (effects.isEmpty()) return;
	const QRectF viewport = projection.isValid() ? projection : QRectF(output.rect());
	const bool whiteboard = effects["whiteboard"].toBool();
	if (whiteboard) output.fill(QColor("#fffefb"));
	const auto masks = effects["masks"].toArray();
	const bool magnifier = effects["magnifier"].toBool();
	const QImage clean = magnifier || !masks.isEmpty() ? (overlay && !whiteboard ? background : output) : QImage{};
	QPointF center(pointer.x() * output.width(), pointer.y() * output.height());
	if (magnifier && !clean.isNull())
	{
		// Magnify the masked image, including when the tool is shown over the desktop.
		QImage safe = clean;
		Masks(safe, masks, clean, viewport);
		const int sourceSide = qMax(1, qMin(safe.width(), safe.height()) / 6);
		const int x = qBound(0, qRound(pointer.x() * safe.width()) - sourceSide / 2, safe.width() - sourceSide);
		const int y = qBound(0, qRound(pointer.y() * safe.height()) - sourceSide / 2, safe.height() - sourceSide);
		const int side = qMax(1, qMin(output.width(), output.height()) / 3);
		const int margin = qMin(24, output.width() / 10);
		const int left = center.x() < output.width() / 2.0 ? output.width() - side - margin : margin;
		const QRect target(left, margin, side, side);
		QPainter painter(&output);
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		painter.drawImage(target, safe, QRect(x, y, sourceSide, sourceSide));
		painter.setPen(QPen(QColor("#8a9aaf"), 1));
		painter.drawRect(target);
	}
	QPainter painter(&output); painter.setRenderHint(QPainter::Antialiasing);
	if (effects["spotlight"].toBool())
	{
		QPainterPath dark; dark.setFillRule(Qt::OddEvenFill); dark.addRect(output.rect());
		double radius = output.width() * .12; dark.addEllipse(center, radius, radius);
		painter.fillPath(dark, QColor(0, 0, 0, 165));
	}
	for (const auto& value : effects["shapes"].toArray())
	{
		auto shape = value.toObject(); QString type = shape["type"].toString();
		const auto p = ScaledPoints(shape["points"].toArray(), viewport);
		if (p.isEmpty()) continue;
		QColor color(shape["color"].toString("#e9424c"));
		const auto width = qBound(1.0, shape["width"].toDouble(3) * viewport.width() / 1280.0, 90.);
		painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter.setBrush(Qt::NoBrush);
		if (type == "text")
		{
			QPainterPath text;
			const auto font = AnnotationTextFont(shape, viewport.width());
			text.addText(p[0], font, shape["text"].toString().left(500));
			const QColor contrast = color.lightness() > 110 ? QColor(15, 19, 26, 210) : QColor(255, 255, 255, 220);
			painter.strokePath(text, QPen(contrast, qBound(1.5, font.pixelSize() / 12.0, 4.0),
				Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			painter.fillPath(text, color);
		}
		else if (type == "pen" || (type == "highlight" && shape["freehand"].toBool()))
		{
			if (type == "highlight")
			{
				color.setAlpha(105);
				painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			}
			if (p.size() > 1) painter.drawPolyline(p.constData(), p.size());
			else painter.drawPoint(p.first());
		}
		else if (p.size() >= 2)
		{
			QRectF r(p.first(), p.last()); r = r.normalized();
			if (type == "highlight") { color.setAlpha(80); painter.fillRect(r, color); }
			else if (type == "rectangle") painter.drawRect(r);
			else
			{
				painter.drawLine(p.first(), p.last());
				double angle = std::atan2(p.last().y() - p.first().y(), p.last().x() - p.first().x());
				const double head = qMin(QLineF(p.first(), p.last()).length() * 0.5, qMax(10.0, width * 3.4));
				for (double delta : {-0.5, 0.5}) painter.drawLine(p.last(), p.last() - QPointF(std::cos(angle + delta) * head, std::sin(angle + delta) * head));
			}
		}
	}
	const auto click = effects["click"].toObject();
	const auto at = click["point"].toArray();
	const double age = at.size() == 2 ? static_cast<double>(QDateTime::currentMSecsSinceEpoch()) - click["at"].toDouble() : -1.0;
	if (age >= 0 && age < 450)
	{
		const auto position = ScaledPoint(at, viewport);
		const double progress = age / 450.0;
		const int style = click["style"].toInt();
		const double scale = qBound(.8, output.width() / 1920.0, 2.);
		QColor ink("#ffd772");
		ink.setAlpha(qRound((style == 3 ? 112 : 225) * (1.0 - progress)));
		const double radius = scale * (style == 1 ? 9 + 21 * progress : 17);
		painter.setPen(style == 3 ? Qt::NoPen : QPen(ink, 2 * scale));
		painter.setBrush(style == 3 ? QBrush(ink) : Qt::NoBrush);
		painter.drawEllipse(position, radius, radius);
	}
	painter.end();
	Masks(output, masks, clean, viewport);
}
