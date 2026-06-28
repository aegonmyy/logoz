#include "chronicle_helpers.h"

#include <algorithm>
#include <cmath>
#include <random>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace chronicle {

namespace {

// Sorted MIME aliases → canonical form
const QHash<QString, QString>& mimeAliases() {
    static const QHash<QString, QString> t = {
        {"application/x-pdf",        "application/pdf"},
        {"image/jpg",                "image/jpeg"},
        {"audio/mp3",                "audio/mpeg"},
        {"text/javascript",          "application/javascript"},
        {"application/x-javascript", "application/javascript"},
        {"text/x-markdown",          "text/markdown"},
    };
    return t;
}

// Canonical MIME → preferred file extension
const QHash<QString, QString>& extTable() {
    static const QHash<QString, QString> t = {
        {"application/pdf",          "pdf"},
        {"application/json",         "json"},
        {"application/zip",          "zip"},
        {"application/octet-stream", "bin"},
        {"text/plain",               "txt"},
        {"text/markdown",            "md"},
        {"text/csv",                 "csv"},
        {"text/html",                "html"},
        {"image/jpeg",               "jpg"},
        {"image/png",                "png"},
        {"image/gif",                "gif"},
        {"image/webp",               "webp"},
        {"audio/mpeg",               "mp3"},
        {"audio/ogg",                "ogg"},
        {"video/mp4",                "mp4"},
        {"video/webm",               "webm"},
    };
    return t;
}

// Produce a JSON-string literal from value (quoted, escaped).
QByteArray jsonStr(const QString& value) {
    QJsonObject tmp;
    tmp.insert(QStringLiteral("v"), value);
    const QByteArray doc = QJsonDocument(tmp).toJson(QJsonDocument::Compact);
    // Extract just the value portion: {"v":"..."} → "..."
    const int colon = doc.indexOf(':');
    return doc.mid(colon + 1, doc.size() - colon - 2);
}

} // anonymous namespace

// ── Content-type ─────────────────────────────────────────────────────────

QString coerceContentType(const QString& raw) {
    QString s = raw.trimmed().toLower();
    // Strip parameters (e.g. "text/html; charset=utf-8" → "text/html")
    const int semi = s.indexOf(';');
    if (semi >= 0) s = s.left(semi).trimmed();
    if (s.size() > kMaxContentTypeLen) s = s.left(kMaxContentTypeLen);

    if (const auto it = mimeAliases().find(s); it != mimeAliases().end())
        return it.value();

    const int slash = s.indexOf('/');
    if (s.isEmpty() || slash <= 0 || slash == s.size() - 1)
        return QStringLiteral("application/octet-stream");

    return s;
}

QString fileExtension(const QString& mimeType) {
    auto it = extTable().find(mimeType);
    return it != extTable().end() ? it.value() : QString();
}

// ── Input sanitisation ────────────────────────────────────────────────────

QString cleanTitle(const QString& raw) {
    QString s = raw.trimmed();
    s.replace('/', '_').replace('\\', '_');
    if (s.size() > kMaxTitleLen) s = s.left(kMaxTitleLen);
    return s;
}

QString cleanDescription(const QString& raw) {
    QString s = raw.trimmed();
    if (s.size() > kMaxDescLen) s = s.left(kMaxDescLen);
    return s;
}

QString cleanTag(const QString& raw) {
    QString s = raw.trimmed();
    if (s.size() > kMaxTagLen) s = s.left(kMaxTagLen);
    return s;
}

QStringList cleanTags(const QStringList& raw) {
    QStringList out;
    out.reserve(std::min(raw.size(), kMaxTags));
    for (const QString& t : raw) {
        if (out.size() >= kMaxTags) break;
        const QString tag = cleanTag(t);
        if (!tag.isEmpty()) out.append(tag);
    }
    return out;
}

// ── Metadata hashing ──────────────────────────────────────────────────────

QByteArray canonicalMetadata(const QString& contentType,
                              qint64 sizeBytes,
                              const QString& title,
                              const QString& description,
                              const QStringList& tags)
{
    // Fields in alphabetical order for determinism.
    const QString ct   = coerceContentType(contentType);
    const QString ttl  = cleanTitle(title);
    const QString desc = cleanDescription(description);
    const QStringList tgs = cleanTags(tags);

    QByteArray json;
    json += "{\"content_type\":";
    json += jsonStr(ct);
    json += ",\"description\":";
    json += jsonStr(desc);
    json += ",\"size_bytes\":";
    json += QByteArray::number(std::max<qint64>(0, sizeBytes));
    json += ",\"tags\":[";
    for (int i = 0; i < tgs.size(); ++i) {
        if (i > 0) json += ',';
        json += jsonStr(tgs[i]);
    }
    json += "],\"title\":";
    json += jsonStr(ttl);
    json += "}";
    return json;
}

QString metadataHash(const QString& contentType,
                     qint64 sizeBytes,
                     const QString& title,
                     const QString& description,
                     const QStringList& tags)
{
    const QByteArray data =
        canonicalMetadata(contentType, sizeBytes, title, description, tags);
    const QString hex = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    return QStringLiteral("v%1:%2").arg(kHashVersion).arg(hex);
}

// ── Envelope ──────────────────────────────────────────────────────────────

QJsonObject buildEnvelope(const QString& cid,
                          const QString& contentType,
                          qint64 sizeBytes,
                          qint64 timestampMs,
                          const QString& title,
                          const QString& description,
                          const QStringList& tags,
                          const QString& metaHash)
{
    QJsonArray tagArr;
    for (const QString& t : cleanTags(tags)) tagArr.append(t);

    QJsonObject obj;
    obj.insert(QStringLiteral("v"),             1);
    obj.insert(QStringLiteral("cid"),           cid);
    obj.insert(QStringLiteral("content_type"),  coerceContentType(contentType));
    obj.insert(QStringLiteral("size_bytes"),    sizeBytes);
    obj.insert(QStringLiteral("timestamp"),     timestampMs);
    obj.insert(QStringLiteral("title"),         cleanTitle(title));
    obj.insert(QStringLiteral("description"),   cleanDescription(description));
    obj.insert(QStringLiteral("tags"),          tagArr);
    obj.insert(QStringLiteral("metadata_hash"), metaHash);
    return obj;
}

bool envelopeFitsInCap(const QJsonObject& envelope) {
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact).size()
           <= kMaxEnvelopeBytes;
}

// ── Retry / backoff ───────────────────────────────────────────────────────

bool isTransient(const QString& errMsg) {
    const QString m = errMsg.toLower();
    // Permanent errors: don't retry
    for (const auto* kw : {"invalid", "validation", "rejected",
                           "malformed", "not allowed"}) {
        if (m.contains(QLatin1String(kw))) return false;
    }
    return true;
}

std::chrono::milliseconds backoffFor(int attempt) {
    // Exponential: 1s, 2s, 4s, 8s, 16s, 30s cap
    const int seconds = std::min(1 << std::max(0, attempt - 1), 30);
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> jitter(0.75, 1.25);
    return std::chrono::milliseconds(
        static_cast<int64_t>(seconds * 1000.0 * jitter(rng)));
}

qint64 uploadTimeoutMs(qint64 sizeBytes) {
    const qint64 mib = static_cast<qint64>(
        std::ceil(static_cast<double>(std::max<qint64>(0, sizeBytes)) /
                  (1024.0 * 1024.0)));
    return std::clamp(kBaseTimeoutMs + mib * kPerMibTimeoutMs,
                      kBaseTimeoutMs, kMaxTimeoutMs);
}

qint64 uploadBudgetMs(qint64 sizeBytes) {
    return std::clamp(uploadTimeoutMs(sizeBytes) * 3,
                      kMinBudgetMs, kMaxBudgetMs);
}

// ── Misc ──────────────────────────────────────────────────────────────────

LogosResult makeErr(const QString& code, const QString& msg) {
    return {false, code, msg};
}

}  // namespace chronicle
