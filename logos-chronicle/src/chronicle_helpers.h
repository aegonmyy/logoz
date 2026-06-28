#ifndef CHRONICLE_HELPERS_H
#define CHRONICLE_HELPERS_H

#include <chrono>

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "logos_types.h"

namespace chronicle {

// ── Limits ────────────────────────────────────────────────────────────────
constexpr int    kMaxFileBytes        = 100 * 1024 * 1024;
constexpr int    kMaxTitleLen         = 200;
constexpr int    kMaxDescLen          = 2000;
constexpr int    kMaxTagLen           = 64;
constexpr int    kMaxTags             = 20;
constexpr int    kMaxContentTypeLen   = 255;
constexpr int    kMaxEnvelopeBytes    = 8 * 1024;
constexpr int    kHashVersion         = 1;

// ── Upload timing ─────────────────────────────────────────────────────────
constexpr qint64 kBaseTimeoutMs    = 45'000;
constexpr qint64 kPerMibTimeoutMs  = 2'000;
constexpr qint64 kMaxTimeoutMs     = 5 * 60 * 1000;
constexpr qint64 kMinBudgetMs      = 90 * 1000;
constexpr qint64 kMaxBudgetMs      = 15 * 60 * 1000;

// Content-type normalisation
QString coerceContentType(const QString& raw);
QString fileExtension(const QString& mimeType);

// Input sanitisation
QString     cleanTitle(const QString& raw);
QString     cleanDescription(const QString& raw);
QString     cleanTag(const QString& raw);
QStringList cleanTags(const QStringList& raw);

// Canonical metadata JSON for hashing — deterministic field order
QByteArray canonicalMetadata(const QString& contentType,
                              qint64 sizeBytes,
                              const QString& title,
                              const QString& description,
                              const QStringList& tags);

// SHA-256 of canonicalMetadata, returned as "v1:<64-char hex>"
QString metadataHash(const QString& contentType,
                     qint64 sizeBytes,
                     const QString& title,
                     const QString& description,
                     const QStringList& tags);

// Build a v=1 wire-format metadata envelope
QJsonObject buildEnvelope(const QString& cid,
                          const QString& contentType,
                          qint64 sizeBytes,
                          qint64 timestampMs,
                          const QString& title,
                          const QString& description,
                          const QStringList& tags,
                          const QString& metaHash);

bool envelopeFitsInCap(const QJsonObject& envelope);

// Retry / backoff
bool                      isTransient(const QString& errMsg);
std::chrono::milliseconds backoffFor(int attempt);
qint64                    uploadTimeoutMs(qint64 sizeBytes);
qint64                    uploadBudgetMs(qint64 sizeBytes);

// Convenience factory
LogosResult makeErr(const QString& code, const QString& msg);

}  // namespace chronicle

#endif // CHRONICLE_HELPERS_H
