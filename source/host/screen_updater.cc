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

#include "host/screen_updater.h"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QMutex>
#include <QWaitCondition>
#include <QThread>

#include "base/message_serialization.h"
#include "codec/cursor_encoder.h"
#include "codec/video_encoder_vpx.h"
#include "codec/video_encoder_zlib.h"
#include "codec/video_util.h"
#include "desktop_capture/capture_scheduler.h"
#include "desktop_capture/cursor_capturer_win.h"
#include "desktop_capture/screen_capturer_gdi.h"

namespace aspia {

class MessageEvent : public QEvent
{
public:
    static const int kType = QEvent::User + 1;

    MessageEvent(QByteArray&& buffer) noexcept
        : QEvent(static_cast<QEvent::Type>(kType)),
          buffer_(std::move(buffer))
    {
        // Nothing
    }

    const QByteArray& buffer() const { return buffer_; }

private:
    QByteArray buffer_;
    DISALLOW_COPY_AND_ASSIGN(MessageEvent);
};

class ScreenUpdaterImpl : public QThread
{
public:
    explicit ScreenUpdaterImpl(QObject* parent);
    ~ScreenUpdaterImpl();

    bool startUpdater(const proto::desktop::Config& config);
    void selectScreen(ScreenCapturer::ScreenId screen_id);

protected:
    // QThread implementation.
    void run() override;

private:
    enum class Event { NO_EVENT, SELECT_SCREEN, TERMINATE };

    QScopedPointer<CaptureScheduler> capture_scheduler_;

    QScopedPointer<ScreenCapturer> screen_capturer_;
    QScopedPointer<VideoEncoder> video_encoder_;

    QScopedPointer<CursorCapturer> cursor_capturer_;
    QScopedPointer<CursorEncoder> cursor_encoder_;

    // By default, we capture the full screen.
    ScreenCapturer::ScreenId screen_id_ = ScreenCapturer::kFullDesktopScreenId;
    int screen_count_ = 0;

    Event event_ = Event::NO_EVENT;
    QWaitCondition event_condition_;
    QMutex event_lock_;

    proto::desktop::HostToClient message_;

    DISALLOW_COPY_AND_ASSIGN(ScreenUpdaterImpl);
};

//================================================================================================
// ScreenUpdaterImpl implementation.
//================================================================================================

ScreenUpdaterImpl::ScreenUpdaterImpl(QObject* parent)
    : QThread(parent)
{
    // Nothing
}

ScreenUpdaterImpl::~ScreenUpdaterImpl()
{
    // Set the event.
    event_lock_.lock();
    event_ = Event::TERMINATE;
    event_lock_.unlock();

    // Notify the thread about the event.
    event_condition_.wakeAll();

    // Waiting for the completion of the thread.
    wait();
}

bool ScreenUpdaterImpl::startUpdater(const proto::desktop::Config& config)
{
    switch (config.video_encoding())
    {
        case proto::desktop::VIDEO_ENCODING_VP8:
            video_encoder_.reset(VideoEncoderVPX::createVP8());
            break;

        case proto::desktop::VIDEO_ENCODING_VP9:
            video_encoder_.reset(VideoEncoderVPX::createVP9());
            break;

        case proto::desktop::VIDEO_ENCODING_ZLIB:
            video_encoder_.reset(VideoEncoderZLIB::create(
                VideoUtil::fromVideoPixelFormat(config.pixel_format()),
                config.compress_ratio()));
            break;

        default:
            qWarning() << "Unsupported video encoding: " << config.video_encoding();
            break;
    }

    if (!video_encoder_)
        return false;

    if (config.features() & proto::desktop::FEATURE_CURSOR_SHAPE)
    {
        cursor_capturer_.reset(new CursorCapturerWin());
        cursor_encoder_.reset(new CursorEncoder());
    }

    capture_scheduler_.reset(
        new CaptureScheduler(std::chrono::milliseconds(config.update_interval())));

    start(QThread::HighPriority);
    return true;
}

void ScreenUpdaterImpl::selectScreen(ScreenCapturer::ScreenId screen_id)
{
    // Set the event.
    QMutexLocker locker(&event_lock_);
    event_ = Event::SELECT_SCREEN;
    screen_id_ = screen_id;

    // Notify the thread about the event.
    event_condition_.wakeAll();
}

void ScreenUpdaterImpl::run()
{
    screen_capturer_.reset(new ScreenCapturerGDI());

    while (true)
    {
        int count = screen_capturer_->screenCount();
        if (screen_count_ != count)
        {
            QMutexLocker locker(&event_lock_);

            screen_count_ = count;

            // The list of screens has changed. We do not know which screen is removed or added.
            // We display the full desktop and send a new list of screens.
            screen_id_ = ScreenCapturer::kFullDesktopScreenId;

            ScreenCapturer::ScreenList screens;
            if (screen_capturer_->screenList(&screens))
            {
                message_.Clear();

                for (const auto& screen : screens)
                {
                    proto::desktop::Screen* item = message_.mutable_screen_list()->add_screen();

                    item->set_id(screen.id);
                    item->set_title(screen.title.toStdString());
                }

                QApplication::postEvent(parent(), new MessageEvent(serializeMessage(message_)));
            }

            screen_capturer_->selectScreen(screen_id_);
        }

        capture_scheduler_->beginCapture();

        const DesktopFrame* screen_frame = screen_capturer_->captureFrame();
        if (screen_frame)
        {
            message_.Clear();

            if (!screen_frame->constUpdatedRegion().isEmpty())
                video_encoder_->encode(screen_frame, message_.mutable_video_packet());

            if (cursor_capturer_ && cursor_encoder_)
            {
                std::unique_ptr<MouseCursor> mouse_cursor(cursor_capturer_->captureCursor());
                if (mouse_cursor)
                {
                    cursor_encoder_->encode(std::move(mouse_cursor),
                                            message_.mutable_cursor_shape());
                }
            }

            if (message_.has_video_packet() || message_.has_cursor_shape())
                QApplication::postEvent(parent(), new MessageEvent(serializeMessage(message_)));
        }

        capture_scheduler_->endCapture();

        std::chrono::milliseconds delay = capture_scheduler_->nextCaptureDelay();

        event_lock_.lock();
        event_condition_.wait(&event_lock_, delay.count());

        switch (event_)
        {
            case Event::NO_EVENT:
                break;

            case Event::TERMINATE:
                event_lock_.unlock();
                return;

            case Event::SELECT_SCREEN:
                screen_capturer_->selectScreen(screen_id_);
                break;
        }

        event_ = Event::NO_EVENT;
        event_lock_.unlock();
    }
}

//================================================================================================
// ScreenUpdater implementation.
//================================================================================================

ScreenUpdater::ScreenUpdater(QObject* parent)
    : QObject(parent)
{
    // Nothing
}

bool ScreenUpdater::start(const proto::desktop::Config& config)
{
    impl_ = new ScreenUpdaterImpl(this);
    return impl_->startUpdater(config);
}

void ScreenUpdater::selectScreen(int64_t screen_id)
{
    impl_->selectScreen(screen_id);
}

void ScreenUpdater::customEvent(QEvent* event)
{
    if (event->type() != MessageEvent::kType)
        return;

    MessageEvent* message_event = dynamic_cast<MessageEvent*>(event);
    if (!message_event)
        return;

    emit sendMessage(message_event->buffer());
}

} // namespace aspia
