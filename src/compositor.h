#ifndef WPSRECORDER_COMPOSITOR_H
#define WPSRECORDER_COMPOSITOR_H
#include "recordingconfig.h"
#include <QImage>
inline constexpr int RecordingEffectsByteLimit = 512 * 1024;
void DrawCamera(QImage& output, const QImage& camera, const KRRRecordingConfig& config);
void ApplyEffects(QImage& output, const QJsonObject& effects, const QPointF& pointer, bool overlay = false,
	const QImage& background = {}, const QRectF& projection = {});
QFont AnnotationTextFont(const QJsonObject& shape, qreal width);
bool ValidateEffects(const QJsonObject& effects, QString& error);
#endif
