#ifndef CHRONICLE_HELPERS_H
#define CHRONICLE_HELPERS_H

#include <chrono>

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QVariantMap>

#include "logos_types.h"

namespace chronicle {

constexpr int MAX_FILE_SIZE  = 100 * 1024 * 1024;
constexpr int CHUNK_SIZE     = 64 * 1024;
constexpr int RETRY_BUDGET_S = 90;
constexpr int MAX_TITLE_LEN  = 200;
constexpr int MAX_DESCRIPTION_LEN = 2000;
constexpr int MAX_TAGS = 20;
constexpr int MAX_TAG_LEN = 64;
constexpr int MAX_CT_LEN     = 255;
constexpr int MAX_ENVELOPE_BYTES = 8 * 1024;
constexpr int METADATA_HASH_VERSION = 1;
constexpr qint64 BASE_ATTEMPT_TIMEOUT_MS = 45 * 1000;
constexpr qint64 PER_MIB_ATTEMPT_TIMEOUT_MS = 2 * 1000;
constexpr qint64 MAX_ATTEMPT_TIMEOUT_MS = 5 * 60 * 1000;
constexpr qint64 MIN_RETRY_BUDGET_MS = RETRY_BUDGET_S * 1000;
constexpr qint64 MAX_RETRY_BUDGET_MS = 15 * 60 * 1000;

const QHash<QString, QString>& aliasMap();
const QHash<QString, QString>& extensionMap();

QString normalizeContentType(const QString& input);
QString sanitizeTitle(const QString& input);
QString sanitizeDescription(const QString& input);
QString sanitizeTag(const QString& input);
QStringList normalizeTags(const QStringList& input);
QString extensionFor(const QString& contentType);
QString stripExtension(const QString& title, const QString& ext);
QString synthesizeFilename(const QString& sanitizedTitle,
                           const QString& contentType);
QByteArray canonicalMetadataJson(const QString& contentType,
                                 qint64 sizeBytes,
                                 const QString& title,
                                 const QString& description,
                                 const QStringList& tags);
QString hashMetadata(const QString& contentType,
                     qint64 sizeBytes,
                     const QString& title,
                     const QString& description,
                     const QStringList& tags);
QJsonObject buildMetadataEnvelope(const QString& cid,
                                  const QString& contentType,
                                  qint64 sizeBytes,
                                  qint64 timestamp,
                                  const QString& title,
                                  const QString& description,
                                  const QStringList& tags,
                                  const QString& metadataHash);
bool envelopeWithinCap(const QJsonObject& envelope);

bool isTransientError(const QString& msg);
std::chrono::milliseconds computeBackoff(int attempt);
qint64 uploadAttemptTimeoutMs(qint64 sizeBytes);
qint64 uploadRetryBudgetMs(qint64 sizeBytes);

LogosResult failure(const QString& code, const QString& msg);

}  // namespace chronicle

#endif // CHRONICLE_HELPERS_H
