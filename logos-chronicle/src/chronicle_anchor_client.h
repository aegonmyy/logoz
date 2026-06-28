#ifndef CHRONICLE_ANCHOR_CLIENT_H
#define CHRONICLE_ANCHOR_CLIENT_H

#include <QLibrary>
#include <QObject>
#include <QString>

// Lazy-loads libchronicle_registry_ffi.so and exposes its C entry-points as
// typed Qt methods. All calls are JSON-in / JSON-out and blocking; the Rust
// side manages its own tokio runtime.
//
// Library search order:
//   1. CHRONICLE_FFI_PATH env var (absolute path override for dev/CI)
//   2. Sibling of chronicle_plugin.so in the Basecamp install layout
//   3. QLibrary default search (LD_LIBRARY_PATH, rpath, ldconfig)
class FfiClient : public QObject {
    Q_OBJECT
public:
    explicit FfiClient(QObject* parent = nullptr);
    ~FfiClient() override;

    bool    load(QString* err = nullptr);
    bool    isLoaded() const { return m_loaded; }
    QString lastError() const { return m_lastErr; }

    QString ffiVersion();
    QString initRegistry(const QString& argsJson);
    QString indexBatch(const QString& argsJson);
    QString getRegistry(const QString& argsJson);

private:
    using CallFn = char* (*)(const char*);
    using FreeFn = void  (*)(char*);
    using VerFn  = char* (*)();

    QString invoke(CallFn fn, const QString& argsJson, const char* name);

    QLibrary m_lib;
    bool     m_loaded    = false;
    QString  m_lastErr;
    CallFn   m_init      = nullptr;
    CallFn   m_batch     = nullptr;
    CallFn   m_getReg    = nullptr;
    FreeFn   m_free      = nullptr;
    VerFn    m_ver       = nullptr;
};

#endif // CHRONICLE_ANCHOR_CLIENT_H
