/* Webcamoid, camera capture application.
 * Copyright (C) 2024  Gonzalo Exequiel Pedone
 *
 * Webcamoid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Webcamoid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Webcamoid. If not, see <http://www.gnu.org/licenses/>.
 *
 * Web-Site: http://webcamoid.github.io/
 */

#include <QMutex>
#include <QThread>
#include <QTime>
#include <QVariant>
#include <akcompressedvideocaps.h>
#include <akcompressedvideopacket.h>
#include <akfrac.h>
#include <akpacket.h>
#include <akpluginmanager.h>
#include <akvideocaps.h>
#include <akvideoconverter.h>
#include <akvideopacket.h>
#include <iak/akelement.h>
#include <rav1e.h>

#include "videoencoderrav1eelement.h"

struct Av1PixFormatTable
{
    AkVideoCaps::PixelFormat pixFormat;
    RaChromaSampling av1Format;
    size_t depth;

    static inline const Av1PixFormatTable *table()
    {
        static const Av1PixFormatTable rav1ePixFormatTable[] = {
            {AkVideoCaps::Format_y8       , RA_CHROMA_SAMPLING_CS400, 8 },
            {AkVideoCaps::Format_y10      , RA_CHROMA_SAMPLING_CS400, 10},
            {AkVideoCaps::Format_y12      , RA_CHROMA_SAMPLING_CS400, 12},
            {AkVideoCaps::Format_yuv420p  , RA_CHROMA_SAMPLING_CS420, 8 },
            {AkVideoCaps::Format_yuv420p10, RA_CHROMA_SAMPLING_CS420, 10},
            {AkVideoCaps::Format_yuv420p12, RA_CHROMA_SAMPLING_CS420, 12},
            {AkVideoCaps::Format_yuv422p  , RA_CHROMA_SAMPLING_CS422, 8 },
            {AkVideoCaps::Format_yuv422p10, RA_CHROMA_SAMPLING_CS422, 10},
            {AkVideoCaps::Format_yuv422p12, RA_CHROMA_SAMPLING_CS422, 12},
            {AkVideoCaps::Format_yuv444p  , RA_CHROMA_SAMPLING_CS444, 8 },
            {AkVideoCaps::Format_yuv444p10, RA_CHROMA_SAMPLING_CS444, 10},
            {AkVideoCaps::Format_yuv444p12, RA_CHROMA_SAMPLING_CS444, 12},
            {AkVideoCaps::Format_none     , RaChromaSampling(-1)    , 0 },
        };

        return rav1ePixFormatTable;
    }

    static inline const Av1PixFormatTable *byPixFormat(AkVideoCaps::PixelFormat format)
    {
        auto fmt = table();

        for (; fmt->pixFormat != AkVideoCaps::Format_none; fmt++)
            if (fmt->pixFormat == format)
                return fmt;

        return fmt;
    }

    static inline const Av1PixFormatTable *byAv1Format(RaChromaSampling format,
                                                       size_t depth)
    {
        auto fmt = table();

        for (; fmt->pixFormat != AkVideoCaps::Format_none; fmt++)
            if (fmt->av1Format == format
                && fmt->depth == depth)
                return fmt;

        return fmt;
    }
};

class VideoEncoderRav1eElementPrivate
{
    public:
        VideoEncoderRav1eElement *self;
        AkVideoConverter m_videoConverter;
        AkCompressedVideoCaps m_outputCaps;
        AkPropertyOptions m_options;
        QByteArray m_headers;
        RaContext *m_encoder {nullptr};
        QMutex m_mutex;
        qint64 m_id {-1};
        int m_index {0};
        bool m_initialized {false};
        bool m_paused {false};
        qint64 m_encodedTimePts {0};
        AkElementPtr m_fpsControl {akPluginManager->create<AkElement>("VideoFilter/FpsControl")};

        explicit VideoEncoderRav1eElementPrivate(VideoEncoderRav1eElement *self);
        ~VideoEncoderRav1eElementPrivate();
        bool init();
        void uninit();
        void updateHeaders();
        void updateOutputCaps(const AkVideoCaps &inputCaps);
        void encodeFrame(const AkVideoPacket &src);
        void sendFrame(const RaPacket *av1Packet) const;
};

VideoEncoderRav1eElement::VideoEncoderRav1eElement():
    AkVideoEncoder()
{
    this->d = new VideoEncoderRav1eElementPrivate(this);
    this->setCodec(this->codecs().value(0));
}

VideoEncoderRav1eElement::~VideoEncoderRav1eElement()
{
    this->d->uninit();
    delete this->d;
}

QStringList VideoEncoderRav1eElement::codecs() const
{
    return {"rav1e"};
}

AkVideoEncoderCodecID VideoEncoderRav1eElement::codecID(const QString &codec) const
{
    return codec == this->codecs().first()?
                AkCompressedVideoCaps::VideoCodecID_av1:
                AkCompressedVideoCaps::VideoCodecID_unknown;
}

QString VideoEncoderRav1eElement::codecDescription(const QString &codec) const
{
    return codec == this->codecs().first()?
                QStringLiteral("AV1 (rav1e)"):
                QString();
}

AkCompressedVideoCaps VideoEncoderRav1eElement::outputCaps() const
{
    return this->d->m_outputCaps;
}

QByteArray VideoEncoderRav1eElement::headers() const
{
    return this->d->m_headers;
}

qint64 VideoEncoderRav1eElement::encodedTimePts() const
{
    return this->d->m_encodedTimePts;
}

AkPropertyOptions VideoEncoderRav1eElement::options() const
{
    return this->d->m_options;
}

AkPacket VideoEncoderRav1eElement::iVideoStream(const AkVideoPacket &packet)
{
    QMutexLocker mutexLocker(&this->d->m_mutex);

    if (this->d->m_paused || !this->d->m_initialized || !this->d->m_fpsControl)
        return {};

    bool discard = false;
    QMetaObject::invokeMethod(this->d->m_fpsControl.data(),
                              "discard",
                              Qt::DirectConnection,
                              Q_RETURN_ARG(bool, discard),
                              Q_ARG(AkVideoPacket, packet));

    if (discard)
        return {};

    this->d->m_videoConverter.begin();
    auto src = this->d->m_videoConverter.convert(packet);
    this->d->m_videoConverter.end();

    if (!src)
        return {};

    this->d->m_fpsControl->iStream(src);

    return {};
}

bool VideoEncoderRav1eElement::setState(ElementState state)
{
    auto curState = this->state();

    switch (curState) {
    case AkElement::ElementStateNull: {
        switch (state) {
        case AkElement::ElementStatePaused:
            this->d->m_paused = state == AkElement::ElementStatePaused;
        case AkElement::ElementStatePlaying:
            if (!this->d->init()) {
                this->d->m_paused = false;

                return false;
            }

            return AkElement::setState(state);
        default:
            break;
        }

        break;
    }
    case AkElement::ElementStatePaused: {
        switch (state) {
        case AkElement::ElementStateNull:
            this->d->uninit();

            return AkElement::setState(state);
        case AkElement::ElementStatePlaying:
            this->d->m_paused = false;

            return AkElement::setState(state);
        default:
            break;
        }

        break;
    }
    case AkElement::ElementStatePlaying: {
        switch (state) {
        case AkElement::ElementStateNull:
            this->d->uninit();

            return AkElement::setState(state);
        case AkElement::ElementStatePaused:
            this->d->m_paused = true;

            return AkElement::setState(state);
        default:
            break;
        }

        break;
    }
    }

    return false;
}

VideoEncoderRav1eElementPrivate::VideoEncoderRav1eElementPrivate(VideoEncoderRav1eElement *self):
    self(self)
{
    this->m_options = {
        {"speed" ,
         QObject::tr("Speed"),
         QObject::tr("Encoding speed"),
         AkPropertyOption::OptionType_Number,
         0.0,
         11.0,
         1.0,
         11.0,
         {}},
        {"lowLatency" ,
         QObject::tr("Low latency"),
         "",
         AkPropertyOption::OptionType_Boolean,
         0.0,
         1.0,
         1.0,
         1.0,
         {}},
        {"tuneContent" ,
         QObject::tr("Tune content"),
         "",
         AkPropertyOption::OptionType_String,
         0.0,
         0.0,
         0.0,
         "pnsr",
         {{"pnsr"        , QObject::tr("PNSR")        , "", "pnsr"        },
          {"psychovisual", QObject::tr("Psychovisual"), "", "psychovisual"}}},
    };

    this->m_videoConverter.setAspectRatioMode(AkVideoConverter::AspectRatioMode_Fit);

    QObject::connect(self,
                     &AkVideoEncoder::inputCapsChanged,
                     [this] (const AkVideoCaps &inputCaps) {
                         this->updateOutputCaps(inputCaps);
                     });

    if (this->m_fpsControl)
        QObject::connect(this->m_fpsControl.data(),
                         &AkElement::oStream,
                         [this] (const AkPacket &packet) {
                             this->encodeFrame(packet);
                         });
}

VideoEncoderRav1eElementPrivate::~VideoEncoderRav1eElementPrivate()
{

}

bool VideoEncoderRav1eElementPrivate::init()
{
    this->uninit();

    auto inputCaps = self->inputCaps();

    if (!inputCaps) {
        qCritical() << "Invalid input format.";

        return false;
    }

    auto eqFormat =
            Av1PixFormatTable::byPixFormat(this->m_videoConverter.outputCaps().format());

    if (eqFormat->pixFormat == AkVideoCaps::Format_none)
        eqFormat = Av1PixFormatTable::byPixFormat(AkVideoCaps::Format_yuv420p);

    auto config = rav1e_config_default();

    if (!config) {
        qCritical() << "Could not allocate rav1e config";

        return false;
    }

    rav1e_config_set_time_base(config,
                               RaRational {uint64_t(this->m_videoConverter.outputCaps().fps().den()),
                                           uint64_t(this->m_videoConverter.outputCaps().fps().num())});

    if (rav1e_config_parse_int(config,
                               "width",
                               this->m_videoConverter.outputCaps().width()) < 0) {
        qCritical() << "Invalid width passed to rav1e";
        rav1e_config_unref(config);

        return false;
    }

    if (rav1e_config_parse_int(config,
                               "height",
                               this->m_videoConverter.outputCaps().height()) < 0) {
        qCritical() << "Invalid height passed to rav1e";
        rav1e_config_unref(config);

        return false;
    }

    if (rav1e_config_parse_int(config,
                               "threads",
                               QThread::idealThreadCount()) < 0)
        qCritical() << "Invalid number of threads, defaulting to auto";

    if (rav1e_config_parse(config,
                           "tune",
                           self->optionValue("tuneContent").toString().toStdString().c_str()) < 0)
        qCritical() << "Error setting the tunning parameter";

    int speed = qBound(0, self->optionValue("speed").toInt(), 10);

    if (rav1e_config_parse_int(config, "speed", speed) < 0)
        qCritical() << "Could not set speed preset, defaulting to auto";

    if (rav1e_config_parse_int(config, "low_latency", self->optionValue("lowLatency").toInt()) < 0)
        qCritical() << "Could not set the low latency mode";

    int gop = qMax(self->gop() * this->m_videoConverter.outputCaps().fps().num()
                   / (1000 * this->m_videoConverter.outputCaps().fps().den()), 1);

    if (rav1e_config_parse_int(config, "key_frame_interval", gop) < 0) {
        qCritical() << "Could not set GOP";
        rav1e_config_unref(config);

        return false;
    }

    if (rav1e_config_parse_int(config, "bitrate", self->bitrate()) < 0) {
        qCritical() << "Could not set bitrate";
        rav1e_config_unref(config);

        return false;
    }

    if (rav1e_config_set_pixel_format(config,
                                      eqFormat->depth,
                                      eqFormat->av1Format,
                                      RA_CHROMA_SAMPLE_POSITION_UNKNOWN,
                                      RA_PIXEL_RANGE_LIMITED) < 0) {
        qCritical() << "Failed to set pixel format properties";
        rav1e_config_unref(config);

        return false;
    }

    if (rav1e_config_set_color_description(config,
                                           RA_MATRIX_COEFFICIENTS_UNSPECIFIED,
                                           RA_COLOR_PRIMARIES_UNSPECIFIED,
                                           RA_TRANSFER_CHARACTERISTICS_UNSPECIFIED) < 0) {
        qCritical() << "Failed to set color properties";
        rav1e_config_unref(config);

        return false;
    }

    this->m_encoder = rav1e_context_new(config);

    if (!this->m_encoder) {
        qCritical() << "Failed to create rav1e encode context";
        rav1e_config_unref(config);

        return false;
    }

    rav1e_config_unref(config);
    this->updateHeaders();

    if (this->m_fpsControl) {
        this->m_fpsControl->setProperty("fps", QVariant::fromValue(this->m_videoConverter.outputCaps().fps()));
        this->m_fpsControl->setProperty("fillGaps", self->fillGaps());
        QMetaObject::invokeMethod(this->m_fpsControl.data(),
                                  "restart",
                                  Qt::DirectConnection);
    }

    this->m_encodedTimePts = 0;
    this->m_initialized = true;

    return true;
}

void VideoEncoderRav1eElementPrivate::uninit()
{
    QMutexLocker mutexLocker(&this->m_mutex);

    if (!this->m_initialized)
        return;

    this->m_initialized = false;

    if (this->m_encoder) {
        RaPacket *packet = nullptr;

        for (;;) {
            auto result = rav1e_receive_packet(this->m_encoder, &packet);

            if (result != RA_ENCODER_STATUS_SUCCESS) {
                if (result != RA_ENCODER_STATUS_ENCODED) {
                    if (result != RA_ENCODER_STATUS_NEED_MORE_DATA)
                        qCritical() << "Failed receive frame: "
                                    << rav1e_status_to_str(result);

                    break;
                }

                continue;
            }

            this->sendFrame(packet);
            rav1e_packet_unref(packet);
            packet = nullptr;
        }
    }

    if (this->m_encoder) {
        rav1e_context_unref(this->m_encoder);
        this->m_encoder = nullptr;
    }

    if (this->m_fpsControl)
        QMetaObject::invokeMethod(this->m_fpsControl.data(),
                                  "restart",
                                  Qt::DirectConnection);

    this->m_paused = false;
}

void VideoEncoderRav1eElementPrivate::updateHeaders()
{
    auto ravieHeaders = rav1e_container_sequence_header(this->m_encoder);

    if (!ravieHeaders)
        return;

    QByteArray headers(reinterpret_cast<const char *>(ravieHeaders->data),
                       ravieHeaders->len);
    rav1e_data_unref(ravieHeaders);

    if (this->m_headers == headers)
        return;

    this->m_headers = headers;
    emit self->headersChanged(headers);
}

void VideoEncoderRav1eElementPrivate::updateOutputCaps(const AkVideoCaps &inputCaps)
{
    if (!inputCaps) {
        if (!this->m_outputCaps)
            return;

        this->m_outputCaps = AkCompressedVideoCaps();
        emit self->outputCapsChanged(this->m_outputCaps);

        return;
    }

    auto codecID = self->codecID(self->codec());

    if (codecID == AkCompressedVideoCaps::VideoCodecID_unknown) {
        if (!this->m_outputCaps)
            return;

        this->m_outputCaps = AkCompressedVideoCaps();
        emit self->outputCapsChanged(this->m_outputCaps);

        return;
    }

    auto eqFormat = Av1PixFormatTable::byPixFormat(inputCaps.format());

    if (eqFormat->pixFormat == AkVideoCaps::Format_none)
        eqFormat = Av1PixFormatTable::byPixFormat(AkVideoCaps::Format_yuv420p);

    auto fps = inputCaps.fps();

    if (!fps)
        fps = {30, 1};

    this->m_videoConverter.setOutputCaps({eqFormat->pixFormat,
                                          inputCaps.width(),
                                          inputCaps.height(),
                                          fps});
    AkCompressedVideoCaps outputCaps(codecID,
                                     this->m_videoConverter.outputCaps(),
                                     self->bitrate());

    if (this->m_outputCaps == outputCaps)
        return;

    this->m_outputCaps = outputCaps;
    emit self->outputCapsChanged(outputCaps);
}

void VideoEncoderRav1eElementPrivate::encodeFrame(const AkVideoPacket &src)
{
    this->m_id = src.id();
    this->m_index = src.index();

    auto frame = rav1e_frame_new(this->m_encoder);

    if (!frame) {
        qCritical() << "Could not allocate rav1e frame";

        return;
    }

    auto specs = AkVideoCaps::formatSpecs(src.caps().format());

    for (size_t plane = 0; plane < src.planes(); ++plane) {
        rav1e_frame_fill_plane(frame,
                               plane,
                               src.constPlane(plane),
                               src.planeSize(plane),
                               src.lineSize(plane),
                               specs.plane(plane).component(0).byteDepth());
    }

    bool send = true;

    while (send) {
        send = false;
        auto result = rav1e_send_frame(this->m_encoder, frame);

        if (result != RA_ENCODER_STATUS_SUCCESS) {
            if (result == RA_ENCODER_STATUS_ENOUGH_DATA) {
                send = true;
            } else {
                qCritical() << "Failed sending frame: "
                            << rav1e_status_to_str(result);

                break;
            }
        }

        RaPacket *packet = nullptr;

        for (;;) {
            auto result = rav1e_receive_packet(this->m_encoder, &packet);

            if (result != RA_ENCODER_STATUS_SUCCESS) {
                if (result != RA_ENCODER_STATUS_ENCODED) {
                    if (result != RA_ENCODER_STATUS_NEED_MORE_DATA)
                        qCritical() << "Failed receive frame: "
                                    << rav1e_status_to_str(result);

                    break;
                }

                continue;
            }

            this->sendFrame(packet);
            rav1e_packet_unref(packet);
            packet = nullptr;
        }

        if (result == RA_ENCODER_STATUS_SUCCESS) {
            this->m_encodedTimePts = src.pts() + src.duration();
            emit self->encodedTimePtsChanged(this->m_encodedTimePts);
        }
    }

    rav1e_frame_unref(frame);
}

void VideoEncoderRav1eElementPrivate::sendFrame(const RaPacket *av1Packet) const
{
    AkCompressedVideoPacket packet(this->m_outputCaps,
                                   av1Packet->len);
    memcpy(packet.data(), av1Packet->data, packet.size());
    packet.setFlags(av1Packet->frame_type == RA_FRAME_TYPE_KEY?
                        AkCompressedVideoPacket::VideoPacketTypeFlag_KeyFrame:
                        AkCompressedVideoPacket::VideoPacketTypeFlag_None);

    qint64 pts = QTime::currentTime().msecsSinceStartOfDay()
                 * this->m_outputCaps.rawCaps().fps().num()
                 / (1000 * this->m_outputCaps.rawCaps().fps().den());

    packet.setPts(pts);
    packet.setDts(pts);
    packet.setDuration(1);
    packet.setTimeBase(this->m_outputCaps.rawCaps().fps().invert());
    packet.setId(this->m_id);
    packet.setIndex(this->m_index);

    emit self->oStream(packet);
}

#include "moc_videoencoderrav1eelement.cpp"
