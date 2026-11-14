#include "util/file_drop_handler.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QMimeData>
#include <QUrl>
#include <QWindow>

FileDropHandler::FileDropHandler(QObject *parent)
    : QObject(parent)
{
}

void FileDropHandler::setupWindow(QWindow *window)
{
    if (!window)
        return;

    // QWindow receives drag-and-drop events through its event() dispatch;
    // no explicit setAcceptDrops() is needed (that is QWidget-only).
    window->installEventFilter(this);
}

bool FileDropHandler::eventFilter(QObject *obj, QEvent *event)
{
    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *de = static_cast<QDragEnterEvent *>(event);
        if (de->mimeData()->hasUrls()) {
            de->acceptProposedAction();
            emit dragEntered();
            return true;
        }
        break;
    }
    case QEvent::DragMove: {
        auto *de = static_cast<QDragMoveEvent *>(event);
        if (de->mimeData()->hasUrls()) {
            de->acceptProposedAction();
            return true;
        }
        break;
    }
    case QEvent::DragLeave:
        emit dragExited();
        return true;

    case QEvent::Drop: {
        auto *de = static_cast<QDropEvent *>(event);
        if (de->mimeData()->hasUrls()) {
            QStringList files;
            const QList<QUrl> urls = de->mimeData()->urls();
            files.reserve(urls.size());
            for (const QUrl &url : urls) {
                if (url.isLocalFile())
                    files.append(url.toLocalFile());
                else
                    files.append(url.toString());
            }
            if (!files.isEmpty()) {
                m_droppedFiles = files;
                emit filesDropped(files);
            }
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(obj, event);
}
