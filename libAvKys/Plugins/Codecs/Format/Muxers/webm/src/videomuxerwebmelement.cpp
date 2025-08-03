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

#include <QCoreApplication>
#include <QFile>
#include <QMutex>
#include <QTemporaryDir>
#include <QThread>
#include <QWaitCondition>
#include <akaudiocaps.h>
#include <akcompressedaudiocaps.h>
#include <akcompressedaudiopacket.h>
#include <akcompressedvideocaps.h>
#include <akcompressedvideopacket.h>
#include <akfrac.h>
#include <akpacket.h>
#include <akpluginmanager.h>
#include <akvideopacket.h>
#include <iak/akelement.h>
#include <mkvparser/mkvreader.h>
#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvmuxertypes.h>
#include <mkvmuxer/mkvwriter.h>

#include "videomuxerwebmelement.h"

struct AudioCodecsTable
{
    AkCompressedAudioCaps::AudioCodecID codecID;
    const char *str;

    inline static const AudioCodecsTable *table()
    {
        static const AudioCodecsTable webmAudioCodecsTable[] {
            {AkCompressedAudioCaps::AudioCodecID_vorbis , mkvmuxer::Tracks::kVorbisCodecId},
            {AkCompressedAudioCaps::AudioCodecID_opus   , mkvmuxer::Tracks::kOpusCodecId  },
            {AkCompressedAudioCaps::AudioCodecID_unknown, ""                              },
        };

        return webmAudioCodecsTable;
    }

    inline static const AudioCodecsTable *byCodecID(AkCompressedAudioCaps::AudioCodecID codecID)
    {
        auto item = table();

        for (; item->codecID; ++item)
            if (item->codecID == codecID)
                return item;

        return item;
    }

    inline static QList<AkCodecID> codecs()
    {
        QList<AkCodecID> codecs;
        auto item = table();

        for (; item->codecID; ++item)
            codecs << item->codecID;

        return codecs;
    }
};

struct VideoCodecsTable
{
    AkCompressedVideoCaps::VideoCodecID codecID;
    const char *str;

    inline static const VideoCodecsTable *table()
    {
        static const VideoCodecsTable webmVideoCodecsTable[] {
            {AkCompressedVideoCaps::VideoCodecID_vp8    , mkvmuxer::Tracks::kVp8CodecId},
            {AkCompressedVideoCaps::VideoCodecID_vp9    , mkvmuxer::Tracks::kVp9CodecId},
            {AkCompressedVideoCaps::VideoCodecID_av1    , mkvmuxer::Tracks::kAv1CodecId},
            {AkCompressedVideoCaps::VideoCodecID_unknown, ""                           },
        };

        return webmVideoCodecsTable;
    }

    inline static const VideoCodecsTable *byCodecID(AkCompressedVideoCaps::VideoCodecID codecID)
    {
        auto item = table();

        for (; item->codecID; ++item)
            if (item->codecID == codecID)
                return item;

        return item;
    }

    inline static QList<AkCodecID> codecs()
    {
        QList<AkCodecID> codecs;
        auto item = table();

        for (; item->codecID; ++item)
            codecs << item->codecID;

        return codecs;
    }
};

class VideoMuxerWebmElementPrivate
{
    public:
        VideoMuxerWebmElement *self;
        mkvmuxer::MkvWriter m_writer;
        mkvmuxer::Segment m_muxerSegment;
        uint64_t m_audioTrackIndex {0};
        uint64_t m_videoTrackIndex {0};
        bool m_accurateClusterDuration {false};
        bool m_fixedSizeClusterTimecode {false};
        bool m_liveMode {true};
        bool m_outputCues {true};
        uint64_t m_maxClusterSize {0};
        bool m_outputCuesBlockNumber {true};
        bool m_cuesBeforeClusters {false};
        uint64_t m_maxClusterDuration {0};
        uint64_t m_timeCodeScale {100000};
        qreal m_audioDuration {0.0};
        qreal m_videoDuration {0.0};
        QMutex m_mutex;
        bool m_initialized {false};
        bool m_paused {false};
        AkElementPtr m_packetSync {akPluginManager->create<AkElement>("Utils/PacketSync")};

        explicit VideoMuxerWebmElementPrivate(VideoMuxerWebmElement *self);
        ~VideoMuxerWebmElementPrivate();
        bool init();
        void uninit();
        void packetReady(const AkPacket &packet);
};

VideoMuxerWebmElement::VideoMuxerWebmElement():
    AkVideoMuxer()
{
    this->d = new VideoMuxerWebmElementPrivate(this);
    this->setMuxer(this->muxers().value(0));
}

VideoMuxerWebmElement::~VideoMuxerWebmElement()
{
    this->d->uninit();
    delete this->d;
}

QStringList VideoMuxerWebmElement::muxers() const
{
    return {"webm"};
}

AkVideoMuxer::FormatID VideoMuxerWebmElement::formatID(const QString &muxer) const
{
    Q_UNUSED(muxer)

    return FormatID_webm;
}

QString VideoMuxerWebmElement::description(const QString &muxer) const
{
    Q_UNUSED(muxer)

    return {"Webm (libwebm)"};
}

QString VideoMuxerWebmElement::extension(const QString &muxer) const
{
    Q_UNUSED(muxer)

    return {"webm"};
}

bool VideoMuxerWebmElement::gapsAllowed(AkCodecType type) const
{
    switch (type) {
    case AkCompressedCaps::CapsType_Audio:
        return false;

    case AkCompressedCaps::CapsType_Video:
        return true;

    default:
        break;
    }

    return true;
}

QList<AkCodecID> VideoMuxerWebmElement::supportedCodecs(const QString &muxer,
                                                        AkCodecType type) const
{
    Q_UNUSED(muxer)

    switch (type) {
    case AkCompressedCaps::CapsType_Audio:
        return AudioCodecsTable::codecs();

    case AkCompressedCaps::CapsType_Video:
        return VideoCodecsTable::codecs();

    case AkCompressedCaps::CapsType_Unknown:
        return AudioCodecsTable::codecs() + VideoCodecsTable::codecs();

    default:
        break;
    }

    return {};
}

AkCodecID VideoMuxerWebmElement::defaultCodec(const QString &muxer,
                                              AkCodecType type) const
{
    auto codecs = this->supportedCodecs(muxer, type);

    if (codecs.isEmpty())
        return 0;

    return codecs.first();
}

void VideoMuxerWebmElement::resetOptions()
{
    AkVideoMuxer::resetOptions();
}

AkPacket VideoMuxerWebmElement::iStream(const AkPacket &packet)
{
    if (this->d->m_paused || !this->d->m_initialized || !this->d->m_packetSync)
        return {};

    return this->d->m_packetSync->iStream(packet);
}

bool VideoMuxerWebmElement::setState(ElementState state)
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

VideoMuxerWebmElementPrivate::VideoMuxerWebmElementPrivate(VideoMuxerWebmElement *self):
    self(self)
{
    if (this->m_packetSync)
        QObject::connect(this->m_packetSync.data(),
                         &AkElement::oStream,
                         [this] (const AkPacket &packet) {
                             this->packetReady(packet);
                         });
}

VideoMuxerWebmElementPrivate::~VideoMuxerWebmElementPrivate()
{

}

bool VideoMuxerWebmElementPrivate::init()
{
    this->uninit();

    if (!this->m_packetSync)
        return false;

    this->m_audioDuration = 0.0;
    this->m_videoDuration = 0.0;
    this->m_audioTrackIndex = 0;
    this->m_videoTrackIndex = 0;

    AkCompressedVideoCaps videoCaps =
            self->streamCaps(AkCompressedCaps::CapsType_Video);

    if (!videoCaps) {
        qCritical() << "No valid video format set";

        return false;
    }

    auto vcodec = VideoCodecsTable::byCodecID(videoCaps.codec());

    if (!vcodec->codecID) {
        qCritical() << "Video codec not supported by this muxer:" << videoCaps.codec();

        return false;
    }

    AkCompressedAudioCaps audioCaps =
            self->streamCaps(AkCompressedCaps::CapsType_Audio);

    const AudioCodecsTable *acodec = nullptr;

    if (audioCaps) {
        acodec = AudioCodecsTable::byCodecID(audioCaps.codec());

        if (!acodec->codecID) {
            qCritical() << "Audio codec not supported by this muxer:" << audioCaps.codec();

            return false;
        }
    }

    auto location = self->location();

    if (!this->m_writer.Open(location.toStdString().c_str())) {
        qCritical() << "Failed to open file for writting:" << location;

        return false;
    }

    // Set Segment element attributes

    if (!this->m_muxerSegment.Init(&this->m_writer)) {
        qCritical() << "Failed to initialize the muxer segment";
        this->m_writer.Close();
        QFile::remove(location);

        return false;
    }

    this->m_muxerSegment.AccurateClusterDuration(this->m_accurateClusterDuration);
    this->m_muxerSegment.UseFixedSizeClusterTimecode(this->m_fixedSizeClusterTimecode);

    this->m_muxerSegment.set_mode(this->m_liveMode?
                                      mkvmuxer::Segment::kLive:
                                      mkvmuxer::Segment::kFile);

    if (this->m_maxClusterDuration > 0)
        this->m_muxerSegment.set_max_cluster_duration(this->m_maxClusterDuration);

    if (this->m_maxClusterSize > 0)
        this->m_muxerSegment.set_max_cluster_size(this->m_maxClusterSize);

    this->m_muxerSegment.OutputCues(this->m_outputCues);

    // Set SegmentInfo element attributes

    auto info = this->m_muxerSegment.GetSegmentInfo();
    info->set_timecode_scale(this->m_timeCodeScale);
    info->set_muxing_app(qApp->applicationName().toStdString().c_str());
    info->set_writing_app(qApp->applicationName().toStdString().c_str());

    // Add the video track to the muxer

    qInfo() << "Adding video track with format:" << videoCaps;
    this->m_videoTrackIndex =
            this->m_muxerSegment.AddVideoTrack(videoCaps.rawCaps().width(),
                                               videoCaps.rawCaps().height(),
                                               0);

    if (this->m_videoTrackIndex < 1) {
        qCritical() << "Could not add video track";
        this->m_writer.Close();
        QFile::remove(location);

        return false;
    }

    auto videoTrack =
            static_cast<mkvmuxer::VideoTrack *>(this->m_muxerSegment.GetTrackByNumber(this->m_videoTrackIndex));

    if (!videoTrack) {
        qCritical() << "Could not get video track";
        this->m_writer.Close();
        QFile::remove(location);

        return false;
    }

    videoTrack->set_name("Video");
    videoTrack->set_language("und");
    videoTrack->set_codec_id(vcodec->str);
    videoTrack->set_width(videoCaps.rawCaps().width());
    videoTrack->set_height(videoCaps.rawCaps().height());
    videoTrack->set_frame_rate(videoCaps.rawCaps().fps().value());

    // Add the audio track to the muxer

    if (audioCaps) {
        qInfo() << "Adding audio track with format:" << audioCaps;
        this->m_audioTrackIndex =
                this->m_muxerSegment.AddAudioTrack(audioCaps.rawCaps().rate(),
                                                   audioCaps.rawCaps().channels(),
                                                   0);

        if (this->m_audioTrackIndex < 1) {
            qCritical() << "Could not add audio track";
            this->m_writer.Close();
            QFile::remove(location);

            return false;
        }

        auto audioTrack =
                static_cast<mkvmuxer::AudioTrack *>(this->m_muxerSegment.GetTrackByNumber(this->m_audioTrackIndex));

        if (!audioTrack) {
            qCritical() << "Could not get audio track";
            this->m_writer.Close();
            QFile::remove(location);

            return false;
        }

        audioTrack->set_name("Audio");
        audioTrack->set_language("und");
        audioTrack->set_codec_id(acodec->str);
        audioTrack->set_bit_depth(audioCaps.rawCaps().bps());
        audioTrack->set_channels(audioCaps.rawCaps().channels());
        audioTrack->set_sample_rate(audioCaps.rawCaps().rate());
    }

    // Write the codec headers

    auto videoHeaders =
            self->streamHeaders(AkCompressedCaps::CapsType_Video);

    if (!videoHeaders.isEmpty())
        videoTrack->SetCodecPrivate(reinterpret_cast<const uint8_t *>(videoHeaders.constData()),
                                    videoHeaders.size());

    if (audioCaps) {
        auto audioHeaders =
                self->streamHeaders(AkCompressedCaps::CapsType_Audio);

        if (!audioHeaders.isEmpty()) {
            auto audioTrack =
                    static_cast<mkvmuxer::AudioTrack *>(this->m_muxerSegment.GetTrackByNumber(this->m_audioTrackIndex));

            if (audioTrack) {
                audioTrack->SetCodecPrivate(reinterpret_cast<const uint8_t *>(audioHeaders.constData()),
                                            audioHeaders.size());
            }
        }
    }

    this->m_packetSync->setProperty("audioEnabled", bool(audioCaps));
    this->m_packetSync->setProperty("discardLast", false);
    this->m_packetSync->setState(AkElement::ElementStatePlaying);

    qInfo() << "Starting Webm muxing";
    this->m_initialized = true;

    return true;
}

void VideoMuxerWebmElementPrivate::uninit()
{
    QMutexLocker mutexLocker(&this->m_mutex);

    if (!this->m_initialized)
        return;

    this->m_initialized = false;
    this->m_packetSync->setState(AkElement::ElementStateNull);

    auto audioStreamDuration =
            self->streamDuration(AkCompressedCaps::CapsType_Audio);
    qreal audioDuration = this->m_audioDuration;

    if (audioStreamDuration > 0) {
        AkCompressedAudioCaps caps =
                self->streamCaps(AkCompressedCaps::CapsType_Audio);
        audioDuration = qreal(audioStreamDuration) / caps.rawCaps().rate();
    }

    auto videoStreamDuration =
            self->streamDuration(AkCompressedCaps::CapsType_Video);
    qreal videoDuration = this->m_videoDuration;

    if (videoStreamDuration > 0) {
        AkCompressedVideoCaps caps =
                self->streamCaps(AkCompressedCaps::CapsType_Video);
        videoDuration = videoStreamDuration / caps.rawCaps().fps().value();
    }

    qreal duration =
            this->m_audioTrackIndex < 1?
                videoDuration:
                qMax(audioDuration, videoDuration);
    this->m_muxerSegment.set_duration(qRound64(duration * 1e9 / this->m_timeCodeScale));

    if (!this->m_muxerSegment.Finalize())
        qCritical() << "Finalization of segment failed";

    this->m_writer.Close();

    if (this->m_cuesBeforeClusters) {
        mkvparser::MkvReader reader;

        if (reader.Open(self->location().toStdString().c_str())) {
            qCritical() << "Filename is invalid or error while opening";
            qInfo() << "Webm muxing stopped";

            return;
        }

        QTemporaryDir tempDir;

        if (!tempDir.isValid()) {
            qCritical() << "Can't create the temporary directory";
            qInfo() << "Webm muxing stopped";

            return;
        }

        QFileInfo fileInfo(self->location());
        auto tmp = tempDir.filePath(fileInfo.baseName()
                                    + "_tmp."
                                    + fileInfo.completeSuffix());
        QFile::remove(tmp);

        if (this->m_writer.Open(tmp.toStdString().c_str())) {
            if (this->m_muxerSegment.CopyAndMoveCuesBeforeClusters(&reader,
                                                                   &this->m_writer)) {
                reader.Close();
                this->m_writer.Close();
                QFile::remove(self->location());
                QFile::rename(tmp, self->location());
            } else {
                qCritical() << "Unable to copy and move cues before clusters";
                reader.Close();
                this->m_writer.Close();
                QFile::remove(tmp);
            }
        } else {
            qCritical() << "Filename is invalid or error while opening";
            reader.Close();
            this->m_writer.Close();
            QFile::remove(tmp);
        }
    }

    this->m_paused = false;
}

void VideoMuxerWebmElementPrivate::packetReady(const AkPacket &packet)
{
    bool isAudio = packet.type() == AkPacket::PacketAudio
                   || packet.type() == AkPacket::PacketAudioCompressed;
    uint64_t track = isAudio?
                           this->m_audioTrackIndex:
                           this->m_videoTrackIndex;
    bool isKey = true;

    if (packet.type() == AkPacket::PacketVideoCompressed)
        isKey = AkCompressedVideoPacket(packet).flags()
                & AkCompressedVideoPacket::VideoPacketTypeFlag_KeyFrame;

    if (!this->m_muxerSegment.AddFrame(reinterpret_cast<const uint8_t *>(packet.constData()),
                                       packet.size(),
                                       track,
                                       uint64_t(packet.pts()
                                                * packet.timeBase().value()
                                                * 1e9),
                                       isKey)) {
        if (isAudio)
            qCritical() << "Failed to write the audio packet";
        else
            qCritical() << "Failed to write the video packet";
    }

    auto streamDuration =
            (packet.pts() + packet.duration()) * packet.timeBase().value();

    if (isAudio)
        this->m_audioDuration = streamDuration;
    else
        this->m_videoDuration = streamDuration;
}

#include "moc_videomuxerwebmelement.cpp"
