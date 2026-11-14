#ifndef MLC_FILE_DROP_HANDLER_H
#define MLC_FILE_DROP_HANDLER_H

#include <QObject>
#include <QStringList>

class QWindow;

/// Intercepts external file drag-and-drop at the window level and exposes
/// dropped file paths to QML. Install with setupWindow() once the root
/// QQuickWindow is available.
// Manually registered in main.cpp via qmlRegisterSingletonInstance (needs setupWindow).
class FileDropHandler : public QObject {
    Q_OBJECT

    /// Most recent list of dropped file paths (local files only).
    Q_PROPERTY(QStringList droppedFiles READ droppedFiles NOTIFY filesDropped)

public:
    explicit FileDropHandler(QObject *parent = nullptr);

    QStringList droppedFiles() const { return m_droppedFiles; }

    /// Install this handler as an event filter on @p window so external
    /// drag-and-drop is captured regardless of QML DropArea limitations.
    Q_INVOKABLE void setupWindow(QWindow *window);

signals:
    /// Emitted when one or more files are dropped onto the window.
    void filesDropped(const QStringList &files);

    /// Emitted when an external drag enters the window bounds.
    void dragEntered();

    /// Emitted when an external drag leaves the window bounds.
    void dragExited();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QStringList m_droppedFiles;
};

#endif // MLC_FILE_DROP_HANDLER_H
