//
// Aspia Project
// Copyright (C) 2018 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#ifndef ASPIA_CLIENT__FILE_TRANSFER_H_
#define ASPIA_CLIENT__FILE_TRANSFER_H_

#include <QQueue>
#include <QPair>
#include <QPointer>
#include <QMap>

#include "client/file_transfer_task.h"
#include "host/file_request.h"
#include "protocol/file_transfer_session.pb.h"

namespace aspia {

class FileTransferQueueBuilder;

class FileTransfer : public QObject
{
    Q_OBJECT

public:
    enum Type
    {
        Downloader = 0,
        Uploader   = 1
    };

    enum Error
    {
        OtherError           = 0,
        DirectoryCreateError = 1,
        FileCreateError      = 2,
        FileOpenError        = 3,
        FileAlreadyExists    = 4,
        FileWriteError       = 5,
        FileReadError        = 6
    };

    enum Action
    {
        Ask        = 0,
        Abort      = 1,
        Skip       = 2,
        SkipAll    = 4,
        Replace    = 5,
        ReplaceAll = 6
    };
    Q_DECLARE_FLAGS(Actions, Action)

    struct Item
    {
        Item(const QString& name, qint64 size, bool is_directory)
            : name(name),
              size(size),
              is_directory(is_directory)
        {
            // Nothing
        }

        QString name;
        bool is_directory;
        qint64 size;
    };

    FileTransfer(Type type, QObject* parent);
    ~FileTransfer() = default;

    void start(const QString& source_path,
               const QString& target_path,
               const QList<Item>& items);

    Actions availableActions(Error error_type) const;
    Action defaultAction(Error error_type) const;
    void setDefaultAction(Error error_type, Action action);
    void applyAction(Error error_type, Action action);

    FileTransferTask& currentTask();

signals:
    void started();
    void finished();
    void currentItemChanged(const QString& source_path, const QString& target_path);
    void progressChanged(int total, int current);
    void error(FileTransfer* transfer, FileTransfer::Error error_type, const QString& message);
    void localRequest(FileRequest* request);
    void remoteRequest(FileRequest* request);

private slots:
    void targetReply(const proto::file_transfer::Request& request,
                     const proto::file_transfer::Reply& reply);
    void sourceReply(const proto::file_transfer::Request& request,
                    const proto::file_transfer::Reply& reply);
    void taskQueueError(const QString& message);
    void taskQueueReady();

private:
    void processTask(bool overwrite);
    void processNextTask();
    void processError(Error error_type, const QString& message);
    void sourceRequest(FileRequest* request);
    void targetRequest(FileRequest* request);

    // The map contains available actions for the error and the current action.
    QMap<Error, QPair<Actions, Action>> actions_;
    QPointer<FileTransferQueueBuilder> builder_;
    QQueue<FileTransferTask> tasks_;
    const Type type_;

    qint64 total_size_ = 0;
    qint64 total_transfered_size_ = 0;
    qint64 task_transfered_size_ = 0;

    int total_percentage_ = 0;
    int task_percentage_ = 0;

    DISALLOW_COPY_AND_ASSIGN(FileTransfer);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(FileTransfer::Actions)

} // namespace aspia

#endif // ASPIA_CLIENT__FILE_TRANSFER_H_
